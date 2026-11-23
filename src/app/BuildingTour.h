#pragma once
#include "BuildingCampus.h"
#include "AppSession.h"
#include "Graphics.h"
#include "SolarSystem.h"

namespace OASIS {
class BuildingTour {
    struct Stop {
        const char* name;
        Buildings::Point eye,look;
        int level;
    };
    static constexpr Stop Stops[]{
        {"01-parvis",{7.75f,1.52f,-3.1f},{7.75f,3.9f,.2f},0},
        {"02-hall",{7.75f,1.52f,4.7f},{3.f,1.53f,3.4f},0},
        {"03-salle-cannee",{1.31f,1.52f,1.63f},{3.8f,1.1f,1.6f},0},
        {"04-etage-regenere",{7.75f,1.52f,4.7f},{17.f,2.18f,4.2f},0},
        {"05-cage",{2.25f,1.52f,6.50f},{2.0f,2.15f,9.8f},0},
        {"06-machines",{1.32f,1.52f,1.78f},{3.3f,1.4f,.9f},-1},
        {"07-transfert",{23.4f,1.52f,4.85f},{30.6f,1.85f,6.2f},15},
        {"08-vide-central",{15.2f,1.64f,5.55f},{16.4f,-9.f,9.5f},15},
        {"09-penthouse",{3.55f,1.52f,1.53f},{1.45f,1.5f,2.86f},29},
        {"10-vitrage-face",{1.72f,1.52f,.83f},{1.72f,1.58f,-5.f},1},
        {"11-vitrage-dos",{1.72f,1.52f,.83f},{1.72f,1.58f,4.5f},1},
        {"12-baie-percee",{1.72f,1.52f,.83f},{1.72f,1.52f,-5.f},1},
        {"13-campus-3d",{0,0,0},{0,0,0},0}
    };
    std::filesystem::path directory_;
    std::future<bool> writing_;
    double held_{},directTotal_{};
    uint64_t lastGpuFrame_{};
    unsigned samples_{};
    float directPeak_{};
    size_t index_{};
    bool requested_{},begun_{},failed_{},complete_{};
    uint64_t approvedRoom_{};
    uint64_t oldRevision_{};
    std::optional<Buildings::Room> approvedResult_;
public:
    explicit BuildingTour(const std::filesystem::path& dir):directory_(dir){}
    void PoseForFrame(XRFrame& frame,const BuildingSession& building)const{
        frame.input.trackingRevision=1000+index_;
        if(index_>=12){
            frame.input.head=SolarLookAt({.4f,2.55f,-3.3f},{0,1.15f,5.5f});
            frame.input.insideSphere=Invalid;
            frame.input.sceneUnitsPerMeter=1.f;
            frame.input.farClipMeters=FarZ;
            return;
        }
        const auto& stop=Stops[index_];
        auto a=stop.eye,b=stop.look;
        const auto base=building.Definition().Elevation(stop.level);
        a.y+=base;b.y+=base;
        frame.input.head=SolarLookAt(building.Scene(a),building.Scene(b));
        frame.input.insideSphere=0;
        frame.input.sceneUnitsPerMeter=SpectacleInteriorUnitsPerMeter;
        frame.input.farClipMeters=SpectacleInteriorFarMeters;
    }
    void Before(Graphics& graphics,BuildingSession& building,XMFLOAT3 head){
        using namespace Buildings;
        if(Finished())return;
        if(!begun_){
            std::string line="itineraire="+std::string(Stops[index_].name)+"\r\n";
            DiagnosticLine(line.c_str());
            begun_=true;
            if(index_==2){
                const auto target=building.Locate(head);
                if(!target.room||target.room>=UINT64_MAX-3){failed_=true;return;}
                approvedRoom_=target.room;
                oldRevision_=building.Plan().revision;
                building.Request({target,Scope::Room,Action::Approve,1,oldRevision_});
            }else if(index_==3){
                const auto target=building.Locate(head);
                oldRevision_=building.Plan().revision;
                building.Request({target,Scope::Floor,Action::Regenerate,1,oldRevision_});
            }else if(index_==11){
                const auto to=building.Scene({1.72f,building.Definition().Elevation(1)+1.52f,-1.f});
                const auto hit=building.Trace(head,to);
                if(!hit||hit->part.material!=Material::Glass){
                    DiagnosticLine("breche=vitrage_introuvable\r\n");
                    failed_=true;
                    return;
                }
                oldRevision_=building.Revision();
                building.Breach(*hit,Add(head,Scale(Sub(to,head),hit->fraction)));
            }
        }
        const bool glazing=index_==9||index_==10;
        const double delay=index_==0?6.:glazing?5.:2.;
        if(held_<delay||building.Busy()||requested_||graphics.CapturePending())return;
        if(index_==2||index_==3){
            const auto& rooms=building.Plan().At(0)->rooms;
            const auto pinned=std::find_if(rooms.begin(),rooms.end(),[&](const Room& r){return r.id==approvedRoom_;});
            if(building.Plan().revision!=oldRevision_+1||pinned==rooms.end()||!pinned->approved||
               (index_==3&&(!approvedResult_||*pinned!=*approvedResult_))){
                DiagnosticLine("generation=verification_echouee\r\n");
                failed_=true;
                return;
            }
            if(index_==2)approvedResult_=*pinned;
            if(index_==3)DiagnosticLine("generation=salle_cannee_identique_apres_regeneration_etage\r\n");
        }
        if(index_==11){
            const auto to=building.Scene({1.72f,building.Definition().Elevation(1)+1.52f,-1.f});
            if(building.Revision()!=oldRevision_+1||building.Trace(head,to)){
                DiagnosticLine("breche=echec_controle_collision\r\n");
                failed_=true;
                return;
            }
            DiagnosticLine("breche=vitrage_ouvert_et_collision_retiree\r\n");
        }
        if(glazing&&samples_){
            char line[232];
            std::snprintf(line,sizeof(line),"vitrage_vue=%s echantillons=%u gpu_direct_moy_ms=%.2f gpu_direct_pic_ms=%.2f fps_soumis=%.2f\r\n",
                Stops[index_].name,samples_,directTotal_/samples_,double(directPeak_),double(graphics.GetMetrics().fps));
            DiagnosticLine(line);
        }
        const auto file=index_+1==std::size(Stops)?"OASIS.png":std::string(Stops[index_].name)+".png";
        graphics.RequestCapture((directory_/file).wstring());
        requested_=true;
    }
    void After(Graphics& graphics,const XRFrame& frame){
        if(frame.submitted)held_+=double(frame.displayPeriod)*1.e-9;
        if((index_==9||index_==10)&&held_>=1.&&frame.submitted&&!requested_){
            const auto gpu=graphics.GetDlssDiagnostic();
            if(gpu.gpuFrame!=lastGpuFrame_&&gpu.directMs>=0.f){
                lastGpuFrame_=gpu.gpuFrame;
                directTotal_+=gpu.directMs;
                directPeak_=std::max(directPeak_,gpu.directMs);
                ++samples_;
            }
        }
        if(requested_&&!writing_.valid()&&graphics.CaptureDone())writing_=graphics.FinishCaptureAsync();
        if(!writing_.valid()||writing_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        try{failed_=!writing_.get();}catch(...){failed_=true;}
        if(failed_)return;
        if(index_+1==std::size(Stops)){complete_=true;return;}
        ++index_;requested_=begun_=false;held_=directTotal_=0.;samples_=0;directPeak_=0.f;
    }
    bool Finished()const{return failed_||complete_;}
    bool Complete()const{return complete_;}
};
}
