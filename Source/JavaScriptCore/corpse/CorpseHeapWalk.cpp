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
#include "CorpseHeapWalk.h"

#if ENABLE(MYA)

#include "CorpseError.h"
#include "CorpseLimits.h"
#include "CorpseRegion.h"
#include "CorpseThread.h"
#include <JavaScriptCore/BlockDirectoryBits.h>
#include <JavaScriptCore/CollectionScope.h>
#include <JavaScriptCore/FreeList.h>
#include <JavaScriptCore/GCSegmentedArray.h>
#include <JavaScriptCore/IndexingHeader.h>
#include <JavaScriptCore/IndexingType.h>
#include <JavaScriptCore/JITCode.h>
#include <JavaScriptCore/JSArrayBufferView.h>
#include <JavaScriptCore/JSCellButterfly.h>
#include <JavaScriptCore/JSString.h>
#include <JavaScriptCore/JSType.h>
#include <JavaScriptCore/MarkedBlock.h>
#include <JavaScriptCore/MarkedSpace.h>
#include <JavaScriptCore/PreciseAllocation.h>
#include <JavaScriptCore/StrongBlock.h>
#include <JavaScriptCore/StructureID.h>
#include <JavaScriptCore/WeakBlock.h>
#include <algorithm>
#include <bit>
#include <span>
#include <string_view>
#include <wtf/BitSet.h>
#include <wtf/CompactPtr.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/SegmentedVector.h>
#include <wtf/HexNumber.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/WTFConfig.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

#if OS(DARWIN)
#include <mach/mach.h>
#endif
#include <wtf/text/WTFString.h>

#if ENABLE(MYA_HEAP)
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#include <bmalloc/pas_enumerator.h>
#include <bmalloc/pas_root.h>
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif

namespace JSC {

class Heap;
class LocalAllocator;
class PreciseAllocation;

namespace Corpse {

static unsigned long long forReport(Address address)
{
    return address.toTargetVMAddress();
}

HeapWalk::HeapWalk(Snapshot& snapshot, Address rootsAddress)
    : m_snapshot(&snapshot)
{
    m_debugInfo = SnapshotDebugInfo::create(snapshot);
    if (!m_debugInfo)
        return;
    auto roots = TargetValue::completeObjectAt(snapshot, *m_debugInfo, rootsAddress);
    if (!roots)
        return;
    m_roots = *roots;
    Remote<VM> vm = Remote<VM*>(roots->properField("vm")).dereference();
    if (!vm) {
        CORPSE_REPORT("The roots at 0x%llx name no VM", forReport(rootsAddress));
        return;
    }
    auto atSafePoint = roots->properField("atSafePoint").integer();
    if (!atSafePoint)
        return;
    initialize(snapshot, WTF::move(vm), atSafePoint);
}

HeapWalk::HeapWalk(Snapshot& snapshot, Address vmAddress, Address javaScriptCoreImage)
    : m_snapshot(&snapshot)
{
    m_debugInfo = SnapshotDebugInfo::create(snapshot);
    if (!m_debugInfo)
        return;
    const TargetType* vmClass = m_debugInfo->classNamed(javaScriptCoreImage, "JSC::VM");
    if (!vmClass) {
        CORPSE_REPORT("The image at 0x%llx has no class 'JSC::VM'", forReport(javaScriptCoreImage));
        return;
    }
    initialize(snapshot, Remote<VM>(TargetValue::at(snapshot, vmAddress, *vmClass)), 0);
}

void HeapWalk::initialize(Snapshot&, Remote<VM>&& vm, std::optional<int64_t> atSafePoint)
{
    // StructureID::decode adds g_jscConfig.startOfStructureHeap. The Structure
    // of every Structure is the VM's structureStructure, so its StructureID
    // names itself, and its address less its ID is that start.
    Remote<Structure> structureStructure = vm.field<void>("structureStructure").base<void>(0).field<Structure*>("m_cell").dereference();
    Remote<JSCell> cell = structureStructure.base<JSCell>(0);
    auto bits = cell.field<void>("m_structureID").field<uint32_t>("m_bits").integer();
    if (!bits)
        return;
    if (*bits <= 0 || static_cast<uint64_t>(*bits) > structureStructure.address().toTargetVMAddress()) {
        CORPSE_REPORT("The structureStructure at 0x%llx has StructureID %lld", forReport(structureStructure.address()), static_cast<long long>(*bits));
        return;
    }
    m_structure = *structureStructure.typed();
    m_jsCell = *cell.typed();
    m_startOfStructureHeap = structureStructure.address().toTargetVMAddress() - static_cast<uint64_t>(*bits);
    m_structureIDOffset = cell.field<void>("m_structureID").field<uint32_t>("m_bits").address() - cell.address();
    Remote<const ClassInfo*> classInfoField = structureStructure.field<const ClassInfo*>("m_classInfo");
    if (!classInfoField)
        return;
    m_classInfoOffset = classInfoField.address() - structureStructure.address();
    Remote<uint8_t> inlineCapacityField = structureStructure.field<uint8_t>("m_inlineCapacity");
    if (!inlineCapacityField)
        return;
    m_inlineCapacityOffset = inlineCapacityField.address() - structureStructure.address();

    // Structure::s_info, which every Structure's ClassInfo is, is in JavaScriptCore's image.
    m_javaScriptCoreImage = structureStructure.field<const ClassInfo*>("m_classInfo").pointerValue().value_or(Address { });
    auto classNamed = [&](const char* name) -> const TargetType* {
        const TargetType* klass = m_javaScriptCoreImage ? m_debugInfo->classNamed(m_javaScriptCoreImage, name) : nullptr;
        if (!klass)
            CORPSE_REPORT("JavaScriptCore's image has no class '%s'", name);
        return klass;
    };
    m_blockHeaderClass = classNamed("JSC::MarkedBlock::Header");
    m_localAllocatorClass = classNamed("JSC::LocalAllocator");
    m_stringImplClass = classNamed("WTF::StringImpl");
    m_fatEntryClass = classNamed("JSC::SymbolTableEntry::FatEntry");
    m_wtfConfigClass = classNamed("WTF::Config");
    m_jsValueClass = classNamed("JSC::JSValue");
    m_structureIDClass = classNamed("JSC::StructureID");
    m_finalObjectClass = classNamed("JSC::JSFinalObject");
    m_preciseAllocationClass = classNamed("JSC::PreciseAllocation");
    m_ropeStringClass = classNamed("JSC::JSRopeString");
    m_cellButterflyClass = classNamed("JSC::JSCellButterfly");
    m_lexicalEnvironmentClass = classNamed("JSC::JSLexicalEnvironment");
    m_stringImplOwnerClass = classNamed("JSC::JSString");
    m_watchpointSetClass = classNamed("JSC::WatchpointSet");
    m_propertyTableEntryClass = classNamed("JSC::PropertyTableEntry");
    m_compactPropertyTableEntryClass = classNamed("JSC::CompactPropertyTableEntry");
    m_weakImplClass = classNamed("JSC::WeakImpl");
    m_expressionInfoChapterClass = classNamed("JSC::ExpressionInfo::Chapter");
    m_expressionInfoEncodedInfoClass = classNamed("JSC::ExpressionInfo::EncodedInfo");
    // The pointee of WTF::StringImpl's m_data8.
    if (auto* stringImpl = m_stringImplClass ? std::get_if<TargetType::Class>(&m_stringImplClass->layout()) : nullptr) {
        for (const TargetType::Base& base : stringImpl->bases) {
            auto* shape = std::get_if<TargetType::Class>(&base.type.layout());
            for (const TargetType::Field& field : shape ? shape->properFields : Vector<TargetType::Field> { }) {
                auto* members = std::get_if<TargetType::Class>(&field.type.layout());
                for (const TargetType::Field& member : members && field.name.isEmpty() ? members->properFields : Vector<TargetType::Field> { }) {
                    auto* pointer = std::get_if<TargetType::Pointer>(&member.type.layout());
                    if (pointer && std::string_view { member.name.legacyCStringPointer() } == "m_data8")
                        m_byteType = &pointer->pointee;
                }
            }
        }
    }
    m_jscConfigClass = classNamed("JSC::Config");
    m_config = m_javaScriptCoreImage ? m_debugInfo->symbolAddress(m_javaScriptCoreImage, "g_config") : std::nullopt;
    if (!m_config)
        CORPSE_REPORT("JavaScriptCore's image has no symbol 'g_config'");
#if ENABLE(JIT)
    m_baselineJITDataClass = classNamed("JSC::BaselineJITData");
#endif
#if ENABLE(DFG_JIT)
    m_dfgJITDataClass = classNamed("JSC::DFG::JITData");
#endif
    if (!m_blockHeaderClass || !m_localAllocatorClass || !m_stringImplClass || !m_fatEntryClass || !m_wtfConfigClass || !m_jscConfigClass || !m_jsValueClass || !m_finalObjectClass || !m_preciseAllocationClass || !m_ropeStringClass || !m_cellButterflyClass || !m_lexicalEnvironmentClass || !m_stringImplOwnerClass || !m_watchpointSetClass || !m_propertyTableEntryClass || !m_compactPropertyTableEntryClass || !m_weakImplClass
        || !m_expressionInfoChapterClass || !m_expressionInfoEncodedInfoClass || !m_byteType || !m_config || !atSafePoint)
        return;
    m_isAtSafePoint = *atSafePoint;
    m_vm = WTF::move(vm);
}

// What MarkedBlock::Handle::isLive reads from the MarkedSpace.
struct MarkedSpaceState {
    HeapVersion markingVersion;
    HeapVersion newlyAllocatedVersion;
};

namespace {

using AtomBits = HeapWalk::AtomBits;

// CSSValue::visitDerived.
static constexpr std::array cssValueSubclasses {
    HeapWalk::TypeFieldSubclass { "AppleColorFilter", "WebCore::CSSAppleColorFilterValue", false },
    HeapWalk::TypeFieldSubclass { "Attr", "WebCore::CSSAttrValue", false },
    HeapWalk::TypeFieldSubclass { "BackgroundRepeat", "WebCore::CSSBackgroundRepeatValue", false },
    HeapWalk::TypeFieldSubclass { "BasicShape", "WebCore::CSSBasicShapeValue", false },
    HeapWalk::TypeFieldSubclass { "BorderImageOutset", "WebCore::CSSBorderImageOutsetValue", false },
    HeapWalk::TypeFieldSubclass { "BorderImageRepeat", "WebCore::CSSBorderImageRepeatValue", false },
    HeapWalk::TypeFieldSubclass { "BorderImageSlice", "WebCore::CSSBorderImageSliceValue", false },
    HeapWalk::TypeFieldSubclass { "BorderImageSource", "WebCore::CSSBorderImageSourceValue", false },
    HeapWalk::TypeFieldSubclass { "BorderImageWidth", "WebCore::CSSBorderImageWidthValue", false },
    HeapWalk::TypeFieldSubclass { "BoxShadowProperty", "WebCore::CSSBoxShadowPropertyValue", false },
    HeapWalk::TypeFieldSubclass { "Canvas", "WebCore::CSSCanvasValue", false },
    HeapWalk::TypeFieldSubclass { "CalcSize", "WebCore::CSSCalcSizeValue", false },
    HeapWalk::TypeFieldSubclass { "Clip", "WebCore::CSSClipValue", false },
    HeapWalk::TypeFieldSubclass { "Color", "WebCore::CSSColorValue", false },
    HeapWalk::TypeFieldSubclass { "ColorScheme", "WebCore::CSSColorSchemeValue", true },
    HeapWalk::TypeFieldSubclass { "Content", "WebCore::CSSContentValue", false },
    HeapWalk::TypeFieldSubclass { "Crossfade", "WebCore::CSSCrossfadeValue", false },
    HeapWalk::TypeFieldSubclass { "CursorImage", "WebCore::CSSCursorImageValue", false },
    HeapWalk::TypeFieldSubclass { "CustomIdent", "WebCore::CSSCustomIdentValue", false },
    HeapWalk::TypeFieldSubclass { "CustomProperty", "WebCore::CSSCustomPropertyValue", false },
    HeapWalk::TypeFieldSubclass { "DynamicRangeLimit", "WebCore::CSSDynamicRangeLimitValue", false },
    HeapWalk::TypeFieldSubclass { "EasingFunction", "WebCore::CSSEasingFunctionValue", false },
    HeapWalk::TypeFieldSubclass { "FilterImage", "WebCore::CSSFilterImageValue", false },
    HeapWalk::TypeFieldSubclass { "Filter", "WebCore::CSSFilterValue", false },
    HeapWalk::TypeFieldSubclass { "FlexWrap", "WebCore::CSSFlexWrapValue", false },
    HeapWalk::TypeFieldSubclass { "Font", "WebCore::CSSFontValue", false },
    HeapWalk::TypeFieldSubclass { "FontFaceSrcLocal", "WebCore::CSSFontFaceSrcLocalValue", false },
    HeapWalk::TypeFieldSubclass { "FontFaceSrcResource", "WebCore::CSSFontFaceSrcResourceValue", false },
    HeapWalk::TypeFieldSubclass { "FontFamilyName", "WebCore::CSSFontFamilyNameValue", false },
    HeapWalk::TypeFieldSubclass { "FontFeature", "WebCore::CSSFontFeatureValue", false },
    HeapWalk::TypeFieldSubclass { "FontPalette", "WebCore::CSSFontPaletteValue", false },
    HeapWalk::TypeFieldSubclass { "FontStyleWithAngle", "WebCore::CSSFontStyleWithAngleValue", false },
    HeapWalk::TypeFieldSubclass { "FontStyleRange", "WebCore::CSSFontStyleRangeValue", false },
    HeapWalk::TypeFieldSubclass { "FontVariation", "WebCore::CSSFontVariationValue", false },
    HeapWalk::TypeFieldSubclass { "Function", "WebCore::CSSFunctionValue", false },
    HeapWalk::TypeFieldSubclass { "Gradient", "WebCore::CSSGradientValue", false },
    HeapWalk::TypeFieldSubclass { "GridAutoFlow", "WebCore::CSSGridAutoFlowValue", false },
    HeapWalk::TypeFieldSubclass { "GridLineValue", "WebCore::CSSGridLineValue", false },
    HeapWalk::TypeFieldSubclass { "GridTemplateAreas", "WebCore::CSSGridTemplateAreasValue", false },
    HeapWalk::TypeFieldSubclass { "GridTemplateList", "WebCore::CSSGridTemplateListValue", false },
    HeapWalk::TypeFieldSubclass { "GridTrackSizes", "WebCore::CSSGridTrackSizesValue", false },
    HeapWalk::TypeFieldSubclass { "Keyword", "WebCore::CSSKeywordValue", false },
    HeapWalk::TypeFieldSubclass { "Image", "WebCore::CSSImageValue", false },
    HeapWalk::TypeFieldSubclass { "ImageSetOption", "WebCore::CSSImageSetOptionValue", false },
    HeapWalk::TypeFieldSubclass { "ImageSet", "WebCore::CSSImageSetValue", false },
    HeapWalk::TypeFieldSubclass { "MaskBorderOutset", "WebCore::CSSMaskBorderOutsetValue", false },
    HeapWalk::TypeFieldSubclass { "MaskBorderRepeat", "WebCore::CSSMaskBorderRepeatValue", false },
    HeapWalk::TypeFieldSubclass { "MaskBorderSlice", "WebCore::CSSMaskBorderSliceValue", false },
    HeapWalk::TypeFieldSubclass { "MaskBorderSource", "WebCore::CSSMaskBorderSourceValue", false },
    HeapWalk::TypeFieldSubclass { "MaskBorderWidth", "WebCore::CSSMaskBorderWidthValue", false },
    HeapWalk::TypeFieldSubclass { "ColorImage", "WebCore::CSSColorImageValue", false },
    HeapWalk::TypeFieldSubclass { "LightDarkImage", "WebCore::CSSLightDarkImageValue", false },
    HeapWalk::TypeFieldSubclass { "NamedImage", "WebCore::CSSNamedImageValue", false },
    HeapWalk::TypeFieldSubclass { "OffsetRotate", "WebCore::CSSOffsetRotateValue", false },
    HeapWalk::TypeFieldSubclass { "PaintImage", "WebCore::CSSPaintImageValue", false },
    HeapWalk::TypeFieldSubclass { "Param", "WebCore::CSSParamValue", false },
    HeapWalk::TypeFieldSubclass { "Path", "WebCore::CSSPathValue", false },
    HeapWalk::TypeFieldSubclass { "ShorthandSubstitution", "WebCore::CSSShorthandSubstitutionValue", false },
    HeapWalk::TypeFieldSubclass { "PinnedAnchorName", "WebCore::CSSPinnedAnchorNameValue", true },
    HeapWalk::TypeFieldSubclass { "Position", "WebCore::CSSPositionValue", false },
    HeapWalk::TypeFieldSubclass { "PositionX", "WebCore::CSSPositionXValue", false },
    HeapWalk::TypeFieldSubclass { "PositionY", "WebCore::CSSPositionYValue", false },
    HeapWalk::TypeFieldSubclass { "Primitive", "WebCore::CSSPrimitiveValue", false },
    HeapWalk::TypeFieldSubclass { "Quotes", "WebCore::CSSQuotesValue", false },
    HeapWalk::TypeFieldSubclass { "Ratio", "WebCore::CSSRatioValue", false },
    HeapWalk::TypeFieldSubclass { "Ray", "WebCore::CSSRayValue", false },
    HeapWalk::TypeFieldSubclass { "Scroll", "WebCore::CSSScrollValue", false },
    HeapWalk::TypeFieldSubclass { "String", "WebCore::CSSStringValue", false },
    HeapWalk::TypeFieldSubclass { "TextShadowProperty", "WebCore::CSSTextShadowPropertyValue", false },
    HeapWalk::TypeFieldSubclass { "TransformList", "WebCore::CSSTransformListValue", false },
    HeapWalk::TypeFieldSubclass { "URL", "WebCore::CSSURLValue", false },
    HeapWalk::TypeFieldSubclass { "UnicodeRange", "WebCore::CSSUnicodeRangeValue", false },
    HeapWalk::TypeFieldSubclass { "ValueList", "WebCore::CSSValueList", false },
    HeapWalk::TypeFieldSubclass { "ValuePair", "WebCore::CSSValuePair", false },
    HeapWalk::TypeFieldSubclass { "Substitution", "WebCore::CSSSubstitutionValue", false },
    HeapWalk::TypeFieldSubclass { "SymbolsFunction", "WebCore::CSSSymbolsFunctionValue", false },
    HeapWalk::TypeFieldSubclass { "View", "WebCore::CSSViewValue", false },
    HeapWalk::TypeFieldSubclass { "WebkitBoxReflect", "WebCore::CSSWebkitBoxReflectValue", false },
};

// StyleRuleBase::visitDerived.
static constexpr std::array styleRuleSubclasses {
    HeapWalk::TypeFieldSubclass { "Style", "WebCore::StyleRule", false },
    HeapWalk::TypeFieldSubclass { "StyleWithNesting", "WebCore::StyleRuleWithNesting", false },
    HeapWalk::TypeFieldSubclass { "NestedDeclarations", "WebCore::StyleRuleNestedDeclarations", false },
    HeapWalk::TypeFieldSubclass { "Page", "WebCore::StyleRulePage", false },
    HeapWalk::TypeFieldSubclass { "FontFace", "WebCore::StyleRuleFontFace", false },
    HeapWalk::TypeFieldSubclass { "FontFeatureValues", "WebCore::StyleRuleFontFeatureValues", false },
    HeapWalk::TypeFieldSubclass { "FontFeatureValuesBlock", "WebCore::StyleRuleFontFeatureValuesBlock", false },
    HeapWalk::TypeFieldSubclass { "FontPaletteValues", "WebCore::StyleRuleFontPaletteValues", false },
    HeapWalk::TypeFieldSubclass { "Media", "WebCore::StyleRuleMedia", false },
    HeapWalk::TypeFieldSubclass { "Supports", "WebCore::StyleRuleSupports", false },
    HeapWalk::TypeFieldSubclass { "Import", "WebCore::StyleRuleImport", false },
    HeapWalk::TypeFieldSubclass { "Keyframes", "WebCore::StyleRuleKeyframes", false },
    HeapWalk::TypeFieldSubclass { "Namespace", "WebCore::StyleRuleNamespace", false },
    HeapWalk::TypeFieldSubclass { "Keyframe", "WebCore::StyleRuleKeyframe", false },
    HeapWalk::TypeFieldSubclass { "Charset", "WebCore::StyleRuleCharset", false },
    HeapWalk::TypeFieldSubclass { "CounterStyle", "WebCore::StyleRuleCounterStyle", false },
    HeapWalk::TypeFieldSubclass { "LayerBlock", "WebCore::StyleRuleLayer", false },
    HeapWalk::TypeFieldSubclass { "LayerStatement", "WebCore::StyleRuleLayer", false },
    HeapWalk::TypeFieldSubclass { "Container", "WebCore::StyleRuleContainer", false },
    HeapWalk::TypeFieldSubclass { "Property", "WebCore::StyleRuleProperty", false },
    HeapWalk::TypeFieldSubclass { "Scope", "WebCore::StyleRuleScope", false },
    HeapWalk::TypeFieldSubclass { "StartingStyle", "WebCore::StyleRuleStartingStyle", false },
    HeapWalk::TypeFieldSubclass { "ViewTransition", "WebCore::StyleRuleViewTransition", false },
    HeapWalk::TypeFieldSubclass { "PositionTry", "WebCore::StyleRulePositionTry", false },
    HeapWalk::TypeFieldSubclass { "Function", "WebCore::StyleRuleFunction", false },
    HeapWalk::TypeFieldSubclass { "FunctionDeclarations", "WebCore::StyleRuleFunctionDeclarations", false },
    HeapWalk::TypeFieldSubclass { "EnvironmentMap", "WebCore::StyleRuleEnvironmentMap", true },
    HeapWalk::TypeFieldSubclass { "Margin", nullptr, false },
};

// NodeRareData::isElementRareData.
static constexpr std::array nodeRareDataSubclasses {
    HeapWalk::TypeFieldSubclass { "false", "WebCore::NodeRareData", false },
    HeapWalk::TypeFieldSubclass { "true", "WebCore::ElementRareData", false },
};

// StyleProperties::isMutable.
static constexpr std::array stylePropertiesSubclasses {
    HeapWalk::TypeFieldSubclass { "false", "WebCore::ImmutableStyleProperties", false },
    HeapWalk::TypeFieldSubclass { "true", "WebCore::MutableStyleProperties", false },
};

static constexpr std::array typeFieldHierarchyTable {
    HeapWalk::TypeFieldHierarchy { "WebCore::CSSValue", "m_classType", "WebCore::CSSValue::ClassType", cssValueSubclasses },
    HeapWalk::TypeFieldHierarchy { "WebCore::StyleRuleBase", "m_type", "WebCore::StyleRuleType", styleRuleSubclasses },
    HeapWalk::TypeFieldHierarchy { "WebCore::NodeRareData", "m_isElementRareData", nullptr, nodeRareDataSubclasses },
    HeapWalk::TypeFieldHierarchy { "WebCore::StyleProperties", "m_isMutable", nullptr, stylePropertiesSubclasses },
};

// What MarkedBlock::Handle::isLive reads from a block and its header.
struct BlockState {
    bool isAllocated;
    HeapVersion markingVersion;
    HeapVersion newlyAllocatedVersion;
    AtomBits marks;
    AtomBits newlyAllocated;
};

// The locked path of MarkedBlock::Handle::isLive, with isMarking false.
// Nothing in a corpse runs concurrently, so the optimistic path it tries first
// gives the same answer.
bool isLive(const MarkedSpaceState& space, const BlockState& block, size_t atom)
{
    if (block.isAllocated)
        return true;

    if (block.newlyAllocatedVersion == space.newlyAllocatedVersion)
        return block.newlyAllocated.get(atom);

    if (block.markingVersion != space.markingVersion)
        return false;

    return block.marks.get(atom);
}

// BlockDirectory::is<Kind>(index): the `kind` word of segment index / bitsPerSegment.
std::optional<bool> directoryBit(const Remote<BlockDirectory>& directory, BlockDirectoryBits::Kind kind, size_t index)
{
    using Segments = Vector<BlockDirectoryBits::Segment>;
    Remote<Segments> segments = directory.field<void>("m_bits").field<Segments>("m_segments");
    auto count = RemoteTraits<Segments>::size(segments);
    if (!count)
        return std::nullopt;
    size_t segmentIndex = index >> BlockDirectoryBits::segmentShift;
    if (segmentIndex >= *count) {
        CORPSE_REPORT("Block %zu is past the %zu bit segments of the BlockDirectory at 0x%llx", index, *count, forReport(directory.address()));
        return std::nullopt;
    }
    Remote<BlockDirectoryBits::Segment> segment = RemoteTraits<Segments>::element(segments, segmentIndex);
    if (!segment)
        return std::nullopt;
    if (segment.type()->byteSize() != sizeof(BlockDirectoryBits::Segment)) {
        CORPSE_REPORT("A BlockDirectory bit segment is %zu bytes, not the %zu this build uses", segment.type()->byteSize(), sizeof(BlockDirectoryBits::Segment));
        return std::nullopt;
    }
    auto words = segment.as<BlockDirectoryBits::Segment>();
    if (!words)
        return std::nullopt;
    return (words->m_data[static_cast<unsigned>(kind)] >> (index & BlockDirectoryBits::indexMask)) & 1;
}

// The MarkedBlock::Handle fields the walk reads.
struct HandleState {
    Address block;
    size_t atomsPerCell;
    size_t startAtom;
    HeapCell::Kind kind;
    size_t index;
    bool isFreeListed;
};

std::optional<HandleState> readHandle(const Remote<MarkedBlock::Handle>& handle)
{
    auto atomsPerCell = handle.field<unsigned>("m_atomsPerCell").integer();
    auto startAtom = handle.field<unsigned>("m_startAtom").integer();
    auto kind = handle.field<void>("m_attributes").field<HeapCell::Kind>("cellKind").integer();
    auto index = handle.field<unsigned>("m_index").integer();
    auto isFreeListed = handle.field<bool>("m_isFreeListed").integer();
    Address block = handle.field<MarkedBlock*>("m_block").dereference().address();
    if (!atomsPerCell || !startAtom || !kind || !index || !isFreeListed)
        return std::nullopt;
    // Enough to keep a corrupted handle from looping forever or leaving its block.
    if (*atomsPerCell <= 0 || *startAtom < 0 || static_cast<size_t>(*startAtom) >= MarkedBlock::endAtom
        || !block || block.toTargetVMAddress() & (MarkedBlock::blockSize - 1)) {
        CORPSE_REPORT("The block handle at 0x%llx describes no block", forReport(handle.address()));
        return std::nullopt;
    }
    return HandleState { block, static_cast<size_t>(*atomsPerCell), static_cast<size_t>(*startAtom), static_cast<HeapCell::Kind>(*kind), static_cast<size_t>(*index), !!*isFreeListed };
}

// FreeList::forEachInterval.
bool forEachFreeInterval(const TargetValue& freeList, const Function<bool(Address start, Address end)>& functor)
{
    auto intervalStart = freeList.properField("m_intervalStart").pointerValue();
    auto intervalEnd = freeList.properField("m_intervalEnd").pointerValue();
    TargetValue nextInterval = freeList.properField("m_nextInterval");
    auto cell = nextInterval.pointerValue();
    auto secret = freeList.properField("m_secret").as<uint64_t>();
    if (!intervalStart || !intervalEnd || !cell || !secret)
        return false;

    Address start = *intervalStart;
    Address end = *intervalEnd;
    for (size_t count = 0; count <= MarkedBlock::atomsPerBlock; ++count) {
        if (start < end && !functor(start, end))
            return false;

        if (FreeList::isSentinel(std::bit_cast<FreeCell*>(static_cast<uintptr_t>(cell->toTargetVMAddress()))))
            return true;

        // FreeCell::advance.
        auto scrambledBits = nextInterval.pointeeAt(*cell).properField("scrambledBits").as<uint64_t>();
        if (!scrambledBits)
            return false;
        auto [offsetToNext, lengthInBytes] = FreeCell::descramble(*scrambledBits, *secret);
        start = *cell;
        end = start + lengthInBytes;
        cell = Address { cell->toTargetVMAddress() + static_cast<int64_t>(offsetToNext) };
    }
    CORPSE_REPORT("The free list at 0x%llx has more intervals than a block has atoms", forReport(freeList.address()));
    return false;
}

// MarkedBlock::Handle::stopAllocating: every cell but those on the free list is newly allocated.
std::optional<AtomBits> stopAllocatingBlock(const HandleState& handle, const TargetValue& freeList)
{
    AtomBits newlyAllocated;
    newlyAllocated.clearAll();
    newlyAllocated.setEachNthBit(handle.atomsPerCell, handle.startAtom, MarkedBlock::endAtom);

    Address blockEnd = handle.block + MarkedBlock::blockSize;
    bool readable = forEachFreeInterval(freeList, [&](Address start, Address end) {
        if (start < handle.block || end > blockEnd) {
            CORPSE_REPORT("A free interval of the block at 0x%llx is outside it", forReport(handle.block));
            return false;
        }
        // MarkedBlock::candidateAtomNumber.
        newlyAllocated.clearEachNthBit(handle.atomsPerCell, (start - handle.block) / MarkedBlock::atomSize, (end - handle.block) / MarkedBlock::atomSize);
        return true;
    });
    if (!readable)
        return std::nullopt;
    return newlyAllocated;
}

} // anonymous namespace

// MarkedSpace::forEachLiveCell, inside a HeapIterationScope, whose
// MarkedSpace::willStartIterating stops every allocator first. It has no
// liveness rules for a collection in progress, where isMarking would be true
// and marksConveyLivenessDuringMarking would decide: at mya's safe point there
// is none, and on any other snapshot the result is unverified.
void HeapWalk::forEachLiveCell(const Function<IterationStatus(const Cell&)>& functor) const
{
    if (!isValid())
        return;
    Remote<Heap> heap = m_vm.field<Heap>("heap");
    Remote<MarkedSpace> space = heap.field<MarkedSpace>("m_objectSpace");
    auto markingVersion = space.field<HeapVersion>("m_markingVersion").as<HeapVersion>();
    auto newlyAllocatedVersion = space.field<HeapVersion>("m_newlyAllocatedVersion").as<HeapVersion>();
    if (!markingVersion || !newlyAllocatedVersion)
        return;
    MarkedSpaceState state { *markingVersion, *newlyAllocatedVersion };

    auto newlyAllocatedAfterStop = stopAllocating(space);
    if (!newlyAllocatedAfterStop)
        return;

    IterationStatus result = IterationStatus::Continue;
    forEachBlock([&](Address block) {
        result = walkBlock(block, *newlyAllocatedAfterStop, state, functor);
        return result;
    });
    if (result == IterationStatus::Done)
        return;
    walkPreciseAllocations(space, functor);
}

// The loop over m_blocks.set() in MarkedSpace::forEachLiveCell. False, and reported, if the set cannot be read.
bool HeapWalk::forEachBlock(const Function<IterationStatus(Address block)>& functor) const
{
    Remote<MarkedSpace> space = m_vm.field<Heap>("heap").field<MarkedSpace>("m_objectSpace");
    return RemoteTraits<BlockSet>::forEach(space.field<void>("m_blocks").field<BlockSet>("m_set"), [&](const Remote<MarkedBlock*>& block) {
        auto address = block.pointerValue();
        if (!address || !*address)
            return IterationStatus::Continue;
        return functor(*address);
    });
}

// MarkedBlock::header().
Remote<MarkedBlock::Header> HeapWalk::header(Address block) const
{
    return Remote<MarkedBlock::Header>(TargetValue::at(snapshot(), block + MarkedBlock::headerAtom * MarkedBlock::atomSize, *m_blockHeaderClass));
}

// MarkedSpace::stopAllocating: BlockDirectory::stopAllocating for each
// directory, which runs LocalAllocator::stopAllocating for each allocator. The
// result is the newly allocated bits each free-listed block ends up with.
std::optional<HashMap<uint64_t, HeapWalk::AtomBits>> HeapWalk::stopAllocating(const Remote<MarkedSpace>& space) const
{
    using Allocators = SentinelLinkedList<LocalAllocator, BasicRawSentinelNode<LocalAllocator>>;
    HashMap<uint64_t, AtomBits> result;
    bool failed = false;

    // MarkedSpace::forEachDirectory.
    Remote<BlockDirectory> directory = space.field<void>("m_directories").field<BlockDirectory*>("m_first").dereference();
    for (unsigned count = 0; directory; ++count) {
        if (count >= maxBlockDirectories) {
            CORPSE_REPORT("The MarkedSpace at 0x%llx lists more than %u BlockDirectories", forReport(space.address()), maxBlockDirectories);
            return std::nullopt;
        }
        bool readable = RemoteTraits<Allocators>::forEach(directory.field<Allocators>("m_localAllocators"), [&](const Remote<BasicRawSentinelNode<LocalAllocator>>& node) {
            // A LocalAllocator's list node is its first base.
            Remote<LocalAllocator> allocator { TargetValue::at(snapshot(), node.address(), *m_localAllocatorClass) };
            if (allocator.base<void>(0).address() != node.address()) {
                CORPSE_REPORT("The LocalAllocator at 0x%llx is not at its list node", forReport(node.address()));
                failed = true;
                return IterationStatus::Done;
            }

            // LocalAllocator::stopAllocating.
            Remote<MarkedBlock::Handle> currentBlock = allocator.field<MarkedBlock::Handle*>("m_currentBlock").dereference();
            if (!currentBlock)
                return IterationStatus::Continue;
            auto handle = readHandle(currentBlock);
            Remote<FreeList> freeList = allocator.field<FreeList>("m_freeList");
            if (!handle || !freeList) {
                failed = true;
                return IterationStatus::Done;
            }
            // MarkedBlock::Handle::stopAllocating returns early for a block that is not free-listed.
            if (!handle->isFreeListed)
                return IterationStatus::Continue;
            auto newlyAllocated = stopAllocatingBlock(*handle, *freeList.typed());
            if (!newlyAllocated) {
                failed = true;
                return IterationStatus::Done;
            }
            result.add(currentBlock.address().toTargetVMAddress(), *newlyAllocated);
            return IterationStatus::Continue;
        });
        if (!readable || failed)
            return std::nullopt;
        directory = directory.field<BlockDirectory*>("m_nextDirectory").dereference();
    }
    return result;
}

// The body of the loop over m_blocks in MarkedSpace::forEachLiveCell:
// MarkedBlock::handle(), then MarkedBlock::Handle::forEachLiveCell.
IterationStatus HeapWalk::walkBlock(Address blockAddress, const HashMap<uint64_t, AtomBits>& newlyAllocatedAfterStop, const MarkedSpaceState& space, const Function<IterationStatus(const Cell&)>& functor) const
{
    Remote<MarkedBlock::Header> header = this->header(blockAddress);
    Remote<MarkedBlock::Handle> handleValue = header.field<MarkedBlock::Handle*>("m_handle").dereference();
    auto handle = readHandle(handleValue);
    if (!handle)
        return IterationStatus::Continue;
    if (handle->block != blockAddress) {
        CORPSE_REPORT("The handle of the block at 0x%llx is the handle of 0x%llx", forReport(blockAddress), forReport(handle->block));
        return IterationStatus::Continue;
    }

    auto markingVersion = header.field<HeapVersion>("m_markingVersion").as<HeapVersion>();
    auto newlyAllocatedVersion = header.field<HeapVersion>("m_newlyAllocatedVersion").as<HeapVersion>();
    auto marks = header.field<AtomBits>("m_marks").as<AtomBits>();
    auto newlyAllocated = header.field<AtomBits>("m_newlyAllocated").as<AtomBits>();
    // MarkedBlock::Handle::isAllocated.
    auto isAllocated = directoryBit(handleValue.field<BlockDirectory*>("m_directory").dereference(), BlockDirectoryBits::Kind::Allocated, handle->index);
    if (!markingVersion || !newlyAllocatedVersion || !marks || !newlyAllocated || !isAllocated)
        return IterationStatus::Continue;

    BlockState block { *isAllocated, *markingVersion, *newlyAllocatedVersion, *marks, *newlyAllocated };
    if (auto stopped = newlyAllocatedAfterStop.getOptional(handleValue.address().toTargetVMAddress())) {
        block.newlyAllocated = *stopped;
        block.newlyAllocatedVersion = space.newlyAllocatedVersion;
    } else if (handle->isFreeListed) {
        // MarkedBlock::Handle::isLive asserts this never happens after stopAllocating.
        CORPSE_REPORT("The free-listed block at 0x%llx has no allocator", forReport(handle->block));
        return IterationStatus::Continue;
    }

    // MarkedBlock::Handle::forEachLiveCell.
    for (size_t atom = handle->startAtom; atom < MarkedBlock::endAtom; atom += handle->atomsPerCell) {
        if (!isLive(space, block, atom))
            continue;
        // MarkedBlock::Handle::cellSize.
        Cell cell { handle->block + atom * MarkedBlock::atomSize, handle->atomsPerCell * MarkedBlock::atomSize, handle->kind };
        if (functor(cell) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

auto HeapWalk::preciseAllocations() const -> Vector<PreciseAllocationState>
{
    Vector<PreciseAllocationState> result;
    if (!isValid())
        return result;
    using Allocations = Vector<PreciseAllocation*>;
    Remote<Allocations> allocations = m_vm.field<Heap>("heap").field<MarkedSpace>("m_objectSpace").field<Allocations>("m_preciseAllocations");
    auto size = RemoteTraits<Allocations>::size(allocations);
    for (size_t index = 0; size && index < *size; ++index) {
        Remote<PreciseAllocation> allocation = RemoteTraits<Allocations>::element(allocations, index).dereference();
        auto isMarked = allocation.field<void>("m_isMarked").as<bool>();
        auto isNewlyAllocated = allocation.field<bool>("m_isNewlyAllocated").integer();
        auto cellSize = allocation.field<void>("m_cellSize").integer();
        if (!allocation || !isMarked || !isNewlyAllocated || !cellSize)
            continue;
        size_t headerSize = roundUpToMultipleOf(PreciseAllocation::alignment, allocation.type()->byteSize());
        result.append({ allocation.address(), allocation.address() + headerSize, static_cast<uint64_t>(*cellSize), *isMarked || *isNewlyAllocated });
    }
    return result;
}

// The loop over m_preciseAllocations in MarkedSpace::forEachLiveCell.
IterationStatus HeapWalk::walkPreciseAllocations(const Remote<MarkedSpace>& space, const Function<IterationStatus(const Cell&)>& functor) const
{
    using Allocations = Vector<PreciseAllocation*>;
    Remote<Allocations> allocations = space.field<Allocations>("m_preciseAllocations");
    auto size = RemoteTraits<Allocations>::size(allocations);
    if (!size)
        return IterationStatus::Continue;
    for (size_t index = 0; index < *size; ++index) {
        Remote<PreciseAllocation> allocation = RemoteTraits<Allocations>::element(allocations, index).dereference();
        auto isMarked = allocation.field<void>("m_isMarked").as<bool>();
        auto isNewlyAllocated = allocation.field<bool>("m_isNewlyAllocated").integer();
        auto cellSize = allocation.field<void>("m_cellSize").integer();
        auto kind = allocation.field<void>("m_attributes").field<HeapCell::Kind>("cellKind").integer();
        if (!isMarked || !isNewlyAllocated || !cellSize || !kind)
            continue;
        // PreciseAllocation::isLive.
        if (!*isMarked && !*isNewlyAllocated)
            continue;
        // PreciseAllocation::cell, with headerSize() from the target's layout.
        size_t headerSize = roundUpToMultipleOf(PreciseAllocation::alignment, allocation.type()->byteSize());
        Cell cell { allocation.address() + headerSize, static_cast<uint64_t>(*cellSize), static_cast<HeapCell::Kind>(*kind), allocation.address() };
        if (functor(cell) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

// The walk of HeapWalk::reach: every live cell as its C++ class, and every C++
// object reached from the roots or from a cell.
class ReachWalk {
public:
    ReachWalk(const HeapWalk& heap, const Vector<HeapWalk::Allocation>& allocations, Vector<HeapWalk::Reference>* references)
        : m_references(references)
        , m_heap(heap)
        , m_snapshot(heap.snapshot())
        , m_allocations(allocations)
    {
        m_reached.fill(false, allocations.size());
        m_typeAtStart.fill(nullptr, allocations.size());
        m_reachedBy.fill({ nullptr, nullptr, nullptr }, allocations.size());
        m_liveBytesInBlock.fill(0, allocations.size());
        m_isBlock.fill(false, allocations.size());
    }

    void run();
    void summarize(const Vector<HeapWalk::Allocation>& excluded, size_t listCount, HeapWalk::Reach&);
    // Fills in result.misses and result.bytesMissedByCause, after summarize.
    void explainMisses(const Vector<HeapWalk::Allocation>& excluded, const Vector<HeapWalk::Allocation>& notReferrers, HeapWalk::Reach&);

private:
    using NotFollowed = HeapWalk::NotFollowed;
    enum class IsObject : bool { No, Yes };

    // The allocation `size` bytes at `address` lie in, if any.
    std::optional<size_t> allocationOf(Address, uint64_t size) const;

    void notFollowed(NotFollowed reason) { ++m_notFollowed[static_cast<size_t>(reason)]; }
    // Records what reached an allocation first, which the untyped report names.
    enum class IsReference : bool { No, Yes };
    // `target`, the address that led to it, is its start if not given. A block
    // or a live cell that the heap itself lists is reached by no reference.
    void markReached(size_t index, Address target = { }, IsReference isReference = IsReference::Yes)
    {
        if (m_references && isReference == IsReference::Yes)
            recordReference(target ? target : m_allocations[index].address);
        if (m_reached[index])
            return;
        m_reached[index] = true;
        m_reachedBy[index] = { m_contextClass, m_contextField, m_rootKind };
    }
    void recordReference(Address to)
    {
        Address from = m_referrer ? m_referrer : m_contextSlot ? m_contextSlot : m_contextObject.first;
        m_references->append({ from, to, m_rootKind });
    }
    // The cells the JSValues from `start`, `bytes` long, hold, as references of `owner`.
    void referenceJSValues(Address owner, Address start, uint64_t bytes);
    Vector<HeapWalk::Reference>* m_references { nullptr };
    Address m_referrer; // What holds the references being read, when it is not the slot or value being read.
    String reachedBy(size_t allocationIndex) const;
    std::optional<uint32_t> lexicalEnvironmentScopeSize(Address, const TargetType& environmentClass);
    std::optional<std::pair<uint64_t, uint64_t>> m_scopeOffsets; // Of the SymbolTable in an environment, and of its maxScopeOffset.
    // The live auxiliary cell `address` lies in, if any.
    const HeapWalk::Cell* auxiliaryCellContaining(Address) const;

    // An object is a value the walk read that is not part of another one: the
    // roots, a JS cell, a block's header, or a value reached through a pointer
    // or as a container's element. Its bytes are what the walk types.
    void enqueue(const TargetValue&, IsObject = IsObject::No);
    // The fewest bytes an object of `type` takes: its size, but for a class
    // whose last member only marks where its trailing storage starts.
    uint64_t minimumObjectSize(const TargetType&);
    void typed(Address, uint64_t size, const TargetType&);
    void walk(const TargetValue&);
    void walkClass(const TargetValue&);
    enum class IsComplete : bool { No, Yes };
    enum class ReadsOwnUnions : bool { No, Yes };
    // Every member but those in `except`, which a reader reads its own way. A
    // plan is kept per first member left out, so each list must start with its
    // own literal. A reader that picks the live member of the unions its class
    // and that class's bases declare passes ReadsOwnUnions::Yes.
    using Except = std::span<const char* const>;
    void walkMembers(const TargetValue&, IsComplete, Except, ReadsOwnUnions = ReadsOwnUnions::No);
    void walkMembers(const TargetValue& value, IsComplete isComplete, const char* except = nullptr, ReadsOwnUnions readsOwnUnions = ReadsOwnUnions::No)
    {
        walkMembers(value, isComplete, except ? Except { &except, 1 } : Except { }, readsOwnUnions);
    }
    void walkInPlace(Address, const TargetType&, const TargetType* owner, const TargetType::Field*);

    struct Slot {
        uint64_t offset;
        enum class Kind : uint8_t { Pointer, Reader, Array, Union } kind;
        const TargetType* type; // The pointee, or the value a reader, the array walk or a union's holder reads.
        const TargetType* owner; // The class whose field it is, which an overrun names.
        const TargetType::Field* field;
    };
    using Plan = Vector<Slot>;
    const Plan& planFor(const TargetType&, IsComplete, Except);
    void buildPlan(Plan&, const TargetType&, uint64_t offset, IsComplete, Except, unsigned depth);
    void addToPlan(Plan&, const TargetType&, uint64_t offset, const TargetType& owner, const TargetType::Field*, unsigned depth);
    // Union slots whose owner is `unionsReadBy` or one of its bases are left to the caller.
    void walkPlan(Address, const Plan&, const TargetType* unionsReadBy = nullptr);
    // Whether any member of a union holds something the walk follows.
    bool unionLeadsSomewhere(const TargetType&);
    // A union no reader picks the live member of, met in `owner`'s `field`, or as a value of its own.
    void unreadUnion(const Slot&);
    struct UnreadUnion {
        const TargetType* type;
        const TargetType* owner;
        const TargetType::Field* field;
        uint64_t count;
    };
    HashMap<std::pair<const TargetType*, const void*>, UnreadUnion> m_unreadUnions;
    // The member `member` of the union in `holder`'s field `unionField`, or of
    // an anonymous union `holder` or one of its bases declares, walked in place.
    void walkUnionMember(const TargetValue& holder, const char* unionField, const char* member);
    // The integer, or bitfield, `name` that `value` or one of its bases declares.
    std::optional<int64_t> integerField(const TargetValue&, const char* name);
    void walkGlobalVariables();
    void walkStacks();
    void scanConservatively(Address start, Address end);
    void walkBlock(Address);
    void walkCell(const HeapWalk::Cell&);

    // The pointee of the pointer `pointer`, of static type `pointee`, if it is
    // in an allocation: as its dynamic type if it has one.
    void followPointer(Address pointer, const TargetType& pointee);
    void follow(Address, const TargetType&);
    void followUntyped(Address);

    // Readers for values whose pointers the debug info cannot describe, picked
    // by the qualified name of a class the walk has reached. True if `value` was
    // one, and has been read.
    bool walkByName(const TargetValue&);
    enum class Reader : uint8_t { None, Vector, HashTable, RobinHoodHashTable, TrailingArray, ButterflyArray, SegmentedVector, ConcurrentBufferArray, SymbolTableEntry, CodeBlock, AlignedStorage, CodePointer, LazyPointer, CompactPointer, PackedPointer, JSString, PropertyTable, Optional, StringImpl, ObjectButterfly, StrongBlock, InlineWatchpointSet, ArraySegment, WeakBlock, ExpressionInfo, ArrayBufferView, ArrayBufferContents, Variant, HasOwnPropertyCache, ImmutableStyleProperties, InlineMap, UnlinkedFunctionExecutable, CSSSelector, JSValue, StringImplShape, HashTableValue, PropertyCondition, InlineCacheHandler, CellButterfly, CSSPrimitiveValue, CSSPrimitiveData, MarkedSpace, TypeField, CompactPointerTuple, StructureID, Function, StructureChain };
    Reader readerFor(const TargetType&);
    static Reader readerNamed(std::string_view qualifiedName);
    void walkVector(const TargetValue&);
    void walkHashTable(const TargetValue&);
    void walkRobinHoodHashTable(const TargetValue&);
    void walkTrailingArray(const TargetValue&);
    void walkButterflyArray(const TargetValue&);
    void walkCodeBlock(const TargetValue&);
    void walkSegmentedVector(const TargetValue&);
    void walkConcurrentBufferArray(const TargetValue&);
    void walkSymbolTableEntry(const TargetValue&);
    void walkAlignedStorage(const TargetValue&);
    // `count` values of type `element` from `address`, each an object of its own.
    void walkElements(Address, const TargetType& element, uint64_t count);
    bool leadsNowhere(const TargetType&);
    void walkCodePointer(const TargetValue&);
    void walkTaggedPointer(const TargetValue&, const char* field, unsigned templateArgument, uint64_t tagMask);
    void walkCompactPointer(const TargetValue&);
    void walkPackedPointer(const TargetValue&);
    void walkJSString(const TargetValue&);
    void walkPropertyTable(const TargetValue&);
    void walkOptional(const TargetValue&);
    void walkStringImpl(const TargetValue&);
    void walkObjectButterfly(const TargetValue&);
    void walkStrongBlock(const TargetValue&);
    void walkInlineWatchpointSet(const TargetValue&);
    void walkArraySegment(const TargetValue&);
    void walkWeakBlock(const TargetValue&);
    void walkExpressionInfo(const TargetValue&);
    void walkArrayBufferView(const TargetValue&);
    void walkArrayBufferContents(const TargetValue&);
    void walkVariant(const TargetValue&);
    // SnapshotDebugInfo::classNamedBeside, once per neighbor and name.
    const TargetType* classBeside(const TargetType& neighbor, const char* name)
    {
        return m_classesBeside.ensure({ &neighbor, name }, [&] {
            return m_heap.m_debugInfo->classNamedBeside(neighbor, name);
        }).iterator->value;
    }
    HashMap<std::pair<const TargetType*, const char*>, const TargetType*> m_classesBeside;
    void walkHasOwnPropertyCache(const TargetValue&);
    void walkImmutableStyleProperties(const TargetValue&);
    void walkInlineMap(const TargetValue&);
    void walkUnlinkedFunctionExecutable(const TargetValue&);
    void walkCSSSelector(const TargetValue&);
    void walkJSValue(const TargetValue&);
    void walkStringImplShape(const TargetValue&);
    void walkHashTableValue(const TargetValue&);
    void walkPropertyCondition(const TargetValue&);
    void walkCSSPrimitiveValue(const TargetValue&);
    void walkCSSPrimitiveData(const TargetValue&);
    void walkTypeField(const TargetValue&);
    void walkCompactPointerTuple(const TargetValue&);
    void walkStructureID(const TargetValue&);
    void walkFunction(const TargetValue&);
    void walkStructureChain(const TargetValue&);
    // The cell at `address`, which the walk reads as a live cell of its own, is referred to.
    void referenceCell(Address);
    // By the base class and the value of its type field; null if it names no subclass.
    HashMap<std::pair<const TargetType*, int64_t>, const TargetType*> m_typeFieldSubclasses;
    // The pointer at `field`'s address in `value`, read whole: a CagedPtr holds its full address.
    std::optional<Address> rawPointer(const TargetValue&, const char* field);

    // The class and field being walked, which an overrun names.
    String context() const;

    const HeapWalk& m_heap;
    Snapshot& m_snapshot;
    const Vector<HeapWalk::Allocation>& m_allocations;
    Vector<bool> m_reached;
    // Each object is walked once as each type it is reached as.
    HashSet<std::pair<uint64_t, uint64_t>> m_visited;
    HashMap<const TargetType*, Reader> m_readers;
    // For complete objects and for base subobjects, by type and the member left out.
    std::array<HashMap<std::pair<const TargetType*, const char*>, std::unique_ptr<Plan>>, 2> m_plans;
    Vector<TargetValue> m_worklist;
    struct Range {
        uint64_t begin;
        uint64_t end;
        const TargetType* type; // Read at `begin`.
    };
    Vector<Range> m_typed; // Every object's bytes, clipped to its allocation, sorted by summarize.
    // What holds the word at `address`, of an allocation the walk reached.
    String holderInReached(Address, size_t allocationIndex) const;
    struct Holder {
        HeapWalk::MissCause cause;
        String description;
        std::optional<size_t> missedHolder; // For MissCause::Missed, the allocation's index.
    };
    Holder holderOf(Address word);
    Vector<const TargetType*> m_typeAtStart; // For each allocation, the first type read where it starts.
    struct ReachedBy {
        const TargetType* klass;
        const TargetType::Field* field;
        const char* root; // What the walk was reading from, if it was in no class.
    };
    Vector<ReachedBy> m_reachedBy; // For each allocation.
    const char* m_rootKind { "the roots" };
    Vector<HeapWalk::Cell> m_auxiliaryCells; // Sorted.
    HashMap<const TargetType*, uint64_t> m_bytesBeyondClass;
    // Where StringImpl keeps its length, flags and characters, by StringImpl's type.
    struct StringImplLayout {
        uint64_t lengthOffset;
        uint64_t flagsOffset;
        uint64_t dataOffset; // Of m_data8, which shares its storage with m_data16.
        const TargetType* latin1; // The pointee of m_data8.
        const TargetType* utf16; // The pointee of m_data16.
    };
    HashMap<const TargetType*, std::optional<StringImplLayout>> m_stringImplLayouts;
    const StringImplLayout* stringImplLayout(const TargetType&);
    HashMap<const TargetType*, std::optional<uint64_t>> m_butterflyOffsets; // Of AuxiliaryBarrier::m_value, by the object's type.
    // A MarkedBlock or a precise allocation is one allocation, whose memory outside its live cells is the JS heap's free memory.
    Vector<bool> m_isBlock;
    Vector<uint64_t> m_liveBytesInBlock; // Its live cells and its header.
    Vector<String> m_overruns;
    std::array<uint64_t, HeapWalk::numberOfNotFollowedReasons> m_notFollowed { };
    uint64_t m_cellsWithClass { 0 };
    uint64_t m_globalVariables { 0 };
    uint64_t m_untypedDataSymbols { 0 };
    uint64_t m_cellBytesBeyondClass { 0 };
    const TargetType* m_contextClass { nullptr };
    const TargetType::Field* m_contextField { nullptr };
    Address m_contextSlot; // The word that holds the pointer being followed, when the walk knows it.
    std::pair<Address, const TargetType*> m_contextObject { }; // The value whose members are being walked.
    Vector<String> m_unreadableArrays;
};

String ReachWalk::context() const
{
    if (!m_contextClass)
        return "the roots"_s;
    if (!m_contextField)
        return makeString("an element of a '"_s, m_contextClass->name(), '\'');
    return makeString(m_contextClass->name(), "::"_s, m_contextField->name);
}

std::optional<size_t> ReachWalk::allocationOf(Address address, uint64_t size) const
{
    auto allocations = m_allocations.span();
    auto after = std::ranges::upper_bound(allocations, address, { }, &HeapWalk::Allocation::address);
    if (after == allocations.begin())
        return std::nullopt;
    const HeapWalk::Allocation& allocation = *(after - 1);
    if (address - allocation.address >= allocation.size || size > allocation.size - (address - allocation.address))
        return std::nullopt;
    return (after - 1) - allocations.begin();
}

void ReachWalk::typed(Address address, uint64_t size, const TargetType& type)
{
    auto index = allocationOf(address, 1);
    if (!index || !size)
        return;
    const HeapWalk::Allocation& allocation = m_allocations[*index];
    if (allocation.address == address && !m_typeAtStart[*index])
        m_typeAtStart[*index] = &type;
    uint64_t end = (allocation.address + allocation.size).toTargetVMAddress();
    if (size > end - address.toTargetVMAddress()) {
        m_overruns.append(makeString("the "_s, type.byteSize(), "-byte '"_s, type.name(), "' at 0x"_s, hex(address.toTargetVMAddress()),
            ", reached through "_s, context(), ", runs past the end of its "_s, allocation.size, "-byte allocation"_s));
        size = end - address.toTargetVMAddress();
    }
    m_typed.append({ address.toTargetVMAddress(), address.toTargetVMAddress() + size, &type });
}

void ReachWalk::enqueue(const TargetValue& value, IsObject isObject)
{
    if (!value)
        return;
    if (!m_visited.add({ value.address().toTargetVMAddress(), std::bit_cast<uint64_t>(&value.type()) }).isNewEntry)
        return;
    m_worklist.append(value);
    if (isObject == IsObject::Yes)
        typed(value.address(), minimumObjectSize(value.type()), value.type());
}

// The field named `name` of `type` or of a non-virtual base, looking into
// unnamed members, such as an anonymous union. `offset` is where the class that
// declares it is in `type`.
static const TargetType::Field* findField(const TargetType& type, std::string_view name, uint64_t& offset, const TargetType** declaringClass = nullptr, unsigned depth = 0)
{
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    if (!klass || depth > 8)
        return nullptr;
    for (const TargetType::Field& field : klass->properFields) {
        std::string_view fieldName { field.name.legacyCStringPointer() };
        if (fieldName == name) {
            if (declaringClass)
                *declaringClass = &type;
            return &field;
        }
        if (fieldName.empty()) {
            uint64_t inner = 0;
            if (auto* found = findField(field.type, name, inner, declaringClass, depth + 1)) {
                offset += field.offset + inner;
                return found;
            }
        }
    }
    for (const TargetType::Base& base : klass->bases) {
        uint64_t inner = 0;
        if (auto* found = findField(base.type, name, inner, declaringClass, depth + 1)) {
            offset += base.offset + inner;
            return found;
        }
    }
    return nullptr;
}

// ImmutableStyleProperties::objectSize: its m_storage is the first of its
// trailing StylePropertyMetadatas, of which it may have none.
uint64_t ReachWalk::minimumObjectSize(const TargetType& type)
{
    if (readerFor(type) != Reader::ImmutableStyleProperties)
        return type.byteSize();
    uint64_t offset = 0;
    auto* storage = findField(type, "m_storage", offset);
    return storage ? offset + storage->offset : type.byteSize();
}

void ReachWalk::run()
{
    auto drain = [&] {
        while (!m_worklist.isEmpty())
            walk(m_worklist.takeLast());
    };
    // An object's butterfly is found among the auxiliary cells, whichever root reaches the object first.
    Vector<HeapWalk::Cell> cells;
    m_heap.forEachLiveCell([&](const HeapWalk::Cell& cell) {
        cells.append(cell);
        if (!isJSCellKind(cell.kind))
            m_auxiliaryCells.append(cell);
        return IterationStatus::Continue;
    });
    std::ranges::sort(m_auxiliaryCells, { }, &HeapWalk::Cell::address);
    // The roots' VM is the test's description of it. The cells reach the VM as
    // JavaScriptCore describes it, with every type it owns complete.
    m_rootKind = m_heap.m_roots ? "the roots" : "the VM";
    if (m_heap.m_roots)
        walkMembers(*m_heap.m_roots, IsComplete::Yes, "vm");
    else if (auto* vm = m_heap.m_vm.typed())
        follow(vm->address(), vm->type());
    drain();
    walkGlobalVariables();
    drain();
    m_rootKind = "a thread's stack";
    walkStacks();
    drain();
    // A precise allocation holds its header and, while it is live, its cell;
    // the cell of one that is dead, and not yet swept, is the JS heap's free memory.
    m_rootKind = "a PreciseAllocation";
    for (const HeapWalk::PreciseAllocationState& precise : m_heap.preciseAllocations()) {
        auto index = allocationOf(precise.header, 1);
        if (!index)
            continue;
        markReached(*index, { }, IsReference::No);
        m_isBlock[*index] = true;
        m_liveBytesInBlock[*index] += (precise.cell - precise.header) + (precise.isLive ? precise.cellSize : 0);
        m_contextClass = nullptr;
        m_contextField = nullptr;
        enqueue(TargetValue::at(m_snapshot, precise.header, *m_heap.m_preciseAllocationClass), IsObject::Yes);
        drain();
    }
    m_rootKind = "a MarkedBlock";
    m_heap.forEachBlock([&](Address block) {
        walkBlock(block);
        drain();
        return IterationStatus::Continue;
    });
    m_rootKind = "a live cell";
    for (const HeapWalk::Cell& cell : cells) {
        walkCell(cell);
        drain();
    }
}

const HeapWalk::Cell* ReachWalk::auxiliaryCellContaining(Address address) const
{
    auto cells = m_auxiliaryCells.span();
    auto after = std::ranges::upper_bound(cells, address, { }, &HeapWalk::Cell::address);
    if (after == cells.begin() || address - (after - 1)->address >= (after - 1)->size)
        return nullptr;
    return &*(after - 1);
}

// SymbolTable::scopeSize() of the lexical environment's JSSymbolTableObject::m_symbolTable: its maxScopeOffset plus one.
std::optional<uint32_t> ReachWalk::lexicalEnvironmentScopeSize(Address environment, const TargetType& klass)
{
    if (!m_scopeOffsets) {
        m_scopeOffsets = std::pair<uint64_t, uint64_t> { 0, 0 };
        uint64_t holder = 0;
        auto* symbolTable = findField(klass, "m_symbolTable", holder);
        uint64_t cellOffset = 0;
        auto* cellField = symbolTable ? findField(symbolTable->type, "m_cell", cellOffset) : nullptr;
        auto* pointer = cellField ? std::get_if<TargetType::Pointer>(&cellField->type.layout()) : nullptr;
        uint64_t maxScopeHolder = 0;
        auto* maxScopeOffset = pointer ? findField(pointer->pointee, "m_maxScopeOffset", maxScopeHolder) : nullptr;
        uint64_t offsetHolder = 0;
        auto* offset = maxScopeOffset ? findField(maxScopeOffset->type, "m_offset", offsetHolder) : nullptr;
        if (!offset)
            return std::nullopt;
        m_scopeOffsets = std::pair<uint64_t, uint64_t> { holder + symbolTable->offset + cellOffset + cellField->offset, maxScopeHolder + maxScopeOffset->offset + offsetHolder + offset->offset };
    }
    if (!m_scopeOffsets->first)
        return std::nullopt;
    auto table = m_snapshot.memory().ptr<uint64_t>(environment + m_scopeOffsets->first);
    if (!table || !*table)
        return std::nullopt;
    auto maxScopeOffset = m_snapshot.memory().ptr<uint32_t>(Address { *table }.stripped() + m_scopeOffsets->second);
    if (!maxScopeOffset)
        return std::nullopt;
    // ScopeOffset's invalid offset is UINT_MAX, so the sum is the scope size either way.
    return *maxScopeOffset + 1;
}

String ReachWalk::reachedBy(size_t allocationIndex) const
{
    auto [klass, field, root] = m_reachedBy[allocationIndex];
    if (!klass)
        return String::fromLatin1(root ? root : "nothing");
    if (!field)
        return makeString("a '"_s, klass->name(), '\'');
    return makeString(klass->name(), "::"_s, field->name);
}

// Every global variable of every image with debug info, as its own image
// describes it, including function-local statics.
void ReachWalk::walkGlobalVariables()
{
    size_t untyped = 0;
    Vector<SnapshotDebugInfo::Range> merged;
    const Vector<Region>& regions = m_snapshot.regions();
    // Memory the target cannot write holds no address it allocated, except
    // memory it froze after writing it: g_config, which is read below.
    auto variables = m_heap.m_debugInfo->globalVariables([&](Address address) {
        auto region = Region::findContaining(regions, address);
        return region && region->isWritable();
    }, untyped, merged);
    m_rootKind = "a global variable";
    for (const SnapshotDebugInfo::GlobalVariable& variable : variables) {
        // A variable of a type liblldb gives no size, such as a Swift one, is data the walk cannot read as a type.
        if (!variable.type.byteSize()) {
            ++untyped;
            continue;
        }
        ++m_globalVariables;
        m_contextClass = nullptr;
        m_contextField = nullptr;
        TargetValue value = TargetValue::at(m_snapshot, variable.address, variable.type);
        if (std::holds_alternative<TargetType::Class>(variable.type.layout()))
            enqueue(value);
        else
            walk(value);
    }
    m_untypedDataSymbols = untyped;
    // The statics a compiler merged into one symbol, such as libpas's, are read as a stack is.
    m_rootKind = "a block of statics merged into one symbol";
    m_contextClass = nullptr;
    m_contextField = nullptr;
    for (const SnapshotDebugInfo::Range& range : merged)
        scanConservatively(range.address, range.address + std::min<uint64_t>(range.size, maxConservativeScanSize));

    // g_config is an array of words, which WTF reads as its Config at
    // startOffsetOfWTFConfig (addressOfWTFConfig), and JavaScriptCore as its
    // own at the WTF Config's spaceForExtensions (addressOfJSCConfig).
    m_rootKind = "WebConfig::g_config";
    Address wtfConfig = *m_heap.m_config + WTF::startOffsetOfWTFConfig;
    enqueue(TargetValue::at(m_snapshot, wtfConfig, *m_heap.m_wtfConfigClass));
    auto* layout = std::get_if<TargetType::Class>(&m_heap.m_wtfConfigClass->layout());
    for (const TargetType::Field& field : layout ? layout->properFields : Vector<TargetType::Field> { }) {
        if (std::string_view { field.name.legacyCStringPointer() } == "spaceForExtensions")
            enqueue(TargetValue::at(m_snapshot, wtfConfig + field.offset, *m_heap.m_jscConfigClass));
    }
}

// The words of each thread's stack in use, from its stack pointer up, as
// conservative roots, as the collector scans a stack: a word inside an
// allocation reaches it, and an allocation that starts with a polymorphic
// object is read as its dynamic type. A thread whose stack pointer is not
// known has its whole stack scanned.
void ReachWalk::walkStacks()
{
    for (const Thread& thread : m_snapshot.threads()) {
        if (!thread.hasStack())
            continue;
        Address start = thread.stackPointer() && thread.stackRegion().contains(thread.stackPointer()) ? thread.stackPointer() : thread.stackRegion().base();
        m_contextClass = nullptr;
        m_contextField = nullptr;
        scanConservatively(start, thread.stackRegion().end());
    }
}

// Every aligned word from `start` to `end` that points into an allocation reaches it.
void ReachWalk::scanConservatively(Address start, Address end)
{
    constexpr size_t wordsPerRead = 64 * 1024;
    start = Address { roundUpToMultipleOf<sizeof(uint64_t)>(start.toTargetVMAddress()) };
    while (start < end) {
        size_t count = std::min<uint64_t>(wordsPerRead, (end - start) / sizeof(uint64_t));
        if (!count)
            break;
        auto words = m_snapshot.memory().span<uint64_t>(start, count);
        start = start + count * sizeof(uint64_t);
        if (!words)
            continue;
        for (uint64_t word : std::span<const uint64_t> { words })
            followUntyped(Address { word }.stripped());
    }
}

void ReachWalk::walkBlock(Address block)
{
    Remote<MarkedBlock::Header> header = m_heap.header(block);
    auto index = allocationOf(block, MarkedBlock::blockSize);
    if (!index || !header.typed())
        return;
    m_isBlock[*index] = true;
    markReached(*index, { }, IsReference::No);
    m_liveBytesInBlock[*index] += header.type()->byteSize();
    m_contextClass = nullptr;
    m_contextField = nullptr;
    enqueue(*header.typed(), IsObject::Yes);
}

void ReachWalk::referenceJSValues(Address owner, Address start, uint64_t bytes)
{
    if (!m_references || !bytes)
        return;
    auto words = m_snapshot.memory().span<EncodedJSValue>(start, static_cast<size_t>(bytes / sizeof(EncodedJSValue)));
    if (!words)
        return;
    SetForScope referrer { m_referrer, owner ? owner : start };
    for (EncodedJSValue word : std::span<const EncodedJSValue> { words }) {
        JSValue value = JSValue::decode(word);
        if (word && value.isCell())
            recordReference(Address { static_cast<uint64_t>(word) });
    }
}

void ReachWalk::walkCell(const HeapWalk::Cell& cell)
{
    m_contextClass = nullptr;
    m_contextField = nullptr;
    auto index = allocationOf(cell.address, cell.size);
    if (index) {
        markReached(*index, { }, IsReference::No);
        // A precise allocation's live bytes were counted with its header.
        if (m_isBlock[*index] && !cell.preciseAllocation)
            m_liveBytesInBlock[*index] += cell.size;
    }
    if (!isJSCellKind(cell.kind))
        return;
    const TargetType* klass = m_heap.cellClass(cell.address);
    if (!klass) {
        notFollowed(NotFollowed::CellWithoutClass);
        return;
    }
    ++m_cellsWithClass;
    uint64_t classSize = klass->byteSize();
    // JSFinalObject::allocationSize: its inline storage, as many JSValues as its Structure's inline capacity, follows the class.
    if (klass == m_heap.m_finalObjectClass) {
        auto bits = m_snapshot.memory().ptr<uint32_t>(cell.address + m_heap.m_structureIDOffset);
        auto inlineCapacity = bits ? m_snapshot.memory().ptr<uint8_t>(Address { m_heap.m_startOfStructureHeap + (*bits & ~StructureID::nukedStructureIDBit) } + m_heap.m_inlineCapacityOffset) : Memory::Ptr<uint8_t> { };
        if (inlineCapacity && *inlineCapacity) {
            uint64_t inlineBytes = std::min<uint64_t>(static_cast<uint64_t>(*inlineCapacity) * sizeof(EncodedJSValue), cell.size - std::min(cell.size, classSize));
            typed(cell.address + classSize, inlineBytes, *m_heap.m_jsValueClass);
            referenceJSValues(cell.address, cell.address + classSize, inlineBytes);
            classSize += inlineBytes;
        }
    }
    // JSCellButterfly::allocationSize: its vector, of vectorLength JSValues, at offsetOfData().
    if (klass == m_heap.m_cellButterflyClass) {
        auto vectorLength = m_snapshot.memory().ptr<uint32_t>(cell.address + JSCellButterfly::offsetOfVectorLength());
        uint64_t data = JSCellButterfly::offsetOfData();
        if (vectorLength && data <= cell.size) {
            uint64_t bytes = std::min<uint64_t>(static_cast<uint64_t>(*vectorLength) * sizeof(EncodedJSValue), cell.size - data);
            typed(cell.address + data, bytes, *m_heap.m_jsValueClass);
            referenceJSValues(cell.address, cell.address + data, bytes);
            classSize = std::max(classSize, data + bytes);
        }
    }
    // JSLexicalEnvironment::allocationSize: its variables, of its SymbolTable's scopeSize() JSValues, at offsetOfVariables().
    if (klass == m_heap.m_lexicalEnvironmentClass) {
        if (auto scopeSize = lexicalEnvironmentScopeSize(cell.address, *klass)) {
            uint64_t variables = roundUpToMultipleOf<sizeof(EncodedJSValue)>(klass->byteSize());
            uint64_t bytes = std::min<uint64_t>(static_cast<uint64_t>(*scopeSize) * sizeof(EncodedJSValue), cell.size - std::min(cell.size, variables));
            typed(cell.address + variables, bytes, *m_heap.m_jsValueClass);
            referenceJSValues(cell.address, cell.address + variables, bytes);
            classSize = std::max(classSize, variables + bytes);
        }
    }
    // A JSRopeString shares JSString's ClassInfo, but is allocated from its own
    // subspace, of its own size (JSRopeString::subspaceFor), and stays one once resolved.
    if (klass == m_heap.m_stringImplOwnerClass && cell.size >= m_heap.m_ropeStringClass->byteSize() && cell.size > klass->byteSize()) {
        klass = m_heap.m_ropeStringClass;
        classSize = std::max<uint64_t>(classSize, klass->byteSize());
    }
    // A variable-sized cell, or a subclass that shares its base's ClassInfo, is bigger than the class.
    if (cell.size > classSize) {
        m_cellBytesBeyondClass += cell.size - classSize;
        m_bytesBeyondClass.add(klass, 0).iterator->value += cell.size - classSize;
    }
    enqueue(TargetValue::at(m_snapshot, cell.address, *klass), IsObject::Yes);
}

void ReachWalk::walk(const TargetValue& value)
{
    walkInPlace(value.address(), value.type(), nullptr, nullptr);
}

void ReachWalk::walkClass(const TargetValue& value)
{
    m_contextClass = &value.type();
    m_contextField = nullptr;
    if (walkByName(value))
        return;
    walkMembers(value, IsComplete::Yes);
}

// A value that is part of the object being walked, at `address`: a class is
// walked through its plan or its reader, a pointer is followed, and an array's
// elements are walked in place.
void ReachWalk::walkInPlace(Address address, const TargetType& type, const TargetType* owner, const TargetType::Field* field)
{
    const TargetType::Layout& layout = type.layout();
    if (std::holds_alternative<TargetType::Class>(layout)) {
        m_contextClass = &type;
        m_contextField = nullptr;
        if (readerFor(type) != Reader::None) {
            walkByName(TargetValue::at(m_snapshot, address, type));
            return;
        }
        auto saved = std::exchange(m_contextObject, std::pair<Address, const TargetType*> { address, &type });
        walkPlan(address, planFor(type, IsComplete::Yes, { }));
        m_contextObject = saved;
        return;
    }
    if (auto* pointer = std::get_if<TargetType::Pointer>(&layout)) {
        auto word = m_snapshot.memory().ptr<uint64_t>(address);
        if (!word || !*word)
            return;
        m_contextClass = owner;
        m_contextField = field;
        m_contextSlot = address;
        followPointer(Address { *word }.stripped(), pointer->pointee);
        m_contextSlot = { };
        return;
    }
    if (auto* array = std::get_if<TargetType::Array>(&layout)) {
        // An integer leads nowhere, and a large table of them is common in static data.
        if (!array->count || !array->element.byteSize() || leadsNowhere(array->element))
            return;
        // An array is a member of a value its allocation or variable holds whole, so one that runs
        // past readable memory means the walk misread that value. None of it is read.
        auto region = Region::findContaining(m_snapshot.regions(), address);
        uint64_t count = array->count;
        if (!region || !region->isReadable() || count > (region->end() - address) / array->element.byteSize()) {
            String holder;
            if (auto [holderAddress, holderType] = m_contextObject; holderType) {
                auto holderAllocation = allocationOf(holderAddress, 1);
                holder = makeString(" in the '"_s, holderType->name(), "' at 0x"_s, hex(holderAddress.toTargetVMAddress()),
                    holderAllocation ? makeString(", reached by "_s, reachedBy(*holderAllocation)) : makeString(", reached by "_s, String::fromLatin1(m_rootKind)));
            }
            m_unreadableArrays.append(makeString("the "_s, array->count, "-element '"_s, type.name(), "' at 0x"_s, hex(address.toTargetVMAddress()),
                ", reached through "_s, context(), holder, ", runs past the readable memory it starts in"_s));
            return;
        }
        for (uint64_t index = 0; index < count; ++index)
            walkInPlace(address + index * array->element.byteSize(), array->element, owner, field);
    }
}

// TargetValue::forEachField, but with each base as a value of its own, which a
// reader may recognise, as a Packed<T*> is a PackedAlignedPtr.
// The context is restored, so what a reader reads after its members is named as read through its own value.
void ReachWalk::walkMembers(const TargetValue& value, IsComplete isComplete, Except except, ReadsOwnUnions readsOwnUnions)
{
    SetForScope contextObject { m_contextObject, std::pair<Address, const TargetType*> { value.address(), &value.type() } };
    SetForScope contextClass { m_contextClass };
    SetForScope contextField { m_contextField };
    walkPlan(value.address(), planFor(value.type(), isComplete, except), readsOwnUnions == ReadsOwnUnions::Yes ? &value.type() : nullptr);
}

// Where `base` is in an object of class `derived`, among its non-virtual bases.
static std::optional<size_t> baseOffset(const TargetType& derived, const TargetType& base, unsigned depth = 0)
{
    if (&derived == &base)
        return 0;
    auto* klass = std::get_if<TargetType::Class>(&derived.layout());
    if (!klass || depth > 32)
        return std::nullopt;
    for (const TargetType::Base& candidate : klass->bases) {
        if (auto offset = baseOffset(candidate.type, base, depth + 1))
            return candidate.offset + *offset;
    }
    return std::nullopt;
}

// A union whose members that lead somewhere are all pointers at its start to
// types without a size, such as function pointers, holds one word that the walk
// reads the same way whichever member is live.
static const TargetType::Field* untypedPointerOfUnion(const TargetType& type, const Function<bool(const TargetType&)>& leadsNowhere)
{
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    const TargetType::Field* result = nullptr;
    for (const TargetType::Field& field : klass ? klass->properFields : Vector<TargetType::Field> { }) {
        if (field.bitSize || !field.type.byteSize() || leadsNowhere(field.type))
            continue;
        auto* pointer = std::get_if<TargetType::Pointer>(&field.type.layout());
        if (field.offset || !pointer || pointer->pointee.byteSize())
            return nullptr;
        result = &field;
    }
    return result;
}

// The members of a class, flattened once per class into the values that may
// lead somewhere: pointers, values a reader reads, and arrays of either.
// Members that are integers, or classes of integers, lead nowhere.
auto ReachWalk::planFor(const TargetType& type, IsComplete isComplete, Except except) -> const Plan&
{
    auto& plans = m_plans[isComplete == IsComplete::Yes];
    std::pair<const TargetType*, const char*> key { &type, except.empty() ? nullptr : except.front() };
    if (auto* plan = plans.get(key))
        return *plan;
    auto plan = makeUnique<Plan>();
    buildPlan(*plan, type, 0, isComplete, except, 0);
    return *plans.add(key, WTF::move(plan)).iterator->value;
}

void ReachWalk::buildPlan(Plan& plan, const TargetType& type, uint64_t offset, IsComplete isComplete, Except except, unsigned depth)
{
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    // A class nests its members a few deep; a cycle would be a liblldb bug.
    if (!klass || depth > 64)
        return;
    // Reading every member of a union follows pointers that are not pointers.
    if (klass->isUnion) {
        if (auto* pointer = untypedPointerOfUnion(type, [&](const TargetType& member) { return leadsNowhere(member); }))
            plan.append({ offset, Slot::Kind::Pointer, &std::get<TargetType::Pointer>(pointer->type.layout()).pointee, &type, pointer });
        else if (unionLeadsSomewhere(type))
            plan.append({ offset, Slot::Kind::Union, &type, &type, nullptr });
        return;
    }
    for (const TargetType::Field& field : klass->properFields) {
        // A field whose type liblldb could not parse has no size.
        std::string_view name { field.name.legacyCStringPointer() };
        if (field.bitSize || !field.type.byteSize() || std::ranges::any_of(except, [&](const char* excepted) { return name == excepted; }))
            continue;
        addToPlan(plan, field.type, offset + field.offset, type, &field, depth);
    }
    auto addBase = [&](const TargetType::Base& base) {
        if (readerFor(base.type) != Reader::None)
            plan.append({ offset + base.offset, Slot::Kind::Reader, &base.type, &base.type, nullptr });
        else
            buildPlan(plan, base.type, offset + base.offset, IsComplete::No, { }, depth + 1);
    };
    for (const TargetType::Base& base : klass->bases)
        addBase(base);
    // A virtual base's offset is only known in a complete object.
    if (isComplete == IsComplete::Yes) {
        for (const TargetType::Base& base : klass->virtualBases)
            addBase(base);
    }
}

void ReachWalk::addToPlan(Plan& plan, const TargetType& type, uint64_t offset, const TargetType& owner, const TargetType::Field* field, unsigned depth)
{
    const TargetType::Layout& layout = type.layout();
    if (auto* klass = std::get_if<TargetType::Class>(&layout)) {
        if (readerFor(type) != Reader::None)
            plan.append({ offset, Slot::Kind::Reader, &type, &owner, field });
        else if (klass->isUnion) {
            if (auto* pointer = untypedPointerOfUnion(type, [&](const TargetType& member) { return leadsNowhere(member); }))
                plan.append({ offset, Slot::Kind::Pointer, &std::get<TargetType::Pointer>(pointer->type.layout()).pointee, &type, pointer });
            else if (unionLeadsSomewhere(type))
                plan.append({ offset, Slot::Kind::Union, &type, &owner, field });
        } else
            buildPlan(plan, type, offset, IsComplete::Yes, { }, depth + 1);
        return;
    }
    if (auto* pointer = std::get_if<TargetType::Pointer>(&layout)) {
        plan.append({ offset, Slot::Kind::Pointer, &pointer->pointee, &owner, field });
        return;
    }
    if (auto* array = std::get_if<TargetType::Array>(&layout)) {
        if (!array->count || !array->element.byteSize() || std::holds_alternative<TargetType::Integer>(array->element.layout()) || std::holds_alternative<TargetType::Other>(array->element.layout()))
            return;
        plan.append({ offset, Slot::Kind::Array, &type, &owner, field });
    }
}

bool ReachWalk::unionLeadsSomewhere(const TargetType& type)
{
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    if (!klass)
        return false;
    for (const TargetType::Field& field : klass->properFields) {
        if (!field.bitSize && field.type.byteSize() && !leadsNowhere(field.type))
            return true;
    }
    return false;
}

void ReachWalk::unreadUnion(const Slot& slot)
{
    const void* where = slot.field ? static_cast<const void*>(slot.field) : static_cast<const void*>(m_contextClass);
    const TargetType* owner = slot.field ? slot.owner : m_contextClass;
    m_unreadUnions.ensure({ slot.type, where }, [&] {
        return UnreadUnion { slot.type, owner, slot.field, 0 };
    }).iterator->value.count++;
}

void ReachWalk::walkPlan(Address address, const Plan& plan, const TargetType* unionsReadBy)
{
    if (plan.isEmpty())
        return;
    // An object's pointers are read in one mapping, which covers every one of them.
    uint64_t end = 0;
    for (const Slot& slot : plan) {
        if (slot.kind == Slot::Kind::Pointer)
            end = std::max<uint64_t>(end, slot.offset + sizeof(uint64_t));
    }
    auto bytes = end ? m_snapshot.memory().span<uint8_t>(address, static_cast<size_t>(end)) : Memory::Span<uint8_t> { };
    for (const Slot& slot : plan) {
        Address slotAddress = address + slot.offset;
        switch (slot.kind) {
        case Slot::Kind::Pointer: {
            uint64_t word = 0;
            if (bytes) {
                memcpySpan(asMutableByteSpan(word), std::span<const uint8_t> { bytes }.subspan(slot.offset, sizeof(word)));
            } else if (auto read = m_snapshot.memory().ptr<uint64_t>(slotAddress))
                word = *read;
            if (!word)
                break;
            m_contextClass = slot.owner;
            m_contextField = slot.field;
            m_contextSlot = slotAddress;
            followPointer(Address { word }.stripped(), *slot.type);
            m_contextSlot = { };
            break;
        }
        case Slot::Kind::Reader:
            m_contextClass = slot.type;
            m_contextField = nullptr;
            walkByName(TargetValue::at(m_snapshot, slotAddress, *slot.type));
            break;
        case Slot::Kind::Array:
            walkInPlace(slotAddress, *slot.type, slot.owner, slot.field);
            break;
        case Slot::Kind::Union:
            if (!unionsReadBy || !baseOffset(*unionsReadBy, *slot.owner))
                unreadUnion(slot);
            break;
        }
    }
}

void ReachWalk::followPointer(Address address, const TargetType& declaredPointee)
{
    const TargetType* definition = &declaredPointee;
    auto* klass = std::get_if<TargetType::Class>(&declaredPointee.layout());
    if (klass && !declaredPointee.byteSize()) {
        definition = declaredPointee.debugInfo().definitionInItsImage(declaredPointee);
        if (!definition) {
            // A class no compile unit of the image defines.
            notFollowed(NotFollowed::Declaration);
            return;
        }
        klass = std::get_if<TargetType::Class>(&definition->layout());
    }
    const TargetType& pointee = *definition;
    if (klass && klass->isPolymorphic) {
        Address completeObject;
        if (auto* dynamicType = pointee.debugInfo().dynamicTypeIfAnyAt(m_snapshot, address, completeObject)) {
            follow(completeObject, *dynamicType);
            return;
        }
        notFollowed(NotFollowed::NoDynamicType);
    }
    if (!pointee.byteSize()) {
        notFollowed(NotFollowed::VoidPointer);
        followUntyped(address);
        return;
    }
    follow(address, pointee);
}

// A pointer whose type says nothing of its pointee, such as a void*, reaches
// the allocation it points into. An allocation that starts with a polymorphic
// object is read as its dynamic type, and any other stays untyped.
void ReachWalk::followUntyped(Address address)
{
    auto index = allocationOf(address, 1);
    if (!index)
        return;
    if (m_reached[*index]) {
        if (m_references)
            recordReference(address);
        return;
    }
    markReached(*index, address);
    Address allocation = m_allocations[*index].address;
    Address completeObject;
    const TargetType* dynamicType = m_heap.m_debugInfo->dynamicTypeIfAnyAt(m_snapshot, allocation, completeObject);
    if (dynamicType && completeObject == allocation)
        enqueue(TargetValue::at(m_snapshot, allocation, *dynamicType), IsObject::Yes);
}

void ReachWalk::follow(Address address, const TargetType& type)
{
    // Only into an allocation, so that a pointer into static data leads nowhere.
    auto index = allocationOf(address, 1);
    if (!index)
        return;
    // A pointee that runs past the end of its allocation is not an object of
    // its type, such as the other member of a union: it is listed as an
    // overrun, and neither reached nor read.
    const HeapWalk::Allocation& allocation = m_allocations[*index];
    if (minimumObjectSize(type) > (allocation.address + allocation.size) - address) {
        notFollowed(NotFollowed::DoesNotFit);
        m_overruns.append(makeString("the "_s, type.byteSize(), "-byte '"_s, type.name(), "' at 0x"_s, hex(address.toTargetVMAddress()),
            ", reached through "_s, context(), m_contextSlot ? makeString(" at 0x"_s, hex(m_contextSlot.toTargetVMAddress())) : String(),
            m_contextObject.second ? makeString(" in a '"_s, m_contextObject.second->name(), "' at 0x"_s, hex(m_contextObject.first.toTargetVMAddress())) : String(),
            ", runs past the end of its "_s, allocation.size, "-byte allocation at 0x"_s, hex(allocation.address.toTargetVMAddress())));
        return;
    }
    markReached(*index, address);
    // A pointer to an integer at the start of an allocation is a buffer of them, such as a malloc'ed string's characters.
    if (allocation.address == address && type.byteSize() && std::holds_alternative<TargetType::Integer>(type.layout())) {
        if (m_visited.add({ address.toTargetVMAddress(), std::bit_cast<uint64_t>(&type) }).isNewEntry)
            typed(address, allocation.size - allocation.size % type.byteSize(), type);
        return;
    }
    enqueue(TargetValue::at(m_snapshot, address, type), IsObject::Yes);
}

// Readers for the classes the debug info cannot describe, picked once per type
// by its qualified name.
auto ReachWalk::readerFor(const TargetType& type) -> Reader
{
    return m_readers.ensure(&type, [&] {
        auto qualifiedName = type.name();
        return readerNamed(std::string_view { qualifiedName.legacyCStringPointer() });
    }).iterator->value;
}

auto ReachWalk::readerNamed(std::string_view name) -> Reader
{
    if (name.starts_with("WTF::Vector<"))
        return Reader::Vector;
    if (name.starts_with("WTF::HashTable<"))
        return Reader::HashTable;
    if (name.starts_with("WTF::RobinHoodHashTable<"))
        return Reader::RobinHoodHashTable;
    if (name.starts_with("WTF::TrailingArray<"))
        return Reader::TrailingArray;
    if (name.starts_with("WTF::ButterflyArray<"))
        return Reader::ButterflyArray;
    if (name.starts_with("WTF::SegmentedVector<"))
        return Reader::SegmentedVector;
    if (name.starts_with("WTF::ConcurrentBuffer<") && name.ends_with(">::Array"))
        return Reader::ConcurrentBufferArray;
    if (name == "JSC::SymbolTableEntry")
        return Reader::SymbolTableEntry;
    if (name == "JSC::CodeBlock")
        return Reader::CodeBlock;
    if (name.starts_with("WTF::AlignedStorage<"))
        return Reader::AlignedStorage;
    if (name.starts_with("WTF::CodePtr<"))
        return Reader::CodePointer;
    // JSC::LazyProperty has LazyRef's tags: lazyTag and initializingTag.
    if (name.starts_with("WTF::LazyUniqueRef<") || name.starts_with("WTF::LazyRef<") || name.starts_with("JSC::LazyProperty<"))
        return Reader::LazyPointer;
    if (name.starts_with("WTF::CompactPtr<"))
        return Reader::CompactPointer;
    if (name.starts_with("WTF::PackedAlignedPtr<"))
        return Reader::PackedPointer;
    if (name == "JSC::JSString")
        return Reader::JSString;
    if (name == "JSC::PropertyTable")
        return Reader::PropertyTable;
    if (name.starts_with("std::optional<") || name.starts_with("std::__1::optional<"))
        return Reader::Optional;
    if (name == "WTF::StringImpl")
        return Reader::StringImpl;
    if (name == "JSC::JSObjectWithButterfly")
        return Reader::ObjectButterfly;
    if (name == "JSC::StrongBlock")
        return Reader::StrongBlock;
    if (name == "JSC::InlineWatchpointSet")
        return Reader::InlineWatchpointSet;
    if (name.starts_with("JSC::GCArraySegment<"))
        return Reader::ArraySegment;
    if (name == "JSC::WeakBlock")
        return Reader::WeakBlock;
    if (name == "JSC::ExpressionInfo")
        return Reader::ExpressionInfo;
    if (name == "JSC::JSArrayBufferView")
        return Reader::ArrayBufferView;
    if (name == "JSC::ArrayBufferContents")
        return Reader::ArrayBufferContents;
    if (name.starts_with("mpark::detail::base<"))
        return Reader::Variant;
    if (name == "JSC::HasOwnPropertyCache")
        return Reader::HasOwnPropertyCache;
    if (name == "WebCore::ImmutableStyleProperties")
        return Reader::ImmutableStyleProperties;
    if (name.starts_with("WTF::InlineMap<"))
        return Reader::InlineMap;
    if (name == "JSC::UnlinkedFunctionExecutable")
        return Reader::UnlinkedFunctionExecutable;
    if (name == "WebCore::CSSSelector")
        return Reader::CSSSelector;
    // WriteBarrierBase<Unknown>::m_value is its one member, an EncodedJSValue.
    if (name == "JSC::JSValue" || name.starts_with("JSC::WriteBarrierBase<JSC::Unknown,"))
        return Reader::JSValue;
    if (name == "WTF::StringImplShape")
        return Reader::StringImplShape;
    if (name == "JSC::HashTableValue")
        return Reader::HashTableValue;
    if (name == "JSC::PropertyCondition")
        return Reader::PropertyCondition;
    if (name == "JSC::InlineCacheHandler")
        return Reader::InlineCacheHandler;
    if (name == "JSC::JSCellButterfly")
        return Reader::CellButterfly;
    if (name == "WebCore::CSSPrimitiveValue")
        return Reader::CSSPrimitiveValue;
    if (name == "JSC::MarkedSpace")
        return Reader::MarkedSpace;
    if (name.starts_with("WTF::CompactPointerTuple<"))
        return Reader::CompactPointerTuple;
    if (name == "JSC::StructureID")
        return Reader::StructureID;
    if (name == "JSC::JSFunction")
        return Reader::Function;
    if (name == "JSC::StructureChain")
        return Reader::StructureChain;
    for (const HeapWalk::TypeFieldHierarchy& hierarchy : typeFieldHierarchyTable) {
        if (name == hierarchy.base)
            return Reader::TypeField;
    }
    if (name.starts_with("WebCore::CSS::PrimitiveData<"))
        return Reader::CSSPrimitiveData;
    return Reader::None;
}

bool ReachWalk::walkByName(const TargetValue& value)
{
    SetForScope contextObject { m_contextObject, std::pair<Address, const TargetType*> { value.address(), &value.type() } };
    switch (readerFor(value.type())) {
    case Reader::None:
        return false;
    case Reader::Vector:
        walkVector(value);
        return true;
    case Reader::HashTable:
        walkHashTable(value);
        return true;
    case Reader::RobinHoodHashTable:
        walkRobinHoodHashTable(value);
        return true;
    case Reader::TrailingArray:
        walkTrailingArray(value);
        return true;
    case Reader::ButterflyArray:
        walkButterflyArray(value);
        return true;
    case Reader::SegmentedVector:
        walkSegmentedVector(value);
        return true;
    case Reader::ConcurrentBufferArray:
        walkConcurrentBufferArray(value);
        return true;
    case Reader::SymbolTableEntry:
        walkSymbolTableEntry(value);
        return true;
    case Reader::CodeBlock:
        walkCodeBlock(value);
        return true;
    case Reader::AlignedStorage:
        walkAlignedStorage(value);
        return true;
    case Reader::CodePointer:
        walkCodePointer(value);
        return true;
    case Reader::LazyPointer:
        // LazyRef::lazyTag and initializingTag: a pointer to the function that will make the object.
        walkTaggedPointer(value, "m_pointer", 1, 0x3);
        return true;
    case Reader::CompactPointer:
        walkCompactPointer(value);
        return true;
    case Reader::PackedPointer:
        walkPackedPointer(value);
        return true;
    case Reader::JSString:
        walkJSString(value);
        return true;
    case Reader::PropertyTable:
        walkPropertyTable(value);
        return true;
    case Reader::Optional:
        walkOptional(value);
        return true;
    case Reader::StringImpl:
        walkStringImpl(value);
        return true;
    case Reader::ObjectButterfly:
        walkObjectButterfly(value);
        return true;
    case Reader::StrongBlock:
        walkStrongBlock(value);
        return true;
    case Reader::InlineWatchpointSet:
        walkInlineWatchpointSet(value);
        return true;
    case Reader::ArraySegment:
        walkArraySegment(value);
        return true;
    case Reader::WeakBlock:
        walkWeakBlock(value);
        return true;
    case Reader::ExpressionInfo:
        walkExpressionInfo(value);
        return true;
    case Reader::ArrayBufferView:
        walkArrayBufferView(value);
        return true;
    case Reader::ArrayBufferContents:
        walkArrayBufferContents(value);
        return true;
    case Reader::Variant:
        walkVariant(value);
        return true;
    case Reader::HasOwnPropertyCache:
        walkHasOwnPropertyCache(value);
        return true;
    case Reader::ImmutableStyleProperties:
        walkImmutableStyleProperties(value);
        return true;
    case Reader::InlineMap:
        walkInlineMap(value);
        return true;
    case Reader::UnlinkedFunctionExecutable:
        walkUnlinkedFunctionExecutable(value);
        return true;
    case Reader::CSSSelector:
        walkCSSSelector(value);
        return true;
    case Reader::JSValue:
        walkJSValue(value);
        return true;
    case Reader::StringImplShape:
        walkStringImplShape(value);
        return true;
    case Reader::HashTableValue:
        walkHashTableValue(value);
        return true;
    case Reader::PropertyCondition:
        walkPropertyCondition(value);
        return true;
    case Reader::InlineCacheHandler:
        // Its union holds the cells and the module environment slot the
        // handler's AccessCase chose (InlineCacheHandler::createPreCompiled),
        // which the handler does not record. The GC does not trace them through
        // the handler, and the walk reads every live cell.
        walkMembers(value, IsComplete::Yes, nullptr, ReadsOwnUnions::Yes);
        return true;
    case Reader::CSSPrimitiveValue:
        walkCSSPrimitiveValue(value);
        return true;
    case Reader::CSSPrimitiveData:
        walkCSSPrimitiveData(value);
        return true;
    case Reader::TypeField:
        walkTypeField(value);
        return true;
    case Reader::CompactPointerTuple:
        walkCompactPointerTuple(value);
        return true;
    case Reader::StructureID:
        walkStructureID(value);
        return true;
    case Reader::Function:
        walkFunction(value);
        return true;
    case Reader::StructureChain:
        walkStructureChain(value);
        return true;
    case Reader::MarkedSpace: {
        // MarkedSpace::prepareForMarking points these into m_preciseAllocations for a collection,
        // and leaves them as they were after it: the buffer may have moved, and the end is no element.
        static constexpr std::array<const char*, 2> collectionPointers { "m_preciseAllocationsForThisCollectionBegin", "m_preciseAllocationsForThisCollectionEnd" };
        walkMembers(value, IsComplete::Yes, collectionPointers);
        return true;
    }
    case Reader::CellButterfly:
        // A JSCellButterfly's IndexingHeader is always its lengths (JSCellButterfly::length).
        walkMembers(value, IsComplete::Yes, "m_header");
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The value of a std::optional, in the union of libstdc++'s _Optional_payload_base
// (_M_value) or libc++'s __optional_destruct_base (__val_), up to six bases and members down.
static const TargetType::Field* optionalValue(const TargetType& type, uint64_t& offset, unsigned depth = 0)
{
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    if (!klass || depth > 8)
        return nullptr;
    for (const TargetType::Field& field : klass->properFields) {
        std::string_view name { field.name.legacyCStringPointer() };
        if (name == "_M_value" || name == "__val_") {
            offset += field.offset;
            return &field;
        }
    }
    for (const TargetType::Field& field : klass->properFields) {
        uint64_t inner = 0;
        if (auto* found = optionalValue(field.type, inner, depth + 1)) {
            offset += field.offset + inner;
            return found;
        }
    }
    for (const TargetType::Base& base : klass->bases) {
        uint64_t inner = 0;
        if (auto* found = optionalValue(base.type, inner, depth + 1)) {
            offset += base.offset + inner;
            return found;
        }
    }
    return nullptr;
}

// Whether a std::optional holds a value: libstdc++'s
// _Optional_payload_base::_M_engaged or libc++'s
// __optional_destruct_base::__engaged_, up to six bases and members down.
static std::optional<bool> isEngagedOptional(const TargetValue& value, unsigned depth = 0)
{
    auto* klass = std::get_if<TargetType::Class>(&value.type().layout());
    if (!klass || depth > 8)
        return std::nullopt;
    for (const TargetType::Field& field : klass->properFields) {
        std::string_view name { field.name.legacyCStringPointer() };
        if (name == "_M_engaged" || name == "__engaged_") {
            auto engaged = value.field(field).integer();
            if (!engaged)
                return std::nullopt;
            return !!*engaged;
        }
    }
    for (const TargetType::Field& field : klass->properFields) {
        if (auto engaged = isEngagedOptional(value.field(field), depth + 1))
            return engaged;
    }
    for (const TargetType::Base& base : klass->bases) {
        if (auto engaged = isEngagedOptional(value.base(base), depth + 1))
            return engaged;
    }
    return std::nullopt;
}

// A std::optional's value only when it holds one. An empty optional's storage
// holds whatever was there before.
void ReachWalk::walkOptional(const TargetValue& optional)
{
    auto engaged = isEngagedOptional(optional);
    if (!engaged) {
        CORPSE_REPORT("The '%s' at 0x%llx has no engaged flag this walk knows", optional.type().name().legacyCStringPointer(), forReport(optional.address()));
        return;
    }
    if (!*engaged)
        return;
    uint64_t offset = 0;
    auto* value = optionalValue(optional.type(), offset);
    if (!value) {
        CORPSE_REPORT("The '%s' at 0x%llx has no value member this walk knows", optional.type().name().legacyCStringPointer(), forReport(optional.address()));
        return;
    }
    walkInPlace(optional.address() + offset, value->type, &optional.type(), value);
}

// Every element, not only the first.
void ReachWalk::walkVector(const TargetValue& vector)
{
    auto storage = vectorStorage(vector);
    if (!storage)
        return;
    auto address = storage->buffer.pointerValue();
    if (!address || !*address)
        return;
    // An inline buffer is in the Vector itself; any other is an allocation of its
    // own, which the Vector holds even with no elements in it.
    if (auto index = allocationOf(*address, 1))
        markReached(*index);
    const TargetType& element = std::get<TargetType::Pointer>(storage->buffer.type().layout()).pointee;
    if (!element.byteSize()) {
        if (storage->size)
            notFollowed(NotFollowed::Declaration);
        return;
    }
    walkElements(*address, element, storage->size);
}

// Every bucket, empty, deleted or live, as the bucket's type. An empty bucket
// holds its traits' empty value, and, in an ENABLE(MYA_HEAP) build, a deleted
// one holds its deleted key and zeros (hashTraitsDeleteBucket), so no bucket
// holds a stale address, and the table's traits are not needed.
void ReachWalk::walkHashTable(const TargetValue& table)
{
    walkMembers(table, IsComplete::Yes, "m_table");
    auto buckets = hashTableBuckets(table);
    if (!buckets || !buckets->size)
        return;
    auto address = buckets->table.pointerValue();
    if (!address)
        return;
    if (auto index = allocationOf(*address, 1))
        markReached(*index);
    const TargetType& bucket = std::get<TargetType::Pointer>(buckets->table.type().layout()).pointee;
    if (!bucket.byteSize()) {
        notFollowed(NotFollowed::Declaration);
        return;
    }
    walkElements(*address, bucket, buckets->size);
}

void ReachWalk::walkElements(Address address, const TargetType& element, uint64_t count)
{
    if (!count)
        return;
    if (!element.byteSize()) {
        notFollowed(NotFollowed::Declaration);
        return;
    }
    // Elements that lead nowhere, such as a buffer of integers, are read as one run.
    if (leadsNowhere(element)) {
        typed(address, count * element.byteSize(), element);
        return;
    }
    for (uint64_t index = 0; index < count; ++index)
        enqueue(TargetValue::at(m_snapshot, address + index * element.byteSize(), element), IsObject::Yes);
}

// Whether a value of `type` holds nothing the walk follows: no pointer, and nothing a reader reads.
bool ReachWalk::leadsNowhere(const TargetType& type)
{
    const TargetType::Layout& layout = type.layout();
    if (std::holds_alternative<TargetType::Integer>(layout) || std::holds_alternative<TargetType::Other>(layout))
        return true;
    if (auto* array = std::get_if<TargetType::Array>(&layout))
        return leadsNowhere(array->element);
    if (std::holds_alternative<TargetType::Class>(layout))
        return readerFor(type) == Reader::None && planFor(type, IsComplete::Yes, { }).isEmpty();
    return false;
}

// SegmentedVector::addressAt: the first InlineCapacity elements in
// m_inlineStorageMember, then the segments of m_segments, sizeOfSegment(i)
// elements each, m_size in all. It has no other members.
void ReachWalk::walkSegmentedVector(const TargetValue& vector)
{
    const TargetType* element = vector.type().templateArgument(0);
    auto segmentSize = vector.type().templateIntegerArgument(1);
    auto inlineCapacity = vector.type().templateIntegerArgument(2);
    auto growthPolicy = vector.type().templateIntegerArgument(3);
    auto size = vector.properField("m_size").integer();
    if (!element || !segmentSize || !*segmentSize || !inlineCapacity || !growthPolicy || !size) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    uint64_t remaining = static_cast<uint64_t>(*size);
    if (*inlineCapacity) {
        // InlineStorageData::m_data, AlignedStorage<T>s, whose slots past m_size hold no element.
        uint64_t count = std::min(remaining, *inlineCapacity);
        walkElements(vector.properField("m_inlineStorageMember").address(), *element, count);
        remaining -= count;
    }
    auto storage = vectorStorage(vector.properField("m_segments"));
    auto segments = storage ? storage->buffer.pointerValue() : std::nullopt;
    if (!segments || !*segments)
        return;
    if (auto index = allocationOf(*segments, 1))
        markReached(*index);
    bool doubling = *growthPolicy == static_cast<uint64_t>(SegmentedVectorGrowthPolicy::Doubling);
    for (size_t segmentIndex = 0; segmentIndex < storage->size && remaining; ++segmentIndex) {
        // SegmentedVector::sizeOfSegment.
        uint64_t capacity = doubling ? *segmentSize << std::min<size_t>(segmentIndex, 48) : *segmentSize;
        // A SegmentPtr is a unique_ptr with an empty deleter: the segment's address.
        auto segment = m_snapshot.memory().ptr<uint64_t>(*segments + segmentIndex * sizeof(uint64_t));
        if (!segment || !*segment)
            return;
        if (auto index = allocationOf(Address { *segment }, 1))
            markReached(*index);
        uint64_t count = std::min(capacity, remaining);
        walkElements(Address { *segment }, *element, count);
        remaining -= count;
    }
}

// ConcurrentBuffer::Array: `size` elements in `data`, which is declared with one.
void ReachWalk::walkConcurrentBufferArray(const TargetValue& array)
{
    auto size = array.properField("size").integer();
    TargetValue data = array.properField("data");
    auto* elements = data ? std::get_if<TargetType::Array>(&data.type().layout()) : nullptr;
    if (!size || !elements)
        return;
    if (*size < 0 || static_cast<uint64_t>(*size) > maxVectorSize) {
        CORPSE_REPORT("The ConcurrentBuffer array at 0x%llx claims %lld elements", forReport(array.address()), static_cast<long long>(*size));
        return;
    }
    walkElements(data.address(), elements->element, static_cast<uint64_t>(*size));
}

// SymbolTableEntry::m_bits: a FatEntry*, unless SymbolTableEntry::SlimFlag is set (SymbolTableEntry::isFat).
void ReachWalk::walkSymbolTableEntry(const TargetValue& entry)
{
    constexpr uint64_t slimFlag = 1; // SymbolTableEntry::SlimFlag, which is private.
    auto bits = entry.properField("m_bits").integer();
    if (!bits || !*bits || (static_cast<uint64_t>(*bits) & slimFlag))
        return;
    follow(Address { static_cast<uint64_t>(*bits) }, *m_heap.m_fatEntryClass);
}

// RobinHoodHashTable: every bucket of m_table, m_tableSize of them. It has no
// deleted buckets: a removal shifts the buckets after it back, and an empty
// bucket holds its traits' empty value.
void ReachWalk::walkRobinHoodHashTable(const TargetValue& table)
{
    walkMembers(table, IsComplete::Yes, "m_table");
    TargetValue buckets = table.properField("m_table");
    auto address = buckets.pointerValue();
    auto size = table.properField("m_tableSize").integer();
    if (!address || !*address || !size)
        return;
    if (*size < 0 || static_cast<uint64_t>(*size) > maxHashTableSize) {
        CORPSE_REPORT("The RobinHoodHashTable at 0x%llx claims %lld buckets", forReport(table.address()), static_cast<long long>(*size));
        return;
    }
    if (auto index = allocationOf(*address, 1))
        markReached(*index);
    walkElements(*address, std::get<TargetType::Pointer>(buckets.type().layout()).pointee, static_cast<uint64_t>(*size));
}

// TrailingArray<Derived, T>, a base of Derived: m_size elements of T after the
// Derived object, at TrailingArray::offsetOfData().
void ReachWalk::walkTrailingArray(const TargetValue& array)
{
    walkMembers(array, IsComplete::No);
    const TargetType* derived = array.type().templateArgument(0);
    const TargetType* element = array.type().templateArgument(1);
    auto size = array.properField("m_size").integer();
    if (!derived || !element || !element->alignment()) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (!size || *size < 0 || static_cast<uint64_t>(*size) > maxVectorSize)
        return;
    auto offset = baseOffset(*derived, array.type());
    if (!offset) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    Address data = array.address() - *offset + roundUpToMultipleOf(element->alignment(), derived->byteSize());
    walkElements(data, *element, static_cast<uint64_t>(*size));
}

// ButterflyArray<Derived, LeadingType, TrailingType>, a base of Derived:
// m_leadingSize elements of LeadingType just before the Derived object, and
// m_trailingSize of TrailingType after it, at offsetOfTrailingData().
void ReachWalk::walkButterflyArray(const TargetValue& array)
{
    walkMembers(array, IsComplete::No);
    const TargetType* derived = array.type().templateArgument(0);
    const TargetType* leading = array.type().templateArgument(1);
    const TargetType* trailing = array.type().templateArgument(2);
    auto leadingSize = array.properField("m_leadingSize").integer();
    auto trailingSize = array.properField("m_trailingSize").integer();
    if (!derived || !leading || !trailing || !trailing->alignment()) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    auto offset = baseOffset(*derived, array.type());
    if (!offset) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (!leadingSize || !trailingSize || *leadingSize < 0 || *trailingSize < 0 || static_cast<uint64_t>(*leadingSize) > maxVectorSize || static_cast<uint64_t>(*trailingSize) > maxVectorSize)
        return;
    Address object = array.address() - *offset;
    walkElements(object - static_cast<uint64_t>(*leadingSize) * leading->byteSize(), *leading, static_cast<uint64_t>(*leadingSize));
    walkElements(object + roundUpToMultipleOf(trailing->alignment(), derived->byteSize()), *trailing, static_cast<uint64_t>(*trailingSize));
}

// CodeBlock::m_jitData, a void*: CodeBlock::baselineJITData() or dfgJITData(),
// by whether the JITCode's type is an optimizing tier.
void ReachWalk::walkCodeBlock(const TargetValue& codeBlock)
{
    walkMembers(codeBlock, IsComplete::No, "m_jitData");
    auto jitData = codeBlock.properField("m_jitData").pointerValue();
    if (!jitData || !*jitData)
        return;
    TargetValue jitCode = codeBlock.properField("m_jitCode").properField("m_ptr").dereference();
    auto jitType = jitCode.properField("m_jitType").integer();
    if (!jitType)
        return;
    const TargetType* type = JITCode::isOptimizingJIT(static_cast<JITType>(*jitType)) ? m_heap.m_dfgJITDataClass : m_heap.m_baselineJITDataClass;
    if (!type) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    follow(*jitData, *type);
}

// AlignedStorage::get(), which NeverDestroyed and LazyNeverDestroyed hold their
// object in: m_storage, as the first template argument. A LazyNeverDestroyed
// not yet constructed is zeros, which lead nowhere.
void ReachWalk::walkAlignedStorage(const TargetValue& storage)
{
    TargetValue bytes = storage.properField("m_storage");
    const TargetType* type = storage.type().templateArgument(0);
    if (!bytes)
        return;
    if (!type || !type->byteSize()) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    TargetValue value = TargetValue::at(m_snapshot, bytes.address(), *type);
    if (std::holds_alternative<TargetType::Class>(type->layout()))
        enqueue(value, IsObject::Yes);
    else
        walk(value);
}

// CodePtr::m_value: a tagged pointer into JIT code, which has no type. It
// reaches the allocation it is in, whose bytes stay untyped. It is CodePtr's
// one member, read whole: liblldb describes some instances of CodePtr as empty
// classes.
void ReachWalk::walkCodePointer(const TargetValue& codePointer)
{
    auto value = m_snapshot.memory().ptr<uint64_t>(codePointer.address());
    if (!value || !*value)
        return;
    if (auto index = allocationOf(Address { *value }.stripped(), 1))
        markReached(*index);
}

// A pointer to the type of a template argument, held in an integer field with tag bits.
void ReachWalk::walkTaggedPointer(const TargetValue& value, const char* field, unsigned templateArgument, uint64_t tagMask)
{
    auto bits = value.properField(field).integer();
    if (!bits || !*bits || (static_cast<uint64_t>(*bits) & tagMask))
        return;
    const TargetType* pointee = value.type().templateArgument(templateArgument);
    if (!pointee) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    followPointer(Address { static_cast<uint64_t>(*bits) }, *pointee);
}

// CompactPtr::decode.
void ReachWalk::walkCompactPointer(const TargetValue& value)
{
    auto bits = value.properField("m_ptr").integer();
    if (!bits || !*bits)
        return;
    uint64_t address = static_cast<uint64_t>(*bits);
#if HAVE(36BIT_ADDRESS)
    // An outsized pointer is encoded through a side table, which the walk does not read.
    if (address & OutsizedCompactPtr::isOutsizedBit) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    address = static_cast<uint64_t>(static_cast<uint32_t>(address)) << CompactPtr<void>::bitsShift;
#endif
    const TargetType* pointee = value.type().templateArgument(0);
    if (!pointee) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    followPointer(Address { address }, *pointee);
}

// PackedAlignedPtr::get: the low bytes of the pointer, in m_storage, stored
// shifted right by the alignment when that saves a byte.
void ReachWalk::walkPackedPointer(const TargetValue& value)
{
    TargetValue storage = value.properField("m_storage");
    if (!storage)
        return;
    const TargetType* pointee = value.type().templateArgument(0);
    auto alignment = value.type().templateIntegerArgument(1);
    constexpr unsigned addressWidth = OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH);
    if (!pointee || !alignment || !hasOneBitSet(*alignment) || getLSBSet(*alignment) >= addressWidth) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    // PackedAlignedPtr's storageSizeWithoutAlignmentShift, storageSize and alignmentShiftSize.
    unsigned shiftIfProfitable = getLSBSet(*alignment);
    size_t sizeWithoutShift = roundUpToMultipleOf<8>(addressWidth) / 8;
    size_t sizeWithShift = roundUpToMultipleOf<8>(addressWidth - shiftIfProfitable) / 8;
    unsigned shift = sizeWithoutShift > sizeWithShift ? shiftIfProfitable : 0;
    size_t size = storage.type().byteSize();
    if (size != sizeWithShift) {
        CORPSE_REPORT("The %zu-byte PackedAlignedPtr at 0x%llx, aligned to %llu, is not the %zu bytes this build stores", size, forReport(value.address()), static_cast<unsigned long long>(*alignment), sizeWithShift);
        return;
    }
    auto bytes = m_snapshot.memory().span<uint8_t>(storage.address(), size);
    if (!bytes)
        return;
    uint64_t address = 0;
    memcpySpan(asMutableByteSpan(address).first(size), std::span<const uint8_t> { bytes });
    if (address)
        followPointer(Address { address << shift }, *pointee);
}

// PropertyTable::m_indexVector: the table's index buffer, with
// PropertyTable::isCompactFlag. The buffer is one allocation, which holds the
// indices and then the entries, of a type that depends on the flag.
void ReachWalk::walkPropertyTable(const TargetValue& table)
{
    walkMembers(table, IsComplete::Yes, "m_indexVector");
    auto bits = table.properField("m_indexVector").integer();
    if (!bits || !*bits)
        return;
    // PropertyTable::isCompactFlag, which is private.
    constexpr uint64_t isCompactFlag = 1;
    bool isCompact = static_cast<uint64_t>(*bits) & isCompactFlag;
    Address buffer { static_cast<uint64_t>(*bits) & ~isCompactFlag };
    if (auto index = allocationOf(buffer, 1))
        markReached(*index);
    // PropertyTable::dataSize, which is private: m_indexSize indices, then (m_indexSize >> 1) + 1 entries.
    auto indexSize = table.properField("m_indexSize").integer();
    const TargetType* entry = isCompact ? m_heap.m_compactPropertyTableEntryClass : m_heap.m_propertyTableEntryClass;
    const TargetType* indexType = isCompact ? m_heap.m_byteType : &table.properField("m_indexSize").type();
    if (!indexSize || *indexSize <= 0 || static_cast<uint64_t>(*indexSize) > maxHashTableSize || !entry || !indexType)
        return;
    uint64_t indexBytes = static_cast<uint64_t>(*indexSize) * indexType->byteSize();
    typed(buffer, indexBytes, *indexType);
    walkElements(buffer + indexBytes, *entry, (static_cast<uint64_t>(*indexSize) >> 1) + 1);
}

// GCArraySegment::data(): the slots after the segment, to blockSize, of which
// the GCSegmentedArray uses only the slots below its top; the others hold
// whatever they held, so the walk reads them as their type and follows none.
void ReachWalk::walkArraySegment(const TargetValue& segment)
{
    walkMembers(segment, IsComplete::Yes);
    const TargetType* element = segment.type().templateArgument(0);
    uint64_t headerSize = segment.type().byteSize();
    if (!element || !element->byteSize() || headerSize >= GCArraySegment<const JSCell*>::blockSize)
        return;
    typed(segment.address() + headerSize, (GCArraySegment<const JSCell*>::blockSize - headerSize) / element->byteSize() * element->byteSize(), *element);
}

// WeakBlock::weakImpls(): weakImplCount() WeakImpls after the block, to blockSize.
void ReachWalk::walkWeakBlock(const TargetValue& block)
{
    walkMembers(block, IsComplete::Yes);
    const TargetType* weakImpl = m_heap.m_weakImplClass;
    if (!weakImpl || !weakImpl->byteSize())
        return;
    uint64_t first = (block.type().byteSize() + weakImpl->byteSize() - 1) / weakImpl->byteSize();
    uint64_t capacity = WeakBlock::blockSize / weakImpl->byteSize();
    if (first < capacity)
        walkElements(block.address() + first * weakImpl->byteSize(), *weakImpl, capacity - first);
}

// ExpressionInfo::chapters() and encodedInfo(): m_numberOfChapters Chapters
// after the ExpressionInfo, then m_numberOfEncodedInfo and
// m_numberOfEncodedInfoExtensions EncodedInfos.
void ReachWalk::walkExpressionInfo(const TargetValue& info)
{
    walkMembers(info, IsComplete::Yes);
    auto chapters = info.properField("m_numberOfChapters").integer();
    auto encodedInfo = info.properField("m_numberOfEncodedInfo").integer();
    auto extensions = info.properField("m_numberOfEncodedInfoExtensions").integer();
    const TargetType* chapter = m_heap.m_expressionInfoChapterClass;
    const TargetType* encoded = m_heap.m_expressionInfoEncodedInfoClass;
    if (!chapters || !encodedInfo || !extensions || !chapter || !encoded || *chapters < 0 || *encodedInfo < 0 || *extensions < 0
        || static_cast<uint64_t>(*chapters) > maxVectorSize || static_cast<uint64_t>(*encodedInfo + *extensions) > maxVectorSize)
        return;
    Address start = info.address() + info.type().byteSize();
    walkElements(start, *chapter, static_cast<uint64_t>(*chapters));
    walkElements(start + static_cast<uint64_t>(*chapters) * chapter->byteSize(), *encoded, static_cast<uint64_t>(*encodedInfo + *extensions));
}

// JSString::m_fiber: a resolved string's String, whose StringImpl it holds,
// or, with JSString::isRopeInPointer set, a rope's first fiber, a cell.
void ReachWalk::walkJSString(const TargetValue& string)
{
    walkMembers(string, IsComplete::Yes, "m_fiber");
    auto fiber = string.properField("m_fiber").integer();
    if (!fiber || !*fiber || (static_cast<uint64_t>(*fiber) & JSString::isRopeInPointer))
        return;
    follow(Address { static_cast<uint64_t>(*fiber) }, *m_heap.m_stringImplClass);
}

// StringImpl's characters: in the StringImpl itself, at tailOffset(), for a
// BufferInternal one whose m_data8 points there; in a buffer of their own, or a
// literal, which m_data8 points to, for the others; and in another StringImpl,
// which the tail points to, for BufferSubstring.
// StringImpl's s_hashMaskBufferOwnership and s_hashFlag8BitBuffer, which are private.
static constexpr uint32_t stringImplBufferOwnershipMask = 3;
static constexpr uint32_t stringImplIs8BitFlag = 1u << 2;

auto ReachWalk::stringImplLayout(const TargetType& type) -> const StringImplLayout*
{
    const auto& layout = m_stringImplLayouts.ensure(&type, [&]() -> std::optional<StringImplLayout> {
        uint64_t lengthOffset = 0;
        uint64_t flagsOffset = 0;
        uint64_t data8Offset = 0;
        uint64_t data16Offset = 0;
        auto* length = findField(type, "m_length", lengthOffset);
        auto* flags = findField(type, "m_hashAndFlags", flagsOffset);
        auto* data8 = findField(type, "m_data8", data8Offset);
        auto* data16 = findField(type, "m_data16", data16Offset);
        auto* latin1 = data8 ? std::get_if<TargetType::Pointer>(&data8->type.layout()) : nullptr;
        auto* utf16 = data16 ? std::get_if<TargetType::Pointer>(&data16->type.layout()) : nullptr;
        if (!length || !flags || !latin1 || !utf16 || !latin1->pointee.byteSize() || !utf16->pointee.byteSize())
            return std::nullopt;
        return StringImplLayout { lengthOffset + length->offset, flagsOffset + flags->offset, data8Offset + data8->offset, &latin1->pointee, &utf16->pointee };
    }).iterator->value;
    return layout ? &*layout : nullptr;
}

// StringImplShape's characters: m_data8 or, without s_hashFlag8BitBuffer, m_data16 (StringImpl::is8Bit).
void ReachWalk::walkStringImplShape(const TargetValue& shape)
{
    walkMembers(shape, IsComplete::Yes, nullptr, ReadsOwnUnions::Yes);
    const StringImplLayout* layout = stringImplLayout(shape.type());
    auto flags = layout ? m_snapshot.memory().ptr<uint32_t>(shape.address() + layout->flagsOffset) : Memory::Ptr<uint32_t> { };
    auto data = layout ? m_snapshot.memory().ptr<uint64_t>(shape.address() + layout->dataOffset) : Memory::Ptr<uint64_t> { };
    if (!flags || !data || !*data)
        return;
    m_contextClass = &shape.type();
    m_contextField = nullptr;
    followPointer(Address { *data }.stripped(), (*flags & stringImplIs8BitFlag) ? *layout->latin1 : *layout->utf16);
}

void ReachWalk::walkStringImpl(const TargetValue& string)
{
    walkMembers(string, IsComplete::Yes);
    const StringImplLayout* layout = stringImplLayout(string.type());
    if (!layout)
        return;
    auto lengthValue = m_snapshot.memory().ptr<uint32_t>(string.address() + layout->lengthOffset);
    auto flagsValue = m_snapshot.memory().ptr<uint32_t>(string.address() + layout->flagsOffset);
    if (!lengthValue || !flagsValue)
        return;
    // StringImpl::tailOffset.
    uint64_t tail = layout->flagsOffset + sizeof(uint32_t);
    switch (*flagsValue & stringImplBufferOwnershipMask) {
    case WTF::StringImpl::BufferInternal: {
        const TargetType& character = (*flagsValue & stringImplIs8BitFlag) ? *layout->latin1 : *layout->utf16;
        tail = roundUpToMultipleOf(character.alignment(), tail);
        auto data = m_snapshot.memory().ptr<uint64_t>(string.address() + layout->dataOffset);
        if (data && Address { *data }.stripped() == string.address() + tail)
            typed(string.address() + tail, static_cast<uint64_t>(*lengthValue) * character.byteSize(), character);
        return;
    }
    case WTF::StringImpl::BufferSubstring:
        if (auto parent = m_snapshot.memory().ptr<uint64_t>(string.address() + roundUpToMultipleOf<alignof(void*)>(tail)); parent && *parent)
            follow(Address { *parent }.stripped(), string.type());
        return;
    default:
        return;
    }
}

// JSObjectWithButterfly::m_butterfly: out-of-line properties before the
// butterfly, and an indexing header and indexed properties from it, in one
// auxiliary cell, each word a JSValue, or a double in a double array.
void ReachWalk::walkObjectButterfly(const TargetValue& object)
{
    walkMembers(object, IsComplete::No, "m_butterfly");
    const auto& offset = m_butterflyOffsets.ensure(&object.type(), [&]() -> std::optional<uint64_t> {
        TargetValue pointer = object.properField("m_butterfly").properField("m_value");
        if (!pointer)
            return std::nullopt;
        return pointer.address() - object.address();
    }).iterator->value;
    auto word = offset ? m_snapshot.memory().ptr<uint64_t>(object.address() + *offset) : Memory::Ptr<uint64_t> { };
    if (!word || !*word)
        return;
    std::optional<Address> butterfly = Address { *word }.stripped();
    // Butterfly::base: the out-of-line properties end where the IndexingHeader
    // starts, sizeof(IndexingHeader) before the butterfly, which is in the
    // allocation only if Structure::hasIndexingHeader.
    auto indexingType = integerField(object, "m_indexingTypeAndMisc");
    auto cellType = integerField(object, "m_type");
    if (!indexingType || !cellType)
        return;
    bool hasIndexingHeader = hasIndexedProperties(static_cast<IndexingType>(*indexingType));
    if (!hasIndexingHeader && isTypedView(static_cast<JSType>(*cellType))) {
        const TargetType* view = m_heap.cellClass(object.address());
        auto mode = view ? integerField(TargetValue::at(m_snapshot, object.address(), *view), "m_mode") : std::nullopt;
        hasIndexingHeader = mode && isWastefulTypedArray(static_cast<TypedArrayMode>(*mode));
    }
    const HeapWalk::Cell* cell = auxiliaryCellContaining(*butterfly - sizeof(IndexingHeader) - (hasIndexingHeader ? 0 : 1));
    if (!cell)
        return;
    if (auto index = allocationOf(cell->address, 1))
        markReached(*index);
    typed(cell->address, cell->size - cell->size % sizeof(EncodedJSValue), *m_heap.m_jsValueClass);
    referenceJSValues(object.address(), cell->address, cell->size - cell->size % sizeof(EncodedJSValue));
}

// StrongBlock: its header, then a JSValue slot for each Strong handle, to the end of the block.
void ReachWalk::walkStrongBlock(const TargetValue& block)
{
    walkMembers(block, IsComplete::Yes);
    uint64_t headerSize = block.type().byteSize();
    if (headerSize < StrongBlock::blockSize)
        typed(block.address() + headerSize, StrongBlock::blockSize - headerSize, *m_heap.m_jsValueClass);
    // Strong handles are roots of their own.
    referenceJSValues({ }, block.address() + headerSize, StrongBlock::blockSize - headerSize);
}

// InlineWatchpointSet::m_data: a fat WatchpointSet*, unless IsThinFlag is set (InlineWatchpointSet::isFat).
void ReachWalk::walkInlineWatchpointSet(const TargetValue& set)
{
    constexpr uint64_t isThinFlag = 1; // InlineWatchpointSet::IsThinFlag, which is private.
    auto bits = set.properField("m_data").integer();
    if (!bits || !*bits || (static_cast<uint64_t>(*bits) & isThinFlag))
        return;
    followPointer(Address { static_cast<uint64_t>(*bits) }.stripped(), *m_heap.m_watchpointSetClass);
}

std::optional<Address> ReachWalk::rawPointer(const TargetValue& value, const char* field)
{
    TargetValue member = value.properField(field);
    if (!member)
        return std::nullopt;
    auto word = m_snapshot.memory().ptr<uint64_t>(member.address());
    if (!word || !*word)
        return std::nullopt;
    return Address { *word }.stripped();
}

// JSArrayBufferView::m_vector, a CagedBarrierPtr<Gigacage::Primitive, void>: a
// FastTypedArray's elements, in an auxiliary cell of their own, which are
// bytes; or an ArrayBuffer's, which its ArrayBufferContents reads.
void ReachWalk::walkArrayBufferView(const TargetValue& view)
{
    walkMembers(view, IsComplete::No);
    auto vector = rawPointer(view, "m_vector");
    if (!vector)
        return;
    if (const HeapWalk::Cell* cell = auxiliaryCellContaining(*vector)) {
        if (auto index = allocationOf(cell->address, 1))
            markReached(*index);
        typed(cell->address, cell->size, *m_heap.m_byteType);
    }
}

// ArrayBufferContents::m_data, a CagedPtr<Gigacage::Primitive, void>: m_sizeInBytes bytes.
void ReachWalk::walkArrayBufferContents(const TargetValue& contents)
{
    walkMembers(contents, IsComplete::Yes);
    auto data = rawPointer(contents, "m_data");
    TargetValue sizeField = contents.properField("m_sizeInBytes");
    auto size = sizeField ? m_snapshot.memory().ptr<uint64_t>(sizeField.address()) : Memory::Ptr<uint64_t> { };
    if (!data || !size || !*size)
        return;
    auto index = allocationOf(*data, 1);
    if (!index)
        return;
    markReached(*index);
    const HeapWalk::Allocation& allocation = m_allocations[*index];
    typed(*data, std::min<uint64_t>(*size, (allocation.address + allocation.size) - *data), *m_heap.m_byteType);
}

// mpark::detail::base, which WTF::Variant is: the alternative index_ names, of
// the template argument of that index, in the bytes of data_. A valueless
// variant's index_ is all ones.
void ReachWalk::walkVariant(const TargetValue& variant)
{
    auto index = variant.properField("index_").integer();
    TargetValue data = variant.properField("data_");
    if (!index || !data || *index < 0)
        return;
    const TargetType* alternative = variant.type().templateArgument(static_cast<unsigned>(*index));
    if (!alternative) {
        if (*index != static_cast<int64_t>((1ull << (8 * variant.properField("index_").type().byteSize())) - 1))
            notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (alternative->byteSize() > data.type().byteSize()) {
        CORPSE_REPORT("The %zu-byte alternative %lld of the variant at 0x%llx is bigger than its storage", alternative->byteSize(), static_cast<long long>(*index), forReport(variant.address()));
        return;
    }
    walkInPlace(data.address(), *alternative, &variant.type(), nullptr);
}

// HasOwnPropertyCache: HasOwnPropertyCache::size Entries, from its address; the class itself has no members.
void ReachWalk::walkHasOwnPropertyCache(const TargetValue& cache)
{
    constexpr uint64_t size = 2 * 1024; // HasOwnPropertyCache::size, which is private.
    if (auto* entry = classBeside(cache.type(), "JSC::HasOwnPropertyCache::Entry"))
        walkElements(cache.address(), *entry, size);
}

// ImmutableStyleProperties::metadataSpan() and valueSpan(): m_arraySize
// StylePropertyMetadatas at m_storage, then as many PackedPtr<const CSSValue>s.
void ReachWalk::walkImmutableStyleProperties(const TargetValue& properties)
{
    walkMembers(properties, IsComplete::Yes, "m_storage");
    uint64_t sizeOffset = 0;
    const TargetType* declaringClass = nullptr;
    auto* arraySize = findField(properties.type(), "m_arraySize", sizeOffset, &declaringClass);
    TargetValue storage = properties.properField("m_storage");
    auto* metadata = classBeside(properties.type(), "WebCore::StylePropertyMetadata");
    auto* value = classBeside(properties.type(), "WTF::Packed<const WebCore::CSSValue *>");
    if (!arraySize || !storage || !metadata || !value)
        return;
    auto count = TargetValue::at(m_snapshot, properties.address() + sizeOffset, *declaringClass).field(*arraySize).integer();
    if (!count || *count < 0 || static_cast<uint64_t>(*count) > maxVectorSize)
        return;
    walkElements(storage.address(), *metadata, static_cast<uint64_t>(*count));
    walkElements(storage.address() + static_cast<uint64_t>(*count) * metadata->byteSize(), *value, static_cast<uint64_t>(*count));
}

// InlineMap::isInline(): with m_capacity at its InlineCapacity, m_size entries
// in m_storage's inlineEntries; otherwise m_capacity entries, empty, deleted or
// live, at m_storage's hashedData.entries.
void ReachWalk::walkInlineMap(const TargetValue& map)
{
    walkMembers(map, IsComplete::Yes, "m_storage");
    auto capacity = map.properField("m_capacity").integer();
    auto size = map.properField("m_size").integer();
    auto inlineCapacity = map.type().templateIntegerArgument(2);
    TargetValue storage = map.properField("m_storage");
    TargetValue entries = storage.properField("hashedData").properField("entries");
    auto* entry = entries ? std::get_if<TargetType::Pointer>(&entries.type().layout()) : nullptr;
    if (!capacity || !size || !inlineCapacity || !entry || *capacity < 0 || *size < 0 || static_cast<uint64_t>(*capacity) > maxHashTableSize) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (static_cast<uint64_t>(*capacity) == *inlineCapacity) {
        walkElements(storage.address(), entry->pointee, std::min<uint64_t>(static_cast<uint64_t>(*size), *inlineCapacity));
        return;
    }
    auto address = entries.pointerValue();
    if (!address || !*address)
        return;
    if (auto index = allocationOf(*address, 1))
        markReached(*index);
    walkElements(*address, entry->pointee, static_cast<uint64_t>(*capacity));
}

void ReachWalk::walkUnionMember(const TargetValue& holder, const char* unionField, const char* member)
{
    uint64_t offset = 0;
    const TargetType* within = &holder.type();
    if (unionField) {
        auto* field = findField(*within, unionField, offset);
        if (!field) {
            CORPSE_REPORT("The '%s' at 0x%llx has no union '%s'", holder.type().name().legacyCStringPointer(), forReport(holder.address()), unionField);
            return;
        }
        offset += field->offset;
        within = &field->type;
    }
    uint64_t memberOffset = 0;
    const TargetType* declaringClass = nullptr;
    auto* field = findField(*within, member, memberOffset, &declaringClass);
    if (!field) {
        CORPSE_REPORT("The '%s' at 0x%llx has no union member '%s'", holder.type().name().legacyCStringPointer(), forReport(holder.address()), member);
        return;
    }
    walkInPlace(holder.address() + offset + memberOffset + field->offset, field->type, declaringClass, field);
}

std::optional<int64_t> ReachWalk::integerField(const TargetValue& value, const char* name)
{
    uint64_t offset = 0;
    const TargetType* declaringClass = nullptr;
    auto* field = findField(value.type(), name, offset, &declaringClass);
    if (!field)
        return std::nullopt;
    return TargetValue::at(m_snapshot, value.address() + offset, *declaringClass).field(*field).integer();
}

// UnlinkedFunctionExecutable's unions: m_decoder, and the cached code blocks'
// offsets, with m_isCached set; m_unlinkedCodeBlockForCall and
// m_unlinkedCodeBlockForConstruct otherwise (UnlinkedFunctionExecutable::visitChildrenImpl).
void ReachWalk::walkUnlinkedFunctionExecutable(const TargetValue& executable)
{
    walkMembers(executable, IsComplete::Yes, nullptr, ReadsOwnUnions::Yes);
    auto isCached = integerField(executable, "m_isCached");
    if (!isCached) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (*isCached) {
        walkUnionMember(executable, nullptr, "m_decoder");
        return;
    }
    walkUnionMember(executable, nullptr, "m_unlinkedCodeBlockForCall");
    walkUnionMember(executable, nullptr, "m_unlinkedCodeBlockForConstruct");
}

// CSSSelector::m_data: rareData with m_hasRareData set, else tagQName for a
// Match::Tag selector, else value (CSSSelector::~CSSSelector).
void ReachWalk::walkCSSSelector(const TargetValue& selector)
{
    walkMembers(selector, IsComplete::Yes, nullptr, ReadsOwnUnions::Yes);
    auto hasRareData = integerField(selector, "m_hasRareData");
    auto match = integerField(selector, "m_match");
    auto* matchType = classBeside(selector.type(), "WebCore::CSSSelector::Match");
    auto tag = matchType ? matchType->enumeratorValue("Tag") : std::nullopt;
    if (!hasRareData || !match || !tag) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    walkUnionMember(selector, "m_data", *hasRareData ? "rareData" : *match == *tag ? "tagQName" : "value");
}

// JSValue::u: a cell, as JSValue::isCell decodes it, reaches its allocation.
// The walk reads every live cell, as its class, of its own.
void ReachWalk::walkJSValue(const TargetValue& value)
{
    auto bits = m_snapshot.memory().ptr<EncodedJSValue>(value.address());
    if (!bits || !*bits)
        return;
    JSValue decoded = JSValue::decode(*bits);
    if (!decoded.isCell())
        return;
    Address cell { static_cast<uint64_t>(*bits) };
    if (auto index = allocationOf(cell, 1))
        markReached(*index, cell);
}

// HashTableValue::m_values: the member its accessors read for its m_attributes
// (HashTableValue::builtinGenerator, function, accessorGetter, and so on).
void ReachWalk::walkHashTableValue(const TargetValue& entry)
{
    walkMembers(entry, IsComplete::Yes, nullptr, ReadsOwnUnions::Yes);
    auto attributes = integerField(entry, "m_attributes");
    auto* attribute = classBeside(entry.type(), "JSC::PropertyAttribute");
    if (!attributes || !attribute) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    auto has = [&](std::string_view name) {
        auto bit = attribute->enumeratorValue(name);
        return bit && (*attributes & *bit);
    };
    const char* member = "getterSetter";
    if (has("Builtin"))
        member = has("Accessor") ? "builtinAccessor" : "builtinGenerator";
    else if (has("Function"))
        member = has("DOMJITFunction") ? "domJITFunction" : "nativeFunction";
    else if (has("Accessor"))
        member = "accessor";
    else if (has("DOMJITAttribute"))
        member = "domJITAttribute";
    else if (has("ConstantInteger"))
        member = "constant";
    else if (has("CellProperty"))
        member = "lazyCellProperty";
    else if (has("ClassStructure"))
        member = "lazyClassStructure";
    else if (has("PropertyCallback"))
        member = "lazyProperty";
    walkUnionMember(entry, "m_values", member);
}

void ReachWalk::referenceCell(Address cell)
{
    if (auto index = allocationOf(cell, 1))
        markReached(*index, cell);
}

// StructureID::decode: the start of the structure heap, plus the bits less StructureID::nukedStructureIDBit.
void ReachWalk::walkStructureID(const TargetValue& structureID)
{
    auto bits = m_snapshot.memory().ptr<uint32_t>(structureID.address());
    if (bits && (*bits & ~StructureID::nukedStructureIDBit))
        referenceCell(Address { m_heap.m_startOfStructureHeap + (*bits & ~StructureID::nukedStructureIDBit) });
}

// JSFunction::m_executableOrRareData: its FunctionRareData, with JSFunction::rareDataTag, or else its executable.
void ReachWalk::walkFunction(const TargetValue& function)
{
    walkMembers(function, IsComplete::No, "m_executableOrRareData");
    constexpr uint64_t rareDataTag = 1; // JSFunction::rareDataTag.
    auto bits = function.properField("m_executableOrRareData").integer();
    if (bits && *bits)
        referenceCell(Address { static_cast<uint64_t>(*bits) & ~rareDataTag });
}

// StructureChain::head(): m_vector's StructureIDs, in an auxiliary cell, up to the first zero.
void ReachWalk::walkStructureChain(const TargetValue& chain)
{
    walkMembers(chain, IsComplete::Yes, "m_vector");
    auto vector = rawPointer(chain.properField("m_vector"), "m_value");
    const HeapWalk::Cell* cell = vector ? auxiliaryCellContaining(*vector) : nullptr;
    if (!cell)
        return;
    if (auto index = allocationOf(cell->address, 1))
        markReached(*index, *vector);
    auto ids = m_snapshot.memory().span<uint32_t>(*vector, static_cast<size_t>(((cell->address + cell->size) - *vector) / sizeof(uint32_t)));
    if (!ids)
        return;
    uint64_t count = 0;
    for (uint32_t bits : std::span<const uint32_t> { ids }) {
        if (!bits)
            break;
        ++count;
        referenceCell(Address { m_heap.m_startOfStructureHeap + (bits & ~StructureID::nukedStructureIDBit) });
    }
    if (m_heap.m_structureIDClass)
        typed(*vector, (count + 1) * sizeof(uint32_t), *m_heap.m_structureIDClass);
}

static constexpr unsigned compactPointerTupleBitsInPointer = 48; // CompactPointerTuple::maxNumberOfBitsInPointer.

// CompactPointerTuple::pointer: the low bits of m_data, as the first template argument, a pointer type.
void ReachWalk::walkCompactPointerTuple(const TargetValue& tuple)
{
    auto data = tuple.properField("m_data").integer();
    const TargetType* pointerType = tuple.type().templateArgument(0);
    auto* pointer = pointerType ? std::get_if<TargetType::Pointer>(&pointerType->layout()) : nullptr;
    if (!data || !pointer) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    uint64_t address = static_cast<uint64_t>(*data) & ((1ull << compactPointerTupleBitsInPointer) - 1);
    if (address)
        followPointer(Address { address }, pointer->pointee);
}

// PropertyCondition::u: prototype for the kinds hasPrototype() names, presence
// for those hasOffset() names, and equivalence, a JSValue, for Equivalence.
void ReachWalk::walkPropertyCondition(const TargetValue& condition)
{
    walkMembers(condition, IsComplete::Yes, nullptr, ReadsOwnUnions::Yes);
    // PropertyCondition::Header is a CompactPointerTuple, whose type is in the bits above the pointer.
    auto header = condition.properField("m_header").properField("m_data").integer();
    auto* kindType = classBeside(condition.type(), "JSC::PropertyCondition::Kind");
    if (!header || !kindType) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    int64_t kind = static_cast<uint8_t>(static_cast<uint64_t>(*header) >> compactPointerTupleBitsInPointer);
    auto is = [&](std::string_view name) {
        auto value = kindType->enumeratorValue(name);
        return value && *value == kind;
    };
    if (is("Absence") || is("AbsenceOfSetEffect") || is("AbsenceOfIndexedProperties") || is("HasPrototype"))
        walkUnionMember(condition, "u", "prototype");
    else if (is("Equivalence"))
        walkUnionMember(condition, "u", "equivalence");
}

// CSSPrimitiveValue::m_value: calc for a CSSUnitType::Calc value (CSSPrimitiveValue::isCalculated), number otherwise.
void ReachWalk::walkCSSPrimitiveValue(const TargetValue& value)
{
    walkMembers(value, IsComplete::Yes, nullptr, ReadsOwnUnions::Yes);
    auto unitType = integerField(value, "m_primitiveUnitType");
    auto* unitTypes = classBeside(value.type(), "WebCore::CSSUnitType");
    auto calc = unitTypes ? unitTypes->enumeratorValue("Calc") : std::nullopt;
    if (!unitType || !calc) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (*unitType == *calc)
        walkUnionMember(value, "m_value", "calc");
}

// The type of `name`, a data member or a static one, that `type` or one of its non-virtual bases declares.
static const TargetType* memberType(const TargetType& type, const char* name, unsigned depth = 0)
{
    auto* klass = std::get_if<TargetType::Class>(&type.layout());
    if (!klass || depth > 32)
        return nullptr;
    for (const TargetType::Field& field : klass->properFields) {
        if (std::string_view { field.name.legacyCStringPointer() } == name)
            return &field.type;
    }
    if (auto* staticType = type.staticFieldType(name))
        return staticType;
    for (const TargetType::Base& base : klass->bases) {
        if (auto* found = memberType(base.type, name, depth + 1))
            return found;
    }
    return nullptr;
}

// CSS::PrimitiveData::payload: calc when its index's storage is
// indexStorageForCalc (PrimitiveDataIndex::isCalc), a number otherwise. That is
// UnitTraits::count, which WebCore asserts is one more than the last enumerator
// of its Raw's unit type: a member, or a static one for a type of one unit.
void ReachWalk::walkCSSPrimitiveData(const TargetValue& data)
{
    walkMembers(data, IsComplete::Yes, nullptr, ReadsOwnUnions::Yes);
    auto storage = data.properField("index").properField("storage").integer();
    const TargetType* numeric = data.type().templateArgument(0);
    const TargetType* raw = numeric ? numeric->templateArgument(0) : nullptr;
    const TargetType* unit = raw ? memberType(*raw, "unit") : nullptr;
    auto enumerators = unit ? unit->enumerators() : Vector<TargetType::Enumerator> { };
    if (!storage || enumerators.isEmpty()) {
        CORPSE_REPORT("The '%s' at 0x%llx has no index storage or unit type this walk knows", data.type().name().legacyCStringPointer(), forReport(data.address()));
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    int64_t calc = std::ranges::max(enumerators, { }, &TargetType::Enumerator::value).value + 1;
    if (*storage == calc)
        walkUnionMember(data, "payload", "calc");
}

// The subclass a TypeFieldHierarchy's type field names, as its visitDerived
// dispatches on it. The object's members, those of its base included, are read
// as that subclass's.
void ReachWalk::walkTypeField(const TargetValue& value)
{
    walkMembers(value, IsComplete::No);
    auto typeName = value.type().name();
    std::string_view name { typeName.legacyCStringPointer() };
    auto hierarchy = std::ranges::find_if(typeFieldHierarchyTable, [&](const HeapWalk::TypeFieldHierarchy& candidate) { return name == candidate.base; });
    auto typeField = integerField(value, hierarchy->typeField);
    if (!typeField) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    const TargetType* subclass = m_typeFieldSubclasses.ensure({ &value.type(), *typeField }, [&]() -> const TargetType* {
        UTF8CString enumeratorName { *typeField ? "true"_s : "false"_s };
        if (hierarchy->enumeration) {
            auto* enumeration = classBeside(value.type(), hierarchy->enumeration);
            auto enumerators = enumeration ? enumeration->enumerators() : Vector<TargetType::Enumerator> { };
            auto enumerator = std::ranges::find(enumerators, *typeField, &TargetType::Enumerator::value);
            if (enumerator == enumerators.end()) {
                CORPSE_REPORT("No enumerator of '%s' has the value %lld", hierarchy->enumeration, static_cast<long long>(*typeField));
                return nullptr;
            }
            enumeratorName = enumerator->name;
        }
        std::string_view name { enumeratorName.legacyCStringPointer() };
        auto entry = std::ranges::find_if(hierarchy->subclasses, [&](const HeapWalk::TypeFieldSubclass& candidate) { return name == candidate.enumerator; });
        if (entry == hierarchy->subclasses.end() || !entry->name) {
            CORPSE_REPORT("'%s' of '%s' names no subclass this walk knows", enumeratorName.legacyCStringPointer(), hierarchy->base);
            return nullptr;
        }
        auto* found = classBeside(value.type(), entry->name);
        if (!found)
            CORPSE_REPORT("'%s', the subclass of '%s' for '%s', is in no image", entry->name, hierarchy->base, enumeratorName.legacyCStringPointer());
        return found;
    }).iterator->value;
    if (!subclass) {
        notFollowed(NotFollowed::TypeNotReached);
        return;
    }
    if (auto index = allocationOf(value.address(), 1)) {
        const HeapWalk::Allocation& allocation = m_allocations[*index];
        if (minimumObjectSize(*subclass) > (allocation.address + allocation.size) - value.address()) {
            notFollowed(NotFollowed::DoesNotFit);
            m_overruns.append(makeString("the "_s, subclass->byteSize(), "-byte '"_s, subclass->name(), "' its type field names at 0x"_s, hex(value.address().toTargetVMAddress()),
                ", reached through "_s, context(), ", runs past the end of its "_s, allocation.size, "-byte allocation"_s));
            return;
        }
    }
    enqueue(TargetValue::at(m_snapshot, value.address(), *subclass), IsObject::Yes);
}

void ReachWalk::summarize(const Vector<HeapWalk::Allocation>& excluded, size_t listCount, HeapWalk::Reach& result)
{
    result.notFollowed = m_notFollowed;
    result.cellsWithClass = m_cellsWithClass;
    result.cellBytesBeyondClass = m_cellBytesBeyondClass;
    result.globalVariables = m_globalVariables;
    result.untypedDataSymbols = m_untypedDataSymbols;
    result.overruns = WTF::move(m_overruns);
    result.unreadableArrays = WTF::move(m_unreadableArrays);
    for (auto& unread : m_unreadUnions.values()) {
        String holder = unread.field ? makeString(unread.owner->name(), "::"_s, unread.field->name)
            : unread.owner ? makeString("a value of its own, in a '"_s, unread.owner->name(), '\'') : "a value of its own"_s;
        result.unreadUnions.append({ makeString('\'', unread.type->name(), "' in "_s, holder), unread.count });
    }
    std::ranges::sort(result.unreadUnions, std::ranges::greater { }, &std::pair<String, uint64_t>::second);
    for (auto& [klass, bytes] : m_bytesBeyondClass)
        result.cellBytesBeyondClassByClass.append({ makeString(klass->name()), bytes });
    std::ranges::sort(result.cellBytesBeyondClassByClass, std::ranges::greater { }, &std::pair<String, uint64_t>::second);
    result.cellBytesBeyondClassByClass.shrink(std::min<size_t>(result.cellBytesBeyondClassByClass.size(), listCount));
    result.isReached = m_reached;
    result.typeAtStart = m_typeAtStart;

    // The typed bytes, each once: an object read as two types, or a field read inside its object, counts once.
    std::ranges::sort(m_typed, { }, &Range::begin);
    result.bytesTypedIn.fill(0, m_allocations.size());
    size_t allocationIndex = 0;
    uint64_t coveredTo = 0;
    for (const Range& range : m_typed) {
        uint64_t begin = std::max(range.begin, coveredTo);
        if (begin >= range.end)
            continue;
        coveredTo = range.end;
        while (allocationIndex < m_allocations.size() && (m_allocations[allocationIndex].address + m_allocations[allocationIndex].size).toTargetVMAddress() <= begin)
            ++allocationIndex;
        // typed() clipped every range to the one allocation it starts in.
        if (allocationIndex < m_allocations.size())
            result.bytesTypedIn[allocationIndex] += range.end - begin;
    }

    auto excludedRanges = excluded.span();
    auto isExcluded = [&](const HeapWalk::Allocation& allocation) {
        auto next = std::ranges::upper_bound(excludedRanges, allocation.address, { }, [](const HeapWalk::Allocation& range) { return range.address + range.size; });
        return next != excludedRanges.end() && next->address < allocation.address + allocation.size;
    };
    Vector<HeapWalk::Untyped> untyped;
    HashMap<const TargetType*, uint64_t> untypedByType;
    HashMap<String, uint64_t> untypedByReacher;
    for (size_t index = 0; index < m_allocations.size(); ++index) {
        const HeapWalk::Allocation& allocation = m_allocations[index];
        result.bytesAllocated += allocation.size;
        if (isExcluded(allocation)) {
            result.bytesExcluded += allocation.size;
            continue;
        }
        uint64_t accounted = allocation.size;
        if (m_isBlock[index]) {
            accounted = std::min(m_liveBytesInBlock[index], allocation.size);
            result.bytesFreeInBlocks += allocation.size - accounted;
        }
        result.bytesTyped += result.bytesTypedIn[index];
        if (!m_reached[index])
            continue;
        result.bytesReached += allocation.size;
        if (accounted > result.bytesTypedIn[index]) {
            uint64_t bytes = accounted - result.bytesTypedIn[index];
            untyped.append({ allocation, bytes, m_typeAtStart[index] ? makeString(m_typeAtStart[index]->name()) : String(), reachedBy(index) });
            if (m_typeAtStart[index])
                untypedByType.add(m_typeAtStart[index], 0).iterator->value += bytes;
            else
                untypedByReacher.add(untyped.last().reachedBy, 0).iterator->value += bytes;
        }
    }
    for (auto& [type, bytes] : untypedByType)
        result.untypedByKind.append({ makeString('\'', type->name(), "' at the start"_s), bytes });
    for (auto& [reacher, bytes] : untypedByReacher)
        result.untypedByKind.append({ makeString("nothing at the start, reached by "_s, reacher), bytes });
    std::ranges::sort(result.untypedByKind, std::ranges::greater { }, &std::pair<String, uint64_t>::second);
    result.untypedByKind.shrink(std::min(result.untypedByKind.size(), listCount));
    std::ranges::sort(untyped, std::ranges::greater { }, &HeapWalk::Untyped::bytesUntyped);
    untyped.shrink(std::min(untyped.size(), listCount));
    result.mostUntyped = WTF::move(untyped);
}

// The deepest field of `type` that covers `offset`, as "Class::field".
static String fieldAt(const TargetType& type, uint64_t offset)
{
    const TargetType* current = &type;
    String result;
    for (unsigned depth = 0; depth < 64; ++depth) {
        if (auto* array = std::get_if<TargetType::Array>(&current->layout())) {
            if (!array->element.byteSize())
                break;
            offset %= array->element.byteSize();
            current = &array->element;
            continue;
        }
        auto* klass = std::get_if<TargetType::Class>(&current->layout());
        if (!klass)
            break;
        const TargetType* next = nullptr;
        for (const TargetType::Field& field : klass->properFields) {
            if (!field.bitSize && offset >= field.offset && offset < field.offset + field.type.byteSize()) {
                result = makeString(current->name(), "::"_s, field.name);
                offset -= field.offset;
                next = &field.type;
                break;
            }
        }
        for (size_t index = 0; !next && index < klass->bases.size(); ++index) {
            const TargetType::Base& base = klass->bases[index];
            if (offset >= base.offset && offset < base.offset + base.type.byteSize()) {
                offset -= base.offset;
                next = &base.type;
            }
        }
        if (!next)
            break;
        current = next;
    }
    return result;
}

String ReachWalk::holderInReached(Address word, size_t allocationIndex) const
{
    const HeapWalk::Allocation& allocation = m_allocations[allocationIndex];
    uint64_t address = word.toTargetVMAddress();
    // The innermost value the walk read that covers the word: the last to start at or before it.
    auto ranges = m_typed.span();
    auto after = std::ranges::upper_bound(ranges, address, { }, &Range::begin);
    for (auto range = after; range != ranges.begin();) {
        --range;
        if (range->end <= address)
            continue;
        if (range->begin < allocation.address.toTargetVMAddress())
            break;
        String field = fieldAt(*range->type, address - range->begin);
        if (!field.isNull())
            return makeString("the field "_s, field, ", which the walk does not follow, at offset "_s, address - range->begin, " of a '"_s, range->type->name(), "' at 0x"_s, hex(range->begin));
        return makeString("offset "_s, address - range->begin, " of a '"_s, range->type->name(), "' at 0x"_s, hex(range->begin), ", in no field of it"_s);
    }
    return makeString("offset "_s, word - allocation.address, " of the "_s, allocation.size, "-byte allocation at 0x"_s, hex(allocation.address.toTargetVMAddress()),
        ", in no value the walk read; it read "_s, m_typeAtStart[allocationIndex] ? makeString('\'', m_typeAtStart[allocationIndex]->name(), "' at its start"_s) : "nothing at its start"_s,
        ", reached by "_s, reachedBy(allocationIndex));
}

auto ReachWalk::holderOf(Address word) -> Holder
{
    using MissCause = HeapWalk::MissCause;
    if (auto index = allocationOf(word, sizeof(uint64_t))) {
        if (m_reached[*index])
            return { MissCause::Reached, holderInReached(word, *index), std::nullopt };
        return { MissCause::Missed, makeString("the missed "_s, m_allocations[*index].size, "-byte allocation at 0x"_s, hex(m_allocations[*index].address.toTargetVMAddress())), *index };
    }
    String symbol = m_heap.m_debugInfo->symbolAt(word);
    if (!symbol.isNull())
        return { MissCause::StaticData, makeString("the symbol "_s, symbol), std::nullopt };
    auto region = Region::findContaining(m_snapshot.regions(), word);
    String name = region ? region->name() : String();
    return { MissCause::OtherMemory, makeString("0x"_s, hex(word.toTargetVMAddress()), " in "_s, name.isEmpty() ? "anonymous memory"_s : name), std::nullopt };
}

// Every word of the snapshot's readable memory whose value lies inside a missed
// allocation is a referrer of it; the best of them says what holds it.
void ReachWalk::explainMisses(const Vector<HeapWalk::Allocation>& excluded, const Vector<HeapWalk::Allocation>& notReferrers, HeapWalk::Reach& result)
{
    using MissCause = HeapWalk::MissCause;
    Vector<size_t> missed;
    for (size_t index = 0; index < m_allocations.size(); ++index) {
        if (!m_reached[index])
            missed.append(index);
    }
    if (missed.isEmpty())
        return;
    CORPSE_DIAGNOSTICS(diagnostics, "explaining the %zu allocations the walk missed", missed.size());
    uint64_t lowest = m_allocations[missed.first()].address.toTargetVMAddress();
    uint64_t highest = (m_allocations[missed.last()].address + m_allocations[missed.last()].size).toTargetVMAddress();

    // For each allocation, the best holder found: the first cause MissCause lists.
    Vector<std::optional<Holder>> holders(m_allocations.size());
    constexpr size_t maxReferrers = 8;
    Vector<Vector<Address>> referrers(m_allocations.size());
    const Vector<Thread>& threads = m_snapshot.threads();
    auto ranges = notReferrers.span();
    auto isNotReferrer = [&](Address word) {
        auto next = std::ranges::upper_bound(ranges, word, { }, [](const HeapWalk::Allocation& range) { return range.address + range.size; });
        if (next != ranges.end() && next->address <= word)
            return true;
        // A word below a thread's stack pointer is left over from a frame that has returned.
        for (const Thread& thread : threads) {
            if (thread.hasStack() && thread.stackPointer() && thread.stackRegion().contains(word) && word < thread.stackPointer())
                return true;
        }
        return false;
    };
    // Most words are no address, or the address of something reached, so a
    // word is first looked up among the misses alone.
    auto missedAllocations = missed.span();
    auto missedIndexOf = [&](uint64_t value) -> std::optional<size_t> {
        auto after = std::ranges::upper_bound(missedAllocations, value, { }, [&](size_t index) {
            return m_allocations[index].address.toTargetVMAddress();
        });
        if (after == missedAllocations.begin())
            return std::nullopt;
        const HeapWalk::Allocation& allocation = m_allocations[*(after - 1)];
        if (value - allocation.address.toTargetVMAddress() >= allocation.size)
            return std::nullopt;
        return *(after - 1);
    };
    auto consider = [&](Address word, uint64_t value) {
        if (value < lowest || value >= highest)
            return;
        auto index = missedIndexOf(value);
        if (!index)
            return;
        // A word of the allocation itself does not hold it.
        const HeapWalk::Allocation& allocation = m_allocations[*index];
        if ((word >= allocation.address && word - allocation.address < allocation.size) || isNotReferrer(word))
            return;
        if (referrers[*index].size() < maxReferrers)
            referrers[*index].append(word);
        std::optional<Holder>& best = holders[*index];
        if (best && best->cause == MissCause::Reached)
            return;
        Holder holder = holderOf(word);
        if (!best || holder.cause < best->cause)
            best = WTF::move(holder);
    };

    constexpr size_t wordsPerRead = 64 * 1024;
    // Memory the target cannot write holds no address it allocated: a word
    // there that matches one, as in a constant table, holds nothing.
    for (const Region& region : m_snapshot.regions()) {
        if (!region.isReadable() || !region.isWritable())
            continue;
        for (auto [start, size] : region.residentParts(m_snapshot.corpsePort())) {
            for (uint64_t offset = 0; offset + sizeof(uint64_t) <= size;) {
                size_t count = std::min<uint64_t>(wordsPerRead, (size - offset) / sizeof(uint64_t));
                Address chunk = start + offset;
                offset += count * sizeof(uint64_t);
                auto words = m_snapshot.memory().span<uint64_t>(chunk, count);
                if (!words)
                    continue;
                std::span<const uint64_t> values { words };
                for (size_t index = 0; index < values.size(); ++index)
                    consider(chunk + index * sizeof(uint64_t), values[index]);
            }
        }
    }

    auto excludedRanges = excluded.span();
    auto isExcluded = [&](const HeapWalk::Allocation& allocation) {
        auto next = std::ranges::upper_bound(excludedRanges, allocation.address, { }, [](const HeapWalk::Allocation& range) { return range.address + range.size; });
        return next != excludedRanges.end() && next->address < allocation.address + allocation.size;
    };
    Vector<size_t> missIndexOf(m_allocations.size());
    for (size_t index : missed) {
        HeapWalk::Miss miss { m_allocations[index], MissCause::NoReferrer, "nothing"_s, { }, { }, isExcluded(m_allocations[index]), m_allocations[index].size };
        Address completeObject;
        if (auto* dynamicType = m_heap.m_debugInfo->dynamicTypeIfAnyAt(m_snapshot, m_allocations[index].address, completeObject))
            miss.content = makeString(dynamicType->name());
        miss.referrers = WTF::move(referrers[index]);
        if (holders[index]) {
            miss.cause = holders[index]->cause;
            miss.holder = holders[index]->description;
        }
        missIndexOf[index] = result.misses.size();
        result.misses.append(WTF::move(miss));
        if (!result.misses.last().isExcluded)
            result.bytesMissedByCause[static_cast<size_t>(result.misses.last().cause)] += m_allocations[index].size;
    }
    // A miss only other misses hold is counted in what the first miss up its chain that something else holds holds.
    for (size_t index : missed) {
        size_t current = index;
        for (unsigned depth = 0; depth < missed.size(); ++depth) {
            const std::optional<Holder>& holder = holders[current];
            if (!holder || holder->cause != MissCause::Missed || *holder->missedHolder == index)
                break;
            current = *holder->missedHolder;
        }
        if (current != index)
            result.misses[missIndexOf[current]].bytesHeld += m_allocations[index].size;
    }
    std::ranges::sort(result.misses, std::ranges::greater { }, &HeapWalk::Miss::bytesHeld);
}

#if OS(DARWIN)
auto HeapWalk::libpasPages(const LibpasRecords& records) const -> std::optional<LibpasPages>
{
    if (!isValid())
        return std::nullopt;
    // Each page's share of ranges sorted by address, for pages visited in address order.
    struct Cursor {
        const Vector<Allocation>& ranges;
        size_t index { 0 };
        uint64_t overlap(uint64_t begin, uint64_t end)
        {
            while (index < ranges.size() && (ranges[index].address + ranges[index].size).toTargetVMAddress() <= begin)
                ++index;
            uint64_t bytes = 0;
            for (size_t i = index; i < ranges.size() && ranges[i].address.toTargetVMAddress() < end; ++i) {
                uint64_t start = std::max(begin, ranges[i].address.toTargetVMAddress());
                uint64_t stop = std::min(end, (ranges[i].address + ranges[i].size).toTargetVMAddress());
                if (stop > start)
                    bytes += stop - start;
            }
            return bytes;
        }
    };
    Cursor objects { records.objects };
    Cursor payload { records.payload };
    Cursor meta { records.meta };
    LibpasPages result;
    uint64_t pageSize = vm_kernel_page_size;
    uint64_t objectBytesInPages = 0;
    uint64_t payloadBytes = 0;
    for (const Region& region : snapshot().regions()) {
        if (region.userTag() != VM_MEMORY_TCMALLOC)
            continue;
        const Vector<uint16_t>* dispositions = snapshot().pageDispositions(region);
        if (!dispositions)
            continue;
        for (size_t index = 0; index < dispositions->size(); ++index) {
            uint16_t disposition = (*dispositions)[index];
            bool isCompressed = disposition & VM_PAGE_QUERY_PAGE_PAGED_OUT;
            if (!(disposition & VM_PAGE_QUERY_PAGE_DIRTY) && !isCompressed)
                continue;
            uint64_t begin = region.base().toTargetVMAddress() + index * pageSize;
            uint64_t pageObjectBytes = objects.overlap(begin, begin + pageSize);
            uint64_t pagePayloadBytes = payload.overlap(begin, begin + pageSize);
            uint64_t pageMetaBytes = meta.overlap(begin, begin + pageSize);
            if (disposition & VM_PAGE_QUERY_PAGE_REUSABLE) {
                result.reusableBytes += pageSize;
                continue;
            }
            result.footprintBytes += pageSize;
            result.compressedBytes += isCompressed ? pageSize : 0;
            if (pagePayloadBytes + pageMetaBytes < pageSize)
                result.unrecordedPages.append(Address { begin });
            objectBytesInPages += pageObjectBytes;
            payloadBytes += pagePayloadBytes;
            result.metaBytes += pageMetaBytes;
        }
    }
    uint64_t allObjectBytes = 0;
    for (const Allocation& object : records.objects)
        allObjectBytes += object.size;
    result.objectBytes = objectBytesInPages;
    result.objectBytesElsewhere = allObjectBytes - std::min(allObjectBytes, objectBytesInPages);
    result.freePayloadBytes = payloadBytes - std::min(payloadBytes, objectBytesInPages);
    result.unrecordedBytes = result.footprintBytes - std::min(result.footprintBytes, payloadBytes + result.metaBytes);
    return result;
}
#endif

auto HeapWalk::heapDump(const Vector<Allocation>& allocations, const Reach& reach, const Vector<Reference>& references) const -> HeapDump
{
    HeapDump dump;
    if (!isValid())
        return dump;
    dump.nodes.append({ { }, 0, "<root>"_s, HeapDump::Kind::Root });
    struct Range {
        uint64_t begin;
        uint64_t end;
        uint32_t node;
    };
    Vector<Range> cells;
    Vector<uint64_t> liveBytes;
    liveBytes.fill(0, allocations.size());
    auto allocationIndex = [&](Address address) -> std::optional<size_t> {
        auto span = allocations.span();
        auto after = std::ranges::upper_bound(span, address, { }, &Allocation::address);
        if (after == span.begin() || address - (after - 1)->address >= (after - 1)->size)
            return std::nullopt;
        return (after - 1) - span.begin();
    };
    forEachLiveCell([&](const Cell& cell) {
        const TargetType* klass = isJSCellKind(cell.kind) ? cellClass(cell.address) : nullptr;
        String name = klass ? makeString(klass->name()) : isJSCellKind(cell.kind) ? "(JS cell)"_s : "(auxiliary cell)"_s;
        cells.append({ cell.address.toTargetVMAddress(), (cell.address + cell.size).toTargetVMAddress(), static_cast<uint32_t>(dump.nodes.size()) });
        dump.nodes.append({ cell.address, cell.size, WTF::move(name), HeapDump::Kind::Cell });
        if (auto index = allocationIndex(cell.address))
            liveBytes[*index] += cell.size;
        return IterationStatus::Continue;
    });
    std::ranges::sort(cells, { }, &Range::begin);

    HashMap<uint64_t, size_t> missIndex;
    for (size_t index = 0; index < reach.misses.size(); ++index)
        missIndex.add(reach.misses[index].allocation.address.toTargetVMAddress(), index);
    constexpr std::array<ASCIILiteral, numberOfMissCauses> causes {
        "held by a reached allocation"_s, "held by static data"_s, "held by other memory"_s, "held by other misses"_s, "held by nothing"_s,
    };
    Vector<uint32_t> allocationNodes;
    allocationNodes.fill(0, allocations.size());
    for (size_t index = 0; index < allocations.size(); ++index) {
        const Allocation& allocation = allocations[index];
        bool isReached = index < reach.isReached.size() && reach.isReached[index];
        const TargetType* type = index < reach.typeAtStart.size() ? reach.typeAtStart[index] : nullptr;
        // A MarkedBlock or a precise allocation is its cells, which are nodes of their own, and the rest.
        String name;
        if (liveBytes[index])
            name = makeString(type ? makeString(type->name()) : "(cells)"_s, " less its cells"_s);
        else if (!isReached) {
            auto miss = missIndex.find(allocation.address.toTargetVMAddress());
            name = makeString("(missed, "_s, miss == missIndex.end() ? "unexplained"_s : causes[static_cast<size_t>(reach.misses[miss->value].cause)], ')');
        } else
            name = type ? makeString(type->name()) : "(untyped)"_s;
        allocationNodes[index] = dump.nodes.size();
        dump.nodes.append({ allocation.address, allocation.size - std::min(allocation.size, liveBytes[index]), WTF::move(name),
            isReached || liveBytes[index] ? HeapDump::Kind::Allocation : HeapDump::Kind::Missed, !!liveBytes[index] });
    }

    HashMap<const char*, uint32_t> rootNodes;
    auto nodeOf = [&](Address address, const char* root) -> std::optional<uint32_t> {
        uint64_t value = address.toTargetVMAddress();
        auto cellSpan = cells.span();
        auto after = std::ranges::upper_bound(cellSpan, value, { }, &Range::begin);
        if (after != cellSpan.begin() && value < (after - 1)->end)
            return (after - 1)->node;
        if (auto index = allocationIndex(address))
            return allocationNodes[*index];
        if (!root)
            return std::nullopt;
        return rootNodes.ensure(root, [&] {
            dump.nodes.append({ { }, 0, makeString('<', String::fromLatin1(root), '>'), HeapDump::Kind::Root });
            dump.edges.append({ 0, static_cast<uint32_t>(dump.nodes.size() - 1) });
            return static_cast<uint32_t>(dump.nodes.size() - 1);
        }).iterator->value;
    };
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> seen;
    for (const Reference& reference : references) {
        auto to = nodeOf(reference.to, nullptr);
        if (!to)
            continue;
        auto from = reference.from ? nodeOf(reference.from, reference.root) : nodeOf({ }, reference.root);
        if (!from || *from == *to)
            continue;
        if (seen.add((static_cast<uint64_t>(*from) << 32) | *to).isNewEntry)
            dump.edges.append({ *from, *to });
    }
    std::ranges::stable_sort(dump.edges, { }, &std::pair<uint32_t, uint32_t>::first);
    return dump;
}

// The JSON of a GCDebugging heap snapshot, as HeapSnapshotBuilder::json writes it.
String HeapWalk::HeapDump::json() const
{
    StringBuilder out;
    HashMap<String, unsigned> classNames;
    Vector<String> orderedClassNames;
    auto classNameIndex = [&](const String& name) {
        return classNames.ensure(name, [&] {
            orderedClassNames.append(name);
            return static_cast<unsigned>(orderedClassNames.size() - 1);
        }).iterator->value;
    };
    out.append("{\"version\":3,\"type\":\"GCDebugging\",\"nodes\":["_s);
    for (size_t index = 0; index < nodes.size(); ++index) {
        const Node& node = nodes[index];
        // <nodeId>, <sizeInBytes>, <nodeClassNameIndex>, <flags>, <labelIndex>, <cellAddress>, <wrappedAddress>.
        // Web Inspector finds no path to a root through an internal node (flag 1).
        unsigned flags = node.kind == Kind::Missed || (node.kind == Kind::Allocation && node.isBlock) ? 1 : 0;
        out.append(index ? ","_s : ""_s, index, ',', node.size, ',', classNameIndex(node.className), ',', flags, ",0,\"0x"_s,
            hex(node.address.toTargetVMAddress(), Lowercase), "\",\"0x0\""_s);
    }
    out.append("],\"nodeClassNames\":["_s);
    for (size_t index = 0; index < orderedClassNames.size(); ++index)
        out.append(index ? ","_s : ""_s, '"', orderedClassNames[index], '"');
    out.append("],\"edges\":["_s);
    for (size_t index = 0; index < edges.size(); ++index)
        out.append(index ? ","_s : ""_s, edges[index].first, ',', edges[index].second, ",0,0"_s);
    out.append("],\"edgeTypes\":[\"Internal\",\"Property\",\"Index\",\"Variable\"],\"edgeNames\":[],\"roots\":["_s);
    // <nodeId>, <rootReasonIndex>, <reachabilityReasonIndex>; each root's reason is its name, a label.
    Vector<String> labels { ""_s };
    for (size_t index = 1; index < nodes.size(); ++index) {
        if (nodes[index].kind != Kind::Root)
            continue;
        out.append(labels.size() > 1 ? ","_s : ""_s, index, ',', labels.size(), ",0"_s);
        labels.append(nodes[index].className);
    }
    out.append("],\"labels\":["_s);
    for (size_t index = 0; index < labels.size(); ++index)
        out.append(index ? ","_s : ""_s, '"', labels[index], '"');
    out.append("]}"_s);
    return out.toString();
}

auto HeapWalk::typeFieldHierarchies() -> std::span<const TypeFieldHierarchy>
{
    return typeFieldHierarchyTable;
}

HeapWalk::Reach HeapWalk::reach(const Vector<Allocation>& allocations, const Vector<Allocation>& excluded, const Vector<Allocation>& notReferrers, size_t listCount, Vector<Reference>* references) const
{
    Reach result;
    if (!isValid())
        return result;
    CORPSE_DIAGNOSTICS(diagnostics, "measuring the reach of the walk of the heap at 0x%llx", forReport(m_vm.address()));
    ReachWalk walk(*this, allocations, references);
    walk.run();
    walk.summarize(excluded, listCount, result);
    walk.explainMisses(excluded, notReferrers, result);
    return result;
}

#if ENABLE(MYA_HEAP)

namespace {

// pas_enumerator's reader: a local copy of the snapshot's bytes, valid until
// the next read, as libmalloc's memory_reader_t promises.
struct LibpasEnumeration {
    Memory& memory;
    Vector<uint8_t> copy;
    HeapWalk::LibpasRecords records;
    bool failed { false };
};

void* readSnapshotMemory(pas_enumerator*, void* address, size_t size, void* argument)
{
    auto& enumeration = *static_cast<LibpasEnumeration*>(argument);
    auto bytes = enumeration.memory.span<uint8_t>(Address { address }, size);
    if (!bytes) {
        enumeration.failed = true;
        return nullptr;
    }
    enumeration.copy = Vector<uint8_t> { std::span<const uint8_t> { bytes } };
    return enumeration.copy.mutableSpan().data();
}

void recordLibpasRange(pas_enumerator*, void* address, size_t size, pas_enumerator_record_kind kind, void* argument)
{
    auto& records = static_cast<LibpasEnumeration*>(argument)->records;
    HeapWalk::Allocation range { Address { address }, size };
    switch (kind) {
    case pas_enumerator_meta_record:
        records.meta.append(range);
        return;
    case pas_enumerator_payload_record:
        records.payload.append(range);
        return;
    case pas_enumerator_object_record:
        records.objects.append(range);
        return;
    }
}

std::optional<HeapWalk::LibpasRecords> enumerateLibpas(Snapshot& snapshot, SnapshotDebugInfo& debugInfo, Address javaScriptCoreImage, bool allKinds)
{
    CORPSE_DIAGNOSTICS(diagnostics, "enumerating libpas's heap from its root for libmalloc's enumeration");
    // libpas is linked into JavaScriptCore, so this process's libpas is the
    // target's, with the root's layout its enumerator reads.
    auto rootSymbol = debugInfo.symbolAddress(javaScriptCoreImage, "pas_root_for_libmalloc_enumeration");
    auto root = rootSymbol ? snapshot.memory().ptr<uint64_t>(*rootSymbol) : Memory::Ptr<uint64_t> { };
    if (!root || !*root) {
        CORPSE_REPORT("JavaScriptCore's image has no root for libmalloc's enumeration of libpas");
        return std::nullopt;
    }
    auto magic = snapshot.memory().ptr<uint64_t>(Address { *root });
    if (!magic || *magic != PAS_ROOT_MAGIC) {
        CORPSE_REPORT("The libpas root at 0x%llx is not one", static_cast<unsigned long long>(*root));
        return std::nullopt;
    }
    LibpasEnumeration enumeration { snapshot.memory(), { }, { } };
    pas_enumerator* enumerator = pas_enumerator_create(std::bit_cast<pas_root*>(static_cast<uintptr_t>(*root)), readSnapshotMemory, &enumeration, recordLibpasRange, &enumeration,
        allKinds ? pas_enumerator_record_meta_records : pas_enumerator_do_not_record_meta_records,
        allKinds ? pas_enumerator_record_payload_records : pas_enumerator_do_not_record_payload_records,
        pas_enumerator_record_object_records);
    bool enumerated = enumerator && pas_enumerator_enumerate_all(enumerator);
    if (enumerator)
        pas_enumerator_destroy(enumerator);
    if (!enumerated || enumeration.failed) {
        CORPSE_REPORT("libpas could not enumerate the heap of the root at 0x%llx", static_cast<unsigned long long>(*root));
        return std::nullopt;
    }
    for (auto* ranges : { &enumeration.records.meta, &enumeration.records.payload, &enumeration.records.objects })
        std::ranges::sort(*ranges, { }, &HeapWalk::Allocation::address);
    return WTF::move(enumeration.records);
}

} // anonymous namespace

auto HeapWalk::libpasAllocations() const -> std::optional<Vector<Allocation>>
{
    if (!isValid())
        return std::nullopt;
    auto records = enumerateLibpas(snapshot(), *m_debugInfo, m_javaScriptCoreImage, false);
    if (!records)
        return std::nullopt;
    return WTF::move(records->objects);
}

auto HeapWalk::libpasRecords() const -> std::optional<LibpasRecords>
{
    if (!isValid())
        return std::nullopt;
    return enumerateLibpas(snapshot(), *m_debugInfo, m_javaScriptCoreImage, true);
}

#else // ENABLE(MYA_HEAP)

auto HeapWalk::libpasAllocations() const -> std::optional<Vector<Allocation>>
{
    CORPSE_REPORT("This build does not export libpas's enumerator: it is not an ENABLE(MYA_HEAP) build");
    return std::nullopt;
}

auto HeapWalk::libpasRecords() const -> std::optional<LibpasRecords>
{
    CORPSE_REPORT("This build does not export libpas's enumerator: it is not an ENABLE(MYA_HEAP) build");
    return std::nullopt;
}

#endif // ENABLE(MYA_HEAP)

// A JS cell has no vtable, so its class comes from its Structure's ClassInfo,
// which is the s_info of the class it describes. Read at offsets found once,
// since every live cell is read this way.
std::optional<Address> HeapWalk::classInfoOf(Address address) const
{
    auto bits = snapshot().memory().ptr<uint32_t>(address + m_structureIDOffset);
    uint32_t structureIDBits = bits ? *bits & ~StructureID::nukedStructureIDBit : 0;
    if (!structureIDBits)
        return std::nullopt;
    auto classInfo = snapshot().memory().ptr<uint64_t>(Address { m_startOfStructureHeap + structureIDBits } + m_classInfoOffset);
    if (!classInfo || !*classInfo)
        return std::nullopt;
    return Address { *classInfo }.stripped();
}

const TargetType* HeapWalk::cellClass(Address address) const
{
    if (!isValid())
        return nullptr;
    auto classInfo = classInfoOf(address);
    if (!classInfo)
        return nullptr;
    return classOfClassInfo(*classInfo, 0);
}

// The class whose s_info the ClassInfo is, checked against the ClassInfo's
// size and parent. Null, having reported why once, if there is none.
const TargetType* HeapWalk::classOfClassInfo(Address address, unsigned depth) const
{
    uint64_t key = address.toTargetVMAddress();
    if (auto klass = m_cellClasses.getOptional(key))
        return *klass;
    if (depth > maxClassInfoDepth) {
        CORPSE_REPORT("The ClassInfo at 0x%llx has more than %u ancestors", forReport(address), maxClassInfoDepth);
        return nullptr;
    }
    Remote<ClassInfo> classInfo = Remote<Structure>(TargetValue { *m_structure }).field<const ClassInfo*>("m_classInfo").pointeeAt(address);
    // CREATE_METHOD_TABLE fills the method table with the class's own functions, or those it inherits.
    Vector<Address, 32> methods;
    if (auto* info = classInfo.typed()) {
        info->properField("methodTable").forEachField([&](const TargetType::Field&, const TargetValue& method) {
            if (auto function = method.pointerValue(); function && *function)
                methods.append(*function);
        });
    }
    const TargetType* klass = m_debugInfo->classOfStaticMember(address, "s_info", methods.span());
    if (klass && !isClassInfoOf(classInfo, *klass, depth))
        klass = nullptr;
    m_cellClasses.add(key, klass);
    return klass;
}

// Whether `classInfo` is the ClassInfo of `klass`: ClassInfo::staticClassSize
// is sizeof the class it was made for, and ClassInfo::parentClass is the
// ClassInfo of one of its bases. Reported if not.
bool HeapWalk::isClassInfoOf(const Remote<ClassInfo>& classInfo, const TargetType& klass, unsigned depth) const
{
    auto size = classInfo.field<unsigned>("staticClassSize").integer();
    if (!size) {
        CORPSE_REPORT("Could not read the class size of the ClassInfo at 0x%llx", forReport(classInfo.address()));
        return false;
    }
    if (static_cast<uint64_t>(*size) != klass.byteSize()) {
        CORPSE_REPORT("The ClassInfo at 0x%llx is for %lld-byte classes, but its '%s' is %zu bytes", forReport(classInfo.address()), static_cast<long long>(*size), klass.name().legacyCStringPointer(), klass.byteSize());
        return false;
    }
    auto parent = classInfo.field<const ClassInfo*>("parentClass").pointerValue();
    if (!parent)
        return false;
    if (!*parent)
        return true;
    const TargetType* parentClass = classOfClassInfo(*parent, depth + 1);
    if (!parentClass) {
        CORPSE_REPORT("The parent of the ClassInfo of '%s' at 0x%llx names no class", klass.name().legacyCStringPointer(), forReport(classInfo.address()));
        return false;
    }
    // A class of one image, such as a WebCore wrapper, describes its bases
    // with that image's types, and its parent's ClassInfo may be in another.
    auto parentName = parentClass->name();
    Vector<const TargetType*, 8> bases { &klass };
    for (size_t index = 0; index < bases.size() && index < maxClassInfoDepth * 4; ++index) {
        if (bases[index] == parentClass || bases[index]->name() == parentName)
            return true;
        if (auto* layout = std::get_if<TargetType::Class>(&bases[index]->layout())) {
            for (const TargetType::Base& base : layout->bases)
                bases.append(&base.type);
        }
    }
    CORPSE_REPORT("The parent of the ClassInfo of '%s' at 0x%llx is for '%s', which is not one of its bases", klass.name().legacyCStringPointer(), forReport(classInfo.address()), parentClass->name().legacyCStringPointer());
    return false;
}

Vector<String> HeapWalk::classInfosWithoutClass(size_t& count) const
{
    Vector<String> result;
    count = 0;
    if (!isValid())
        return result;
    // "6s_infoE" ends the linkage name of every X::s_info.
    for (const SnapshotDebugInfo::Symbol& symbol : m_debugInfo->dataSymbolsEndingWith(m_javaScriptCoreImage, "6s_infoE")) {
        ++count;
        if (!classOfClassInfo(symbol.address, 0))
            result.append(symbol.name);
    }
    return result;
}

Remote<JSCell> HeapWalk::jsCell(Address address) const
{
    if (!m_jsCell)
        return { };
    return Remote<JSCell>(m_jsCell->at(address));
}

// StructureID::decode.
Remote<Structure> HeapWalk::structure(uint32_t structureIDBits) const
{
    uint32_t bits = structureIDBits & ~StructureID::nukedStructureIDBit;
    if (!m_structure || !bits)
        return { };
    return Remote<Structure>(m_structure->at(Address { m_startOfStructureHeap + bits }));
}

} // namespace Corpse
} // namespace JSC

#endif // ENABLE(MYA)
