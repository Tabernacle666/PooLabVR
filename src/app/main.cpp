#include "AppSession.h"
#include "SolarSystem.h"
#include <cwchar>
#include <shellapi.h>
#include <thread>

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    std::wstring worldPath,diagnosticDirectory;
    bool interactive=false,desktop=false,automatedDlss=false,sphereBaseline=false;
    bool orbitCalibration=false,fieldCalibration=false,solarCalibration=false,passageCalibration=false;
    bool lunarCalibration=false,shuttleCalibration=false;
    bool building=false,buildingLoop=false;
    int count=0;
    auto* args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args)OASIS::Fail("Command line");
    for(int i=1;i<count;++i){
        if(wcscmp(args[i],L"--world")==0&&i+1<count)
            worldPath=args[++i];
        else if(wcscmp(args[i],L"--diagnostic")==0&&i+1<count)
            diagnosticDirectory=args[++i];
        else if(wcscmp(args[i],L"--interactive")==0)interactive=true;
        else if(wcscmp(args[i],L"--flat")==0)desktop=true;
        else if(wcscmp(args[i],L"--dlss-loop")==0)automatedDlss=true;
        else if(wcscmp(args[i],L"--sphere-baseline")==0)sphereBaseline=true;
        else if(wcscmp(args[i],L"--sphere-phase2")==0)orbitCalibration=true;
        else if(wcscmp(args[i],L"--sphere-phase3")==0)fieldCalibration=true;
        else if(wcscmp(args[i],L"--solar-system")==0)solarCalibration=true;
        else if(wcscmp(args[i],L"--solar-passage")==0)passageCalibration=true;
        else if(wcscmp(args[i],L"--lunar")==0)lunarCalibration=true;
        else if(wcscmp(args[i],L"--shuttle")==0)shuttleCalibration=true;
        else if(wcscmp(args[i],L"--building")==0)building=true;
        else if(wcscmp(args[i],L"--building-loop")==0)buildingLoop=building=true;
        else OASIS::Fail("Unknown option");
    }
    LocalFree(args);
    const auto sphereScenarios=unsigned(sphereBaseline)+unsigned(orbitCalibration)+unsigned(fieldCalibration)+
        unsigned(solarCalibration)+unsigned(passageCalibration)+unsigned(lunarCalibration)+unsigned(shuttleCalibration);
    const bool sphereDiagnostic=sphereScenarios!=0u;
    if((sphereDiagnostic&&(automatedDlss||!worldPath.empty()))||
       sphereScenarios>1u)
        OASIS::Fail("Sphere diagnostics use one native VR scenario");
    if(building&&(automatedDlss||sphereDiagnostic||!worldPath.empty()))OASIS::Fail("Building pilot uses the OASIS island");
    if(automatedDlss||sphereDiagnostic||buildingLoop){
        desktop=false;
        interactive=false;
        if(diagnosticDirectory.empty())diagnosticDirectory=L"diagnostics";
    }
    if(!diagnosticDirectory.empty()&&!OASIS::StartDiagnostic(diagnosticDirectory)){
        if(!automatedDlss&&!sphereDiagnostic)MessageBoxW(nullptr,L"Impossible de creer le journal OASIS.",L"OASIS",MB_OK|MB_ICONERROR);
        return 1;
    }
    if(!diagnosticDirectory.empty())
        OASIS::DiagnosticLine(buildingLoop?"diagnostic=megastructure\r\n":shuttleCalibration?"diagnostic=navette\r\n":lunarCalibration?"diagnostic=lune\r\n":
            passageCalibration?"diagnostic=franchissement\r\n":solarCalibration?"diagnostic=systeme_solaire\r\n":fieldCalibration?"diagnostic=sphere_phase3\r\n":
            orbitCalibration?"diagnostic=sphere_phase2\r\n":sphereBaseline?"diagnostic=sphere_avant\r\n":
            automatedDlss?"diagnostic=minimal\r\n":"diagnostic=complet\r\n");
    const std::wstring capturePath=diagnosticDirectory.empty()?std::wstring{}:diagnosticDirectory+L"\\OASIS.png";
    HWND window=OASIS::CreateOutputWindow(instance);
    int result=0;
    std::atomic<bool> stop{};
    std::atomic<bool> finished{};
    std::atomic<bool> frameSeen{};
    std::atomic<ULONGLONG> modeStarted{GetTickCount64()};
    const uint32_t passageOwner=lunarCalibration?OASIS::SolarMoon:passageCalibration?0u:OASIS::Invalid;
    std::thread render([&]{result=OASIS::Run(stop,frameSeen,modeStarted,window,worldPath,capturePath,interactive,desktop,
        automatedDlss,sphereBaseline,orbitCalibration,fieldCalibration,solarCalibration,shuttleCalibration,passageOwner,building,buildingLoop);
        finished.store(true,std::memory_order_release);});
    ULONGLONG exitDeadline=0,lastStallReport=0;
    bool stalled=false;
    const ULONGLONG diagnosticDeadline=capturePath.empty()?0:GetTickCount64()+((sphereDiagnostic||buildingLoop)?180000:60000);
    while(!finished.load(std::memory_order_acquire)) {
        MSG message;
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            if(message.message==WM_QUIT)stop.store(true,std::memory_order_relaxed);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if(GetAsyncKeyState(VK_ESCAPE)&0x8000)stop.store(true,std::memory_order_relaxed);
        if(interactive)OASIS::ReportSessionStall(lastStallReport,stalled);
        const bool startupExpired=interactive&&!frameSeen.load(std::memory_order_acquire)&&
            GetTickCount64()-modeStarted.load(std::memory_order_acquire)>=60000;
        if(diagnosticDeadline&&(startupExpired||(!interactive&&GetTickCount64()>=diagnosticDeadline))&&!stop.exchange(true,std::memory_order_relaxed)){
            OASIS::DiagnosticLine((sphereDiagnostic||buildingLoop)?"erreur=delai_de_180_secondes\r\n":
                "erreur=delai_de_60_secondes\r\n");
            if(!OASIS::diagnosticCaptured.load(std::memory_order_acquire))OASIS::DiagnosticLine("capture=aucune\r\n");
        }
        if(stop.load(std::memory_order_relaxed)){
            if(!exitDeadline)exitDeadline=GetTickCount64()+3000;
            else if(GetTickCount64()>=exitDeadline&&!finished.load(std::memory_order_acquire)){
                if(!OASIS::diagnosticCaptured.load(std::memory_order_acquire))OASIS::DiagnosticLine("capture=aucune\r\n");
                OASIS::DiagnosticLine("resultat=fermeture_forcee\r\n");
                TerminateProcess(GetCurrentProcess(),1);
            }
        }
        Sleep(5);
    }
    render.join();
    if(IsWindow(window))DestroyWindow(window);
    return result;
}
