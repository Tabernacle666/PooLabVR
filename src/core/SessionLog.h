#pragma once
// Rendering has no runtime journal. Legacy trace call sites are compiled away.
#define LogEvent(...) ((void)0)
namespace lab {
inline bool SessionLogCompact() noexcept{return false;}
inline bool SessionLogEnabled() noexcept{return false;}
}
