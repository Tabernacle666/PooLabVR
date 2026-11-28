#pragma once
#include "Core.h"
#include <atomic>
#include <string>

namespace OASIS {
extern std::atomic<bool> diagnosticCaptured;
extern std::atomic<ULONGLONG> territoryStartedTick;
bool StartDiagnostic(const std::wstring& directory);
bool FlushDiagnostic() noexcept;
void DiagnosticLine(const char* text) noexcept;
void MarkDiagnosticStartupProgress() noexcept;
void ReportSessionStall(ULONGLONG& lastStallReport,bool& stalled);
HWND CreateOutputWindow(HINSTANCE instance);

int Run(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,std::atomic<ULONGLONG>& startupProgress,
        HWND window,const std::wstring& worldPath,const std::wstring& capturePath,
        bool interactive,bool desktop,bool automatedDlss,
        bool solarCalibration,bool shuttleCalibration,
        uint32_t passageOwner,bool building,bool buildingLoop,bool mg2);
}
