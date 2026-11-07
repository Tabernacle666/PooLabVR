#include "HomePhysics.h"
#include <algorithm>
#include <cmath>
namespace lab {
void HomePhysics::PublishRendering(){
    if(!gpu_)return;
    if(!gpu_->SetMultisampling(gpu_->serviceContext,&multisampling_))Fail("Home gpu_v1: multisampling rejected");
    if(!gpu_->SetAntialiasing(gpu_->serviceContext,&antialiasing_)||!gpu_->SetFogTransport(gpu_->serviceContext,&transport_))Fail("Home gpu_v1: rendering settings rejected");
    for(const auto& s:surfaces_)if(!gpu_->SetSurfaceSettings(gpu_->serviceContext,&s))Fail("Home gpu_v1: surface settings rejected");
}
void HomePhysics::SetAntialiasingMode(uint32_t mode){if(mode>2)return;antialiasing_.mode=mode;PublishRendering();status_="FXAA MODE APPLIED";}
void HomePhysics::SetMultisampling(uint32_t samples){LabMultisamplingV1 s{sizeof(s),samples};if(!gpu_->SetMultisampling(gpu_->serviceContext,&s)){status_="MSAA MODE UNSUPPORTED";return;}multisampling_=s;status_=samples==4?"MSAA 4X APPLIED":"MSAA OFF APPLIED";}
void HomePhysics::AdjustPopulationSpace(uint32_t i,float sign){
    auto next=populationSpace_;if(i==0)next.enabled^=1;
    else if(i<=3){float* c[]{&next.center.x,&next.center.y,&next.center.z};*c[i-1]=std::clamp(*c[i-1]+sign*.10f,-16.f,16.f);}
    else if(i<=6){float* c[]{&next.halfExtent.x,&next.halfExtent.y,&next.halfExtent.z};*c[i-4]=std::clamp(*c[i-4]+sign*.10f,.1f,16.f);}
    else if(i==7)next.returnStrength=std::clamp(next.returnStrength+sign*.25f,0.f,16.f);
    if(!physics_->SetPopulationSpace(physics_->serviceContext,scene_,&next)){status_="FLIGHT VOLUME REFUSED";return;}populationSpace_=next;status_="FLIGHT VOLUME UPDATED / EXISTING FLIES PRESERVED";
}
void HomePhysics::AdjustAntialiasing(float sign){antialiasing_.mode=uint32_t((int(antialiasing_.mode)+(sign>0?1:2))%3);PublishRendering();status_="SPATIAL FXAA ACTIVE";}
void HomePhysics::AdjustTransport(uint32_t i,float sign){
    if(i==0)transport_.enabled^=1;
    else if(i==1)fanWind_=std::clamp(fanWind_+sign*.1f,0.f,4.f);
    else if(i==2)wind_.radius=std::clamp(wind_.radius+sign*.05f,.05f,4.f);
    else if(i==3)wind_.length=std::clamp(wind_.length+sign*.25f,.1f,16.f);
    else if(i==4)transport_.contrast=std::clamp(transport_.contrast+sign*.05f,0.f,1.f);
    else if(i==5)transport_.frequency=std::clamp(transport_.frequency+sign*.05f,.1f,2.f);
    else if(i==6)fog_.turbulence=std::clamp(fog_.turbulence+sign*.05f,0.f,1.f);
    else if(i==7)wind_.dissipation=std::clamp(wind_.dissipation+sign*.02f,0.f,8.f);
    PublishRendering();auto f=fog_;f.enabled=fog_.enabled&&visible_;gpu_->SetFogSettings(gpu_->serviceContext,&f);gpu_->SetFogWind(gpu_->serviceContext,&wind_);status_="3D TRANSPORT SETTINGS ACTIVE";
}
void HomePhysics::AdjustSurface(uint32_t surface,uint32_t i,float sign){
    if(surface>=surfaces_.size())return;auto& s=surfaces_[surface];
    if(i<3){float* c[]{&s.color.x,&s.color.y,&s.color.z};*c[i]=std::clamp(*c[i]+sign*.01f,0.f,1.f);}
    else if(i==3)s.roughness=std::clamp(s.roughness+sign*.025f,.04f,1.f);
    else if(i==4)s.normalAmplitude=std::clamp(s.normalAmplitude+sign*.005f,0.f,.3f);
    else if(i==5)s.normalFrequency=std::clamp(s.normalFrequency+sign*2,.1f,300.f);
    else if(i==6)s.junctionWidth=std::clamp(s.junctionWidth+sign*.002f,0.f,.15f);
    else if(i==7)s.junctionContrast=std::clamp(s.junctionContrast+sign*.025f,0.f,1.f);
    if(!gpu_->SetSurfaceSettings(gpu_->serviceContext,&s))Fail("Home gpu_v1: live material rejected");status_="SURFACE SETTINGS ACTIVE";
}
void HomePhysics::RotateFan(float sign){
    if(!frozen_){status_="FREEZE BEFORE ROTATING";return;}if(!RefreshEditor())return;
    SelectNode(3);auto& selected=selections_[selection_];LabVec3V1 pivot{};for(const auto& e:entities_)if(e.body==fanBase_)pivot=e.pose.position;
    float a=sign*XM_PI/36,c=std::cos(a),s=std::sin(a),h=a*.5f,sh=std::sin(h),ch=std::cos(h);
    std::vector<LabPhysicsEditBodyV1> bodies;for(const auto& e:entities_)if(std::find(selected.bodies.begin(),selected.bodies.end(),e.body)!=selected.bodies.end()){
        auto p=e.pose;float x=p.position.x-pivot.x,z=p.position.z-pivot.z;p.position.x=pivot.x+c*x+s*z;p.position.z=pivot.z-s*x+c*z;
        auto q=p.orientation;p.orientation={ch*q.x+sh*q.z,ch*q.y+sh*q.w,ch*q.z-sh*q.x,ch*q.w-sh*q.y};bodies.push_back({e.body,p});
    }
    LabPhysicsEditBatchV1 batch{sizeof(batch),uint32_t(bodies.size()),bodies.data(),0,0,nullptr};
    if(!physics_->ApplyEdit(physics_->serviceContext,scene_,&batch)){status_="ROTATION BLOCKED: PHYSICAL LAYOUT";return;}
    RefreshEditor();CacheSelection(selected);PublishAcoustics();status_="FAN AXIS ROTATED";
}
}
