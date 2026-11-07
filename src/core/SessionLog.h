#pragma once
#include "Core.h"
#ifndef LABOVR_DIAGNOSTIC_TRACE
#define LABOVR_DIAGNOSTIC_TRACE 0
#endif
namespace lab {
#if LABOVR_DIAGNOSTIC_TRACE
void StartSessionLog(const wchar_t* path);
void StopSessionLog() noexcept;
bool SessionLogEnabled() noexcept;
void LogEvent(uint64_t frame,const char* stage,const char* format,...) noexcept;
void SetSessionLogCompact(bool compact) noexcept;
bool SessionLogCompact() noexcept;
void LogFatal(const char* message) noexcept;
#else
inline void StartSessionLog(const wchar_t*) {}
inline void StopSessionLog() noexcept {}
inline bool SessionLogEnabled() noexcept {return false;}
inline void LogFatal(const char*) noexcept {}
inline void SetSessionLogCompact(bool) noexcept {}
inline bool SessionLogCompact() noexcept {return false;}
#define LogEvent(...) ((void)0)
#endif
}
