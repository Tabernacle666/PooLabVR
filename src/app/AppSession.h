#pragma once
#include "Core.h"
#include <atomic>
#include <string>

namespace OASIS {
extern std::atomic<bool> diagnosticCaptured;
bool StartDiagnostic(const std::wstring& directory);
void DiagnosticLine(const char* text) noexcept;
void MarkDiagnosticStartupProgress() noexcept;
void ReportSessionStall(ULONGLONG& lastStallReport,bool& stalled);
HWND CreateOutputWindow(HINSTANCE instance);

int Run(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,std::atomic<ULONGLONG>& startupProgress,
        HWND window,const std::wstring& worldPath,const std::wstring& capturePath,
        bool interactive,bool desktop,bool automatedDlss,bool sphereBaseline,
        bool orbitCalibration,bool fieldCalibration,bool solarCalibration,bool shuttleCalibration,
        uint32_t passageOwner,bool building,bool buildingLoop,bool oasisLodLoop,bool mg2);
}
