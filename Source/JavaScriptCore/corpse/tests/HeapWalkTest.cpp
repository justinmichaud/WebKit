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
#include "HeapWalkTest.h"

#if ENABLE(MYA_HEAP)

#include "LibJSCToolsTestUtilities.h"

#include <JavaScriptCore/AlignedMemoryAllocator.h>
#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseHeapWalk.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseSnapshotDebugInfo.h>
#include <JavaScriptCore/CorpseTargetType.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/JSGlobalObjectInlines.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/Subspace.h>
#include <JavaScriptCore/VM.h>
#include <JavaScriptCore/Watchpoint.h>
#include <array>
#include <atomic>
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/RunLoop.h>
#include <wtf/Scope.h>
#include <wtf/SentinelLinkedList.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

#if OS(DARWIN)
#include <JavaScriptCore/CorpseProcess.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <spawn.h>
#include <unistd.h>

extern char** environ;
#endif

#endif // ENABLE(MYA_HEAP)

namespace JSCToolsTest {

#if ENABLE(MYA_HEAP)

namespace {

using JSC::Corpse::Address;
using JSC::Corpse::HeapAnalyzer;
using JSC::Corpse::Snapshot;
using JSC::Corpse::SnapshotDebugInfo;
using JSC::Corpse::TargetObject;
using JSC::Corpse::sameType;
using JSC::Corpse::TargetClass;
using JSC::Corpse::TargetType;
using JSC::Corpse::TargetTypeRef;
using NeedsReader = TargetClass::NeedsReader;

static bool isJavaScriptCoreImage(const String& image)
{
#if OS(DARWIN)
    return image.endsWith("/JavaScriptCore"_s);
#else
    return image.contains("/libJavaScriptCore.so"_s);
#endif
}

// Held only in types that need readers of their own, so the walk does not reach them.
struct Unreached {
    uint64_t value { 0 };
};

struct UnreachedNode : BasicRawSentinelNode<UnreachedNode> {
};

struct Leaf {
    uint64_t value { 0 };
};

struct VirtualBaseHoldsLeaf {
    Leaf* leaf { new Leaf };
};

struct WithVirtualBase : virtual VirtualBaseHoldsLeaf {
};

// Its virtual base is at a different offset than in a WithVirtualBase.
struct DerivedWithVirtualBase : WithVirtualBase {
    Leaf* own { new Leaf };
};

struct WithBitfield {
    bool flag : 1 { false };
    Unreached* pointer { new Unreached };
};

struct HoldsLeaf {
    Leaf* leaf { new Leaf };
};

struct FirstParent : HoldsLeaf {
};

struct SecondParent : HoldsLeaf {
};

// Two HoldsLeaf subobjects, each walked through its own parent.
struct Diamond : FirstParent, SecondParent {
    Leaf* own { new Leaf };
};

class FirstPolymorphicBase {
public:
    virtual ~FirstPolymorphicBase() = default;
};

class SecondPolymorphicBase {
public:
    virtual ~SecondPolymorphicBase() = default;
};

struct PolymorphicHoldsLeaf {
    virtual ~PolymorphicHoldsLeaf() = default;
    Leaf* leaf { new Leaf };
};

struct FirstPolymorphicParent : PolymorphicHoldsLeaf {
};

struct SecondPolymorphicParent : PolymorphicHoldsLeaf {
};

// Two PolymorphicHoldsLeaf subobjects, each with a vtable that leads back to the whole object.
struct PolymorphicDiamond final : FirstPolymorphicParent, SecondPolymorphicParent {
    Leaf* own { new Leaf };
};

// Held through SecondPolymorphicBase, whose vtable has a nonzero offset to top.
class HeldThroughSecondBase final : public FirstPolymorphicBase, public SecondPolymorphicBase {
public:
    ~HeldThroughSecondBase() final = default;

    Leaf* leaf { new Leaf };
};

struct HoldsLeafByValue {
    uint64_t value { 0 };
    HoldsLeaf held;
};

// liblldb finds no instance of this template by its name.
template<typename T, size_t count>
struct Leaves {
    T* leaves[count] { new T, new T };
};

// The VM's client data, which the walk from the VM finds from its vtable.
class MyaRoots final : public JSC::VM::ClientData {
public:
    explicit MyaRoots(JSC::VM& vm, JSC::JSGlobalObject* globalObject)
        : vm(&vm)
        , importMap(&globalObject->importMap())
        , symbolTableCache(&globalObject->symbolTableCache())
    {
        vector.constructAndAppend();
        deque.constructAndAppend();
        hashMap.add(1, Unreached { });
        list.push(new UnreachedNode);
    }
    ~MyaRoots() final = default;

    String overrideSourceURL(const JSC::StackFrame&, const String& originalSourceURL) const final { return originalSourceURL; }

    JSC::VM* vm;
    // Only declared here, though polymorphic. The VM's heap reaches the object.
    JSC::GCActivityCallback* activityCallback { vm->heap.fullActivityCallback() };
    JSC::GCActivityCallback* activityCallbacks[2] { activityCallback, activityCallback };
    // Only declared here, and nothing else reaches the object.
    JSC::ImportMap* importMap;
    MyaRoots* self { this };
    Vector<Unreached> vector;
    Deque<Unreached> deque;
    HashMap<int, Unreached> hashMap;
    SentinelLinkedList<UnreachedNode> list;
    union {
        Unreached* pointer { new Unreached };
        uint64_t bits;
    } inUnion;
    WithVirtualBase withVirtualBase;
    DerivedWithVirtualBase derivedWithVirtualBase;
    WithBitfield withBitfield;
    Diamond diamond;
    std::array<HoldsLeaf, 2> array;
    SecondPolymorphicBase* secondBase { new HeldThroughSecondBase };
    Leaves<Leaf, 2>* templateInstance { new Leaves<Leaf, 2> };
    // Defined in this executable; the object's class is JavaScriptCore's.
    JSC::FireDetail* fireDetail { nullptr };
    HoldsLeaf* holdsLeaf { new HoldsLeaf };
    // The object holdsLeaf points to, read as a class that neither is nor has a subobject of it.
    Leaf* punned { reinterpret_cast<Leaf*>(holdsLeaf) };
    Diamond* diamondPointer { new Diamond };
    // A subobject of the Diamond at its own address.
    HoldsLeaf* firstOfDiamond { static_cast<FirstParent*>(diamondPointer) };
    // A subobject of the Diamond inside it. Diamond has no vtable, so nothing at this address leads back to the Diamond.
    HoldsLeaf* secondOfDiamond { static_cast<SecondParent*>(diamondPointer) };
    HoldsLeafByValue* byValue { new HoldsLeafByValue };
    // A member of the object byValue points to.
    HoldsLeaf* memberOfByValue { &byValue->held };
    PolymorphicDiamond* polymorphicDiamond { new PolymorphicDiamond };
    PolymorphicHoldsLeaf* secondOfPolymorphicDiamond { static_cast<SecondPolymorphicParent*>(polymorphicDiamond) };
    Leaf** pointerToPointer { &holdsLeaf->leaf };
    const char* string { "heap walk" };
    std::atomic<Leaf*> atomicLeaf { new Leaf };
    std::atomic<uint64_t> atomicScalar { 0 };
    // Its vtable is JavaScriptCore's, whose WeakGCMap points to the VM.
    JSC::WeakGCHashTable* symbolTableCache;
};

struct HeapWalkFixture {
    uint64_t roots;
    uint64_t vm;
    uint64_t apiLock;
    uint64_t runLoop;
    uint64_t activityCallback;
    uint64_t importMap;
    uint64_t isoSubspace;
    uint64_t gigacageAllocator;
    uint64_t virtualBaseLeaf;
    uint64_t derivedVirtualBaseLeaf;
    uint64_t derivedOwnLeaf;
    uint64_t firstParentLeaf;
    uint64_t secondParentLeaf;
    uint64_t diamondLeaf;
    uint64_t arrayLeaves[2];
    uint64_t heldThroughSecondBase;
    uint64_t heldThroughSecondBaseLeaf;
    uint64_t templateInstanceLeaf;
    uint64_t fireDetail;
    uint64_t holdsLeaf;
    uint64_t diamond;
    uint64_t secondOfDiamond;
    uint64_t secondOfDiamondLeaf;
    uint64_t byValue;
    uint64_t memberOfByValue;
    uint64_t pointerToPointerSlot;
    uint64_t pointerToPointerLeaf;
    uint64_t polymorphicDiamond;
    uint64_t polymorphicDiamondLeaves[3];
    uint64_t atomicLeaf;
    // Functions whose first parameter points to a class, as the image of each function defines it.
    uint64_t javaScriptCoreVM;
    uint64_t allocator;
    uint64_t subspace;
};

Address createFixture()
{
    static HeapWalkFixture fixture;

    JSC::initialize();
    JSC::VM& vm = JSC::VM::create(JSC::HeapType::Large).leakRef();
    setTargetVM(vm);
    JSC::JSLockHolder locker(vm);
    JSC::JSGlobalObject* globalObject = JSC::JSGlobalObject::create(vm, JSC::JSGlobalObject::createStructure(vm, JSC::jsNull()));
    MyaRoots* roots = new MyaRoots(vm, globalObject);
    vm.clientData = roots;
    RELEASE_ASSERT(roots->activityCallback);
    // FireDetail cannot be allocated with new.
    static JSC::StringFireDetail fireDetail("heap-walk");
    roots->fireDetail = &fireDetail;
    auto* heldThroughSecondBase = static_cast<HeldThroughSecondBase*>(roots->secondBase);

    fixture = {
        Address { roots }.toMachVMAddress(),
        Address { &vm }.toMachVMAddress(),
        Address { &vm.apiLock() }.toMachVMAddress(),
        Address { &vm.runLoop() }.toMachVMAddress(),
        Address { roots->activityCallback }.toMachVMAddress(),
        Address { roots->importMap }.toMachVMAddress(),
        Address { &vm.heap.structureSpace }.toMachVMAddress(),
        Address { vm.heap.primitiveGigacageAllocator.get() }.toMachVMAddress(),
        Address { roots->withVirtualBase.leaf }.toMachVMAddress(),
        Address { roots->derivedWithVirtualBase.leaf }.toMachVMAddress(),
        Address { roots->derivedWithVirtualBase.own }.toMachVMAddress(),
        Address { static_cast<FirstParent&>(roots->diamond).leaf }.toMachVMAddress(),
        Address { static_cast<SecondParent&>(roots->diamond).leaf }.toMachVMAddress(),
        Address { roots->diamond.own }.toMachVMAddress(),
        { Address { roots->array[0].leaf }.toMachVMAddress(), Address { roots->array[1].leaf }.toMachVMAddress() },
        Address { heldThroughSecondBase }.toMachVMAddress(),
        Address { heldThroughSecondBase->leaf }.toMachVMAddress(),
        Address { roots->templateInstance->leaves[1] }.toMachVMAddress(),
        Address { roots->fireDetail }.toMachVMAddress(),
        Address { roots->holdsLeaf }.toMachVMAddress(),
        Address { roots->diamondPointer }.toMachVMAddress(),
        Address { roots->secondOfDiamond }.toMachVMAddress(),
        Address { roots->secondOfDiamond->leaf }.toMachVMAddress(),
        Address { roots->byValue }.toMachVMAddress(),
        Address { roots->memberOfByValue }.toMachVMAddress(),
        Address { roots->pointerToPointer }.toMachVMAddress(),
        Address { *roots->pointerToPointer }.toMachVMAddress(),
        Address { roots->polymorphicDiamond }.toMachVMAddress(),
        { Address { static_cast<FirstPolymorphicParent*>(roots->polymorphicDiamond)->leaf }.toMachVMAddress(), Address { static_cast<SecondPolymorphicParent*>(roots->polymorphicDiamond)->leaf }.toMachVMAddress(), Address { roots->polymorphicDiamond->own }.toMachVMAddress() },
        Address { roots->atomicLeaf.load() }.toMachVMAddress(),
        Address { reinterpret_cast<const void*>(&JSC::sanitizeStackForVM) }.toMachVMAddress(),
        functionTakingPointerTo<JSC::AlignedMemoryAllocator>().toMachVMAddress(),
        functionTakingPointerTo<JSC::Subspace>().toMachVMAddress(),
    };
    return Address { &fixture };
}

class RecordingAnalyzer final : public HeapAnalyzer {
public:
    void analyzeNode(const TargetObject& object) final
    {
        auto& types = nodes.ensure(object.address.toMachVMAddress(), [] { return Vector<TargetTypeRef> { }; }).iterator->value;
        TEST_ASSERT(!types.containsIf([&](const TargetType& type) { return sameType(type, object.type); }), "the walk reaches each node once");
        types.append(object.type);
    }

    void analyzeEdge(const TargetObject& from, const JSC::Corpse::TargetCanonicalPointer& to) final
    {
        TEST_ASSERT(isNode(from) && isNode(to.node), "an edge joins objects the walk reached");
        if (std::holds_alternative<TargetClass>(to.node.type))
            TEST_ASSERT(baseAt(to.node.type, to.subobject.address - to.node.address, std::get<TargetClass>(to.subobject.type)), "an edge designates a base of the object it reaches");
        else
            TEST_ASSERT(to.subobject.address == to.node.address && &to.subobject.type == &to.node.type, "an edge to what is not a class designates the node itself");
        if (from.address.toMachVMAddress() == rootAddress)
            fromRoot.append(to.node);
        edges.append({ from.address.toMachVMAddress(), to.node.address.toMachVMAddress() });
        sources.ensure(to.node.address.toMachVMAddress(), [] { return HashSet<uint64_t> { }; }).iterator->value.add(from.address.toMachVMAddress());
        if (to.node.address.toMachVMAddress() == vmAddress && std::holds_alternative<TargetClass>(from.type))
            imagesPointingToVM.add(std::get<TargetClass>(from.type).image);
    }

    void analyzeOpaqueEdge(const TargetObject& from, Address address, const JSC::Corpse::TargetOpaque& pointee) final
    {
        TEST_ASSERT(isNode(from), "an opaque pointer is in an object the walk reached");
        if (pointee.kind == JSC::Corpse::TargetOpaque::Kind::IgnoredDeclaration)
            ignoredDeclarationHolders.add(from.address.toMachVMAddress());
        if (from.address.toMachVMAddress() == rootAddress)
            opaqueInRoot.append({ address.toMachVMAddress() - rootAddress, pointee.name });
        else
            ++opaquePointerCount;
    }

    void analyzeNeedsReader(const TargetObject& from, const TargetObject& subobject) final
    {
        TEST_ASSERT(isNode(from), "a subobject that needs a reader is in an object the walk reached");
        needsReader.append({ subobject.address.toMachVMAddress(), std::get<TargetClass>(subobject.type).needsReader });
    }

    static std::optional<TargetTypeRef> find(const Vector<TargetTypeRef>& types, const String& name)
    {
        for (const TargetType& type : types) {
            if (JSC::Corpse::name(type) == name)
                return type;
        }
        return std::nullopt;
    }

    bool isNode(const TargetObject& object) const
    {
        auto it = nodes.find(object.address.toMachVMAddress());
        return it != nodes.end() && it->value.containsIf([&](const TargetType& type) { return &type == &object.type; });
    }

    std::optional<TargetTypeRef> reached(uint64_t address, ASCIILiteral name) const
    {
        auto it = nodes.find(address);
        return it == nodes.end() ? std::nullopt : find(it->value, name);
    }

    bool reachedFromRoot(uint64_t address, ASCIILiteral name) const
    {
        return fromRoot.containsIf([&](auto& to) {
            return to.address.toMachVMAddress() == address && JSC::Corpse::name(to.type) == name;
        });
    }

    size_t edgesFromRootTo(uint64_t address) const
    {
        size_t count = 0;
        for (auto& [from, to] : edges)
            count += from == rootAddress && to == address;
        return count;
    }

    bool needsReaderAt(uint64_t address, NeedsReader reason) const
    {
        return needsReader.contains(std::pair { address, reason });
    }

    size_t needsReaderIn(uint64_t address, size_t size) const
    {
        size_t count = 0;
        for (auto& [subobject, reason] : needsReader)
            count += subobject >= address && subobject < address + size;
        return count;
    }

    uint64_t rootAddress { 0 };
    uint64_t vmAddress { 0 };
    Vector<TargetObject> fromRoot;
    Vector<std::pair<uint64_t, String>> opaqueInRoot;
    HashSet<String> imagesPointingToVM;
    // The addresses of the nodes that point to a class in CorpseIgnoredDeclarations.h.
    HashSet<uint64_t> ignoredDeclarationHolders;
    size_t opaquePointerCount { 0 };
    // The address of each subobject that needs a reader, and why.
    Vector<std::pair<uint64_t, NeedsReader>> needsReader;
    HashMap<uint64_t, Vector<TargetTypeRef>> nodes;
    // The addresses of the nodes at each end of each edge.
    Vector<std::pair<uint64_t, uint64_t>> edges;
    // The address of each node, and the addresses of the nodes with edges to it.
    HashMap<uint64_t, HashSet<uint64_t>> sources;
};

void analyze(Snapshot& snapshot, Address fixtureAddress)
{
    auto fixture = snapshot.memory().ptr<HeapWalkFixture>(fixtureAddress);
    TEST_ASSERT(fixture, "the target's fixture reads");
    if (!fixture)
        return;
    Ref debugInfo = SnapshotDebugInfo::create(snapshot);
    const TargetType* vmType = parameterClass(debugInfo.get(), Address { fixture->javaScriptCoreVM });
    if (!vmType)
        return;
    TEST_ASSERT(isJavaScriptCoreImage(std::get<TargetClass>(*vmType).image), "a function of JavaScriptCore gives the VM as JavaScriptCore defines it");

    RecordingAnalyzer analyzer;
    analyzer.rootAddress = fixture->roots;
    analyzer.vmAddress = fixture->vm;
    {
        // The ImportMap, which only a pointer to a class this executable only declares reaches.
        ExpectedErrors expectedErrors;
        JSC::Corpse::walkHeap(snapshot, debugInfo.get(), { Address { fixture->vm }, *vmType }, analyzer);
    }
    if (verbose)
        dataLogLn("    the walk reached ", analyzer.nodes.size(), " addresses through ", analyzer.edges.size(), " edges; ", analyzer.needsReader.size(), " subobjects need readers; ", analyzer.opaquePointerCount, " opaque pointers");

    auto rootOffset = [&](size_t offset) {
        return fixture->roots + offset;
    };
    std::optional<TargetTypeRef> rootsType = analyzer.reached(fixture->roots, "JSCToolsTest::(anonymous namespace)::MyaRoots"_s);
    TEST_ASSERT(rootsType && std::get<TargetClass>(rootsType->get()).image.endsWith("/testLibJSCTools"_s), "the walk reaches the roots through the VM's client data, as this executable defines them");
    TEST_ASSERT(analyzer.reached(fixture->vm, "JSC::VM"_s), "the walk reaches the VM");
    TEST_ASSERT_EQ(analyzer.nodes.get(fixture->vm).size(), 1u, "the VM is one node");
    TEST_ASSERT_EQ(analyzer.imagesPointingToVM.size(), 2u, "both images' classes point to the VM, which they define the same way");
    TEST_ASSERT(analyzer.reached(fixture->apiLock, "JSC::JSLock"_s), "the walk reaches the VM's lock through its Ref");
    TEST_ASSERT(analyzer.reached(fixture->runLoop, "WTF::RunLoop"_s), "the walk reaches the VM's run loop through its Ref");
    TEST_ASSERT(!analyzer.reached(fixture->activityCallback, "JSC::GCActivityCallback"_s), "the walk does not read an object as a class the image that points to it only declares");
    TEST_ASSERT(analyzer.reached(fixture->activityCallback, "JSC::FullGCActivityCallback"_s), "the walk reaches that object through the image that defines its class");
    TEST_ASSERT_EQ(analyzer.edgesFromRootTo(fixture->activityCallback), 3u, "each pointer to the declared class is an edge to that node, once the walk has found it");
    TEST_ASSERT(!analyzer.nodes.contains(fixture->importMap), "an object that only a pointer to a declared class reaches is not a node, and is reported");
#if USE(COCOA_EVENT_LOOP)
    TEST_ASSERT(analyzer.ignoredDeclarationHolders.contains(fixture->runLoop), "the run loop's CoreFoundation handles are ignored declarations");
#endif
    TEST_ASSERT(analyzer.opaqueInRoot.contains(std::pair { static_cast<uint64_t>(OBJECT_OFFSETOF(MyaRoots, string)), String { "char"_s } }), "the walk reports a pointer to a scalar");
    TEST_ASSERT_EQ(analyzer.opaqueInRoot.size(), 1u, "nothing else in the roots is opaque");
    TEST_ASSERT(analyzer.reachedFromRoot(fixture->pointerToPointerSlot, "JSCToolsTest::(anonymous namespace)::Leaf *"_s), "a pointer to a pointer reaches the pointer it points to as a node");
    TEST_ASSERT(analyzer.sources.get(fixture->pointerToPointerLeaf).contains(fixture->pointerToPointerSlot), "the walk reads that pointer too");
    TEST_ASSERT(analyzer.fromRoot.containsIf([&](auto& to) {
        return to.address.toMachVMAddress() == fixture->fireDetail && JSC::Corpse::name(to.type) == "JSC::StringFireDetail"_s && isJavaScriptCoreImage(std::get<TargetClass>(to.type).image);
    }), "the walk reads an object as the class the image of its vtable defines");
    TEST_ASSERT(analyzer.reached(fixture->diamond, "JSCToolsTest::(anonymous namespace)::Diamond"_s) && analyzer.reached(fixture->diamond, "JSCToolsTest::(anonymous namespace)::HoldsLeaf"_s), "an object and a subobject at its address are two nodes");
    TEST_ASSERT(analyzer.reached(fixture->holdsLeaf, "JSCToolsTest::(anonymous namespace)::HoldsLeaf"_s) && analyzer.reached(fixture->holdsLeaf, "JSCToolsTest::(anonymous namespace)::Leaf"_s), "an object read as two unrelated classes is two nodes");
    TEST_ASSERT(fixture->secondOfDiamond > fixture->diamond && analyzer.reached(fixture->secondOfDiamond, "JSCToolsTest::(anonymous namespace)::HoldsLeaf"_s), "a base inside an object without a vtable is a node of its own");
    TEST_ASSERT(analyzer.sources.get(fixture->secondOfDiamondLeaf).contains(fixture->diamond) && analyzer.sources.get(fixture->secondOfDiamondLeaf).contains(fixture->secondOfDiamond), "a pointer in that base is read from both nodes");
    TEST_ASSERT(analyzer.reachedFromRoot(fixture->polymorphicDiamond, "JSCToolsTest::(anonymous namespace)::PolymorphicDiamond"_s) && !analyzer.nodes.contains(fixture->polymorphicDiamond + sizeof(PolymorphicHoldsLeaf)), "a pointer to the second base of an object with a vtable reaches the whole object");
    for (uint64_t leaf : fixture->polymorphicDiamondLeaves)
        TEST_ASSERT(analyzer.sources.get(leaf).contains(fixture->polymorphicDiamond), "the walk reads every field of an object reached through its second base, including both copies of a base inherited twice");
    TEST_ASSERT(analyzer.reached(fixture->byValue, "JSCToolsTest::(anonymous namespace)::HoldsLeafByValue"_s) && analyzer.reached(fixture->memberOfByValue, "JSCToolsTest::(anonymous namespace)::HoldsLeaf"_s), "a member that a pointer designates is a node inside the node that holds it");
    TEST_ASSERT(analyzer.reachedFromRoot(fixture->heldThroughSecondBase, "JSCToolsTest::(anonymous namespace)::HeldThroughSecondBase"_s), "the walk reads an object held through its second base from the start of the object");
    TEST_ASSERT(analyzer.reached(fixture->heldThroughSecondBaseLeaf, "JSCToolsTest::(anonymous namespace)::Leaf"_s), "the walk reads the fields of an object reached through its second base");
    TEST_ASSERT(analyzer.reachedFromRoot(fixture->firstParentLeaf, "JSCToolsTest::(anonymous namespace)::Leaf"_s), "the walk reads the first copy of a base that is inherited twice");
    TEST_ASSERT(analyzer.reachedFromRoot(fixture->secondParentLeaf, "JSCToolsTest::(anonymous namespace)::Leaf"_s), "the walk reads the second copy of a base that is inherited twice");
    TEST_ASSERT(analyzer.reachedFromRoot(fixture->diamondLeaf, "JSCToolsTest::(anonymous namespace)::Leaf"_s), "the walk reads a class's own fields beside its parents'");
    TEST_ASSERT_EQ(analyzer.edgesFromRootTo(fixture->virtualBaseLeaf), 1u, "the walk reads a virtual base once, where the complete object holds it");
    TEST_ASSERT_EQ(analyzer.edgesFromRootTo(fixture->derivedVirtualBaseLeaf), 1u, "the walk reads a base's virtual base once, where the derived object holds it");
    TEST_ASSERT(analyzer.reachedFromRoot(fixture->derivedOwnLeaf, "JSCToolsTest::(anonymous namespace)::Leaf"_s), "the walk reads the fields of a class with a virtual base");
    TEST_ASSERT(analyzer.reachedFromRoot(fixture->arrayLeaves[0], "JSCToolsTest::(anonymous namespace)::Leaf"_s), "the walk reads the first element of an array");
    TEST_ASSERT(analyzer.reached(fixture->templateInstanceLeaf, "JSCToolsTest::(anonymous namespace)::Leaf"_s), "the walk reads a class it reached that liblldb cannot find by name");
    TEST_ASSERT(analyzer.reachedFromRoot(fixture->arrayLeaves[1], "JSCToolsTest::(anonymous namespace)::Leaf"_s), "the walk reads the last element of an array");

    size_t needsReaderInRoot = 6;
    TEST_ASSERT(analyzer.needsReaderAt(rootOffset(OBJECT_OFFSETOF(MyaRoots, vector)), NeedsReader::Container), "a Vector needs a reader");
    TEST_ASSERT(analyzer.needsReaderAt(rootOffset(OBJECT_OFFSETOF(MyaRoots, deque)), NeedsReader::Container), "a Deque needs a reader");
    TEST_ASSERT(analyzer.needsReaderAt(rootOffset(OBJECT_OFFSETOF(MyaRoots, hashMap)), NeedsReader::Container), "a HashMap's table needs a reader");
    TEST_ASSERT(analyzer.needsReaderAt(rootOffset(OBJECT_OFFSETOF(MyaRoots, list)), NeedsReader::Container), "a SentinelLinkedList needs a reader");
    TEST_ASSERT(analyzer.needsReaderAt(rootOffset(OBJECT_OFFSETOF(MyaRoots, inUnion)), NeedsReader::Union), "a union needs a reader");
    TEST_ASSERT(analyzer.needsReaderAt(rootOffset(OBJECT_OFFSETOF(MyaRoots, withBitfield)), NeedsReader::Bitfield), "a class with a bitfield needs a reader");
#if defined(_LIBCPP_VERSION)
    needsReaderInRoot += 2;
    TEST_ASSERT(analyzer.needsReaderAt(rootOffset(OBJECT_OFFSETOF(MyaRoots, atomicLeaf)), NeedsReader::Atomic), "a std::atomic of a pointer needs a reader");
    TEST_ASSERT(analyzer.needsReaderAt(rootOffset(OBJECT_OFFSETOF(MyaRoots, atomicScalar)), NeedsReader::Atomic), "a std::atomic of a scalar needs a reader");
#else
    // libstdc++ holds the value as a plain member, which liblldb describes.
    TEST_ASSERT(analyzer.reachedFromRoot(fixture->atomicLeaf, "JSCToolsTest::(anonymous namespace)::Leaf"_s), "the walk reads a std::atomic of a pointer");
#endif
    TEST_ASSERT_EQ(analyzer.needsReaderIn(fixture->roots, sizeof(MyaRoots)), needsReaderInRoot, "nothing else in the roots needs a reader");
    for (auto& types : analyzer.nodes.values()) {
        for (const TargetType& type : types) {
            if (JSC::Corpse::name(type).contains("Unreached"_s))
                TEST_ASSERT_EQ(JSC::Corpse::name(type), String { }, "the walk does not look inside a type that needs a reader");
        }
    }
}

// liblldb can answer differently for a class once it has read another compile unit: after the compile unit of
// JSC::GigacageAlignedMemoryAllocator's destructor, liblldb 21 calls JSC::IsoSubspace incomplete. The dynamic types
// of an allocator and an IsoSubspace are read in each order, from debug info opened afresh.
void analyzeDynamicTypesInEitherOrder(Snapshot& snapshot, Address fixtureAddress)
{
    auto fixture = snapshot.memory().ptr<HeapWalkFixture>(fixtureAddress);
    TEST_ASSERT(fixture, "the target's fixture reads");
    if (!fixture)
        return;
    struct Object {
        uint64_t address;
        uint64_t staticTypeFunction;
        ASCIILiteral dynamicType;
    };
    Object allocator { fixture->gigacageAllocator, fixture->allocator, "JSC::GigacageAlignedMemoryAllocator"_s };
    Object isoSubspace { fixture->isoSubspace, fixture->subspace, "JSC::IsoSubspace"_s };
    auto readInOrder = [&](const Object& first, const Object& second) {
        // Drops the modules the debug info opened before from liblldb's cache, with the types it read from them.
        lldb::SBDebugger::MemoryPressureDetected();
        Ref debugInfo = SnapshotDebugInfo::create(snapshot);
        for (const Object* object : { &first, &second }) {
            const TargetType* staticType = parameterClass(debugInfo.get(), Address { object->staticTypeFunction });
            if (!staticType)
                continue;
            TargetObject node = debugInfo->canonicalPointer(snapshot, Address { object->address }, *staticType).node;
            TEST_ASSERT_EQ(JSC::Corpse::name(node.type), String { object->dynamicType }, "an object has its dynamic type whichever compile units liblldb read first");
        }
    };
    readInOrder(allocator, isoSubspace);
    readInOrder(isoSubspace, allocator);
}

#if OS(DARWIN)

Vector<pid_t> webContentProcesses()
{
    Vector<pid_t> pids(std::max(proc_listallpids(nullptr, 0), 0));
    pids.shrink(std::max(proc_listallpids(pids.mutableSpan().data(), pids.sizeInBytes()), 0));
    pids.removeAllMatching([](pid_t pid) {
        if (pid <= 0)
            return true;
        UTF8CString path = JSC::Corpse::Process::create(pid)->executablePath();
        return path.isNull() || !std::string_view { path.legacyCStringPointer() }.contains("com.apple.WebKit.WebContent");
    });
    return pids;
}

Seconds cpuTime(pid_t pid)
{
    struct proc_taskinfo info;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info)) != sizeof(info))
        return 0_s;
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return Seconds::fromNanoseconds((info.pti_total_user + info.pti_total_system) * timebase.numer / timebase.denom);
}

// The web content process `miniBrowser` started that has used the most CPU time, once it has been idle for three seconds.
pid_t idleWebContentProcess(const Vector<pid_t>& startedBefore)
{
    static constexpr unsigned timeoutSeconds = 300;
    pid_t busiest = 0;
    Seconds busiestTime;
    unsigned idleSeconds = 0;
    for (unsigned waited = 0; waited < timeoutSeconds && idleSeconds < 3; ++waited) {
        sleep(1);
        pid_t previous = busiest;
        Seconds previousTime = busiestTime;
        busiest = 0;
        busiestTime = 0_s;
        for (pid_t pid : webContentProcesses()) {
            if (startedBefore.contains(pid) || cpuTime(pid) <= busiestTime)
                continue;
            busiest = pid;
            busiestTime = cpuTime(pid);
        }
        idleSeconds = busiest && busiest == previous && busiestTime - previousTime < 100_ms ? idleSeconds + 1 : 0;
    }
    return idleSeconds == 3 ? busiest : 0;
}

// Walks the heap of the web content process of a MiniBrowser, built beside this test, that has loaded `website`.
void walkWebsite()
{
    UTF8CString executable = JSC::Corpse::Process::create(getpid())->executablePath();
    TEST_ASSERT(!executable.isNull(), "this process's executable path is readable");
    if (executable.isNull())
        return;
    String directory = String::fromUTF8(executable.span());
    CString miniBrowser = makeString(directory.left(directory.reverseFind('/')), "/MiniBrowser.app/Contents/MacOS/MiniBrowser"_s).utf8();

    Vector<pid_t> startedBefore = webContentProcesses();
    char* const arguments[] = { const_cast<char*>(miniBrowser.legacyCStringPointer()), const_cast<char*>("--url"), const_cast<char*>(website), nullptr };
    pid_t browser = 0;
    bool launched = !posix_spawn(&browser, miniBrowser.legacyCStringPointer(), nullptr, nullptr, arguments, environ);
    TEST_ASSERT(launched, "the MiniBrowser beside this test launches");
    if (!launched)
        return;
    auto killBrowser = makeScopeExit([&] {
        killChild(browser);
    });

    pid_t webContent = idleWebContentProcess(startedBefore);
    TEST_ASSERT(webContent, "MiniBrowser's web content process loads the website and goes idle");
    auto snapshot = webContent ? takeSnapshot(webContent) : nullptr;
    if (!snapshot)
        return;

    // JSC::VMManager::s_recentVM and JSC::sanitizeStackForVM(JSC::VM&), which JavaScriptCore exports.
    Address recentVM = snapshot->symbol("_ZN3JSC9VMManager10s_recentVME");
    Address takesVM = snapshot->symbol("_ZN3JSC18sanitizeStackForVMERNS_2VME");
    auto vm = recentVM ? snapshot->memory().ptr<uint64_t>(recentVM) : JSC::Corpse::Memory::Ptr<uint64_t> { };
    TEST_ASSERT(vm && *vm && takesVM, "the web content process has a VM");
    if (!vm || !*vm || !takesVM)
        return;

    Ref debugInfo = SnapshotDebugInfo::create(*snapshot);
    const TargetType* vmType = parameterClass(debugInfo.get(), takesVM);
    if (!vmType)
        return;

    RecordingAnalyzer analyzer;
    analyzer.vmAddress = *vm;
    JSC::Corpse::walkHeap(*snapshot, debugInfo.get(), { Address { *vm }, *vmType }, analyzer);
    size_t definedByWebCore = 0;
    for (auto& types : analyzer.nodes.values()) {
        definedByWebCore += types.containsIf([](const TargetType& type) {
            return std::holds_alternative<TargetClass>(type) && std::get<TargetClass>(type).image.endsWith("/WebCore"_s);
        });
    }
    dataLogLn("    the walk reached ", analyzer.nodes.size(), " addresses through ", analyzer.edges.size(), " edges, ", definedByWebCore, " of them objects of classes WebCore defines; ", analyzer.needsReader.size(), " subobjects need readers");
    TEST_ASSERT(analyzer.reached(*vm, "JSC::VM"_s), "the walk reaches the VM");
    TEST_ASSERT(definedByWebCore, "the walk reaches objects of classes that WebCore defines");
}

#endif // OS(DARWIN)

} // anonymous namespace

void testHeapWalk()
{
    SuiteTracer tracer("HeapWalk");
    if (!tracer.shouldRun())
        return;

    analyzeInAndOutOfProcess(createFixture, analyzeDynamicTypesInEitherOrder);
    analyzeInAndOutOfProcess(createFixture, analyze);
}

void testHeapWalkWebsite()
{
    SuiteTracer tracer("HeapWalkWebsite");
    if (!tracer.shouldRun() || !website)
        return;

#if OS(DARWIN)
    walkWebsite();
#else
    skipSuite("HeapWalkWebsite", "launching a browser is implemented only on Darwin");
#endif
}

#endif // ENABLE(MYA_HEAP)

} // namespace JSCToolsTest
