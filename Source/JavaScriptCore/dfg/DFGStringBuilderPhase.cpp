/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
 */

#include "config.h"
#include "DFGStringBuilderPhase.h"

#if ENABLE(DFG_JIT)

#include "DFGBasicBlockInlines.h"
#include "DFGGraph.h"
#include "DFGInsertionSet.h"
#include "DFGNaturalLoops.h"
#include "DFGPhase.h"
#include "JSCJSValueInlines.h"
#include <cstdlib>
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace DFG {

namespace {
static bool envFlag(const char* name)
{
    const char* e = getenv(name);
    return e && e[0] && e[0] != '0';
}
// Diagnostic logging only (off by default). The optimization itself is DEFAULT-ON and needs no env.
bool sbVerbose()
{
    static bool v = envFlag("SB_DETECT");
    return v;
}
// Hidden kill switch: lets us build an OFF binary for correctness diffs without a build change.
// The shipped default is ON (this returns false unless SB_DISABLE is explicitly set).
bool sbDisabled()
{
    static bool v = envFlag("SB_DISABLE");
    return v;
}
// Isolation switch for the >3-piece StrCat-chain collapse only (leaves the local/field MakeRope
// accumulator paths on); lets us A/B this extension's delta against the rest of the phase.
bool sbNoStrCat()
{
    static bool v = envFlag("SB_NO_STRCAT");
    return v;
}
}

class StringBuilderConcatPhase : public Phase {
public:
    StringBuilderConcatPhase(Graph& graph)
        : Phase(graph, "string builder concat"_s)
    {
    }

    // Is `edge` a read of local `operand` (GetLocal / Phi threaded to that local)?
    bool readsLocal(Edge edge, Operand operand)
    {
        Node* n = edge.node();
        if (!n)
            return false;
        if ((n->op() == GetLocal || n->op() == Phi) && n->operand() == operand)
            return true;
        return false;
    }

    // Like readsLocal, but sees through a single value-preserving coercion. An op_strcat operand is
    // wrapped by Fixup in ToPrimitive; once the operand is proven to be a String that node is lowered
    // to Identity. Either way the accumulator read `V` reaches the concat as coercion(GetLocal(V))
    // rather than a bare GetLocal(V). Each of these coercions is the identity on a string (and a
    // builder rope IS a string), so peeling it is sound. Returns the coercion node peeled (nullptr if
    // the read was already direct) via `wrapper` so the safety scan can excuse it.
    bool readsLocalThroughCoercion(Edge edge, Operand operand, Node*& wrapper)
    {
        wrapper = nullptr;
        if (readsLocal(edge, operand))
            return true;
        Node* n = edge.node();
        if (n && (n->op() == Identity || n->op() == ToPrimitive || n->op() == ToString || n->op() == CallStringConstructor)
            && readsLocal(n->child1(), operand)) {
            wrapper = n;
            return true;
        }
        return false;
    }

    static bool isThreadingOrBookkeeping(Node* n)
    {
        switch (n->op()) {
        case GetLocal: case SetLocal: case Phi: case Upsilon: case Flush: case PhantomLocal:
        case MovHint: case ZombieHint: case KillStack: case Check: case Phantom:
            return true;
        default:
            return false;
        }
    }

    // Performance guard: the deferred builder only pays off when the SAME accumulator local is
    // appended to at a SINGLE site per loop iteration, so the builder cell threads through the loop
    // Phi and is reused each iteration. With two or more concat stores to the same local in the loop
    // (e.g. base64's `result += a; result += b; result += c`), the builder is NOT reused across the
    // back-edge -- it is re-created every iteration (SB_STAT shows creates>>appends), which is
    // catastrophically slower than a plain rope. Detect that here and leave such accumulators as
    // normal ropes. Counts SetLocal(local, MakeRope|StrCat) in loop blocks.
    bool hasMultipleConcatStoresInLoop(Operand local)
    {
        unsigned count = 0;
        for (BlockIndex bi = m_graph.numBlocks(); bi--;) {
            BasicBlock* b = m_graph.block(bi);
            if (!b || !m_graph.m_cpsNaturalLoops->loopDepth(b))
                continue;
            for (unsigned j = 0; j < b->size(); ++j) {
                Node* n = b->at(j);
                if (n->op() != SetLocal || n->operand() != local)
                    continue;
                Node* v = n->child1().node();
                if (v && (v->op() == MakeRope || v->op() == StrCat)) {
                    if (++count > 1)
                        return true;
                }
            }
        }
        return false;
    }

    // Safety: in-place append mutates the accumulator rope, so it is only sound if the old value is
    // never observed by identity. The accumulator's own def-use cycle (Phi/GetLocal/SetLocal
    // threading + the accumulating MakeRope) is fine; a *snapshot* is any OTHER node inside a loop
    // that consumes the accumulator's value (e.g. `snaps.push(s)`). Post-loop reads (loopDepth 0)
    // are safe -- they see the final accumulated value. Bail if such an in-loop consumer exists.
    bool hasUnsafeInLoopUse(Operand local, Node* accumulatorMakeRope, Node* accumulatorReadWrapper = nullptr)
    {
        bool unsafe = false;
        for (BlockIndex bi = m_graph.numBlocks(); bi--;) {
            BasicBlock* b = m_graph.block(bi);
            if (!b || !m_graph.m_cpsNaturalLoops->loopDepth(b))
                continue;
            for (unsigned j = 0; j < b->size(); ++j) {
                Node* n = b->at(j);
                // accumulatorReadWrapper is the Identity/ToPrimitive/ToString that feeds the
                // accumulator's own value into the concat (its child is GetLocal(V)); it is part of the
                // accumulator cycle, not an outside snapshot, so it must not count as an unsafe use.
                if (n == accumulatorMakeRope || n == accumulatorReadWrapper || isThreadingOrBookkeeping(n))
                    continue;
                m_graph.doToChildren(n, [&](Edge edge) {
                    Node* c = edge.node();
                    if (c && (c->op() == GetLocal || c->op() == Phi) && c->operand() == local)
                        unsafe = true;
                });
                if (unsafe)
                    return true;
            }
        }
        return false;
    }

    // ---- FIELD/PROPERTY accumulator support ----

    // Peel a single value-preserving coercion off `edge` and return the underlying node (the same
    // set of coercions readsLocalThroughCoercion peels). `wrapper` records the peeled node, if any.
    Node* peelCoercion(Edge edge, Node*& wrapper)
    {
        wrapper = nullptr;
        Node* n = edge.node();
        if (n && (n->op() == Identity || n->op() == ToPrimitive || n->op() == ToString || n->op() == CallStringConstructor)) {
            wrapper = n;
            return n->child1().node();
        }
        return n;
    }

    // Nodes that may take the base as a child but neither read the accumulator field's VALUE nor
    // expose the base object to code that could: pure structure/array checks, IC-status metadata,
    // and the object-shaping primitives emitted when constructing the object literal (structure
    // transition + butterfly allocation). None of these can observe a pre-append builder state.
    static bool isPureBaseCheck(Node* n)
    {
        switch (n->op()) {
        case CheckStructure: case CheckStructureOrEmpty: case CheckArray: case CheckArrayOrEmpty:
        case FilterGetByStatus: case FilterPutByStatus: case GetButterfly:
        case PutStructure: case AllocatePropertyStorage: case ReallocatePropertyStorage:
        case NukeStructureAndSetButterfly:
            return true;
        default:
            return false;
        }
    }

    // The base local must always hold a FRESH object allocated (NewObject) outside any loop, and
    // never anything else: every definition is SetLocal(NewObject) at loopDepth 0, there is at
    // least one, and it is never a function argument (which could be aliased by the caller).
    bool baseLocalIsFreshNewObject(Operand baseLocal)
    {
        bool sawNewObjectDef = false;
        for (BlockIndex bi = m_graph.numBlocks(); bi--;) {
            BasicBlock* b = m_graph.block(bi);
            if (!b)
                continue;
            bool inLoop = m_graph.m_cpsNaturalLoops->loopDepth(b) > 0;
            for (unsigned j = 0; j < b->size(); ++j) {
                Node* n = b->at(j);
                if (n->op() == SetArgumentDefinitely || n->op() == SetArgumentMaybe) {
                    if (n->operand() == baseLocal)
                        return false; // parameter: possibly aliased by caller
                    continue;
                }
                if (n->op() != SetLocal || n->operand() != baseLocal)
                    continue;
                Node* def = n->child1().node();
                if (!def)
                    return false;
                if (def->op() == NewObject) {
                    if (inLoop)
                        return false; // re-allocated inside the loop -- not a stable accumulator
                    sawNewObjectDef = true;
                    continue;
                }
                // A non-cell constant (e.g. the dead `var p = undefined` entry-init that the real
                // NewObject assignment overwrites) can never be an object alias, so it is harmless.
                if (def->hasConstant() && !def->asJSValue().isCell())
                    continue;
                // Anything else (a call result, another local, a constant cell) might alias an
                // object visible elsewhere -- bail.
                return false;
            }
        }
        return sawNewObjectDef;
    }

    // Grow `aliasLocals` to a fixpoint: any SetLocal/Upsilon whose stored value is already an alias
    // of the tracked cell taints its target local too. `isSeed` decides whether a given node is a
    // known alias of the tracked cell.
    template<typename SeedFunctor>
    void collectAliasLocals(HashSet<uint64_t>& aliasLocals, const SeedFunctor& isSeed)
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (BlockIndex bi = m_graph.numBlocks(); bi--;) {
                BasicBlock* b = m_graph.block(bi);
                if (!b)
                    continue;
                for (unsigned j = 0; j < b->size(); ++j) {
                    Node* n = b->at(j);
                    if (n->op() != SetLocal)
                        continue;
                    Node* v = n->child1().node();
                    if (!v)
                        continue;
                    bool srcIsAlias = isSeed(v) || ((v->op() == GetLocal || v->op() == Phi) && aliasLocals.contains(v->operand().asBits()));
                    if (srcIsAlias) {
                        if (aliasLocals.add(n->operand().asBits()).isNewEntry)
                            changed = true;
                    }
                }
            }
        }
    }

    // Sound-leaning guard for a field accumulator `base.f = MakeRope(base.f, piece...)`.
    // Bails (returns true) unless we can show that neither the object `base` nor the builder VALUE
    // can be observed mid-loop by anything other than the accumulator's own get/put and pure
    // structure checks/threading. Because the append mutates the builder in place, any surviving
    // reference to a pre-append state would be corrupted, so the analysis is deliberately strict.
    bool hasUnsafeFieldUse(Operand baseLocal, Node* base, Node* getNode, Node* putNode,
        Node* makeRope, Node* coercionWrapper, PropertyOffset offset, unsigned identifierNumber)
    {
        // Locals that may hold the base object.
        HashSet<uint64_t> baseAliasLocals;
        baseAliasLocals.add(baseLocal.asBits());
        auto isBaseSeed = [&](Node* n) { return n == base || n->op() == NewObject; };
        collectAliasLocals(baseAliasLocals, isBaseSeed);
        auto isBaseAlias = [&](Node* n) {
            if (n == base || n->op() == NewObject)
                return true;
            return (n->op() == GetLocal || n->op() == Phi) && baseAliasLocals.contains(n->operand().asBits());
        };

        // Locals that may hold the builder VALUE (the field read result + the MakeRope result).
        HashSet<uint64_t> valAliasLocals;
        auto isValSeedBase = [&](Node* n) { return n == makeRope || n == getNode; };
        collectAliasLocals(valAliasLocals, isValSeedBase);
        auto isValAlias = [&](Node* n) {
            if (n == makeRope || n == getNode)
                return true;
            return (n->op() == GetLocal || n->op() == Phi) && valAliasLocals.contains(n->operand().asBits());
        };

        for (BlockIndex bi = m_graph.numBlocks(); bi--;) {
            BasicBlock* b = m_graph.block(bi);
            if (!b)
                continue;
            bool inLoop = m_graph.m_cpsNaturalLoops->loopDepth(b) > 0;
            for (unsigned j = 0; j < b->size(); ++j) {
                Node* n = b->at(j);
                // The accumulator's own nodes, its coercion wrapper, and pure bookkeeping never
                // constitute an observation of the pre-append state.
                if (n == getNode || n == putNode || n == makeRope || n == coercionWrapper)
                    continue;
                if (isThreadingOrBookkeeping(n))
                    continue;

                bool unsafe = false;
                m_graph.doToChildren(n, [&](Edge edge) {
                    Node* c = edge.node();
                    if (!c)
                        return;
                    if (isValAlias(c)) {
                        // Any consumer of the builder value other than the accumulator itself could
                        // snapshot a pre-append state.
                        if (sbVerbose())
                            dataLogLn("[SB-FIELD]   unsafe VAL use: node D@", n->index(), " op ", Graph::opName(n->op()), " child D@", c->index());
                        unsafe = true;
                        return;
                    }
                    if (isBaseAlias(c)) {
                        if (isPureBaseCheck(n))
                            return; // structure/status checks do not read the field value
                        // A field access on the base is only safe outside the loop (the seed store
                        // before, the final materializing read after). In-loop, or via GetByVal/
                        // dynamic access, or as a call argument / heap store, it is an escape.
                        if ((n->op() == GetByOffset || n->op() == PutByOffset) && !inLoop) {
                            StorageAccessData& sad = n->storageAccessData();
                            // A different field is unrelated; our field outside the loop is the
                            // seed/final and is fine.
                            if (sad.identifierNumber != identifierNumber || sad.offset != offset)
                                return;
                            return;
                        }
                        if (sbVerbose())
                            dataLogLn("[SB-FIELD]   unsafe BASE use: node D@", n->index(), " op ", Graph::opName(n->op()), " inLoop ", inLoop, " child D@", c->index());
                        unsafe = true;
                    }
                });
                if (unsafe)
                    return true;
            }
        }
        return false;
    }

    bool run()
    {
        if (sbDisabled())
            return false;

        m_graph.ensureCPSNaturalLoops();

        // Lower to the native builder ONLY when compiling for FTL. The builder append is emitted by
        // two independent backends -- DFGSpeculativeJIT::compileMakeRope (DFG tier) and
        // FTLLowerDFGToB3 (FTL tier). The DFG-tier inlined fast path has a latent codegen defect
        // that crashes when a recognized accumulator runs sustained in DFG with polymorphic pieces
        // (e.g. alternating 8-bit/16-bit/empty content, which forces re-speculation). While gated
        // behind SB_OPT this was never hit; DEFAULT-ON would hit it for real workloads. Since DFG
        // is a transient tier that tiers up to FTL -- where this optimization was actually
        // validated and where the hot-loop win lives -- we restrict lowering to FTL: the DFG tier
        // simply never sees a tagged MakeRope and emits a correct normal rope. Detection/logging
        // still run in every tier (harmless, diagnostics only).
        bool lower = m_graph.m_plan.isFTL();

        bool changed = false;
        unsigned candidates = 0;
        for (BlockIndex blockIndex = m_graph.numBlocks(); blockIndex--;) {
            BasicBlock* block = m_graph.block(blockIndex);
            if (!block)
                continue;
            bool inLoop = m_graph.m_cpsNaturalLoops->loopDepth(block) > 0;
            for (unsigned i = 0; i < block->size(); ++i) {
                Node* node = block->at(i);
                if (node->op() != SetLocal)
                    continue;
                Operand dst = node->operand();
                Node* value = node->child1().node();
                // The accumulator concat is a MakeRope. `V += p + q` (op_strcat) also lands here as a
                // MakeRope: Fixup's attemptToMakeFastStringAdd lowers the op_strcat into a MakeRope
                // before this phase runs, coercing the accumulator read to Identity/ToPrimitive(GetLocal(V))
                // (peeled by readsLocalThroughCoercion below).
                if (!value || value->op() != MakeRope)
                    continue;
                // Accumulator: the value's FIRST fiber reads the local we store to (V = V + p [+ q]).
                // We require child1 to be the direct read (not a nested rope) so that the extra fibers
                // are leaf pieces we can append; a nested tree (V buried deeper) is left alone rather
                // than pessimized. An op_strcat that Fixup proved to be all-string is lowered to a
                // MakeRope whose accumulator fiber is ToPrimitive(GetLocal(V)) (not a bare GetLocal(V)),
                // so peel that value-preserving coercion; readWrapper records it for the safety scan.
                Node* readWrapper = nullptr;
                bool isAccumulator = readsLocalThroughCoercion(value->child1(), dst, readWrapper);
                if (!isAccumulator)
                    continue;
                if (!inLoop)
                    continue;

                ++candidates;
                if (sbVerbose()) {
                    dataLogLn("[SB] accumulator MakeRope D@", value->index(),
                        " localValue ", dst.value(), " bc#", node->origin.semantic.bytecodeIndex().offset(),
                        " in ", m_graph.m_codeBlock->inferredName().data(),
                        " loopDepth ", m_graph.m_cpsNaturalLoops->loopDepth(block));
                }
                // DEFAULT-ON: always lower a safe local/StrCat accumulator (no env var needed) --
                // but only in the FTL tier (see `lower` above).
                if (!lower || hasUnsafeInLoopUse(dst, value, readWrapper))
                    continue;
                // Skip accumulators written at more than one site in the loop: the builder is not
                // reused across the back-edge there and thrashes (re-created every iteration).
                if (hasMultipleConcatStoresInLoop(dst))
                    continue;

                // If this is `V = V + piece` (2-input) and the piece is itself a 2-input concat of
                // leaves (MakeRope(a, b) -- the common `V += a + b` shape), pull a and b up so the
                // accumulator node becomes MakeRope(V, a, b). Then a single 3-input append3 appends
                // a and b straight into the builder with NO piece rope. The old piece node loses its
                // use and is DCE'd. Only leaf (non-rope) operands are pulled up.
                if (!value->child3()) {
                    Node* piece = value->child2().node();
                    if (piece && piece->op() == MakeRope && piece->child2() && !piece->child3()) {
                        Node* a = piece->child1().node();
                        Node* b = piece->child2().node();
                        if (a && b && a->op() != MakeRope && b->op() != MakeRope) {
                            value->child2() = Edge(a, KnownStringUse);
                            value->child3() = Edge(b, KnownStringUse);
                            if (sbVerbose())
                                dataLogLn("[SB] flattened piece -> 3-input append3");
                        }
                    }
                }

                // Record the accumulator MakeRope's bytecode (a SINGLE node, 2 or 3 inputs).
                // compileMakeRope keys off this set to append the extra fibers into the builder:
                // an inlined fast path with operationStringBuilderAppend{,3} as the create/grow slow
                // path. One node (not a chain) keeps it OSR-safe: the whole `V = V + ...` op maps to
                // one node whose mutation is all-or-nothing.
                unsigned off = value->origin.semantic.bytecodeIndex().offset();
                m_graph.m_stringBuilderConcatBytecodes.add(off);
                if (sbVerbose())
                    dataLogLn("[SB] tagged accumulator (", value->child3() ? "3" : "2", "-input) at offset=", off);
                changed = true;
            }
        }

        // FIELD/PROPERTY accumulator (`obj.f += piece`). The pattern in CPS after Fixup is
        // PutByOffset(storage=base, base, MakeRope(coercion?(GetByOffset(base, f)), piece...)), where
        // the MakeRope's accumulator fiber reads the SAME field of the SAME object the PutByOffset
        // stores to. Tagging that MakeRope reuses the existing builder routing exactly as the local
        // case does; only detection + the strict object-escape safety guard (hasUnsafeFieldUse) live
        // here. Soundness rests on that guard: base must be a fresh NewObject with no in-loop use
        // other than this accumulator's own Get/Put, so no mid-loop snapshot of the field can exist.
        if (lower) {
            for (BlockIndex blockIndex = m_graph.numBlocks(); blockIndex--;) {
                BasicBlock* block = m_graph.block(blockIndex);
                if (!block || !m_graph.m_cpsNaturalLoops->loopDepth(block))
                    continue;
                for (unsigned i = 0; i < block->size(); ++i) {
                    Node* putNode = block->at(i);
                    if (putNode->op() != PutByOffset)
                        continue;
                    Node* storage = putNode->child1().node();
                    Node* base = putNode->child2().node();
                    Node* value = putNode->child3().node();
                    if (!base || !value || value->op() != MakeRope)
                        continue;
                    StorageAccessData& putSad = putNode->storageAccessData();

                    // The accumulator fiber must read the same field of the same object.
                    Node* wrapper = nullptr;
                    Node* getNode = peelCoercion(value->child1(), wrapper);
                    if (!getNode || getNode->op() != GetByOffset)
                        continue;
                    if (getNode->child2().node() != base)
                        continue;
                    StorageAccessData& getSad = getNode->storageAccessData();
                    if (getSad.offset != putSad.offset || getSad.identifierNumber != putSad.identifierNumber)
                        continue;
                    // Conservative: inline property only (storage == base for both), so there is no
                    // separate butterfly node to reason about.
                    if (storage != base || getNode->child1().node() != base)
                        continue;
                    // The base must be a fresh, un-aliased object read from a local defined solely
                    // by a NewObject outside any loop.
                    if (base->op() != GetLocal)
                        continue;
                    Operand baseLocal = base->operand();
                    if (!baseLocalIsFreshNewObject(baseLocal))
                        continue;

                    ++candidates;
                    if (sbVerbose()) {
                        dataLogLn("[SB-FIELD] candidate MakeRope D@", value->index(),
                            " base loc ", baseLocal.value(), " field id", getSad.identifierNumber,
                            " off ", getSad.offset, " bc#", value->origin.semantic.bytecodeIndex().offset(),
                            " in ", m_graph.m_codeBlock->inferredName().data());
                    }
                    if (hasUnsafeFieldUse(baseLocal, base, getNode, putNode, value, wrapper, getSad.offset, getSad.identifierNumber)) {
                        if (sbVerbose())
                            dataLogLn("[SB-FIELD] bailed: unsafe use of base/builder value");
                        continue;
                    }

                    // Same 2->3 input flattening as the local case: pull a leaf-only piece rope up
                    // so a single 3-input append avoids allocating the piece rope.
                    if (!value->child3()) {
                        Node* piece = value->child2().node();
                        if (piece && piece->op() == MakeRope && piece->child2() && !piece->child3()) {
                            Node* a = piece->child1().node();
                            Node* b = piece->child2().node();
                            if (a && b && a->op() != MakeRope && b->op() != MakeRope) {
                                value->child2() = Edge(a, KnownStringUse);
                                value->child3() = Edge(b, KnownStringUse);
                            }
                        }
                    }

                    unsigned off = value->origin.semantic.bytecodeIndex().offset();
                    m_graph.m_stringBuilderConcatBytecodes.add(off);
                    if (sbVerbose())
                        dataLogLn("[SB-FIELD] tagged field accumulator (", value->child3() ? "3" : "2", "-input) at offset=", off);
                    changed = true;
                }
            }
        }

        // CHAINED accumulator (`V += p0 + p1 + p2 + ...`, more than three operands). One op_strcat
        // becomes a left-leaning chain of <=3-operand nodes, each taking the previous as its first
        // operand, with the accumulator read buried at the bottom-left leaf. The nodes are all
        // StrCat when the operands are Untyped, or all MakeRope once Fixup proves them strings (which
        // happens in FTL after the operand-producing calls inline) -- the accumulator read is a
        // MakeRope's first child in the latter, or a ToPrimitive(GetLocal) in the former. All chain
        // nodes share the op_strcat bytecode, so collapsing them into ONE variadic
        // StringBuilderStrCatMany keeps the whole op atomic w.r.t. OSR (a chain of in-place appends
        // would double-count on exit) while building the string once instead of allocating the
        // chain of intermediate ropes. Emitted in both DFG and FTL; each tier has its own inline
        // append fast path in compileStringBuilderStrCatMany.
        if (!sbNoStrCat()) {
            for (BlockIndex blockIndex = m_graph.numBlocks(); blockIndex--;) {
                BasicBlock* block = m_graph.block(blockIndex);
                if (!block || !m_graph.m_cpsNaturalLoops->loopDepth(block))
                    continue;
                for (unsigned i = 0; i < block->size(); ++i) {
                    Node* node = block->at(i);
                    if (node->op() != SetLocal)
                        continue;
                    Operand dst = node->operand();
                    Node* top = node->child1().node();
                    if (!top || (top->op() != StrCat && top->op() != MakeRope))
                        continue;
                    NodeType chainOp = top->op();

                    // Descend the chain along child1 (the accumulator spine) while the first operand
                    // is another node of the same kind from the same op_strcat bytecode.
                    BytecodeIndex bc = top->origin.semantic.bytecodeIndex();
                    Node* bottom = top;
                    while (true) {
                        Node* first = bottom->child1().node();
                        if (first && first->op() == chainOp && first->origin.semantic.bytecodeIndex() == bc)
                            bottom = first;
                        else
                            break;
                    }

                    // A single MakeRope accumulator (V + up to two pieces, no chain) is handled by the
                    // inlined-append path above; only take over the multi-node chains here.
                    if (chainOp == MakeRope && bottom == top)
                        continue;

                    // The spine bottom's first operand must be the accumulator read (through at most
                    // one value-preserving coercion, e.g. the ToPrimitive Fixup inserts for StrCat).
                    Node* readWrapper = nullptr;
                    if (!readsLocalThroughCoercion(bottom->child1(), dst, readWrapper))
                        continue;

                    ++candidates;
                    if (sbVerbose()) {
                        dataLogLn("[SB-STRCAT] chain accumulator (", Graph::opName(chainOp), ") top D@", top->index(),
                            " bottom D@", bottom->index(), " loc ", dst.value(), " bc#", bc.offset(),
                            " in ", m_graph.m_codeBlock->inferredName().data());
                    }

                    if (hasUnsafeInLoopUse(dst, bottom, readWrapper))
                        continue;
                    // Same multi-site guard as the local MakeRope path: a local appended at more than
                    // one site in the loop does not reuse the builder across the back-edge.
                    if (hasMultipleConcatStoresInLoop(dst))
                        continue;

                    // Collect the pieces in concatenation order: walk bottom-to-top, each chain node
                    // contributing child2 then child3 (its non-spine operands).
                    Vector<Node*, 8> chainTopToBottom;
                    for (Node* c = top; ; c = c->child1().node()) {
                        chainTopToBottom.append(c);
                        if (c == bottom)
                            break;
                    }
                    Vector<Edge, 8> pieces;
                    for (unsigned k = chainTopToBottom.size(); k--;) {
                        Node* c = chainTopToBottom[k];
                        pieces.append(Edge(c->child2().node(), UntypedUse));
                        if (c->child3())
                            pieces.append(Edge(c->child3().node(), UntypedUse));
                    }

                    // Keep the accumulator's original first-operand edge (the ToPrimitive/GetLocal),
                    // so the coercion order matches the source op_strcat exactly.
                    Edge accEdge = Edge(bottom->child1().node(), UntypedUse);
                    top->convertToStringBuilderStrCatMany(m_graph, accEdge, pieces);
                    if (sbVerbose())
                        dataLogLn("[SB-STRCAT] collapsed ", chainTopToBottom.size(), " node(s) -> ", pieces.size(), " pieces");
                    changed = true;
                }
            }
        }

        if (candidates && sbVerbose()) {
            dataLogLn("[SB] ", candidates, " string-accumulator candidate(s) in ",
                m_graph.m_codeBlock->inferredName().data());
        }
        return changed;
    }
};

bool performStringBuilderConcat(Graph& graph)
{
    return runPhase<StringBuilderConcatPhase>(graph);
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
