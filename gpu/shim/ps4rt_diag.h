// SPDX-License-Identifier: GPL-2.0-or-later
// Runtime bridge only. Full developer diagnostics live under tools/diagnostics/native.
// Public/player sources compile without the developer toolkit.
#pragma once
#ifndef PS4RT_DIAGNOSTICS
#define PS4RT_DIAGNOSTICS 0
#endif
#if PS4RT_DIAGNOSTICS
#include "ps4rt_diag_impl.h"
#else
namespace Ps4RtDiag {
inline constexpr bool Built = false;
// The renderer retains a diagnostic state member; no diagnostic behavior in players.
struct Rules {};
} // namespace Ps4RtDiag
#endif
