#pragma once
#include "AppSession.h"
#include "Graphics.h"
#include "SolarSystem.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <optional>
#include <utility>
#include <vector>

namespace OASIS {

class OasisLodTour {
    using Level=SphereRepresentation;
    static constexpr XrDuration Settle=650'000'000,Measure=3'000'000'000;
    static constexpr size_t BoundaryCount=8,StopCount=3*BoundaryCount;
    struct Boundary {
        const char* name;
        float surface;
        Level automatic,nearLevel,farLevel;
    };
    struct Window {
        uint64_t first{},last{};
        unsigned requested{},submitted{},matched{},nativeOasis{},nativeOther{},lunarNative{};
        unsigned relief{},reliefUltra{};
        std::array<std::vector<float>,4> timings;
    };
    std::filesystem::path directory_;
    std::array<Boundary,BoundaryCount> boundaries_;
    std::array<Window,StopCount> windows_;
    size_t index_{};
    XrTime started_{};
    bool requestedCapture_{},failed_{};
    std::future<bool> saving_;

    const Boundary& Shot(size_t index)const{
        return boundaries_[index<BoundaryCount?index:(index-BoundaryCount)/2];
    }
    std::optional<Level> Override(size_t index)const{
        if(index<BoundaryCount)return std::nullopt;
        const auto& shot=Shot(index);
        return (index-BoundaryCount)%2==0?shot.farLevel:shot.nearLevel;
    }
    static const char* Name(Level level){
        switch(level){
        case Level::Depth:return "relief";
        case Level::DepthUltra:return "relief_ultra";
        case Level::Geometry:return "3d";
        default:return "autre";
        }
    }
    static float Percentile(std::vector<float>& data,double quantile){
        if(data.empty())return -1.f;
        const auto position=data.begin()+size_t(std::ceil(double(data.size())*quantile))-1;
        std::nth_element(data.begin(),position,data.end());
        return *position;
    }
public:
    OasisLodTour(std::filesystem::path directory,const SphereLodSettings& settings)
        :directory_(std::move(directory)){
        const auto enter=OasisNativeSurfaceRange(settings,false),exit=OasisNativeSurfaceRange(settings,true);
        constexpr float d=.25f;
        boundaries_={{
            {"entree_relief_exterieur",OasisReliefEnter+d,Level::Depth,Level::DepthUltra,Level::Depth},
            {"entree_relief_interieur",OasisReliefEnter-d,Level::DepthUltra,Level::DepthUltra,Level::Depth},
            {"entree_3d_exterieur",enter+d,Level::DepthUltra,Level::Geometry,Level::DepthUltra},
            {"entree_3d_interieur",enter-d,Level::Geometry,Level::Geometry,Level::DepthUltra},
            {"sortie_3d_interieur",exit-d,Level::Geometry,Level::Geometry,Level::DepthUltra},
            {"sortie_3d_exterieur",exit+d,Level::DepthUltra,Level::Geometry,Level::DepthUltra},
            {"sortie_relief_interieur",OasisReliefExit-d,Level::DepthUltra,Level::DepthUltra,Level::Depth},
            {"sortie_relief_exterieur",OasisReliefExit+d,Level::Depth,Level::DepthUltra,Level::Depth}
        }};
        for(auto& window:windows_)for(auto& timings:window.timings)timings.reserve(240);
        DiagnosticLine("oasis_lod=24_poses mesure_s=3 orbites_figees campus=neuf angle=oblique\r\n");
    }
    void PoseForFrame(XRFrame& frame)const{
        if(Finished())return;
        const auto radial=Unit(XMFLOAT3{.48f,.21f,-1.f});
        const auto body=SolarBodyToCosmos(0u,SolarBody(0u,0.));
        const auto eye=Add(SpectacleCenter,Scale(radial,SpectacleRadius+Shot(index_).surface));
        frame.input.head=SolarLookAt(TransformPoint(eye,body),TransformPoint(SpectacleCenter,body));
        frame.input.solarTime=0.;
        frame.input.insideSphere=Invalid;
        frame.input.sceneUnitsPerMeter=1.f;
        frame.input.farClipMeters=FarZ;
        frame.input.trackingRevision=2000u+index_;
        frame.sphereForces[0]=Override(index_);
    }
    void Before(Graphics& graphics,const XRFrame& frame){
        if(Finished()||!started_||requestedCapture_||graphics.CapturePending()||
           frame.displayTime-started_<Settle+Measure)return;
        const auto& shot=Shot(index_);
        char file[96];
        const auto mode=Override(index_);
        std::snprintf(file,sizeof(file),"%02u-%s-%s.png",unsigned(index_+1),shot.name,
                      mode?Name(*mode):"auto");
        graphics.RequestCapture((directory_/file).wstring());
        requestedCapture_=true;
    }
    void Collect(Graphics& graphics){
        std::array<SphereGpuTimings,12> samples{};
        const size_t count=graphics.ReadSphereGpuTimings(samples);
        for(size_t i=0;i<count;++i){
            const auto& sample=samples[i];
            for(auto& window:windows_){
                if(!window.first||sample.frame<window.first||sample.frame>window.last)continue;
                if(sample.directMs>=0.f)window.timings[1].push_back(sample.directMs);
                if(sample.computeMs>=0.f)window.timings[2].push_back(sample.computeMs);
                if(sample.presentMs>=0.f)window.timings[3].push_back(sample.presentMs);
                break;
            }
        }
    }
    void After(Graphics& graphics,const XRFrame& frame,double cpuMs){
        if(Finished())return;
        if(!started_){
            if(frame.submitted&&frame.input.focused)started_=frame.displayTime;
            return;
        }
        const auto elapsed=frame.displayTime-started_-Settle;
        if(elapsed>=0&&elapsed<Measure){
            auto& window=windows_[index_];
            window.requested+=frame.render;
            if(frame.submitted){
                if(!window.first)window.first=frame.input.frameId;
                window.last=frame.input.frameId;
                ++window.submitted;
                const auto actual=frame.sphereLevels[0];
                const auto expected=Override(index_).value_or(Shot(index_).automatic);
                window.matched+=actual==expected&&
                    (expected!=Level::Geometry||frame.sphereNative==0u);
                window.nativeOasis+=frame.sphereNative==0u;
                window.nativeOther+=frame.sphereNative!=0u&&frame.sphereNative!=Invalid;
                window.lunarNative+=frame.lunarGeometry;
                window.relief+=actual==Level::Depth;
                window.reliefUltra+=actual==Level::DepthUltra;
                window.timings[0].push_back(float(cpuMs));
            }
        }
        Collect(graphics);
        if(requestedCapture_&&!saving_.valid()&&graphics.CaptureDone())
            saving_=graphics.FinishCaptureAsync();
        if(!saving_.valid()||saving_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        try{failed_=!saving_.get();}catch(...){failed_=true;}
        if(failed_)return;
        ++index_;
        started_=frame.displayTime;
        requestedCapture_=false;
    }
    bool Finished()const{return failed_||index_==StopCount;}
    bool Complete()const{return index_==StopCount;}
    bool Write(){
        bool valid=Complete();
        for(size_t i=0;i<StopCount;++i){
            auto& window=windows_[i];
            const auto& shot=Shot(i);
            const auto mode=Override(i);
            const auto expected=mode.value_or(shot.automatic);
            const bool correct=window.submitted>0&&window.submitted==window.matched&&!window.timings[1].empty();
            valid&=correct;
            const double fps=double(window.submitted)*1.e9/double(Measure);
            char line[512];
            std::snprintf(line,sizeof(line),
                "lod_pose=%02u seuil=%s peau_m=%.2f mode=%s attendu=%s images=%u demandees=%u "
                "conformes=%u relief=%u ultra=%u 3d_oasis=%u 3d_autre=%u lune_3d=%u "
                "fps_soumis=%.2f gpu_echantillons=%zu "
                "cpu_p95_ms=%.3f direct_p50_ms=%.3f direct_p95_ms=%.3f compute_p95_ms=%.3f "
                "present_p95_ms=%.3f valide=%u\r\n",
                unsigned(i+1),shot.name,shot.surface,mode?"force":"auto",Name(expected),window.submitted,
                window.requested,window.matched,window.relief,window.reliefUltra,window.nativeOasis,window.nativeOther,
                window.lunarNative,fps,window.timings[1].size(),
                Percentile(window.timings[0],.95),Percentile(window.timings[1],.50),
                Percentile(window.timings[1],.95),Percentile(window.timings[2],.95),
                Percentile(window.timings[3],.95),unsigned(correct));
            DiagnosticLine(line);
        }
        return valid;
    }
};
}
