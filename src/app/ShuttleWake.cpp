#include "ShuttleWake.h"
#include "shuttle/ShuttleLayout.h"
#include <DirectXCollision.h>

namespace OASIS {
namespace {
XMFLOAT3 CrossProduct(XMFLOAT3 a,XMFLOAT3 b) {
    XMFLOAT3 result;
    XMStoreFloat3(&result,XMVector3Cross(XMLoadFloat3(&a),XMLoadFloat3(&b)));
    return result;
}

XMFLOAT3 ModelVector(Vaisseaux::Vec3 v) {
    return {float(v.x),float(v.z),float(v.y)};
}

constexpr XMFLOAT3 HullLow{-1.85f,-.50f,-1.05f},HullHigh{1.65f,1.23f,1.05f};
float HullExit(XMFLOAT3 p,XMFLOAT3 d) {
    const auto axis=[](float p,float d,float low,float high) {
        if(std::abs(d)<.0001f)return 100.f;
        return ((d>0.f?high:low)-p)/d;
    };
    return std::min({axis(p.x,d.x,HullLow.x,HullHigh.x),axis(p.y,d.y,HullLow.y,HullHigh.y),
                     axis(p.z,d.z,HullLow.z,HullHigh.z)});
}
}

float ShuttleWake::Random() noexcept {
    seed_^=seed_<<13u;
    seed_^=seed_>>17u;
    seed_^=seed_<<5u;
    return float(seed_>>8u)*(1.f/16777216.f);
}

void ShuttleWake::Clear() noexcept {
    particles_={};emitterPhase_={};impact_={};
    next_=0;dustPhase_=0.f;time_=-1.;
}

void ShuttleWake::Emit(Particle particle) noexcept {
    particles_[next_]=particle;
    next_=(next_+1u)%particles_.size();
}

void ShuttleWake::EmitDust(XMFLOAT3 direction,float speed,std::span<const Triangle> canopy) {
    const auto crossAxis=std::abs(direction.y)<.93f?XMFLOAT3{0,1,0}:XMFLOAT3{1,0,0};
    const auto right=Unit(CrossProduct(direction,crossAxis));
    const auto up=CrossProduct(right,direction);
    const auto start=Add(ShuttleLayout::Eye,
        Add(Scale(right,(Random()-.5f)*.95f),Scale(up,(Random()-.5f)*.65f)));
    float best=std::max(.05f,HullExit(start,direction));
    bool glass=false;
    for(const auto& t:canopy){
        float distance{};
        if(TriangleTests::Intersects(XMLoadFloat3(&start),XMLoadFloat3(&direction),
            XMLoadFloat3(&t[0]),XMLoadFloat3(&t[1]),XMLoadFloat3(&t[2]),distance)&&
            distance>.02f&&distance<=best){
            best=distance;
            glass=true;
        }
    }
    const float arrival=std::clamp(4.f/speed,.075f,.34f);
    const float stretch=std::clamp(speed*arrival,2.f,8.f);
    const auto hit=Add(start,Scale(direction,best+.035f));
    const auto incoming=Scale(direction,-stretch/arrival);
    const float strength=std::clamp(std::sqrt(speed/55.f),.24f,1.f)*(.6f+.4f*Random());
    Emit({Add(hit,Scale(incoming,-arrival)),incoming,direction,0.f,arrival+.14f,arrival,strength,Kind::Dust,glass});
}

void ShuttleWake::Step(const FrameInput& input,FXMMATRIX root,XMFLOAT3 velocity,
                       std::span<const Vaisseaux::DeviceLoad,EmitterCount> loads,float density,
                       std::span<const Triangle> canopy) {
    impact_={};
    const double elapsed=input.solarTime-time_;
    if(time_<0.||elapsed<0.||elapsed>.25||body_!=input.insideSphere){
        Clear();
        body_=input.insideSphere;
        time_=input.solarTime;
        return;
    }
    time_=input.solarTime;
    if(elapsed==0.)return;
    const float dt=float(elapsed);
    for(auto& p:particles_){
        if(p.age>=p.life)continue;
        const float before=p.age;
        p.age+=dt;
        if(p.kind==Kind::Dust&&before<p.arrival&&p.age>=p.arrival&&p.strength>impact_.strength){
            impact_={TransformPoint(Add(p.origin,Scale(p.velocity,p.arrival)),root),p.strength,p.glass};
        }
    }
    if(!input.focused)return;

    const auto craftVelocity=TransformDirection(velocity,root);
    for(size_t i=0;i<loads.size();++i){
        const auto& load=loads[i];
        const bool fan=Vaisseaux::Lunaire::Mounts[i].organ==Vaisseaux::Organ::Fan;
        const auto force=ModelVector(load.forceN);
        const float maxForce=fan?float(input.insideSphere==Invalid?
            Vaisseaux::Lunaire::Propulsion.fan.vacuumGasMaxN:
            Vaisseaux::Lunaire::Propulsion.fan.curve.back().forceN):
            float(Vaisseaux::Lunaire::Propulsion.reactor.curve.back().forceN);
        const float power=std::clamp(Length(force)/maxForce,0.f,1.f);
        if(power<.004f){
            emitterPhase_[i]=0.f;
            continue;
        }
        auto& phase=emitterPhase_[i];
        if(phase==0.f)phase=1.f;
        phase+=dt*(fan?15.f:5.4f)*std::sqrt(power);
        if(phase<1.f)continue;
        phase=std::fmod(phase,1.f);
        const auto flow=Scale(Unit(force),-1.f);
        const auto spread=XMFLOAT3{Random()-.5f,Random()-.5f,Random()-.5f};
        const float port=fan?.16f:-ShuttleLayout::JetExitX;
        const auto start=Add(ModelVector(load.positionMetres),Add(Scale(flow,port),Scale(spread,fan?.30f:.12f)));
        const auto exhaust=Add(Scale(flow,(fan?8.f:3.f)+(fan?9.f:5.f)*power),Scale(spread,fan?2.6f:1.4f));
        Emit({TransformPoint(start,root),Add(craftVelocity,TransformDirection(exhaust,root)),
              Unit(TransformDirection(flow,root)),0.f,
              fan?.32f+.16f*Random():2.35f+.55f*Random(),0.f,
              .42f+.58f*std::sqrt(power),fan?Kind::Wash:Kind::Smoke,false});
    }

    const float speed=Length(velocity);
    if(input.insideSphere!=Invalid||speed<1.f||density<=0.f){
        dustPhase_=0.f;
        return;
    }
    dustPhase_+=dt*density*std::min(24.f,.19f*speed);
    if(dustPhase_>=1.f){
        dustPhase_=std::fmod(dustPhase_,1.f);
        EmitDust(Scale(velocity,1.f/speed),speed,canopy);
    }
}
}
