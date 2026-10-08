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
#include <JavaScriptCore/MarkingConstraint.h>
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
#include <bit>
#include <dlfcn.h>
#include <limits>
#include <unistd.h>
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/HexNumber.h>
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

// Each demo of https://github.com/justinmichaud/leak-tests leaks from a timer. Its
// timers fire this many times before the first snapshot, so that its code and
// shapes exist by then, and this many times between the two snapshots.
constexpr unsigned warmUpTicks = 4;
constexpr unsigned leakingTicks = 64;

// What the browser gives the demos, in C++ as WebCore has it, so that what holds
// a leak is a C++ object, as in a web process.

// WebCore's ScriptWrappable: a C++ object a JS wrapper wraps, and which holds
// that wrapper weakly.
class TestWrappable : public RefCounted<TestWrappable> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(TestWrappable);
public:
    virtual ~TestWrappable() = default;
    // What the wrapper's visitChildren keeps alive, as a MessageEvent's wrapper
    // keeps its data alive (JSMessageEvent::visitAdditionalChildren).
    virtual JSC::JSCell* additionalChild() const { return nullptr; }
    // What the wrapper's visitOutputConstraints keeps alive: what a constraint
    // decides from the C++ object's state, as WebCore's output constraints do.
    virtual JSC::JSCell* outputConstraintChild() const { return nullptr; }

    JSC::Weak<JSC::JSObject> wrapper;
};

// WebCore's Element, which its Document holds.
class TestElement final : public TestWrappable {
public:
    ~TestElement() final = default;
};

// WebCore's Document, which a global variable holds, as a web process's
// Page holds its frames' documents.
class TestDocument final : public TestWrappable {
public:
    ~TestDocument() final = default;

    Vector<Ref<TestElement>> children;
};

// WebCore's MessageEvent: its JSValueInWrappedObject holds the data as a
// Weak, which the event's wrapper visits.
class TestMessageEvent final : public TestWrappable {
public:
    JSC::JSCell* additionalChild() const final { return data.get(); }

    JSC::Weak<JSC::JSCell> data;
};

// A value a frame's script returned to its parent: it keeps the frame's global
// object, and so its document, alive through an output constraint, the
// <constraint-marks> of simple-08's retained path.
class TestRealmValue final : public TestWrappable {
public:
    ~TestRealmValue() final = default;
    JSC::JSCell* outputConstraintChild() const final { return realm.get(); }

    JSC::Weak<JSC::JSGlobalObject> realm;
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
    DECLARE_VISIT_OUTPUT_CONSTRAINTS;

private:
    JSTestWrapper(JSC::VM& vm, JSC::Structure* structure, Ref<TestWrappable>&& wrapped)
        : Base(vm, structure)
        , m_wrapped(WTF::move(wrapped))
    {
    }

    Ref<TestWrappable> m_wrapped;
};

const JSC::ClassInfo JSTestWrapper::s_info = { "TestWrapper"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSTestWrapper) };

template<typename Visitor>
void JSTestWrapper::visitChildrenImpl(JSC::JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<JSTestWrapper>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    if (JSC::JSCell* child = thisObject->wrapped().additionalChild())
        visitor.appendUnbarriered(child);
}

DEFINE_VISIT_CHILDREN(JSTestWrapper);

template<typename Visitor>
void JSTestWrapper::visitOutputConstraintsImpl(JSC::JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<JSTestWrapper>(cell);
    Base::visitOutputConstraints(thisObject, visitor);
    if (JSC::JSCell* child = thisObject->wrapped().outputConstraintChild())
        visitor.appendUnbarriered(child);
}

DEFINE_VISIT_OUTPUT_CONSTRAINTS(JSTestWrapper);

// WebCore's DOMGCOutputConstraint: every marked wrapper's visitOutputConstraints,
// found in the wrappers' space, not through any reference.
class TestOutputConstraint final : public JSC::MarkingConstraint {
public:
    explicit TestOutputConstraint(JSC::VM& vm)
        : MarkingConstraint("Tout"_s, "Test Output"_s, JSC::ConstraintVolatility::GreyedByMarking, JSC::ConstraintConcurrency::Concurrent, JSC::ConstraintParallelism::Parallel)
        , m_vm(vm)
    {
    }

private:
    template<typename Visitor>
    void executeImplImpl(Visitor& visitor)
    {
        auto visitCell = [](Visitor& visitor, JSC::HeapCell* heapCell, JSC::HeapCell::Kind kind) {
            JSC::SetRootMarkReasonScope rootScope(visitor, JSC::RootMarkReason::Output);
            if (!JSC::isJSCellKind(kind))
                return;
            auto* cell = static_cast<JSC::JSCell*>(heapCell);
            cell->methodTable()->visitOutputConstraints(cell, visitor);
        };
        RefPtr<SharedTask<void(Visitor&)>> task = m_vm.destructibleObjectSpace().template forEachMarkedCellInParallel<Visitor>(visitCell);
        visitor.addParallelConstraintTask(task);
    }
    void executeImpl(JSC::AbstractSlotVisitor& visitor) final { executeImplImpl(visitor); }
    void executeImpl(JSC::SlotVisitor& visitor) final { executeImplImpl(visitor); }

    JSC::VM& m_vm;
};

// JSNodeOwner: an element's wrapper lives as long as its element is in a
// document, which every element here is.
class TestElementOwner final : public JSC::WeakHandleOwner {
public:
    bool isReachableFromOpaqueRoots(JSC::Handle<JSC::Unknown>, void*, JSC::AbstractSlotVisitor&, ASCIILiteral* reason) final
    {
        if (reason)
            *reason = "an element in a document"_s;
        return true;
    }
};

// WebCore's DOMTimer: its ScheduledAction holds the function as a Strong.
struct TestTimer {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(TestTimer);
    JSC::Strong<JSC::JSObject> function;
};

// A demo of the leak-tests repository, run in a function, which stands for its
// <script type="module">. `holders` records what Web Inspector's retained
// sizes must charge the leak to.
struct Demo {
    ASCIILiteral page;
    ASCIILiteral script;
    ASCIILiteral holders;
    // Whether JSC's own heap snapshot, which has no C++ objects, finds the leak too.
    bool isFoundWithoutC { false };
};

// What a demo leaks: a cell and its typed array's storage, a message copied for
// a worker and its bytes, or a removed frame's C++ document.
struct Leaked {
    uint64_t object;
    uint64_t storage; // 0 if it has none.
    bool isAfterFirstSnapshot;
};

struct DemoState {
    // Each demo is a page of its own, with a document that has the element `base`.
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
    JSC::Strong<JSC::Structure> wrapperStructure;
    JSC::Strong<JSC::Structure> messageEventStructure; // Its prototype has MessageEvent.prototype.data.
    JSC::Strong<JSC::Structure> workerStructure; // Its prototype has Worker.prototype.postMessage.
    JSC::Strong<JSC::Structure> documentStructure; // Its prototype has Document.prototype.createElement.
    JSC::Strong<JSC::Structure> iframeStructure; // Its prototype has HTMLIFrameElement.prototype.remove.
    Vector<DemoState> demos;
    size_t currentDemo { 0 };
    bool isAfterFirstSnapshot { false };
};

TestBrowser* browser { nullptr };

TestElementOwner& elementOwner()
{
    static NeverDestroyed<TestElementOwner> owner;
    return owner;
}

// The leaks in the leak-tests repository, each
// in C++ as WebCore makes it where a web page's leak goes through WebCore. The
// scripts are the demos', with setInterval's callback fired by the test.
// Uint16Array is wrapped so that the test knows which objects leak; nothing
// the dump is checked with comes from JS names, which the dump does not have,
// so obfuscated names change nothing.
const std::array<Demo, 9> demos { {
    { "simple-01.html"_s,
        "const base = window.base\n"
        "let myGlobal = { arr: [] }\n"
        "setInterval(() => {\n"
        "    myGlobal.arr.push(new Uint16Array(1000))\n"
        "}, 1000);\n"_s,
        "recordHolder(myGlobal); recordHolder(myGlobal.arr);"_s, true },
    { "simple-02.html"_s,
        "const base = window.base\n"
        "let myGlobal = { arr: [] }\n"
        "function polyVariantMake() { return new Uint16Array(1000) }\n"
        "setInterval(() => {\n"
        "    myGlobal.arr.push(polyVariantMake())\n"
        "}, 100);\n"_s,
        "recordHolder(myGlobal); recordHolder(myGlobal.arr);"_s, true },
    // MessageEvent's data is held by the event's C++ object.
    { "simple-03.html"_s,
        "let globObj = { arr: [] }\n"
        "setInterval(() => {\n"
        "    globObj.arr.push(new MessageEvent(\"test\", { data: { xxxxxx: new Uint16Array(1000) } }))\n"
        "}, 100)\n"_s,
        "recordHolder(globObj); recordHolder(globObj.arr);"_s, true },
    { "simple-04.html"_s,
        "const base = window.base\n"
        "let myGlobal = { arr: [] }\n"
        "function polyVariantMake() { return new Uint16Array(1000) }\n"
        "function polyVariantMake2() { return new Uint16Array(1000) }\n"
        "function polyVariantMake3() { return new Uint16Array(1000) }\n"
        "function polyVariantMake4() { return new Uint16Array(1000) }\n"
        "setInterval(() => {\n"
        "    myGlobal.arr.push(polyVariantMake())\n"
        "    myGlobal.arr.push(polyVariantMake2())\n"
        "    myGlobal.arr.push(polyVariantMake3())\n"
        "    myGlobal.arr.push(polyVariantMake4())\n"
        "}, 100);\n"_s,
        "recordHolder(myGlobal); recordHolder(myGlobal.arr);"_s, true },
    // The array grows inside what the event's C++ object holds.
    { "simple-05.html"_s,
        "let globObj = { e: new MessageEvent(\"test\", { data: { xxxxxx: [] } }) }\n"
        "setInterval(() => {\n"
        "    globObj.e.data.xxxxxx.push(new Uint16Array(1000))\n"
        "}, 100)\n"_s,
        "recordHolder(globObj); recordHolder(globObj.e); recordWrapped(globObj.e); recordHolder(globObj.e.data.xxxxxx);"_s },
    // Each closure holds the one before it: x.copy.[...].arr.
    { "simple-06.html"_s,
        "let x = () => 0\n"
        "setInterval(() => {\n"
        "    x = (function() {\n"
        "      let arr = new Uint16Array(1000)\n"
        "      let copy = x\n"
        "      return () => copy() + arr.length\n"
        "    })()\n"
        "}, 100)\n"_s,
        "recordScopeOf(() => x);"_s, true },
    // The chain of closures hangs off an element's wrapper, which the element
    // keeps alive, and the element is its document's.
    { "simple-07.html"_s,
        "setInterval(() => {\n"
        "    window.base.onClick = (function() {\n"
        "      let prev = window.base.onClick\n"
        "      let x = new Uint16Array(1000)\n"
        "      return (a) => a + prev(x.length)\n"
        "    })()\n"
        "}, 100)\n"_s,
        "recordHolder(window.base); recordWrapped(window.base); recordWrapped(document);"_s },
    // Each frame's realm, with its document and its window.obj, is kept alive by a
    // string from it, only through an output constraint.
    { "simple-08.html"_s,
        "const strings = [];\n"
        "setInterval(() => {\n"
        "  const iframe = document.createElement('iframe');\n"
        "  document.body.appendChild(iframe);\n"
        "  const str = iframe.contentWindow.eval('window.obj = new Uint16Array(1000); String(\"hi\")');\n"
        "  iframe.remove();\n"
        "  strings.push(str);\n"
        "}, 100);\n"_s,
        "recordHolder(strings);"_s },
    // The messages wait, copied, in the busy worker's queue, which only C++ holds.
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
    "Object.defineProperty(globalThis, 'base', { get() { return elementById('base'); } });\n"
    "Object.defineProperty(globalThis, 'document', { get() { return theDocument(); } });\n"
    "globalThis.MessageEvent = function MessageEvent(type, init) { return createMessageEvent(init.data); };\n"
    "Object.defineProperty(MessageEvent.prototype, 'data', { get() { return messageEventData(this); } });\n"
    "globalThis.Worker = function Worker(url) { return createWorker(); };\n"
    "Worker.prototype.postMessage = function postMessage(message) { workerPostMessage(this, message); };\n"
    "globalThis.Document = function Document() { };\n"
    "Document.prototype.createElement = function createElement(name) { return createIframe(); };\n"
    "Object.defineProperty(Document.prototype, 'body', { get() { return { appendChild(child) { } }; } });\n"
    "globalThis.HTMLIFrameElement = function HTMLIFrameElement() { };\n"
    "HTMLIFrameElement.prototype.remove = function remove() { };\n"
    // The prototypes of the wrappers the browser makes.
    "[MessageEvent.prototype, Worker.prototype, Document.prototype, HTMLIFrameElement.prototype];\n"_s;

// What a frame's script sees: its window, and Uint16Array wrapped as the demos' is.
constexpr ASCIILiteral frameScript = "globalThis.window = globalThis;\n"
    "(function() {\n"
    "    const RealUint16Array = Uint16Array;\n"
    "    globalThis.Uint16Array = function Uint16Array(length) { return recordLeak(new RealUint16Array(length)); };\n"
    "})();\n"_s;

DemoState& currentDemo()
{
    return browser->demos[browser->currentDemo];
}

JSTestWrapper* wrap(JSC::JSGlobalObject* globalObject, TestWrappable& wrapped, JSC::Structure* structure, JSC::WeakHandleOwner* owner = nullptr)
{
    if (auto* existing = wrapped.wrapper.get())
        return uncheckedDowncast<JSTestWrapper>(existing);
    auto* wrapper = JSTestWrapper::create(globalObject->vm(), structure, wrapped);
    wrapped.wrapper = JSC::Weak<JSC::JSObject>(wrapper, owner);
    return wrapper;
}

JSTestWrapper* wrapperArgument(JSC::CallFrame* callFrame, size_t index)
{
    return dynamicDowncast<JSTestWrapper>(callFrame->argument(index));
}

JSC_DEFINE_HOST_FUNCTION(elementById, (JSC::JSGlobalObject* globalObject, JSC::CallFrame*))
{
    return JSC::JSValue::encode(wrap(globalObject, currentDemo().document->children.first().get(), browser->wrapperStructure.get(), &elementOwner()));
}

JSC_DEFINE_HOST_FUNCTION(theDocument, (JSC::JSGlobalObject* globalObject, JSC::CallFrame*))
{
    return JSC::JSValue::encode(wrap(globalObject, *currentDemo().document, browser->documentStructure.get(), &elementOwner()));
}

JSC_DEFINE_HOST_FUNCTION(createMessageEvent, (JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame))
{
    Ref event = adoptRef(*new TestMessageEvent);
    JSC::JSValue data = callFrame->argument(0);
    RELEASE_ASSERT(data.isCell());
    event->data = JSC::Weak<JSC::JSCell>(data.asCell());
    return JSC::JSValue::encode(wrap(globalObject, event.get(), browser->messageEventStructure.get()));
}

JSC_DEFINE_HOST_FUNCTION(messageEventData, (JSC::JSGlobalObject*, JSC::CallFrame* callFrame))
{
    auto* wrapper = wrapperArgument(callFrame, 0);
    RELEASE_ASSERT(wrapper);
    return JSC::JSValue::encode(wrapper->wrapped().additionalChild());
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

// The scope a closure closes over, which holds the variables of the demo's script.
JSC_DEFINE_HOST_FUNCTION(recordScopeOf, (JSC::JSGlobalObject*, JSC::CallFrame* callFrame))
{
    auto* function = dynamicDowncast<JSC::JSFunction>(callFrame->argument(0));
    RELEASE_ASSERT(function);
    currentDemo().holders.append(std::bit_cast<uint64_t>(function->scope()));
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

// A frame's window.eval: what it returns is the frame's TestRealmValue.
JSC_DEFINE_HOST_FUNCTION(frameEval, (JSC::JSGlobalObject* frame, JSC::CallFrame* callFrame))
{
    JSC::JSValue code = callFrame->argument(0);
    RELEASE_ASSERT(code.isString());
    evaluate(frame, asString(code)->value(frame));
    Ref value = adoptRef(*new TestRealmValue);
    value->realm = JSC::Weak<JSC::JSGlobalObject>(frame);
    return JSC::JSValue::encode(wrap(frame, value.get(), browser->wrapperStructure.get()));
}

// document.createElement('iframe'): a frame with a realm and a document of its own.
JSC_DEFINE_HOST_FUNCTION(createIframe, (JSC::JSGlobalObject* globalObject, JSC::CallFrame*))
{
    JSC::VM& vm = globalObject->vm();
    JSC::JSGlobalObject* frame = JSC::JSGlobalObject::create(vm, JSC::JSGlobalObject::createStructure(vm, JSC::jsNull()));
    Ref document = adoptRef(*new TestDocument);
    currentDemo().leaked.append({ std::bit_cast<uint64_t>(document.ptr()), 0, browser->isAfterFirstSnapshot });
    frame->putDirect(vm, JSC::Identifier::fromString(vm, "document"_s), wrap(frame, document.get(), browser->documentStructure.get()));
    frame->putDirect(vm, JSC::Identifier::fromString(vm, "eval"_s), JSC::JSFunction::create(vm, frame, 1, "eval"_s, frameEval, JSC::ImplementationVisibility::Public));
    frame->putDirect(vm, JSC::Identifier::fromString(vm, "recordLeak"_s), JSC::JSFunction::create(vm, frame, 1, "recordLeak"_s, recordLeak, JSC::ImplementationVisibility::Public));
    evaluate(frame, frameScript);
    Ref element = adoptRef(*new TestElement);
    auto* iframe = wrap(globalObject, element.get(), browser->iframeStructure.get());
    iframe->putDirect(vm, JSC::Identifier::fromString(vm, "contentWindow"_s), frame);
    return JSC::JSValue::encode(iframe);
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
    browser->wrapperStructure.set(vm, JSTestWrapper::createStructure(vm, globalObject, globalObject->objectPrototype()));
    browser->demos.resize(demos.size());
    for (DemoState& demo : browser->demos) {
        demo.document = adoptRef(*new TestDocument);
        demo.document->children.append(adoptRef(*new TestElement));
    }

    auto addFunction = [&](ASCIILiteral name, unsigned length, JSC::NativeFunction function) {
        globalObject->putDirect(vm, JSC::Identifier::fromString(vm, name), JSC::JSFunction::create(vm, globalObject, length, name, function, JSC::ImplementationVisibility::Public));
    };
    addFunction("elementById"_s, 1, elementById);
    addFunction("theDocument"_s, 0, theDocument);
    addFunction("createMessageEvent"_s, 1, createMessageEvent);
    addFunction("messageEventData"_s, 1, messageEventData);
    addFunction("createWorker"_s, 0, createWorker);
    addFunction("workerPostMessage"_s, 2, workerPostMessage);
    addFunction("createIframe"_s, 0, createIframe);
    addFunction("setInterval"_s, 2, setIntervalForTest);
    addFunction("recordLeak"_s, 1, recordLeak);
    addFunction("recordHolder"_s, 1, recordHolder);
    addFunction("recordWrapped"_s, 1, recordWrapped);
    addFunction("recordScopeOf"_s, 1, recordScopeOf);
    JSC::JSValue prototypes = evaluate(globalObject, browserScript);
    auto prototype = [&](unsigned index) {
        JSC::JSValue prototype = prototypes.getObject()->get(globalObject, index);
        RELEASE_ASSERT(prototype.isObject());
        return prototype;
    };
    browser->messageEventStructure.set(vm, JSTestWrapper::createStructure(vm, globalObject, prototype(0)));
    browser->workerStructure.set(vm, JSTestWrapper::createStructure(vm, globalObject, prototype(1)));
    browser->documentStructure.set(vm, JSTestWrapper::createStructure(vm, globalObject, prototype(2)));
    browser->iframeStructure.set(vm, JSTestWrapper::createStructure(vm, globalObject, prototype(3)));
    vm.heap.addMarkingConstraint(makeUnique<TestOutputConstraint>(vm));

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
        successors.resize(this->dump.nodes.size());
        // Every edge, whether retained sizes count it or not: every path a person can follow.
        for (auto& edge : this->dump.edges)
            successors[edge.from].append(edge.to);
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

    static constexpr uint32_t unvisited = std::numeric_limits<uint32_t>::max();
    HeapWalk::HeapDump dump;
    HeapWalk::HeapDump::Retention retention;
    Vector<Vector<uint32_t>> successors;
};

// The two dumps of one walk of the heap, mya's or JSC's collector's.
struct Walk {
    ASCIILiteral name;
    // Whether storage outside a cell, such as a typed array's, is a node of its own; JSC
    // counts it in the cell's size.
    bool hasStorageNodes;
    Graph before;
    Graph after;
};

// The nodes of each demo's leak, in the second dump, and for each node the demos whose
// leaked nodes it dominates, as a mask, so its retained size grows by their leaks.
struct Leaks {
    Vector<Vector<uint32_t>> nodes;
    Vector<uint64_t> bytesBetween; // Of what leaked between the dumps.
    Vector<String> missing; // The first leaked object that is no node, if any.
    Vector<uint32_t> dominatorOf;
};

Leaks leaksIn(const Walk& walk)
{
    const Graph& graph = walk.after;
    Leaks leaks;
    leaks.nodes.resize(demos.size());
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
                if (leaked.isAfterFirstSnapshot)
                    leaks.bytesBetween[index] += graph.dump.nodes[*node].size;
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

// Whether a walk finds a demo's leak, as a person reads Web Inspector's views of its
// dumps: each holder is on a path from <root> to every leaked object, so the path a
// person expects is there, whatever other paths there are; and, sorted by how much its
// retained size grew between the dumps, each holder is found by going down that list: no
// node grew more but its own dominators and those of the other demos' leaks, and it grew
// by at least half of what leaked; what grew more is what holds it, what it holds,
// what holds another demo's leak, or what no path reaches. Empty if it does, or why it does not.
String whyNotFound(const Walk& walk, const Leaks& leaks, size_t index)
{
    const Graph& graph = walk.after;
    const auto& nodes = graph.dump.nodes;
    if (!leaks.missing[index].isNull())
        return leaks.missing[index];
    // Each node of the first dump by its address, or, for a root's node, which has none, by its name.
    HashMap<uint64_t, uint32_t> nodesBefore;
    HashMap<String, uint32_t> rootsBefore;
    for (uint32_t node = 1; node < walk.before.dump.nodes.size(); ++node) {
        const auto& before = walk.before.dump.nodes[node];
        if (before.kind == HeapWalk::HeapDump::Kind::Root)
            rootsBefore.add(before.className, node);
        else if (before.address)
            nodesBefore.add(before.address.toTargetVMAddress(), node);
    }
    auto growth = [&](uint32_t node) -> int64_t {
        if (!node)
            return 0;
        int64_t after = graph.retention.retainedSize[node];
        std::optional<uint32_t> before;
        if (nodes[node].kind == HeapWalk::HeapDump::Kind::Root) {
            if (auto found = rootsBefore.find(nodes[node].className); found != rootsBefore.end())
                before = found->value;
        } else if (nodes[node].address) {
            auto found = nodesBefore.find(nodes[node].address.toTargetVMAddress());
            if (found != nodesBefore.end() && walk.before.dump.nodes[found->value].className == nodes[node].className)
                before = found->value;
        }
        return before ? after - static_cast<int64_t>(walk.before.retention.retainedSize[*before]) : after;
    };
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
        int64_t holderGrowth = growth(*node);
        if (holderGrowth < static_cast<int64_t>(leaks.bytesBetween[index] / 2))
            return makeString("the retained size of "_s, name, " grew by "_s, holderGrowth, " bytes, of the "_s, leaks.bytesBetween[index], " that leaked between the dumps"_s);
        unsigned rank = 0;
        for (uint32_t other = 1; other < nodes.size(); ++other) {
            // What no path reaches, such as an allocation the walk missed, holds nothing.
            if (growth(other) <= holderGrowth || fromRoot[other] == Graph::unvisited)
                continue;
            ++rank;
            // Its own dominators, what it holds, and what holds another demo's leak.
            if (graph.retention.dominates(other, *node) || graph.retention.dominates(*node, other) || (leaks.dominatorOf[other] & ~(1u << index)))
                continue;
            return makeString("the retained size of "_s, name, " grew by "_s, holderGrowth, " bytes, less than that of the unrelated "_s, nodes[other].className, " at 0x"_s,
                hex(nodes[other].address.toTargetVMAddress()), ", by "_s, growth(other));
        }
        dataLogLnIf(verbose, "    ", walk.name, ": ", demos[index].page, ": ", name, " grew by ", holderGrowth, " bytes of ", leaks.bytesBetween[index], " leaked, and is number ", rank + 1, " by growth");
    }
    return { };
}

} // anonymous namespace

// The leaks of https://github.com/justinmichaud/leak-tests: two dumps of this process
// around timers that leak, as `mya heapdump` writes them, find every one. The same heap
// as JSC's collector's own visitor walks it, as Web Inspector has it, finds only those
// held by JS objects alone: what C++ holds, or what holds it, is in no node.
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

    Leaks myaLeaks = leaksIn(mya);
    Leaks jscLeaks = leaksIn(jsc);
    for (size_t index = 0; index < demos.size(); ++index) {
        const Demo& demo = demos[index];
        TEST_ASSERT(browser->demos[index].leaked.size() >= warmUpTicks + leakingTicks, makeString(demo.page, " leaks an object on every tick"_s));
        String myaMiss = whyNotFound(mya, myaLeaks, index);
        if (!myaMiss.isNull())
            dataLogLn("    mya: ", demo.page, ": ", myaMiss);
        TEST_ASSERT(myaMiss.isNull(), makeString("mya's heap dumps find the leak of "_s, demo.page));
        String jscMiss = whyNotFound(jsc, jscLeaks, index);
        dataLogLnIf(verbose || jscMiss.isNull() != demo.isFoundWithoutC, "    JSC's heap snapshot: ", demo.page, ": ", jscMiss.isNull() ? "finds it"_s : jscMiss);
        TEST_ASSERT_EQ(jscMiss.isNull(), demo.isFoundWithoutC, makeString("JSC's own heap snapshots find the leak of "_s, demo.page, " only if it holds no C++ object"_s));
    }
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
