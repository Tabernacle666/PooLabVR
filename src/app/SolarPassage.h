#pragma once
#include "SpectacleSpace.h"
#include "OasisMetric.h"

namespace OASIS {
struct VrTravel { XMFLOAT3 meters{}; float yaw{},pitch{}; };
inline constexpr double SolarExitCarrySeconds=3.;
inline constexpr float SolarInteriorFarMeters=OasisWorldFarMeters;

// One permanent world frame. Owner 0 identifies the campus interaction
// domain; it is no longer an enclosing volume or a scaling boundary.
class SolarPassage {
    XMFLOAT3 head_{};
    XMFLOAT4 trackingRotation_{0.f,0.f,0.f,1.f};
    XMMATRIX Rotation()const noexcept{return XMMatrixRotationQuaternion(XMLoadFloat4(&trackingRotation_));}
    void SetRotation(FXMMATRIX rotation)noexcept{
        XMStoreFloat4(&trackingRotation_,XMQuaternionNormalize(XMQuaternionRotationMatrix(rotation)));
    }
    static float Yaw(const Pose& pose)noexcept{
        const auto f=TransformDirection({0.f,0.f,1.f},Matrix(pose));
        return std::atan2(f.x,f.z);
    }
    void AlignLocalUp()noexcept{
        const auto rotation=Rotation();
        const auto forward=TransformDirection({0.f,0.f,1.f},rotation);
        const auto right=TransformDirection({1.f,0.f,0.f},rotation);
        const float yaw=std::hypot(forward.x,forward.z)>.001f?std::atan2(forward.x,forward.z):std::atan2(-right.z,right.x);
        SetRotation(XMMatrixRotationY(yaw));
    }
public:
    XMFLOAT3 LocalHead()const noexcept{return head_;}
    void SetLocalHead(XMFLOAT3 p)noexcept{head_=p;}
    bool Inside()const noexcept{return true;}
    uint32_t Sphere()const noexcept{return 0u;}
    float UnitsPerMeter()const noexcept{return 1.f;}
    void Start(const Pose& spawn,const Pose& raw,double)noexcept{
        head_=spawn.p;SetRotation(XMMatrixRotationY(Yaw(spawn)-Yaw(raw)));
    }
    void Scripted(const Pose& spawn,const Pose& raw,FrameInput&,bool)noexcept{
        head_=spawn.p;
        SetRotation(XMMatrixTranspose(XMMatrixRotationQuaternion(XMLoadFloat4(&raw.q)))*
                    XMMatrixRotationQuaternion(XMLoadFloat4(&spawn.q)));
    }
    void Resume(const Pose& previous,const Pose& raw,FrameInput&,bool recenter,bool level=true)noexcept{
        if(recenter)SetRotation(XMMatrixRotationY(-Yaw(raw)));
        else{
            const auto prior=XMMatrixRotationQuaternion(XMLoadFloat4(&previous.q));
            const auto next=XMMatrixRotationQuaternion(XMLoadFloat4(&raw.q));
            SetRotation(XMMatrixTranspose(next)*prior*Rotation());
        }
        if(recenter&&level)AlignLocalUp();
    }
    void LevelRig()noexcept{AlignLocalUp();}
    XMMATRIX RigRotation()const noexcept{return Rotation();}
    void RotateRig(XMFLOAT3 headFromPivot,FXMMATRIX delta)noexcept{
        const auto before=Rotation();const auto after=delta*before;
        head_=Add(head_,Sub(TransformDirection(headFromPivot,after),TransformDirection(headFromPivot,before)));
        SetRotation(after);
    }
    XMMATRIX TrackingToFrame(const Pose& raw)const noexcept{
        return XMMatrixTranslation(-raw.p.x,-raw.p.y,-raw.p.z)*
            Rotation()*XMMatrixTranslation(head_.x,head_.y,head_.z);
    }
    XMMATRIX TrackingToCosmos(const Pose& raw,double)const noexcept{return TrackingToFrame(raw);}
    void Advance(const VrTravel& travel,XMFLOAT3 rawMeters,double begin,FrameInput& input,bool keepRig=false)noexcept{
        if(input.solarTime<=begin)return;
        const auto physical=TransformDirection(rawMeters,Rotation());
        head_=Add(head_,Add(travel.meters,physical));
        if(travel.yaw!=0.f||(!keepRig&&travel.pitch!=0.f))
            SetRotation(XMMatrixRotationX(keepRig?0.f:travel.pitch)*Rotation()*XMMatrixRotationY(travel.yaw));
    }
    void Publish(const Pose& raw,FrameInput& input,XMFLOAT4X4& trackingToWorld)const noexcept{
        const auto tracking=TrackingToFrame(raw);
        XMStoreFloat4x4(&trackingToWorld,tracking);
        input.head=FromMatrix(Matrix(raw)*tracking);
        input.sceneUnitsPerMeter=1.f;input.insideSphere=0u;
        input.farClipMeters=OasisWorldFarMeters;
    }
};
}
