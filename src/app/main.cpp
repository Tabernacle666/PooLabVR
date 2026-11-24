#include "AppSession.h"
#include "SessionLaunch.h"
#include "SolarSystem.h"
#include <cwchar>
#include <cstdio>
#include <exception>
#include <shellapi.h>
#include <thread>

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    std::wstring worldPath,diagnosticDirectory;
    bool interactive=false,desktop=false,automatedDlss=false;
    bool solarCalibration=false,passageCalibration=false,shuttleCalibration=false;
    bool building=false,buildingLoop=false,mg2=false;
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
        else if(wcscmp(args[i],L"--solar-system")==0)solarCalibration=true;
        else if(wcscmp(args[i],L"--solar-passage")==0)passageCalibration=true;
        else if(wcscmp(args[i],L"--shuttle")==0)shuttleCalibration=true;
        else if(wcscmp(args[i],L"--building")==0)building=true;
        else if(wcscmp(args[i],L"--building-loop")==0)buildingLoop=building=true;
        else if(wcscmp(args[i],L"--mg2")==0)mg2=true;
        else OASIS::Fail("Unknown option");
    }
    LocalFree(args);
    const auto sphereScenarios=unsigned(solarCalibration)+unsigned(passageCalibration)+unsigned(shuttleCalibration);
    const bool sphereDiagnostic=sphereScenarios!=0u;
    const bool sessionLaunch=OASIS::LaunchSessionMode()!=OASIS::SessionMode::Default;
    if((sphereDiagnostic&&(automatedDlss||!worldPath.empty()))||
       sphereScenarios>1u)
        OASIS::Fail("Sphere diagnostics use one native VR scenario");
    if(building&&(automatedDlss||sphereDiagnostic||!worldPath.empty()))OASIS::Fail("Building pilot uses the OASIS island");
    if(mg2){
        if(automatedDlss||sphereDiagnostic||buildingLoop||desktop||!worldPath.empty())
            OASIS::Fail("MG2 diagnostic uses the live OASIS campus in VR");
        interactive=true;
        if(diagnosticDirectory.empty())diagnosticDirectory=L"diagnostics\\mg2";
    }
    if(automatedDlss||sphereDiagnostic||buildingLoop){
        desktop=false;
        interactive=false;
        if(diagnosticDirectory.empty())diagnosticDirectory=L"diagnostics";
    }
    if(!diagnosticDirectory.empty()&&!OASIS::StartDiagnostic(diagnosticDirectory)){
        const auto message=L"Impossible de creer le journal OASIS dans :\n"+diagnosticDirectory;
        if(!automatedDlss&&!sphereDiagnostic)MessageBoxW(nullptr,message.c_str(),L"OASIS",MB_OK|MB_ICONERROR);
        return 1;
    }
    if(!diagnosticDirectory.empty())
        OASIS::DiagnosticLine(mg2?"diagnostic=mg2\r\n":buildingLoop?"diagnostic=megastructure\r\n":shuttleCalibration?"diagnostic=navette\r\n":
            passageCalibration?"diagnostic=franchissement\r\n":solarCalibration?"diagnostic=systeme_solaire\r\n":
            automatedDlss?"diagnostic=minimal\r\n":"diagnostic=complet\r\n");
    if(sessionLaunch){
        switch(OASIS::LaunchSessionMode()){
            case OASIS::SessionMode::Cosmos:OASIS::DiagnosticLine("session_mode=cosmos\r\n");break;
            case OASIS::SessionMode::Forest:OASIS::DiagnosticLine("session_mode=foret\r\n");break;
            case OASIS::SessionMode::Hall:OASIS::DiagnosticLine("session_mode=hall\r\n");break;
            case OASIS::SessionMode::Loop:OASIS::DiagnosticLine("session_mode=loop\r\n");break;
            case OASIS::SessionMode::Sun:OASIS::DiagnosticLine("session_mode=sun\r\n");break;
            case OASIS::SessionMode::Plafond:OASIS::DiagnosticLine("session_mode=plafond\r\n");break;
            default:break;
        }
    }
    const std::wstring capturePath=diagnosticDirectory.empty()?std::wstring{}:diagnosticDirectory+L"\\OASIS.png";
    HWND window=OASIS::CreateOutputWindow(instance);
    int result=0;
    std::atomic<bool> stop{};
    std::atomic<bool> finished{};
    std::atomic<bool> frameSeen{};
    std::atomic<ULONGLONG> startupProgress{GetTickCount64()};
    const uint32_t passageOwner=passageCalibration?0u:OASIS::Invalid;
    std::thread render([&]{
        try{
            result=OASIS::Run(stop,frameSeen,startupProgress,window,worldPath,capturePath,interactive,desktop,
                automatedDlss,solarCalibration,shuttleCalibration,passageOwner,building,buildingLoop,mg2);
        }catch(const std::exception& error){
            char line[640];
            std::snprintf(line,sizeof(line),"erreur=exception_render_non_interceptee raison=%.480s\r\n",error.what());
            OASIS::DiagnosticLine(line);
            result=1;
        }catch(...){
            OASIS::DiagnosticLine("erreur=exception_render_inconnue\r\n");
            result=1;
        }
        finished.store(true,std::memory_order_release);
    });
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
        const bool waitingForFirstFrame=interactive&&!frameSeen.load(std::memory_order_acquire);
        const auto lastStartupProgress=startupProgress.load(std::memory_order_acquire);
        const bool startupExpired=waitingForFirstFrame&&GetTickCount64()-lastStartupProgress>=(sessionLaunch?900000ull:60000ull);
        if(diagnosticDeadline&&(startupExpired||(!interactive&&GetTickCount64()>=diagnosticDeadline))&&!stop.exchange(true,std::memory_order_relaxed)){
            OASIS::DiagnosticLine((sphereDiagnostic||buildingLoop)?"erreur=delai_de_180_secondes\r\n":
                sessionLaunch?"erreur=demarrage_sans_progression_900_secondes\r\n":
                mg2?"erreur=demarrage_sans_progression_60_secondes\r\n":"erreur=delai_de_60_secondes\r\n");
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
