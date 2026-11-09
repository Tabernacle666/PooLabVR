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
// The fixed-camera control window suspends both formatting and background disk writes.
void SetSessionLogQuiet(bool quiet) noexcept;
// Normal VR logs remain low-volume even when --log is explicitly requested.
void SetSessionLogGameplay(bool gameplay) noexcept;
void LogFatal(const char* message) noexcept;
#else
inline void StartSessionLog(const wchar_t*) {}
inline void StopSessionLog() noexcept {}
inline bool SessionLogEnabled() noexcept {return false;}
inline void LogFatal(const char*) noexcept {}
inline void SetSessionLogCompact(bool) noexcept {}
inline bool SessionLogCompact() noexcept {return false;}
inline void SetSessionLogQuiet(bool) noexcept {}
inline void SetSessionLogGameplay(bool) noexcept {}
#define LogEvent(...) ((void)0)
#endif
}
