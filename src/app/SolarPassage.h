#pragma once
#include "SpectacleSpace.h"

namespace OASIS {

struct VrTravel {
    XMFLOAT3 meters{};
    float yaw{},pitch{};
};

inline constexpr double SolarExitCarrySeconds=3.;
inline constexpr float SolarInteriorFarMeters=400.f;
static_assert(SolarInteriorFarMeters*SpectacleInteriorUnitsPerMeter>
    LunarOrbitRadius+LunarBoundsRadius+SpectacleRadius);

class SolarPassage {
    // The tracking rig lives in the currently occupied coordinate frame.
    XMFLOAT3 head_{};
    XMFLOAT4 trackingRotation_{0.f,0.f,0.f,1.f};
    uint32_t owner_{Invalid};

    struct ExitCarry {
        static constexpr double FadeSeconds=.25;
        XMFLOAT3 velocity{};
        double until{};

        double TravelTimeLeft(double time) const noexcept {
            const double remaining=std::clamp(until-time,0.,SolarExitCarrySeconds);
            const double fade=std::min(remaining,FadeSeconds);
            return remaining-fade+.5*fade*fade/FadeSeconds;
        }
        XMFLOAT3 Displacement(double begin,double end) const noexcept {
            return Scale(velocity,float(TravelTimeLeft(begin)-TravelTimeLeft(end)));
        }
    } exitCarry_;

    XMMATRIX Rotation() const noexcept {
        return XMMatrixRotationQuaternion(XMLoadFloat4(&trackingRotation_));
    }
    void SetRotation(FXMMATRIX rotation) noexcept {
        XMStoreFloat4(&trackingRotation_,XMQuaternionNormalize(XMQuaternionRotationMatrix(rotation)));
    }
    static float Yaw(const Pose& pose) noexcept {
        const auto f=TransformDirection({0.f,0.f,1.f},Matrix(pose));
        return std::atan2(f.x,f.z);
    }
    void Turn(float yaw,float pitch) noexcept {
        if(yaw!=0.f||pitch!=0.f)SetRotation(XMMatrixRotationX(pitch)*Rotation()*XMMatrixRotationY(yaw));
    }
    void AlignLocalUp() noexcept {
        const auto rotation=Rotation();
        const auto forward=TransformDirection({0.f,0.f,1.f},rotation);
        const auto right=TransformDirection({1.f,0.f,0.f},rotation);
        const float yaw=std::hypot(forward.x,forward.z)>.001f?std::atan2(forward.x,forward.z):
            std::atan2(-right.z,right.x);
        // Level the tracking rig. The headset and hands keep their raw poses.
        SetRotation(XMMatrixRotationY(yaw));
    }
    uint32_t Locate(XMFLOAT3 position,double time) const noexcept {
        const auto nearest=NearestSpectacle(position,SolarPlanetCount,time);
        const auto offset=Sub(position,SolarCenter(nearest,time));
        constexpr float radius=SpectacleRadius-float(SpectacleSeamMeters);
        return Dot(offset,offset)<=radius*radius?nearest:Invalid;
    }
    void ChangeFrame(FXMMATRIX transform,uint32_t owner) noexcept {
        head_=TransformPoint(head_,transform);
        SetRotation(Rotation()*transform);
        owner_=owner;
        if(Inside())AlignLocalUp();
    }
    static void Record(FrameInput& input,uint32_t owner,bool entering,double time,
                       XMFLOAT3 point,XMFLOAT3 center) noexcept {
        const auto surface=Add(center,Scale(Unit(Sub(point,center)),SpectacleRadius));
        input.crossings[input.crossingCount++]={surface,entering,owner,time};
    }

public:
    bool Inside() const noexcept {return owner_!=Invalid;}
    uint32_t Sphere() const noexcept {return owner_;}
    float UnitsPerMeter() const noexcept {return Inside()?SpectacleInteriorUnitsPerMeter:1.f;}

    void Start(const Pose& spawn,const Pose& raw,double time) noexcept {
        owner_=Invalid;
        head_=spawn.p;
        SetRotation(XMMatrixRotationY(Yaw(spawn)-Yaw(raw)));
        exitCarry_={};
        const auto owner=Locate(head_,time);
        if(owner!=Invalid){
            const auto body=SolarBodyToCosmos(owner,SolarBody(owner,time));
            ChangeFrame(XMMatrixInverse(nullptr,body),owner);
        }
    }

    void Resume(const Pose& previous,const Pose& raw,FrameInput& input,bool recenter) noexcept {
        if(recenter)SetRotation(XMMatrixRotationY(-Yaw(raw)));
        else{
            const auto prior=XMMatrixRotationQuaternion(XMLoadFloat4(&previous.q));
            const auto next=XMMatrixRotationQuaternion(XMLoadFloat4(&raw.q));
            SetRotation(XMMatrixTranspose(next)*prior*Rotation());
        }
        // Resume from the current overlap instead of replaying a missed interval.
        if(Inside())return;
        const auto owner=Locate(head_,input.solarTime);
        if(owner==Invalid)return;
        const auto body=SolarBody(owner,input.solarTime);
        Record(input,owner,true,input.solarTime,head_,body.p);
        ChangeFrame(XMMatrixInverse(nullptr,SolarBodyToCosmos(owner,body)),owner);
        exitCarry_={};
    }

    XMMATRIX TrackingToFrame(const Pose& raw) const noexcept {
        const float units=UnitsPerMeter();
        return XMMatrixTranslation(-raw.p.x,-raw.p.y,-raw.p.z)*XMMatrixScaling(units,units,units)*
            Rotation()*XMMatrixTranslation(head_.x,head_.y,head_.z);
    }

    XMMATRIX TrackingToCosmos(const Pose& raw,double time) const noexcept {
        const auto tracking=TrackingToFrame(raw);
        return Inside()?tracking*SolarBodyToCosmos(owner_,SolarBody(owner_,time)):tracking;
    }

    void Advance(const VrTravel& travel,XMFLOAT3 rawMeters,double begin,FrameInput& input) noexcept {
        const double end=input.solarTime;
        const double seconds=end-begin;
        if(seconds<=0.)return;
        const float rate=float(1./seconds);
        const auto rawVelocity=Scale(rawMeters,rate);
        auto commandVelocity=Scale(travel.meters,rate);
        const float yawRate=travel.yaw*rate;
        const float pitchRate=travel.pitch*rate;
        // Subdivide longer frames so orbital curvature stays small at contact.
        constexpr double MaxStep=.02;
        const unsigned steps=unsigned(std::ceil(seconds/MaxStep));
        for(unsigned step=0;step<steps;++step){
            const double limit=end-seconds*(1.-double(step+1u)/steps);
            while(begin<limit){
                const bool leaving=Inside();
                const float units=UnitsPerMeter();
                const double interval=limit-begin;
                const float timeStep=float(interval);
                const auto physical=TransformDirection(rawVelocity,Rotation());
                const auto delta=Add(Scale(Add(commandVelocity,physical),units*timeStep),
                    exitCarry_.Displacement(begin,limit));
                double hit=2.;
                uint32_t owner=Invalid;
                if(leaving){
                    const double radius=SpectacleRadius+SpectacleSeamMeters*units;
                    hit=MembraneHitFraction(Sub(head_,SpectacleCenters[owner_]),delta,radius,true);
                    if(hit<=1.)owner=owner_;
                }else{
                    const double radius=SpectacleRadius-SpectacleSeamMeters;
                    for(uint32_t candidate=0;candidate<SolarPlanetCount;++candidate){
                        const auto center=SolarCenter(candidate,begin);
                        const auto relative=Sub(delta,Sub(SolarCenter(candidate,limit),center));
                        const double fraction=MembraneHitFraction(Sub(head_,center),relative,radius,false);
                        if(fraction<hit){hit=fraction;owner=candidate;}
                    }
                }
                if(owner==Invalid){
                    head_=Add(head_,delta);
                    Turn(yawRate*timeStep,leaving?0.f:pitchRate*timeStep);
                    begin=limit;
                    continue;
                }
                const double contact=std::lerp(begin,limit,hit);
                head_=Add(head_,Scale(delta,float(hit)));
                Turn(yawRate*float(contact-begin),leaving?0.f:pitchRate*float(contact-begin));
                const auto body=SolarBody(owner,contact);
                const auto bodyToCosmos=SolarBodyToCosmos(owner,body);
                const auto change=leaving?bodyToCosmos:XMMatrixInverse(nullptr,bodyToCosmos);
                const auto world=leaving?TransformPoint(head_,bodyToCosmos):head_;
                Record(input,owner,!leaving,contact,world,body.p);
                ChangeFrame(change,leaving?Invalid:owner);
                if(leaving){
                    exitCarry_={SolarTransportVelocity(owner,body,contact,world),contact+SolarExitCarrySeconds};
                    commandVelocity=TransformDirection(commandVelocity,change);
                }else{
                    exitCarry_={};
                    commandVelocity={};
                }
                begin=contact;
                if(input.crossingCount==input.crossings.size())return;
            }
        }
    }

    void Publish(const Pose& raw,FrameInput& input,XMFLOAT4X4& trackingToWorld) const noexcept {
        const auto tracking=TrackingToCosmos(raw,input.solarTime);
        XMStoreFloat4x4(&trackingToWorld,tracking);
        input.head=FromMatrix(Matrix(raw)*tracking);
        input.sceneUnitsPerMeter=UnitsPerMeter();
        input.insideSphere=Sphere();
        input.farClipMeters=Inside()?SolarInteriorFarMeters:FarZ;
    }
};
}
