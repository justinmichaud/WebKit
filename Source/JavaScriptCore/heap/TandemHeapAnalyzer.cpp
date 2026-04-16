/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "TandemHeapAnalyzer.h"

#if ENABLE(REFTRACKER) && __has_include(<meta>)

namespace JSC {

void TandemHeapAnalyzer::recordCell(JSCell* cell)
{
    if (!cell)
        return;
    Locker locker { m_lock };
    auto [it, inserted] = m_seen.insert(static_cast<const void*>(cell));
    if (!inserted)
        return;
    if (m_cellCallback)
        m_cellCallback(m_userData, cell);
}

void TandemHeapAnalyzer::analyzeNode(JSCell* cell)
{
    recordCell(cell);
}

void TandemHeapAnalyzer::analyzeEdge(JSCell* /*from*/, JSCell* to, RootMarkReason)
{
    recordCell(to);
}

void TandemHeapAnalyzer::analyzePropertyNameEdge(JSCell*, JSCell* to, UniquedStringImpl*)
{
    recordCell(to);
}

void TandemHeapAnalyzer::analyzeVariableNameEdge(JSCell*, JSCell* to, UniquedStringImpl*)
{
    recordCell(to);
}

void TandemHeapAnalyzer::analyzeIndexEdge(JSCell*, JSCell* to, uint32_t)
{
    recordCell(to);
}

} // namespace JSC

#endif // ENABLE(REFTRACKER) && __has_include(<meta>)
