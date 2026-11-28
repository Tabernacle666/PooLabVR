#pragma once
#include "AppSession.h"
#include "BuildingCampus.h"
#include "TargetCalibration.h"
#include <cstdio>

namespace OASIS {
inline constexpr double TerritoryDuration=84.;

class TerritoryBenchmark {
    enum Phase : size_t { Warmup, Reference0, Grow29, Reference29, Forest, Interior, Island, Edit, Count };
    struct Stage { double end; const char* name; };
    static constexpr std::array<Stage,Count> Stages{{
        {6.,"preheat"},{13.,"reference_0"},{25.,"croissance_29"},{32.,"reference_29"},
        {44.,"foret_berge"},{56.,"couloir_bureau_vitrage"},{68.,"toit_dessous"},{TerritoryDuration,"croissance_30_breche"}
    }};
    struct Image {
        uint64_t id;
        Phase phase;
        float cpu,interval;
        float gpu{-1.f},depth{-1.f},opaqueResident{-1.f},rasterCull{-1.f};
    };
    struct Segment {
        unsigned attempts{},submitted{},referenceMismatch{},mg2Failures{};
        uint32_t steps{},cameraInstances{},registry{},routed{},variants{};
        uint64_t detailRoutes{},nodes{},readbackBytes{},vram{},budget{};
        float ageMs{};
    };
    struct Growth {
        uint32_t target{},previousRegistry{};
        int64_t requested{};
        double published{-1.},routed{-1.};
        bool accepted{};
    };
    BuildingCampus& campus_;
    const Pose office_;
    const XMFLOAT3 hallEye_,hallToward_,hallAcross_,glassEye_,glassLook_,glassEnd_,roofEye_;
    std::array<Segment,Count> segments_{};
    std::vector<Image> images_;
    Growth grow29_{29u},grow30_{30u};
    int64_t origin_{},lastSubmit_{},breachRequested_{};
    Phase lastPhase_{Count};
    uint64_t beforeBreach_{};
    double breachPublished_{-1.};
    bool breachQueued_{};
    bool profileLogged_{};

    static Phase PhaseAt(double elapsed) noexcept {
        const auto found=std::find_if(Stages.begin(),Stages.end(),[elapsed](const Stage& stage){return elapsed<stage.end;});
        return static_cast<Phase>(found-Stages.begin());
    }
    static XMFLOAT3 Mix(XMFLOAT3 a,XMFLOAT3 b,float t) noexcept {return Add(a,Scale(Sub(b,a),t));}
    Pose Reference(double seconds) const {
        const float t=float(std::clamp(seconds/7.,0.,1.));
        const auto base=campus_.Pilot().Spawn().p;
        const auto eye=Add(base,{std::lerp(-1.75f,1.75f,t),0.f,-4.f});
        return SolarLookAt(eye,Add(base,{std::lerp(-5.f,11.f,t),2.3f,11.f}));
    }
    void Request(Growth& event,const Mg2ClusterStatus& mg2){
        event.requested=QpcCounter();
        event.previousRegistry=mg2.registryInstances;
        const auto current=campus_.ContinentSteps();
        event.accepted=current<event.target&&campus_.GrowContinent(event.target-current);
    }
    static void Observe(Growth& event,uint32_t steps,const Mg2ClusterStatus& mg2,const XRFrame& frame,int64_t now){
        if(!event.accepted||steps!=event.target)return;
        if(event.published<0.)event.published=1000.*QpcSeconds(now-event.requested);
        if(event.routed<0.&&frame.submitted&&frame.forestPages&&mg2.registryInstances>event.previousRegistry&&
           mg2.selectionEligible==mg2.registryInstances&&mg2.routedInstances==mg2.registryInstances&&!frame.forestRequestFailed)
            event.routed=1000.*QpcSeconds(now-event.requested);
    }
    void Collect(Graphics& graphics){
        std::array<SphereGpuTimings,12> gpu{};
        const auto count=graphics.ReadSphereGpuTimings(gpu);
        for(size_t i=0;i<count;++i){
            const auto& s=gpu[i];
            const auto image=std::lower_bound(images_.begin(),images_.end(),s.frame,
                [](const Image& sample,uint64_t id){return sample.id<id;});
            if(image==images_.end()||image->id!=s.frame)continue;
            image->gpu=s.gpuImageSpanMs;
            image->depth=s.PassMs(DirectGpuPass::CpuDepth);
            image->opaqueResident=s.PassMs(DirectGpuPass::OpaqueResident);
            image->rasterCull=s.PassMs(DirectGpuPass::RasterCull);
        }
    }
    struct Distribution {
        size_t count{};
        float p50{-1.f},p95{-1.f};
    };
    Distribution Summarize(Phase phase,float Image::* metric) const {
        std::vector<float> values;
        values.reserve(segments_[phase].submitted);
        for(const auto& image:images_){
            const float value=image.*metric;
            if(image.phase==phase&&std::isfinite(value)&&value>=0.f)values.push_back(value);
        }
        if(values.empty())return {};
        std::sort(values.begin(),values.end());
        return {values.size(),values[(values.size()-1u)/2u],values[values.size()-1u-values.size()/20u]};
    }
public:
    TerritoryBenchmark(BuildingCampus& campus,std::optional<Pose> office)
        :campus_(campus),office_(office.value_or(SolarLookAt(
            campus.Pilot().Scene({1.72f,campus.Pilot().Definition().Elevation(1)+1.52f,.83f}),
            campus.Pilot().Scene({1.72f,campus.Pilot().Definition().Elevation(1)+1.58f,4.5f})))),
         hallEye_(campus.Pilot().Scene({7.75f,1.52f,4.7f})),
         hallToward_(campus.Pilot().Scene({3.f,1.53f,3.4f})),
         hallAcross_(campus.Pilot().Scene({17.f,2.18f,4.2f})),
         glassEye_(campus.Pilot().Scene({1.72f,campus.Pilot().Definition().Elevation(1)+1.52f,.83f})),
         glassLook_(campus.Pilot().Scene({1.72f,campus.Pilot().Definition().Elevation(1)+1.58f,-5.f})),
         glassEnd_(campus.Pilot().Scene({1.72f,campus.Pilot().Definition().Elevation(1)+1.52f,-1.f})),
         roofEye_(TargetCalibration::Spawn(campus.Pilot()).p){
        images_.reserve(size_t(TerritoryDuration)*120u);
    }
    void StartClock() noexcept {
        if(origin_)return;
        territoryStartedTick.store(GetTickCount64(),std::memory_order_release);
        origin_=QpcCounter();
    }
    double Elapsed() const noexcept {return origin_?QpcSeconds(QpcCounter()-origin_):0.;}
    void Prepare(Graphics& graphics,FrameInput& input,double elapsed){
        const auto phase=PhaseAt(elapsed);
        uint64_t rig=uint64_t(phase)*4u;
        Pose view=Reference(0.);
        switch(phase){
            case Reference0: view=Reference(elapsed-6.);break;
            case Reference29: view=Reference(elapsed-25.);break;
            case Grow29:
                if(!grow29_.requested)Request(grow29_,graphics.GetMg2ClusterStatus());
                view=Reference(7.);
                break;
            case Forest:{
                const float local=float(elapsed-32.);
                // Follow the dry side of the southwest river branch.
                const XMFLOAT3 eye{-57.f+local*(5.f/3.6f),2.10f,107.5f};
                const auto ahead=Add(eye,{9.f,-1.25f,.5f});
                view=SolarLookAt(eye,Mix(ahead,{-42.f,.23f,120.f},std::clamp(local/10.f,0.f,1.f)));
                break;
            }
            case Interior:
                if(elapsed<48.){
                    const float look=float(1.-std::abs(elapsed-46.)*.5);
                    view=SolarLookAt(hallEye_,Mix(hallToward_,hallAcross_,look));
                }else if(elapsed<52.){
                    ++rig;view=office_;
                }else{
                    rig+=2u;view=SolarLookAt(glassEye_,glassLook_);
                }
                break;
            case Island:
                if(elapsed<62.)view=SolarLookAt(roofEye_,{14.f,27.f,171.f});
                else{++rig;view=SolarLookAt({-110.f,-7.5f,149.f},{-74.f,-6.f,158.f});}
                break;
            case Edit:
                if(elapsed<76.){
                    view=Reference(0.);
                    if(!grow30_.requested&&campus_.PublishedContinentSteps()==29u)
                        Request(grow30_,graphics.GetMg2ClusterStatus());
                }else{
                    ++rig;view=SolarLookAt(glassEye_,glassLook_);
                    if(!breachRequested_){
                        auto& pilot=campus_.Pilot();
                        breachRequested_=QpcCounter();
                        beforeBreach_=pilot.GeometryRevision();
                        const auto hit=pilot.Trace(glassEye_,glassEnd_);
                        breachQueued_=hit&&hit->part.material==Buildings::Material::Glass;
                        if(breachQueued_)pilot.Breach(*hit,Mix(glassEye_,glassEnd_,hit->fraction));
                    }
                }
                break;
            default:break;
        }
        input.head=view;
        input.trackingRevision=rig+1u;
        input.insideSphere=0u;
        input.sceneUnitsPerMeter=OasisSceneUnitsPerMeter;
        input.farClipMeters=OasisWorldFarMeters;
        input.solarTime=8.;
    }
    void Record(Graphics& graphics,const XRFrame& frame,double elapsed,int64_t cpuStart){
        const auto phase=PhaseAt(elapsed);
        if(!origin_||phase==Count){Collect(graphics);return;}
        const auto now=QpcCounter();
        auto& segment=segments_[phase];
        ++segment.attempts;
        const auto mg2=graphics.GetMg2ClusterStatus();
        const auto steps=campus_.PublishedContinentSteps();
        Observe(grow29_,steps,mg2,frame,now);
        Observe(grow30_,steps,mg2,frame,now);
        const auto& pilot=campus_.Pilot();
        if(breachQueued_&&breachPublished_<0.&&pilot.GeometryRevision()!=beforeBreach_&&!pilot.Trace(glassEye_,glassEnd_))
            breachPublished_=1000.*QpcSeconds(now-breachRequested_);
        if(frame.submitted){
            ++segment.submitted;
            segment.mg2Failures+=unsigned(frame.forestRequestFailed);
            segment.steps=steps;segment.detailRoutes=campus_.ContinentDetailRoutes();
            segment.cameraInstances=frame.cameraInstances;
            segment.registry=mg2.registryInstances;segment.routed=mg2.routedInstances;segment.variants=mg2.variants;
            segment.nodes=mg2.gpuSelectVisitedNodes;segment.readbackBytes=mg2.gpuSelectReadbackBytes;segment.ageMs=mg2.generationAgeMs;
            const auto metrics=graphics.GetMetrics();
            segment.vram=std::max(segment.vram,metrics.vramUsed);
            segment.budget=metrics.vramBudget;
            if(phase==Reference0||phase==Reference29){
                const auto expected=phase==Reference0?0u:29u;
                if(steps!=expected||!frame.forestPages||!mg2.registryInstances||
                   mg2.routedInstances!=mg2.registryInstances||(expected&&grow29_.routed<0.))
                    ++segment.referenceMismatch;
            }
            const float interval=lastSubmit_&&lastPhase_==phase?float(1000.*QpcSeconds(now-lastSubmit_)):-1.f;
            images_.push_back({frame.input.frameId,phase,float(1000.*QpcSeconds(now-cpuStart)),interval});
            lastSubmit_=now;lastPhase_=phase;
            if(!profileLogged_){
                const auto state=graphics.GetRenderingState();
                char line[320];
                std::snprintf(line,sizeof(line),"territoire_profil version=1 preheat_s=6 duree_s=84 far_m=%.0f soleil=8 aa=native dlss=off yeux=%u rendu=%ux%u periode_ms=%.3f foret=160 variantes_botaniques=4 as=%s pilote=%u\r\n",
                    double(OasisWorldFarMeters),frame.viewCount,state.width,state.height,double(frame.displayPeriod)*1.e-6,
                    Mg2BackendName(mg2.configured),mg2.driver);
                DiagnosticLine(line);profileLogged_=true;
            }
        }
        Collect(graphics);
    }
    bool Finish(Graphics& graphics,bool elapsed){
        Collect(graphics);
        bool complete=elapsed;
        char line[1024];
        for(size_t i=0;i<Stages.size();++i){
            const auto phase=static_cast<Phase>(i);
            const auto& s=segments_[i];
            const auto cpu=Summarize(phase,&Image::cpu),gpu=Summarize(phase,&Image::gpu);
            const auto interval=Summarize(phase,&Image::interval);
            const bool valid=s.submitted&&gpu.count&&!s.referenceMismatch&&!s.mg2Failures;
            complete=complete&&valid;
            std::snprintf(line,sizeof(line),
                "territoire_sequence=%s valide=%u extents=%u detail_routes=%llu images=%u tentatives=%u gpu_echantillons=%zu "
                "intervalle_ms_p50=%.2f intervalle_ms_p95=%.2f cpu_ms_p50=%.2f cpu_ms_p95=%.2f gpu_ms_p50=%.2f gpu_ms_p95=%.2f "
                "depth_gpu_ms_p95=%.2f resident_gpu_ms_p95=%.2f cull_gpu_ms_p95=%.2f "
                "camera_instances=%u mg2_registre=%u mg2_routes=%u mg2_variantes=%u mg2_noeuds=%llu mg2_readback_octets=%llu "
                "mg2_age_ms=%.2f vram_pic_mio=%llu budget_mio=%llu echec_mg2=%u reference_incompatible=%u\r\n",
                Stages[i].name,unsigned(valid),s.steps,static_cast<unsigned long long>(s.detailRoutes),s.submitted,s.attempts,gpu.count,
                interval.p50,interval.p95,cpu.p50,cpu.p95,gpu.p50,gpu.p95,
                Summarize(phase,&Image::depth).p95,Summarize(phase,&Image::opaqueResident).p95,Summarize(phase,&Image::rasterCull).p95,
                s.cameraInstances,s.registry,s.routed,s.variants,static_cast<unsigned long long>(s.nodes),
                static_cast<unsigned long long>(s.readbackBytes),s.ageMs,static_cast<unsigned long long>(s.vram>>20),
                static_cast<unsigned long long>(s.budget>>20),s.mg2Failures,s.referenceMismatch);
            DiagnosticLine(line);
        }
        for(const auto& event:{grow29_,grow30_}){
            std::snprintf(line,sizeof(line),"territoire_croissance extent=%u demandee=%u acceptee=%u publication_ms=%.2f convergence_mg2_ms=%.2f\r\n",
                event.target,unsigned(event.requested!=0),unsigned(event.accepted),event.published,event.routed);
            DiagnosticLine(line);
            complete=complete&&event.published>=0.&&event.routed>=0.;
        }
        std::snprintf(line,sizeof(line),"territoire_breche cible=vitrage_O_niveau_1 trouvee=%u publication_ms=%.2f collision_retiree=%u\r\n",
            unsigned(breachQueued_),breachPublished_,unsigned(breachPublished_>=0.));
        DiagnosticLine(line);
        return complete&&breachPublished_>=0.;
    }
};
}
