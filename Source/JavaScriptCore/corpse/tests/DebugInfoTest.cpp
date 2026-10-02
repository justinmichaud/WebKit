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

#include <JavaScriptCore/CorpseAddress.h>
#include <JavaScriptCore/CorpseImage.h>
#include <JavaScriptCore/CorpseRemote.h>
#include <JavaScriptCore/CorpseSnapshot.h>
#include <JavaScriptCore/CorpseSnapshotDebugInfo.h>
#include <JavaScriptCore/CorpseTargetType.h>
#include <JavaScriptCore/CorpseTargetValue.h>
#include <JavaScriptCore/Watchpoint.h>
#include <lldb/API/LLDB.h>
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/SentinelLinkedList.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

#endif // HAVE(LLDB)

namespace JSCToolsTest {

#if HAVE(LLDB)

namespace {

using JSC::Corpse::Address;
using JSC::Corpse::Image;
using JSC::Corpse::Remote;
using JSC::Corpse::RemoteTraits;
using JSC::Corpse::Snapshot;
using JSC::Corpse::SnapshotDebugInfo;
using JSC::Corpse::TargetType;
using JSC::Corpse::TargetValue;

constexpr const char* sameImageString = "debug-info-target";
constexpr uint64_t crossImageMarker = 0x6d79612d74657374;

// Defined here and derived from a JavaScriptCore class, so its vtable and
// debug info are in this executable while its base's are in JavaScriptCore.
class CrossImageFireDetail final : public JSC::FireDetail {
public:
    ~CrossImageFireDetail() final = default;
    void dump(PrintStream&) const final { }

    uint64_t m_marker { crossImageMarker };
    bool m_flag : 1 { true };
    unsigned m_small : 5 { 21 };
    int m_signed : 4 { -3 };
};

// The first vtable slot of LaterDestructor is SlotZeroBase::first, which it
// does not override, so only the destructor names the class.
class SlotZeroBase {
public:
    virtual void first() { }
    virtual ~SlotZeroBase() = default;
};

class LaterDestructor final : public SlotZeroBase {
public:
    ~LaterDestructor() final = default;

    uint64_t m_marker { crossImageMarker };
};

// Diamond reaches VirtualBase through both Left and Right, and holds one VirtualBase.
class VirtualBase {
public:
    virtual ~VirtualBase() = default;

    uint64_t m_base { 1 };
};

class Left : public virtual VirtualBase {
public:
    uint64_t m_left { 2 };
};

class Right : public virtual VirtualBase {
public:
    uint64_t m_right { 3 };
};

class Diamond final : public Left, public Right {
public:
    ~Diamond() final = default;

    uint64_t m_diamond { 4 };
};

// The complete object at `object`, as its dynamic type.
std::optional<TargetValue> completeObjectAt(Snapshot& snapshot, Address object)
{
    RefPtr debugInfo = SnapshotDebugInfo::create(snapshot);
    TEST_ASSERT(debugInfo, "a snapshot has debug info");
    if (!debugInfo)
        return std::nullopt;
    auto value = TargetValue::completeObjectAt(snapshot, *debugInfo, object);
    TEST_ASSERT(value, "an object with a virtual destructor has a dynamic type");
    TEST_ASSERT(!value || value->address() == object, "an object held through its primary base starts where its complete object does");
    return value;
}

Address createSameImageObject()
{
    static JSC::StringFireDetail object(sameImageString);
    return Address { static_cast<JSC::FireDetail*>(&object) };
}

void analyzeSameImageObject(Snapshot& snapshot, Address object)
{
    auto value = completeObjectAt(snapshot, object);
    if (!value)
        return;
    TEST_ASSERT(std::string_view { value->type().name().legacyCStringPointer() } == "JSC::StringFireDetail",
        "the dynamic type is the class the object really is, not the one it is held as");
    TEST_ASSERT_EQ(value->type().byteSize(), sizeof(JSC::StringFireDetail), "the debug info gives the class its size");

    auto string = value->properField("m_string").as<uint64_t>();
    if (!string)
        return;
    std::string_view expected { sameImageString };
    auto characters = snapshot.memory().span<char>(Address { *string }.stripped(), expected.size() + 1);
    TEST_ASSERT(characters && (std::string_view { characters.data(), expected.size() } == expected) && !characters[expected.size()],
        "the field holds the target's string");
}

Address createCrossImageObject()
{
    static CrossImageFireDetail object;
    return Address { static_cast<JSC::FireDetail*>(&object) };
}

void analyzeCrossImageObject(Snapshot& snapshot, Address object)
{
    auto value = completeObjectAt(snapshot, object);
    if (!value)
        return;
    TEST_ASSERT(std::string_view { value->type().name().legacyCStringPointer() } == "JSCToolsTest::(anonymous namespace)::CrossImageFireDetail",
        "the dynamic type is the test's own class, in its anonymous namespace");
    TEST_ASSERT_EQ(value->type().byteSize(), sizeof(CrossImageFireDetail), "the debug info gives the class its size");
    auto marker = value->properField("m_marker").as<uint64_t>();
    TEST_ASSERT(marker && *marker == crossImageMarker, "the field holds the target's value");
    TEST_ASSERT(value->properField("m_flag").integer() == 1, "a one-bit field reads alone");
    TEST_ASSERT(value->properField("m_small").integer() == 21, "an unsigned bitfield reads without its neighbours");
    TEST_ASSERT(value->properField("m_signed").integer() == -3, "a signed bitfield is sign extended");
    ExpectedErrors expectedErrors;
    TEST_ASSERT(!value->properField("m_small").as<uint32_t>(), "a bitfield reads only as an integer");
}

Address createLaterDestructorObject()
{
    static LaterDestructor object;
    return Address { static_cast<SlotZeroBase*>(&object) };
}

void analyzeLaterDestructorObject(Snapshot& snapshot, Address object)
{
    auto value = completeObjectAt(snapshot, object);
    if (!value)
        return;
    TEST_ASSERT(std::string_view { value->type().name().legacyCStringPointer() } == "JSCToolsTest::(anonymous namespace)::LaterDestructor",
        "a class whose slot 0 is an inherited function is found by its destructor, not as its base");
    TEST_ASSERT_EQ(value->type().byteSize(), sizeof(LaterDestructor), "a class whose destructor is not its first virtual function is found by its destructor");
    auto marker = value->properField("m_marker").as<uint64_t>();
    TEST_ASSERT(marker && *marker == crossImageMarker, "the field holds the target's value");
}

Address createDiamondObject()
{
    static NeverDestroyed<Diamond> object;
    return Address { static_cast<Left*>(&object.get()) };
}

void analyzeDiamondObject(Snapshot& snapshot, Address object)
{
    auto value = completeObjectAt(snapshot, object);
    if (!value)
        return;
    HashMap<String, uint64_t> fields;
    unsigned visits = 0;
    value->forEachField([&](const TargetType::Field& field, const TargetValue& fieldValue) {
        ++visits;
        if (auto bits = fieldValue.as<uint64_t>())
            fields.set(String::fromUTF8(field.name.legacyCStringPointer()), *bits);
    });
    TEST_ASSERT_EQ(visits, 4u, "every field is visited once, the shared virtual base's included");
    TEST_ASSERT(fields.get("m_diamond"_s) == 4 && fields.get("m_left"_s) == 2 && fields.get("m_right"_s) == 3 && fields.get("m_base"_s) == 1,
        "every field is read where the complete object puts it");
}

Address createSystemLibraryObject()
{
    static NeverDestroyed<std::runtime_error> object("debug-info-system-library");
    return Address { static_cast<std::exception*>(&object.get()) };
}

void analyzeSystemLibraryObject(Snapshot& snapshot, Address object)
{
#if OS(DARWIN)
    // The OS ships no debug info for the libraries in the shared cache.
    RefPtr debugInfo = SnapshotDebugInfo::create(snapshot);
    TEST_ASSERT(debugInfo, "a snapshot has debug info");
    if (!debugInfo)
        return;
    ExpectedErrors expectedErrors;
    Address completeObject;
    TEST_ASSERT(!debugInfo->dynamicTypeAt(snapshot, object, completeObject), "a class in the shared cache has no dynamic type");
#else
    // From /usr/lib/debug/.build-id, where libstdc++6-<version>-dbg puts it.
    auto value = completeObjectAt(snapshot, object);
    TEST_ASSERT(value && value->type().byteSize() == sizeof(std::runtime_error),
        "a class in a system library resolves through that library's separate debug info");
#endif
}

// An object of a class this executable defines, and one of a class JavaScriptCore does.
struct ObjectsInBothImages {
    uint64_t crossImage;
    uint64_t sameImage;
};

Address createObjectsInBothImages()
{
    static ObjectsInBothImages objects { createCrossImageObject().toTargetVMAddress(), createSameImageObject().toTargetVMAddress() };
    return Address { &objects };
}

// The target's executable was replaced after it was loaded, so its file is not
// the image the target runs: its identity differs, and liblldb refuses it.
void analyzeObjectsInReplacedExecutable(Snapshot& snapshot, Address address)
{
    auto objects = snapshot.memory().ptr<ObjectsInBothImages>(address);
    TEST_ASSERT(objects, "the target's objects read");
    // liblldb keeps every module it opened, by identity, for the life of the
    // process, and would find the original executable among them. A rebuilt
    // image is one liblldb has not seen.
    lldb::SBDebugger::MemoryPressureDetected();
#if OS(LINUX)
    // The kernel marks the old file's mappings deleted, and the image is
    // reported and left out, so liblldb never reads the new file as it.
    RefPtr<SnapshotDebugInfo> debugInfo;
    {
        ExpectedErrors expectedErrors;
        debugInfo = SnapshotDebugInfo::create(snapshot);
    }
    TEST_ASSERT(std::ranges::none_of(snapshot.images(), [](const Image& image) {
        return std::string_view { image.path().legacyCStringPointer() }.find(".replaced-") != std::string_view::npos;
    }), "the replaced executable is not among the images");
#else
    // liblldb refuses the new file, whose UUID is not the image's.
    RefPtr debugInfo = SnapshotDebugInfo::create(snapshot);
#endif
    TEST_ASSERT(debugInfo, "a snapshot whose executable was replaced still has the debug info of its other images");
    if (!objects || !debugInfo)
        return;
    Address completeObject;
    {
        ExpectedErrors expectedErrors;
        TEST_ASSERT(!debugInfo->dynamicTypeAt(snapshot, Address { objects->crossImage }, completeObject), "a class of the replaced executable has no type");
    }
    const TargetType* type = debugInfo->dynamicTypeAt(snapshot, Address { objects->sameImage }, completeObject);
    TEST_ASSERT(type && std::string_view { type->name().legacyCStringPointer() } == "JSC::StringFireDetail", "a class of JavaScriptCore still has its type");
}

// No object: the analysis asks about addresses that hold none.
Address createNothing()
{
    return { };
}

void analyzeNullAndUnreadable(Snapshot& snapshot, Address)
{
    RefPtr debugInfo = SnapshotDebugInfo::create(snapshot);
    TEST_ASSERT(debugInfo, "a snapshot has debug info");
    if (!debugInfo)
        return;
    Address completeObject;
    ExpectedErrors expectedErrors(2);
    TEST_ASSERT(!debugInfo->dynamicTypeAt(snapshot, Address { }, completeObject), "a null address has no dynamic type");
    TEST_ASSERT(!debugInfo->dynamicTypeAt(snapshot, Address { static_cast<uint64_t>(0x10) }, completeObject), "unreadable memory has no dynamic type");
}

// What the container wrappers read. The class is polymorphic so that its vtable gives its type.
struct ContainerNode : public BasicRawSentinelNode<ContainerNode> {
    uint64_t value { 0 };
};
using ContainerList = SentinelLinkedList<ContainerNode, BasicRawSentinelNode<ContainerNode>>;

class Containers {
public:
    virtual ~Containers() = default;

    Vector<uint64_t> vector;
    HashSet<uint64_t> set;
    HashMap<uint64_t, ContainerNode*> map;
    ContainerList list;
    std::array<ContainerNode, 3> nodes;
    ContainerNode* const node { nullptr }; // Only for its type.
};

Address createContainers()
{
    static NeverDestroyed<Containers> object;
    Containers& containers = object.get();
    containers.vector = { 10, 20, 30 };
    // Removing half the values leaves deleted buckets among the full and empty ones.
    for (uint64_t value = 1; value <= 100; ++value)
        containers.set.add(value);
    for (uint64_t value = 2; value <= 100; value += 2)
        containers.set.remove(value);
    for (size_t index = 0; index < containers.nodes.size(); ++index) {
        containers.nodes[index].value = 7 + index;
        containers.list.append(&containers.nodes[index]);
    }
    // A deleted entry's value is destroyed, and, in an ENABLE(MYA_HEAP) build, cleared.
    for (uint64_t key = 1; key <= 20; ++key)
        containers.map.add(key, &containers.nodes[key % containers.nodes.size()]);
    for (uint64_t key = 2; key <= 20; key += 2)
        containers.map.remove(key);
    return Address { &containers };
}

void analyzeContainers(Snapshot& snapshot, Address object)
{
    auto value = completeObjectAt(snapshot, object);
    if (!value)
        return;

    Remote<Vector<uint64_t>> vector { value->properField("vector") };
    Vector<uint64_t> elements;
    auto size = RemoteTraits<Vector<uint64_t>>::size(vector);
    for (size_t index = 0; size && index < *size; ++index) {
        if (auto element = RemoteTraits<Vector<uint64_t>>::element(vector, index).as<uint64_t>())
            elements.append(*element);
    }
    TEST_ASSERT(elements == Vector<uint64_t>({ 10, 20, 30 }), "a Vector reads its elements");

    Vector<uint64_t> values;
    bool readable = RemoteTraits<HashSet<uint64_t>>::forEach(Remote<HashSet<uint64_t>> { value->properField("set") }, [&](const Remote<uint64_t>& bucket) {
        if (auto element = bucket.as<uint64_t>())
            values.append(*element);
        return IterationStatus::Continue;
    });
    std::ranges::sort(values);
    Vector<uint64_t> odd;
    for (uint64_t element = 1; element <= 100; element += 2)
        odd.append(element);
    TEST_ASSERT(readable && values == odd, "a HashSet reads its values, and skips its empty and deleted buckets");

    using Map = HashMap<uint64_t, ContainerNode*>;
    using Entry = Map::KeyValuePairType;
    Vector<std::pair<uint64_t, uint64_t>> entries;
    readable = RemoteTraits<Map>::forEach(Remote<Map> { value->properField("map") }, [&](const Remote<Entry>& bucket) {
        if (auto entry = bucket.as<Entry>())
            entries.append({ entry->key, std::bit_cast<uint64_t>(entry->value) });
        return IterationStatus::Continue;
    });
    std::ranges::sort(entries);
    Address nodeArray = value->properField("nodes").address();
    Vector<std::pair<uint64_t, uint64_t>> expectedEntries;
    for (uint64_t key = 1; key <= 20; key += 2)
        expectedEntries.append({ key, (nodeArray + (key % 3) * sizeof(ContainerNode)).toTargetVMAddress() });
    TEST_ASSERT(readable && entries == expectedEntries, "a HashMap reads its entries, and skips its empty and deleted buckets");

    // Every bucket, as the reach walk reads it: a deleted one holds its deleted key and zeros.
    auto buckets = hashTableBuckets(value->properField("map").properField("m_impl"));
    unsigned deleted = 0;
    unsigned stale = 0;
    for (unsigned index = 0; buckets && index < buckets->size; ++index) {
        auto entry = Remote<Entry*>(TargetValue { buckets->table }).dereference().offsetBy(index).as<Entry>();
        if (!entry || !HashTraits<uint64_t>::isDeletedValue(entry->key))
            continue;
        ++deleted;
        if (entry->value)
            ++stale;
    }
    TEST_ASSERT(deleted, "the HashMap has deleted buckets");
    TEST_ASSERT_EQ(stale, 0u, "a deleted HashMap bucket's value reads as zero");

    Remote<ContainerNode*> nodePointer { value->properField("node") };
    Vector<uint64_t> nodes;
    readable = RemoteTraits<ContainerList>::forEach(Remote<ContainerList> { value->properField("list") }, [&](const Remote<BasicRawSentinelNode<ContainerNode>>& node) {
        if (auto element = nodePointer.pointeeAt(node.address()).field<uint64_t>("value").as<uint64_t>())
            nodes.append(*element);
        return IterationStatus::Continue;
    });
    TEST_ASSERT(readable && nodes == Vector<uint64_t>({ 7, 8, 9 }), "a SentinelLinkedList reads its nodes in order");
}

} // anonymous namespace

void testDebugInfo()
{
    SuiteTracer tracer("DebugInfo");
    if (!tracer.shouldRun())
        return;

    analyzeInAndOutOfProcess(createSameImageObject, analyzeSameImageObject);
    analyzeInAndOutOfProcess(createCrossImageObject, analyzeCrossImageObject);
    analyzeInAndOutOfProcess(createLaterDestructorObject, analyzeLaterDestructorObject);
    analyzeInAndOutOfProcess(createDiamondObject, analyzeDiamondObject);
    analyzeInAndOutOfProcess(createSystemLibraryObject, analyzeSystemLibraryObject);
    analyzeInAndOutOfProcess(createContainers, analyzeContainers);

    // Everything is read from the snapshot, so the process may be gone.
    analyzeAfterTargetExits(createSameImageObject, analyzeSameImageObject);
    analyzeAfterTargetExits(createCrossImageObject, analyzeCrossImageObject);
    analyzeAfterTargetExits(createLaterDestructorObject, analyzeLaterDestructorObject);
    analyzeAfterTargetExits(createDiamondObject, analyzeDiamondObject);
    analyzeAfterTargetExits(createSystemLibraryObject, analyzeSystemLibraryObject);
    analyzeAfterTargetExits(createContainers, analyzeContainers);

    analyzeAfterTargetExits(createNothing, analyzeNullAndUnreadable);
    analyzeAfterExecutableReplacedAndTargetExits(createObjectsInBothImages, analyzeObjectsInReplacedExecutable);

    analyzeAfterExecutableReplaced(createObjectsInBothImages, analyzeObjectsInReplacedExecutable);
    analyzeInAndOutOfProcess(createNothing, analyzeNullAndUnreadable);
}

#else // No SB API, so there is nothing to ask.

void testDebugInfo()
{
    SuiteTracer tracer("DebugInfo");
    if (!tracer.shouldRun())
        return;

#if ENABLE(MYA_HEAP)
    TEST_ASSERT(false, "mya_heap is enabled but liblldb's headers were not found");
#else
    skipSuite("DebugInfo", "mya_heap is not enabled");
#endif
}

#endif // HAVE(LLDB)

} // namespace JSCToolsTest
