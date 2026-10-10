/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "LibJSCToolsTestUtilities.h"

#include <JavaScriptCore/CorpsePlatform.h>

#if HAVE(LLDB)

#include <JavaScriptCore/CallData.h>
#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseHeapWalk.h>
#include <JavaScriptCore/CorpseImage.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/DeferGCInlines.h>
#include <JavaScriptCore/HeapSnapshotBuilder.h>
#include <JavaScriptCore/Identifier.h>
#include <JavaScriptCore/JSArrayBuffer.h>
#include <JavaScriptCore/JSArrayBufferView.h>
#include <JavaScriptCore/JSCast.h>
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <JavaScriptCore/JSDestructibleObject.h>
#include <JavaScriptCore/JSFunction.h>
#include <JavaScriptCore/JSGlobalObject.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSObjectInlines.h>
#include <JavaScriptCore/JSString.h>
#include <JavaScriptCore/SubspaceInlines.h>
#include <JavaScriptCore/ObjectConstructor.h>
#include <JavaScriptCore/SourceCode.h>
#include <JavaScriptCore/StrongInlines.h>
#include <JavaScriptCore/StructureInlines.h>
#include <JavaScriptCore/VM.h>
#include <JavaScriptCore/Weak.h>
#include <JavaScriptCore/WeakHandleOwner.h>
#include <JavaScriptCore/WeakInlines.h>
#include <array>
#include <algorithm>
#include <bit>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits>
#include <tuple>
#include <unistd.h>
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/HexNumber.h>
#include <wtf/JSONValues.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RefCounted.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSCToolsTest {

namespace {

using JSC::Corpse::Address;
using JSC::Corpse::HeapWalk;
using JSC::Corpse::Snapshot;
namespace JSCastingHelpers = JSC::JSCastingHelpers; // For CREATE_METHOD_TABLE.

// Each demo leaks from a timer. Its timers fire this many times before the first
// snapshot, so that its code and shapes exist by then, and this many times between
// the two snapshots.
constexpr unsigned warmUpTicks = 4;
constexpr unsigned leakingTicks = 64;

// What the browser gives the demos, in C++ as WebCore has it, so that what holds
// a leak is a C++ object, as in a web process.

// WebCore's ScriptWrappable and EventTarget: a C++ object a JS wrapper wraps, which
// holds that wrapper weakly, and its event listeners' functions.
class TestWrappable : public RefCounted<TestWrappable> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(TestWrappable);
public:
    virtual ~TestWrappable() = default;
    // What the wrapper adds as an opaque root, as JSNode::visitAdditionalChildren adds
    // its node's root.
    virtual void* opaqueRoot() { return nullptr; }

    void addEventListener(JSC::JSObject* function)
    {
        Locker locker { listenersLock };
        listeners.append(JSC::Weak<JSC::JSObject>(function));
    }

    template<typename Visitor>
    void visitEventListeners(Visitor& visitor)
    {
        Locker locker { listenersLock };
        for (auto& listener : listeners) {
            if (auto* function = listener.get())
                visitor.appendUnbarriered(function);
        }
    }

    JSC::Weak<JSC::JSObject> wrapper;

private:
    // JSEventListener holds its function in a Weak, which its target's wrapper visits.
    Lock listenersLock;
    Vector<JSC::Weak<JSC::JSObject>> listeners WTF_GUARDED_BY_LOCK(listenersLock);
};

// WebCore's Node: a tree, whose root is its nodes' opaque root.
class TestNode : public TestWrappable {
public:
    void* opaqueRoot() final
    {
        TestNode* node = this;
        while (node->parent)
            node = node->parent;
        return node;
    }

    void appendChild(Ref<TestNode>&& child)
    {
        RELEASE_ASSERT(!child->parent);
        child->parent = this;
        children.append(WTF::move(child));
    }

    void remove()
    {
        RELEASE_ASSERT(parent);
        Ref protectedThis { *this };
        parent->children.removeFirstMatching([&](auto& child) { return child.ptr() == this; });
        parent = nullptr;
    }

    TestNode* parent { nullptr };
    Vector<Ref<TestNode>> children;
};

// WebCore's Element.
class TestElement final : public TestNode {
public:
    ~TestElement() final = default;
};

// WebCore's Document, which a global variable holds, as a web process's
// Page holds its frames' documents. Its first child is its body.
class TestDocument final : public TestNode {
public:
    ~TestDocument() final = default;
};

// WebCore's SerializedScriptValue: what postMessage copies a message into.
class TestSerializedValue final : public ThreadSafeRefCounted<TestSerializedValue> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(TestSerializedValue);
public:
    Vector<uint8_t> data;
};

// WebCore's Worker and the run loop of its thread: messages wait in a queue of
// tasks until the worker takes them, which a busy worker does not.
class TestWorker final : public TestWrappable {
public:
    ~TestWorker() final = default;

    Deque<Function<void()>> queuedTasks;
};

// JSDOMWrapper<T>: the JS object of a TestWrappable, which owns it.
class JSTestWrapper final : public JSC::JSDestructibleObject {
public:
    using Base = JSC::JSDestructibleObject;

    template<typename CellType, JSC::SubspaceAccess>
    static JSC::CompleteSubspace* subspaceFor(JSC::VM& vm)
    {
        return &vm.destructibleObjectSpace();
    }

    static JSTestWrapper* create(JSC::VM& vm, JSC::Structure* structure, Ref<TestWrappable>&& wrapped)
    {
        auto* wrapper = new (NotNull, JSC::allocateCell<JSTestWrapper>(vm)) JSTestWrapper(vm, structure, WTF::move(wrapped));
        wrapper->finishCreation(vm);
        return wrapper;
    }

    static JSC::Structure* createStructure(JSC::VM& vm, JSC::JSGlobalObject* globalObject, JSC::JSValue prototype)
    {
        return JSC::Structure::create(vm, globalObject, prototype, JSC::TypeInfo(JSC::ObjectType, StructureFlags), info());
    }

    static void destroy(JSC::JSCell* cell)
    {
        static_cast<JSTestWrapper*>(cell)->JSTestWrapper::~JSTestWrapper();
    }

    TestWrappable& wrapped() const { return m_wrapped; }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

private:
    JSTestWrapper(JSC::VM& vm, JSC::Structure* structure, Ref<TestWrappable>&& wrapped)
        : Base(vm, structure)
        , m_wrapped(WTF::move(wrapped))
    {
    }

    Ref<TestWrappable> m_wrapped;
};

const JSC::ClassInfo JSTestWrapper::s_info = { "TestWrapper"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSTestWrapper) };

// JSEventTarget's and JSNode's visitAdditionalChildren: the listeners' functions, and the
// node's root as an opaque root.
template<typename Visitor>
void JSTestWrapper::visitChildrenImpl(JSC::JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<JSTestWrapper>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    thisObject->wrapped().visitEventListeners(visitor);
    if (void* root = thisObject->wrapped().opaqueRoot())
        visitor.addOpaqueRoot(root);
}

DEFINE_VISIT_CHILDREN(JSTestWrapper);

// JSNodeOwner: a node's wrapper lives while its node's root is an opaque root, which
// the wrapper of any node in its tree adds.
class TestNodeOwner final : public JSC::WeakHandleOwner {
public:
    bool isReachableFromOpaqueRoots(JSC::Handle<JSC::Unknown>, void* context, JSC::AbstractSlotVisitor& visitor, ASCIILiteral* reason) final
    {
        if (reason)
            *reason = "Connected node"_s;
        return visitor.containsOpaqueRoot(static_cast<TestNode*>(context)->opaqueRoot());
    }
};

// WebCore's DOMTimer: its ScheduledAction holds the function as a Strong.
struct TestTimer {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(TestTimer);
    JSC::Strong<JSC::JSObject> function;
};

// A demo, run in a function, which stands for its <script type="module">.
// `holders` records what a dump's retained sizes must charge the leak to, the
// last being the one whose entry a person must find at the top of what grew.
struct Demo {
    ASCIILiteral page;
    ASCIILiteral script;
    ASCIILiteral holders;
};

// What a demo leaks: a cell and its typed array's storage, or a message copied for
// a worker and its bytes.
struct Leaked {
    uint64_t object;
    uint64_t storage; // 0 if it has none.
    bool isAfterFirstSnapshot;
};

struct DemoState {
    // Each demo is a page of its own, with a document that has a body.
    RefPtr<TestDocument> document;
    Vector<std::unique_ptr<TestTimer>> timers;
    Vector<uint64_t> holders;
    Vector<Leaked> leaked;
};

// The browser of every demo, held by global variables as a web process holds its documents.
struct TestBrowser {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(TestBrowser);
    JSC::VM* vm { nullptr };
    JSC::Strong<JSC::JSGlobalObject> globalObject;
    JSC::Strong<JSC::Structure> workerStructure; // Its prototype has Worker.prototype.postMessage.
    JSC::Strong<JSC::Structure> documentStructure; // Its prototype has Document.prototype.createElement.
    JSC::Strong<JSC::Structure> elementStructure; // Its prototype has Element.prototype.appendChild.
    Vector<DemoState> demos;
    size_t currentDemo { 0 };
    bool isAfterFirstSnapshot { false };
};

TestBrowser* browser { nullptr };

TestNodeOwner& nodeOwner()
{
    static NeverDestroyed<TestNodeOwner> owner;
    return owner;
}

// Two leaks that only a walk of C++ objects charges to what holds them. Uint16Array is
// wrapped so that the test knows which objects leak.
const std::array<Demo, 2> demos { {
    // Removed elements, kept in an array. Each holds its children in C++; each child's
    // wrapper lives because the removed element's wrapper adds their tree's root as an
    // opaque root, and each child's listener lives because its wrapper visits it. JSC's
    // own snapshot has no edge from a removed element to its children, nor from a child
    // to its wrapper: each child's wrapper is a root of its own, and nothing ties the
    // listeners' typed arrays to `detached`.
    { "detached-subtree.html"_s,
        "const detached = [];\n"
        "setInterval(() => {\n"
        "  const div = document.createElement('div');\n"
        "  for (let i = 0; i < 20; ++i) {\n"
        "    const child = document.createElement('span');\n"
        "    const big = new Uint16Array(1000);\n"
        "    child.addEventListener('click', () => big.length);\n"
        "    div.appendChild(child);\n"
        "  }\n"
        "  document.body.appendChild(div);\n"
        "  div.remove();\n"
        "  detached.push(div);\n"
        "}, 100);\n"_s,
        "recordHolder(detached);"_s },
    // simple-09 of https://github.com/justinmichaud/leak-tests: the messages wait,
    // copied, in the busy worker's queue, which only C++ holds, so that JSC's own
    // snapshot has no node for them at all.
    { "simple-09.html"_s,
        "const worker = new Worker(\"worker.js\");\n"
        "let count = 0;\n"
        "setInterval(() => {\n"
        "  const largeArray = new ArrayBuffer(1000);\n"
        "  worker.postMessage({ id: count++, data: largeArray });\n"
        "}, 100);\n"_s,
        "recordHolder(worker); recordWrapped(worker);"_s },
} };

// What the demos see of the browser, built on the host functions below.
constexpr ASCIILiteral browserScript = "globalThis.window = globalThis;\n"
    "Object.defineProperty(globalThis, 'document', { get() { return theDocument(); } });\n"
    "globalThis.Worker = function Worker(url) { return createWorker(); };\n"
    "Worker.prototype.postMessage = function postMessage(message) { workerPostMessage(this, message); };\n"
    "globalThis.Element = function Element() { };\n"
    "Element.prototype.appendChild = function appendChild(child) { appendChildForTest(this, child); return child; };\n"
    "Element.prototype.remove = function remove() { removeForTest(this); };\n"
    "Element.prototype.addEventListener = function addEventListener(type, listener) { addEventListenerForTest(this, listener); };\n"
    "globalThis.Document = function Document() { };\n"
    "Document.prototype.createElement = function createElement(name) { return createElementForTest(); };\n"
    "Object.defineProperty(Document.prototype, 'body', { get() { return bodyOf(this); } });\n"
    // The prototypes of the wrappers the browser makes.
    "[Worker.prototype, Document.prototype, Element.prototype];\n"_s;

DemoState& currentDemo()
{
    return browser->demos[browser->currentDemo];
}

JSTestWrapper* wrap(JSC::JSGlobalObject* globalObject, TestWrappable& wrapped, JSC::Structure* structure, TestNode* node = nullptr)
{
    if (auto* existing = wrapped.wrapper.get())
        return uncheckedDowncast<JSTestWrapper>(existing);
    auto* wrapper = JSTestWrapper::create(globalObject->vm(), structure, wrapped);
    wrapped.wrapper = JSC::Weak<JSC::JSObject>(wrapper, node ? &nodeOwner() : nullptr, node);
    return wrapper;
}

JSTestWrapper* wrapperArgument(JSC::CallFrame* callFrame, size_t index)
{
    return dynamicDowncast<JSTestWrapper>(callFrame->argument(index));
}

TestNode& nodeArgument(JSC::CallFrame* callFrame, size_t index)
{
    auto* wrapper = wrapperArgument(callFrame, index);
    RELEASE_ASSERT(wrapper);
    return static_cast<TestNode&>(wrapper->wrapped());
}

JSC_DEFINE_HOST_FUNCTION(theDocument, (JSC::JSGlobalObject* globalObject, JSC::CallFrame*))
{
    TestDocument& document = *currentDemo().document;
    return JSC::JSValue::encode(wrap(globalObject, document, browser->documentStructure.get(), &document));
}

JSC_DEFINE_HOST_FUNCTION(bodyOf, (JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame))
{
    TestNode& body = nodeArgument(callFrame, 0).children.first();
    return JSC::JSValue::encode(wrap(globalObject, body, browser->elementStructure.get(), &body));
}

JSC_DEFINE_HOST_FUNCTION(createElementForTest, (JSC::JSGlobalObject* globalObject, JSC::CallFrame*))
{
    Ref element = adoptRef(*new TestElement);
    return JSC::JSValue::encode(wrap(globalObject, element.get(), browser->elementStructure.get(), element.ptr()));
}

JSC_DEFINE_HOST_FUNCTION(appendChildForTest, (JSC::JSGlobalObject*, JSC::CallFrame* callFrame))
{
    nodeArgument(callFrame, 0).appendChild(nodeArgument(callFrame, 1));
    return JSC::JSValue::encode(JSC::jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(removeForTest, (JSC::JSGlobalObject*, JSC::CallFrame* callFrame))
{
    nodeArgument(callFrame, 0).remove();
    return JSC::JSValue::encode(JSC::jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(addEventListenerForTest, (JSC::JSGlobalObject*, JSC::CallFrame* callFrame))
{
    auto* wrapper = wrapperArgument(callFrame, 0);
    JSC::JSObject* listener = callFrame->argument(1).getObject();
    RELEASE_ASSERT(wrapper && listener);
    wrapper->wrapped().addEventListener(listener);
    return JSC::JSValue::encode(JSC::jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(createWorker, (JSC::JSGlobalObject* globalObject, JSC::CallFrame*))
{
    Ref worker = adoptRef(*new TestWorker);
    return JSC::JSValue::encode(wrap(globalObject, worker.get(), browser->workerStructure.get()));
}

// The message is copied, as structured cloning copies it, and queued.
JSC_DEFINE_HOST_FUNCTION(workerPostMessage, (JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame))
{
    auto* wrapper = wrapperArgument(callFrame, 0);
    JSC::JSObject* message = callFrame->argument(1).getObject();
    RELEASE_ASSERT(wrapper && message);
    JSC::VM& vm = globalObject->vm();
    auto* buffer = JSC::toPossiblySharedArrayBuffer(vm, message->get(globalObject, JSC::Identifier::fromString(vm, "data"_s)));
    RELEASE_ASSERT(buffer);
    Ref serialized = adoptRef(*new TestSerializedValue);
    serialized->data = Vector<uint8_t>(buffer->span());
    currentDemo().leaked.append({ std::bit_cast<uint64_t>(serialized.ptr()), std::bit_cast<uint64_t>(serialized->data.span().data()), browser->isAfterFirstSnapshot });
    static_cast<TestWorker&>(wrapper->wrapped()).queuedTasks.append([message = WTF::move(serialized)] {
        UNUSED_PARAM(message);
    });
    return JSC::JSValue::encode(JSC::jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(setIntervalForTest, (JSC::JSGlobalObject*, JSC::CallFrame* callFrame))
{
    JSC::JSObject* function = callFrame->argument(0).getObject();
    RELEASE_ASSERT(function);
    auto timer = makeUnique<TestTimer>();
    timer->function.set(*browser->vm, function);
    currentDemo().timers.append(WTF::move(timer));
    return JSC::JSValue::encode(JSC::jsNumber(currentDemo().timers.size()));
}

// The test's own record of what a demo leaks, kept as integers, so that nothing it records holds anything.
JSC_DEFINE_HOST_FUNCTION(recordLeak, (JSC::JSGlobalObject*, JSC::CallFrame* callFrame))
{
    auto* view = dynamicDowncast<JSC::JSArrayBufferView>(callFrame->argument(0));
    RELEASE_ASSERT(view && view->vector());
    currentDemo().leaked.append({ std::bit_cast<uint64_t>(view), std::bit_cast<uint64_t>(view->vector()), browser->isAfterFirstSnapshot });
    return JSC::JSValue::encode(view);
}

JSC_DEFINE_HOST_FUNCTION(recordHolder, (JSC::JSGlobalObject*, JSC::CallFrame* callFrame))
{
    JSC::JSValue holder = callFrame->argument(0);
    RELEASE_ASSERT(holder.isCell());
    currentDemo().holders.append(std::bit_cast<uint64_t>(holder.asCell()));
    return JSC::JSValue::encode(JSC::jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(recordWrapped, (JSC::JSGlobalObject*, JSC::CallFrame* callFrame))
{
    auto* wrapper = wrapperArgument(callFrame, 0);
    RELEASE_ASSERT(wrapper);
    currentDemo().holders.append(std::bit_cast<uint64_t>(&wrapper->wrapped()));
    return JSC::JSValue::encode(JSC::jsUndefined());
}

JSC::JSValue evaluate(JSC::JSGlobalObject* globalObject, const String& script)
{
    NakedPtr<JSC::Exception> exception;
    JSC::JSValue result = JSC::evaluate(globalObject, JSC::makeSource(script, JSC::SourceOrigin { }, JSC::SourceTaintedOrigin::Untainted), JSC::JSValue(), exception);
    if (exception)
        dataLogLn("    the script threw: ", exception->value());
    RELEASE_ASSERT(!exception);
    return result;
}

// Makes the browser, starts each demo, and fires its timers warmUpTicks times.
NEVER_INLINE void startDemos()
{
    JSC::VM& vm = JSC::VM::create(JSC::HeapType::Large).leakRef();
    JSC::JSLockHolder locker(vm);
    browser = new TestBrowser;
    browser->vm = &vm;
    JSC::JSGlobalObject* globalObject = JSC::JSGlobalObject::create(vm, JSC::JSGlobalObject::createStructure(vm, JSC::jsNull()));
    browser->globalObject.set(vm, globalObject);
    browser->demos.resize(demos.size());
    for (DemoState& demo : browser->demos) {
        demo.document = adoptRef(*new TestDocument);
        demo.document->appendChild(adoptRef(*new TestElement));
    }

    auto addFunction = [&](ASCIILiteral name, unsigned length, JSC::NativeFunction function) {
        globalObject->putDirect(vm, JSC::Identifier::fromString(vm, name), JSC::JSFunction::create(vm, globalObject, length, name, function, JSC::ImplementationVisibility::Public));
    };
    addFunction("theDocument"_s, 0, theDocument);
    addFunction("bodyOf"_s, 1, bodyOf);
    addFunction("createElementForTest"_s, 0, createElementForTest);
    addFunction("appendChildForTest"_s, 2, appendChildForTest);
    addFunction("removeForTest"_s, 1, removeForTest);
    addFunction("addEventListenerForTest"_s, 2, addEventListenerForTest);
    addFunction("createWorker"_s, 0, createWorker);
    addFunction("workerPostMessage"_s, 2, workerPostMessage);
    addFunction("setInterval"_s, 2, setIntervalForTest);
    addFunction("recordLeak"_s, 1, recordLeak);
    addFunction("recordHolder"_s, 1, recordHolder);
    addFunction("recordWrapped"_s, 1, recordWrapped);
    JSC::JSValue prototypes = evaluate(globalObject, browserScript);
    auto prototype = [&](unsigned index) {
        JSC::JSValue prototype = prototypes.getObject()->get(globalObject, index);
        RELEASE_ASSERT(prototype.isObject());
        return prototype;
    };
    browser->workerStructure.set(vm, JSTestWrapper::createStructure(vm, globalObject, prototype(0)));
    browser->documentStructure.set(vm, JSTestWrapper::createStructure(vm, globalObject, prototype(1)));
    browser->elementStructure.set(vm, JSTestWrapper::createStructure(vm, globalObject, prototype(2)));

    for (size_t index = 0; index < demos.size(); ++index) {
        browser->currentDemo = index;
        evaluate(globalObject, makeString("(function() {\n"
            "const Uint16Array = function(length) { return recordLeak(new globalThis.Uint16Array(length)); };\n"_s,
            demos[index].script, demos[index].holders, "\n})();\n"_s));
        TEST_ASSERT(!browser->demos[index].timers.isEmpty() && !browser->demos[index].holders.isEmpty(), makeString(demos[index].page, " sets a timer and names what holds its leak"_s));
    }
}

// Fires every demo's timers `ticks` times.
NEVER_INLINE void fireTimers(unsigned ticks)
{
    JSC::VM& vm = *browser->vm;
    JSC::JSLockHolder locker(vm);
    JSC::JSGlobalObject* globalObject = browser->globalObject.get();
    for (unsigned tick = 0; tick < ticks; ++tick) {
        for (size_t index = 0; index < browser->demos.size(); ++index) {
            browser->currentDemo = index;
            for (auto& timer : browser->demos[index].timers) {
                JSC::JSObject* function = timer->function.get();
                auto callData = JSC::getCallData(function);
                NakedPtr<JSC::Exception> exception;
                JSC::call(globalObject, function, callData, JSC::jsUndefined(), JSC::MarkedArgumentBuffer { }, exception);
                RELEASE_ASSERT(!exception);
            }
        }
    }
}

// The heap after a full collection, where no thread changes it: a snapshot of
// this process, and JSC's own GCDebugging heap snapshot, which its collector's
// visitor builds, as Web Inspector has it.
struct Snapshots {
    std::unique_ptr<Snapshot> mya;
    std::string jsc; // In system malloc, which mya does not count, so that it is not in the next snapshot's heap.
};

Snapshots takeSnapshots()
{
    JSC::VM& vm = *browser->vm;
    JSC::JSLockHolder locker(vm);
    std::string jsc;
    {
        // It collects, so it is taken first, of the heap mya then sees.
        JSC::HeapSnapshotBuilder builder(vm.ensureHeapProfiler(), JSC::HeapSnapshotBuilder::SnapshotType::GCDebuggingSnapshot);
        builder.buildSnapshot();
        jsc = builder.json().utf8().toStdString();
    }
    vm.heap.collectSync(JSC::CollectionScope::Full);
    JSC::DeferGC deferGC(vm);
    RELEASE_ASSERT(!vm.heap.collectionScope() && !vm.heap.objectSpace().isMarking());
    return { takeSnapshot(getpid()), WTF::move(jsc) };
}

// The dump of a snapshot as `mya heapdump` makes it, from the VM.
std::optional<HeapWalk::HeapDump> dumpOf(Snapshot& snapshot)
{
    Dl_info javaScriptCore { };
    if (!dladdr(reinterpret_cast<const void*>(static_cast<void(*)()>(&JSC::initialize)), &javaScriptCore))
        return std::nullopt;
    HeapWalk heap(snapshot, Address { browser->vm }, Address { javaScriptCore.dli_fbase });
    if (!heap.isValid())
        return std::nullopt;
    return heap.heapDump();
}

// A heap graph, and what Web Inspector computes from it.
struct Graph {
    Graph(HeapWalk::HeapDump&& dump)
        : dump(WTF::move(dump))
        , retention(this->dump.retention())
    {
        size_t size = this->dump.nodes.size();
        successors.resize(size);
        Vector<Vector<uint32_t>> retainingSuccessors(size);
        // Every edge, whether retained sizes count it or not: every path a person can follow.
        for (auto& edge : this->dump.edges) {
            successors[edge.from].append(edge.to);
            if (edge.retains)
                retainingSuccessors[edge.from].append(edge.to);
        }
        isReached.fill(false, size);
        isReached[0] = true;
        Vector<uint32_t> worklist { 0 };
        while (!worklist.isEmpty()) {
            for (uint32_t successor : retainingSuccessors[worklist.takeLast()]) {
                if (!isReached[successor]) {
                    isReached[successor] = true;
                    worklist.append(successor);
                }
            }
        }
        // The tree of dominators, each node before those it dominates.
        Vector<Vector<uint32_t>> dominated(size);
        for (uint32_t node = 1; node < size; ++node)
            dominated[retention.immediateDominator[node]].append(node);
        worklist.append(0);
        while (!worklist.isEmpty()) {
            uint32_t node = worklist.takeLast();
            dominatorTreeOrder.append(node);
            worklist.appendVector(dominated[node]);
        }
        isBlockRecord.fill(false, size);
        for (uint32_t node : dominatorTreeOrder) {
            if (node)
                isBlockRecord[node] = isBlockRecord[retention.immediateDominator[node]] || this->dump.nodes[node].className == "JSC::MarkedBlock::Handle"_s;
        }
    }

    // Each node reached from `start`, and the node it was first reached from.
    Vector<uint32_t> parentsFrom(uint32_t start) const
    {
        Vector<uint32_t> parents;
        parents.fill(unvisited, dump.nodes.size());
        parents[start] = start;
        Deque<uint32_t> worklist { start };
        while (!worklist.isEmpty()) {
            uint32_t node = worklist.takeFirst();
            for (uint32_t successor : successors[node]) {
                if (parents[successor] != unvisited)
                    continue;
                parents[successor] = node;
                worklist.append(successor);
            }
        }
        return parents;
    }

    String path(const Vector<uint32_t>& parents, uint32_t target) const
    {
        Vector<uint32_t> path { target };
        while (parents[path.last()] != path.last())
            path.append(parents[path.last()]);
        StringBuilder result;
        for (size_t index = path.size(); index--;)
            result.append("\n        "_s, dump.nodes[path[index]].className, " at 0x"_s, hex(dump.nodes[path[index]].address.toTargetVMAddress()));
        return result.toString();
    }

    // The allocator's own memory, which Web Inspector routes no path through, and what no
    // path reaches, such as an allocation the walk missed: no entry of a list of what grew.
    // A block's handle, and what it alone holds, such as the WeakBlocks of its weak set,
    // is the JS heap's own record of the block, which grows with whatever the heap holds.
    bool isInternal(uint32_t node) const
    {
        const auto& value = dump.nodes[node];
        return value.kind == HeapWalk::HeapDump::Kind::Missed || value.isBlock || !isReached[node] || isBlockRecord[node];
    }

    static constexpr uint32_t unvisited = std::numeric_limits<uint32_t>::max();
    HeapWalk::HeapDump dump;
    HeapWalk::HeapDump::Retention retention;
    Vector<Vector<uint32_t>> successors;
    Vector<bool> isReached; // From <root>, by the edges that retain.
    Vector<bool> isBlockRecord;
    Vector<uint32_t> dominatorTreeOrder;
};

// The two dumps of one walk of the heap, mya's or JSC's collector's.
struct Walk {
    Walk(ASCIILiteral name, bool hasStorageNodes, HeapWalk::HeapDump&& before, HeapWalk::HeapDump&& after)
        : name(name)
        , hasStorageNodes(hasStorageNodes)
        , before(WTF::move(before))
        , after(WTF::move(after))
    {
        // Each node of the second dump is the node of the first at its address, of its class,
        // or, for a root's node, which has no address, of its name.
        HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> nodesBefore;
        HashMap<String, uint32_t> rootsBefore;
        for (uint32_t node = 1; node < this->before.dump.nodes.size(); ++node) {
            const auto& value = this->before.dump.nodes[node];
            if (value.kind == HeapWalk::HeapDump::Kind::Root)
                rootsBefore.add(value.className, node);
            else if (value.address)
                nodesBefore.add(value.address.toTargetVMAddress(), node);
        }
        const auto& nodes = this->after.dump.nodes;
        nodeBefore.fill(Graph::unvisited, nodes.size());
        nodeBefore[0] = 0;
        for (uint32_t node = 1; node < nodes.size(); ++node) {
            if (nodes[node].kind == HeapWalk::HeapDump::Kind::Root) {
                if (auto found = rootsBefore.find(nodes[node].className); found != rootsBefore.end())
                    nodeBefore[node] = found->value;
            } else if (nodes[node].address) {
                auto found = nodesBefore.find(nodes[node].address.toTargetVMAddress());
                if (found != nodesBefore.end() && this->before.dump.nodes[found->value].className == nodes[node].className)
                    nodeBefore[node] = found->value;
            }
        }
    }

    bool isNew(uint32_t node) const { return nodeBefore[node] == Graph::unvisited; }

    // How much a node's retained size grew between the dumps: all of it, if it is new.
    int64_t growth(uint32_t node) const
    {
        if (!node)
            return 0;
        int64_t retained = after.retention.retainedSize[node];
        return isNew(node) ? retained : retained - static_cast<int64_t>(before.retention.retainedSize[nodeBefore[node]]);
    }

    ASCIILiteral name;
    // Whether storage outside a cell, such as a typed array's, is a node of its own; JSC
    // counts it in the cell's size.
    bool hasStorageNodes;
    Graph before;
    Graph after;
    Vector<uint32_t> nodeBefore; // Graph::unvisited for a node new in the second dump.
};

// The nodes of each demo's leak in one of a walk's dumps: in the first, what leaked before it.
Vector<Vector<uint32_t>> leakedNodesIn(const Graph& graph, bool hasStorageNodes, bool isFirst)
{
    Vector<Vector<uint32_t>> nodes(demos.size());
    for (size_t index = 0; index < demos.size(); ++index) {
        for (const Leaked& leaked : browser->demos[index].leaked) {
            if (isFirst && leaked.isAfterFirstSnapshot)
                continue;
            for (uint64_t address : { leaked.object, hasStorageNodes ? leaked.storage : 0 }) {
                auto node = address ? graph.dump.nodeAt(Address { address }) : std::nullopt;
                if (node && graph.dump.nodes[*node].kind != HeapWalk::HeapDump::Kind::Missed && graph.dump.nodes[*node].address == Address { address })
                    nodes[index].append(*node);
            }
        }
    }
    return nodes;
}

// The nodes of each demo's leak, in the second dump, and for each node the demos whose
// leaked nodes it dominates, as a mask, so its retained size grows by their leaks.
struct Leaks {
    Vector<Vector<uint32_t>> nodes;
    Vector<Vector<uint32_t>> nodesBetween; // Of what leaked between the dumps.
    Vector<uint64_t> bytesBetween;
    Vector<String> missing; // The first leaked object that is no node, if any.
    Vector<uint32_t> dominatorOf;
};

Leaks leaksIn(const Walk& walk)
{
    const Graph& graph = walk.after;
    Leaks leaks;
    leaks.nodes.resize(demos.size());
    leaks.nodesBetween.resize(demos.size());
    leaks.bytesBetween.fill(0, demos.size());
    leaks.missing.resize(demos.size());
    leaks.dominatorOf.fill(0, graph.dump.nodes.size());
    for (size_t index = 0; index < demos.size(); ++index) {
        for (const Leaked& leaked : browser->demos[index].leaked) {
            for (uint64_t address : { leaked.object, walk.hasStorageNodes ? leaked.storage : 0 }) {
                if (!address)
                    continue;
                auto node = graph.dump.nodeAt(Address { address });
                if (!node || graph.dump.nodes[*node].kind == HeapWalk::HeapDump::Kind::Missed || graph.dump.nodes[*node].address != Address { address }) {
                    if (leaks.missing[index].isNull())
                        leaks.missing[index] = makeString("the leaked 0x"_s, hex(address), " is no node of its own"_s);
                    continue;
                }
                leaks.nodes[index].append(*node);
                if (leaked.isAfterFirstSnapshot) {
                    leaks.nodesBetween[index].append(*node);
                    leaks.bytesBetween[index] += graph.dump.nodes[*node].size;
                }
                for (uint32_t dominator = *node; !(leaks.dominatorOf[dominator] & (1u << index)); dominator = graph.retention.immediateDominator[dominator]) {
                    leaks.dominatorOf[dominator] |= 1u << index;
                    if (!dominator)
                        break;
                }
            }
        }
    }
    return leaks;
}

// Whether a walk's dumps charge a demo's leak to what holds it, which a person needs to
// find it by any grouping: every leaked object is a node, each holder is on a path from
// <root> to every one of them, whatever other paths there are, and the retained size of
// each grew by at least half of what leaked. Empty if they do, or why they do not.
String whyNotSeen(const Walk& walk, const Leaks& leaks, size_t index)
{
    const Graph& graph = walk.after;
    const auto& nodes = graph.dump.nodes;
    if (!leaks.missing[index].isNull())
        return leaks.missing[index];
    auto fromRoot = graph.parentsFrom(0);
    for (uint64_t holder : browser->demos[index].holders) {
        auto node = graph.dump.nodeAt(Address { holder });
        if (!node || nodes[*node].address != Address { holder })
            return makeString("its holder at 0x"_s, hex(holder), " is no node"_s);
        String name = makeString("the "_s, nodes[*node].className, " at 0x"_s, hex(holder));
        if (fromRoot[*node] == Graph::unvisited)
            return makeString("no path from <root> reaches "_s, name);
        auto fromHolder = graph.parentsFrom(*node);
        for (uint32_t leaked : leaks.nodes[index]) {
            if (fromHolder[leaked] == Graph::unvisited)
                return makeString(name, " reaches no path to the leaked "_s, nodes[leaked].className, " at 0x"_s, hex(nodes[leaked].address.toTargetVMAddress()), ", which <root> reaches so:"_s, graph.path(fromRoot, leaked));
        }
        int64_t holderGrowth = walk.growth(*node);
        if (holderGrowth < static_cast<int64_t>(leaks.bytesBetween[index] / 2))
            return makeString("the retained size of "_s, name, " grew by "_s, holderGrowth, " bytes, of the "_s, leaks.bytesBetween[index], " that leaked between the dumps"_s);
    }
    return { };
}

// For each node, the bytes it retains that are noise: what no demo's leak owns, and not the
// allocator's own. A demo's leak owns what a node retains when the node, or one that
// dominates it, dominates what that demo leaked and nothing another demo leaked.
Vector<uint64_t> noiseRetainedIn(const Graph& graph, const Vector<Vector<uint32_t>>& leakedNodes)
{
    size_t size = graph.dump.nodes.size();
    Vector<uint32_t> demosDominated;
    demosDominated.fill(0, size);
    for (size_t index = 0; index < leakedNodes.size(); ++index) {
        for (uint32_t node : leakedNodes[index]) {
            for (uint32_t dominator = node; !(demosDominated[dominator] & (1u << index)); dominator = graph.retention.immediateDominator[dominator]) {
                demosDominated[dominator] |= 1u << index;
                if (!dominator)
                    break;
            }
        }
    }
    Vector<bool> isOwned;
    isOwned.fill(false, size);
    for (uint32_t node : graph.dominatorTreeOrder) {
        if (node)
            isOwned[node] = isOwned[graph.retention.immediateDominator[node]] || std::has_single_bit(demosDominated[node]);
    }
    Vector<uint64_t> noise;
    noise.fill(0, size);
    for (size_t position = graph.dominatorTreeOrder.size(); position--;) {
        uint32_t node = graph.dominatorTreeOrder[position];
        if (!isOwned[node] && !graph.isInternal(node))
            noise[node] += graph.dump.nodes[node].size;
        if (node)
            noise[graph.retention.immediateDominator[node]] += noise[node];
    }
    return noise;
}

// The keys of a grouping, for each node of each of a walk's dumps, the same in both for
// what a person reads as one entry; noKey for a node in no entry.
struct Keys {
    static constexpr uint32_t noKey = std::numeric_limits<uint32_t>::max();
    Vector<uint32_t> before;
    Vector<uint32_t> after;
    Vector<String> descriptions;
};

// The retained path of each node: the classes on its path of dominators from <root>.
Keys retainedPathKeys(const Walk& walk)
{
    Keys keys;
    HashMap<String, uint32_t> classes;
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> paths;
    keys.descriptions.append("<root>"_s);
    auto keysOf = [&](const Graph& graph) {
        Vector<uint32_t> result;
        result.fill(0, graph.dump.nodes.size());
        for (uint32_t node : graph.dominatorTreeOrder) {
            if (!node)
                continue;
            const String& className = graph.dump.nodes[node].className;
            uint32_t classIndex = classes.ensure(className, [&] { return classes.size(); }).iterator->value;
            uint32_t parent = result[graph.retention.immediateDominator[node]];
            result[node] = paths.ensure((static_cast<uint64_t>(parent) << 32) | classIndex, [&] {
                keys.descriptions.append(makeString(keys.descriptions[parent], " > "_s, className));
                return static_cast<uint32_t>(keys.descriptions.size() - 1);
            }).iterator->value;
        }
        return result;
    };
    keys.before = keysOf(walk.before);
    keys.after = keysOf(walk.after);
    return keys;
}

// An entry of the list of what grew between a walk's dumps, its nodes grouped by key.
struct Entry {
    uint32_t key;
    Vector<uint32_t> members; // In the second dump.
    int64_t growth { 0 }; // Of what its members retain, none of which another one dominates.
    int64_t noiseGrowth { 0 };
    // For each demo, the bytes of what it leaked between the dumps that a member dominates.
    Vector<uint64_t> charge;
};

// The entries of a walk's dumps by `keys`, the largest growth first.
Vector<Entry> entriesOf(const Walk& walk, const Leaks& leaks, const Keys& keys)
{
    auto noiseBefore = noiseRetainedIn(walk.before, leakedNodesIn(walk.before, walk.hasStorageNodes, true));
    auto noiseAfter = noiseRetainedIn(walk.after, leakedNodesIn(walk.after, walk.hasStorageNodes, false));
    HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> entryOfKey;
    Vector<Entry> entries;
    // What a node retains is counted once in its entry: not if a node it is dominated by has its key.
    auto isCounted = [](const Graph& graph, const Vector<uint32_t>& keyOf, uint32_t node) {
        for (uint32_t dominator = graph.retention.immediateDominator[node]; dominator; dominator = graph.retention.immediateDominator[dominator]) {
            if (keyOf[dominator] == keyOf[node])
                return false;
        }
        return true;
    };
    for (uint32_t node = 1; node < walk.after.dump.nodes.size(); ++node) {
        uint32_t key = keys.after[node];
        if (key == Keys::noKey || walk.after.isInternal(node))
            continue;
        uint32_t index = entryOfKey.ensure(key, [&] {
            entries.append({ key, { }, 0, 0, { } });
            entries.last().charge.fill(0, demos.size());
            return static_cast<uint32_t>(entries.size() - 1);
        }).iterator->value;
        entries[index].members.append(node);
        if (isCounted(walk.after, keys.after, node)) {
            entries[index].growth += walk.after.retention.retainedSize[node];
            entries[index].noiseGrowth += noiseAfter[node];
        }
    }
    for (uint32_t node = 1; node < walk.before.dump.nodes.size(); ++node) {
        auto found = entryOfKey.find(keys.before[node]);
        if (keys.before[node] == Keys::noKey || walk.before.isInternal(node) || found == entryOfKey.end() || !isCounted(walk.before, keys.before, node))
            continue;
        entries[found->value].growth -= walk.before.retention.retainedSize[node];
        entries[found->value].noiseGrowth -= noiseBefore[node];
    }
    for (size_t index = 0; index < demos.size(); ++index) {
        for (uint32_t leaked : leaks.nodesBetween[index]) {
            HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> charged;
            for (uint32_t node = leaked; node; node = walk.after.retention.immediateDominator[node]) {
                auto found = entryOfKey.find(keys.after[node]);
                if (found != entryOfKey.end() && charged.add(found->value).isNewEntry)
                    entries[found->value].charge[index] += walk.after.dump.nodes[leaked].size;
            }
        }
    }
    std::ranges::sort(entries, [&](const Entry& a, const Entry& b) {
        if (a.growth != b.growth)
            return a.growth > b.growth;
        return codePointCompareLessThan(keys.descriptions[a.key], keys.descriptions[b.key]);
    });
    return entries;
}

// The last classes of a key, as a person reads a retained path's end.
String describe(const Keys& keys, uint32_t key)
{
    const String& description = keys.descriptions[key];
    auto steps = description.split(" > "_s);
    if (steps.size() <= 3)
        return description;
    return makeString("... > "_s, steps[steps.size() - 3], " > "_s, steps[steps.size() - 2], " > "_s, steps[steps.size() - 1]);
}

// Whether a person reading a list of what grew between a walk's dumps, grouped by `keys`,
// finds what holds a demo's leak: the entry of its last holder
// - is charged with at least half of what the demo leaked between the dumps;
// - is charged with nothing another demo leaked, which would be another cause;
// - grew more than every entry that neither holds it nor is held by it grew by noise,
//   what no demo's leak owns.
// Empty if it does, or why it does not.
String whyNotFound(const Walk& walk, const Leaks& leaks, const Keys& keys, const Vector<Entry>& entries, size_t index)
{
    if (!leaks.missing[index].isNull())
        return leaks.missing[index];
    const Graph& graph = walk.after;
    uint64_t leaked = leaks.bytesBetween[index];
    uint64_t holder = browser->demos[index].holders.last();
    auto node = graph.dump.nodeAt(Address { holder });
    if (!node || graph.dump.nodes[*node].address != Address { holder })
        return makeString("its holder at 0x"_s, hex(holder), " is no node"_s);
    auto entry = std::ranges::find_if(entries, [&](const Entry& entry) { return entry.key == keys.after[*node]; });
    if (entry == entries.end())
        return makeString("its holder, the "_s, graph.dump.nodes[*node].className, " at 0x"_s, hex(holder), ", is in no entry"_s);
    String name = makeString("the entry of its holder, "_s, describe(keys, entry->key));
    if (entry->charge[index] * 2 < leaked)
        return makeString(name, ", is charged with "_s, entry->charge[index], " of the "_s, leaked, " bytes it leaked between the dumps"_s);
    for (size_t other = 0; other < demos.size(); ++other) {
        if (other != index && entry->charge[other])
            return makeString(name, ", is charged with what "_s, demos[other].page, " leaked too"_s);
    }
    auto isRelated = [&](const Entry& a, const Entry& b) {
        for (uint32_t x : a.members) {
            for (uint32_t y : b.members) {
                if (graph.retention.dominates(x, y) || graph.retention.dominates(y, x))
                    return true;
            }
        }
        return false;
    };
    for (const Entry& other : entries) {
        if (other.noiseGrowth > entry->growth && !isRelated(other, *entry))
            return makeString(name, ", grew by "_s, entry->growth, " bytes, less than the unrelated "_s, describe(keys, other.key), " by "_s, other.noiseGrowth, " bytes of noise"_s);
    }
    dataLogLnIf(verbose, "    ", walk.name, ": ", demos[index].page, ": found ", keys.descriptions[entry->key], ", number ", entry - entries.begin() + 1, " of ", entries.size(), " by growth, ", entry->members.size(), " nodes, which grew by ", entry->growth, " bytes, ", entry->charge[index], " of the ", leaked, " it leaked");
    return { };
}

Ref<JSON::Array> jsonOf(const HeapWalk::HeapDump& dump)
{
    // Each node as <address>, <size>, <class>, <kind>, <isBlock>; each edge as <from>, <to>, <kind>, <retains>, <label>.
    Ref nodes = JSON::Array::create();
    for (auto& node : dump.nodes) {
        nodes->pushDouble(static_cast<double>(node.address.toTargetVMAddress()));
        nodes->pushDouble(static_cast<double>(node.size));
        nodes->pushString(node.className);
        nodes->pushInteger(static_cast<int>(node.kind));
        nodes->pushBoolean(node.isBlock);
    }
    Ref edges = JSON::Array::create();
    for (auto& edge : dump.edges) {
        edges->pushInteger(edge.from);
        edges->pushInteger(edge.to);
        edges->pushInteger(static_cast<int>(edge.kind));
        edges->pushBoolean(edge.retains);
        edges->pushString(edge.label.isNull() ? emptyString() : edge.label);
    }
    Ref result = JSON::Array::create();
    result->pushArray(WTF::move(nodes));
    result->pushArray(WTF::move(edges));
    return result;
}

// With JSC_TOOLS_LEAK_REPORT set, writes to that path the demos, what the test recorded of
// them, the four dumps and the test's own verdicts, which leak-report/generate makes a report of.
void writeReport(const Walk& mya, const Walk& jsc, const Vector<std::array<String, 4>>& verdicts)
{
    const char* path = getenv("JSC_TOOLS_LEAK_REPORT");
    if (!path)
        return;
    Ref report = JSON::Object::create();
    report->setInteger("warmUpTicks"_s, warmUpTicks);
    report->setInteger("leakingTicks"_s, leakingTicks);
    Ref demosArray = JSON::Array::create();
    for (size_t index = 0; index < demos.size(); ++index) {
        Ref demo = JSON::Object::create();
        demo->setString("page"_s, demos[index].page);
        demo->setString("script"_s, demos[index].script);
        demo->setString("holderScript"_s, demos[index].holders);
        Ref holders = JSON::Array::create();
        for (uint64_t holder : browser->demos[index].holders)
            holders->pushDouble(static_cast<double>(holder));
        demo->setArray("holders"_s, WTF::move(holders));
        Ref leaked = JSON::Array::create();
        for (const Leaked& leak : browser->demos[index].leaked) {
            leaked->pushDouble(static_cast<double>(leak.object));
            leaked->pushDouble(static_cast<double>(leak.storage));
            leaked->pushBoolean(leak.isAfterFirstSnapshot);
        }
        demo->setArray("leaked"_s, WTF::move(leaked));
        // mya's seen, mya's found, JSC's seen, JSC's found.
        Ref verdict = JSON::Array::create();
        for (const String& string : verdicts[index])
            verdict->pushString(string);
        demo->setArray("verdicts"_s, WTF::move(verdict));
        demosArray->pushObject(WTF::move(demo));
    }
    report->setArray("demos"_s, WTF::move(demosArray));
    Ref walks = JSON::Array::create();
    for (const Walk* walk : { &mya, &jsc }) {
        Ref object = JSON::Object::create();
        object->setString("name"_s, walk->name);
        object->setBoolean("hasStorageNodes"_s, walk->hasStorageNodes);
        object->setArray("before"_s, jsonOf(walk->before.dump));
        object->setArray("after"_s, jsonOf(walk->after.dump));
        walks->pushObject(WTF::move(object));
    }
    report->setArray("walks"_s, WTF::move(walks));
    CString json = report->toJSONString().utf8();
    FILE* file = fopen(path, "w");
    TEST_ASSERT(file, makeString("the report is written to "_s, String::fromUTF8(path)));
    if (!file)
        return;
    IGNORE_CLANG_WARNINGS_BEGIN("unsafe-buffer-usage-in-libc-call")
    fwrite(json.data(), 1, json.length(), file);
    IGNORE_CLANG_WARNINGS_END
    fclose(file);
    dataLogLn("    wrote the report's data, ", json.length(), " bytes, to ", path);
}

} // anonymous namespace

// Two leaks held through C++: two dumps of this process around timers that leak, as
// `mya heapdump` writes them, charge each to the JS object the page's code keeps it in,
// and that object's entry grows more than anything unrelated. The same heap as JSC's
// collector's own visitor walks it, as Web Inspector has it, does neither: it has no node
// for a C++ object, so no path from that object to what leaked.
void testLeaks()
{
    SuiteTracer tracer("Leaks");
    if (!tracer.shouldRun())
        return;

    startDemos();
    fireTimers(warmUpTicks);
    Snapshots first = takeSnapshots();
    browser->isAfterFirstSnapshot = true;
    fireTimers(leakingTicks);
    Snapshots second = takeSnapshots();
    TEST_ASSERT(first.mya && second.mya, "both snapshots are taken");
    if (!first.mya || !second.mya)
        return;

    auto myaBefore = dumpOf(*first.mya);
    auto myaAfter = dumpOf(*second.mya);
    auto jscBefore = HeapWalk::HeapDump::fromJSON(String::fromUTF8(first.jsc.c_str()));
    auto jscAfter = HeapWalk::HeapDump::fromJSON(String::fromUTF8(second.jsc.c_str()));
    TEST_ASSERT(myaBefore && myaAfter && jscBefore && jscAfter, "both snapshots are dumped, by mya and by JSC");
    if (!myaBefore || !myaAfter || !jscBefore || !jscAfter)
        return;
    Walk mya { "mya"_s, true, WTF::move(*myaBefore), WTF::move(*myaAfter) };
    Walk jsc { "JSC's heap snapshot"_s, false, WTF::move(*jscBefore), WTF::move(*jscAfter) };
    for (const Walk* walk : { &mya, &jsc }) {
        uint64_t bytes = 0;
        for (auto& node : walk->after.dump.nodes)
            bytes += node.size;
        TEST_ASSERT_EQ(walk->after.retention.retainedSize[0], bytes, makeString(walk->name, ": <root> retains every node"_s));
    }
    // Each of mya's edges names what held it: a member, a class, or a root.
    unsigned unlabeled = 0;
    for (auto& edge : mya.after.dump.edges)
        unlabeled += edge.from && edge.label.isEmpty();
    TEST_ASSERT_EQ(unlabeled, 0u, "mya: every edge but <root>'s own names what held it"_s);
    // JSC's edges name properties, variables and indices, which only the report reads.
    TEST_ASSERT(std::ranges::any_of(jsc.after.dump.edges, [](auto& edge) { return !edge.label.isEmpty(); }), "JSC's heap snapshot: its edges' names are read"_s);

    Leaks myaLeaks = leaksIn(mya);
    Leaks jscLeaks = leaksIn(jsc);
    // The only grouping the dumps have. Another, such as Web Inspector's allocation
    // sites, is another Keys.
    Keys myaKeys = retainedPathKeys(mya);
    Keys jscKeys = retainedPathKeys(jsc);
    Vector<Entry> myaEntries = entriesOf(mya, myaLeaks, myaKeys);
    Vector<Entry> jscEntries = entriesOf(jsc, jscLeaks, jscKeys);
    Vector<std::array<String, 4>> verdicts;
    for (size_t index = 0; index < demos.size(); ++index) {
        const Demo& demo = demos[index];
        TEST_ASSERT(browser->demos[index].leaked.size() >= warmUpTicks + leakingTicks, makeString(demo.page, " leaks an object on every tick"_s));
        String myaUnseen = whyNotSeen(mya, myaLeaks, index);
        if (!myaUnseen.isNull())
            dataLogLn("    mya: ", demo.page, ": ", myaUnseen);
        TEST_ASSERT(myaUnseen.isNull(), makeString("mya's heap dumps charge the leak of "_s, demo.page, " to what holds it"_s));
        // What holds the holders is the page's code: its timer's function, whose scope has
        // them. A compiled function's caches, such as the cells a DFG code block refers to,
        // are weak, and hold nothing.
        auto timer = mya.after.dump.nodeAt(Address { browser->demos[index].timers.first()->function.get() });
        for (uint64_t holder : browser->demos[index].holders) {
            auto node = mya.after.dump.nodeAt(Address { holder });
            TEST_ASSERT(timer && node && mya.after.retention.dominates(*timer, *node), makeString("mya: the timer of "_s, demo.page, " dominates its holder at 0x"_s, hex(holder)));
        }
        String myaMiss = whyNotFound(mya, myaLeaks, myaKeys, myaEntries, index);
        if (!myaMiss.isNull())
            dataLogLn("    mya: ", demo.page, ": not found: ", myaMiss);
        TEST_ASSERT(myaMiss.isNull(), makeString("mya's heap dumps find what holds the leak of "_s, demo.page));
        // Each of JSC's misses is expected, and said why: what holds the leak goes through C++.
        String jscUnseen = whyNotSeen(jsc, jscLeaks, index);
        String jscMiss = whyNotFound(jsc, jscLeaks, jscKeys, jscEntries, index);
        dataLogLn("    JSC's heap snapshot: ", demo.page, ": as expected, does not charge it to what holds it: ", jscUnseen.isNull() ? "it does"_s : jscUnseen);
        dataLogLn("    JSC's heap snapshot: ", demo.page, ": as expected, does not find what holds it: ", jscMiss.isNull() ? "it does"_s : jscMiss);
        TEST_ASSERT(!jscUnseen.isNull(), makeString("JSC's own heap snapshots do not charge the leak of "_s, demo.page, " to what holds it"_s));
        TEST_ASSERT(!jscMiss.isNull(), makeString("JSC's own heap snapshots do not find what holds the leak of "_s, demo.page));
        auto verdict = [](const String& miss) { return miss.isNull() ? "found"_s : miss; };
        verdicts.append({ verdict(myaUnseen), verdict(myaMiss), verdict(jscUnseen), verdict(jscMiss) });
    }
    writeReport(mya, jsc, verdicts);
}

} // namespace JSCToolsTest

#else // HAVE(LLDB)

namespace JSCToolsTest {

void testLeaks()
{
    SuiteTracer tracer("Leaks");
    if (!tracer.shouldRun())
        return;
    skipSuite("Leaks", "liblldb's headers were not found");
}

} // namespace JSCToolsTest

#endif // HAVE(LLDB)
