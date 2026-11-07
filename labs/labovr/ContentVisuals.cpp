#include "ClientVisuals.h"
#include "ContentIds.h"
#include "LabInputMapping.h"
namespace lab {
namespace {
void AddObject(RenderScene& r,const Pose& pose,XMFLOAT3 scale,Mesh mesh,Material material,uint32_t id){r.objects.push_back({pose,pose,scale,uint32_t(mesh),uint32_t(material),id,DynamicObject});++r.count;}

// The controller supplies a grip pose and analog controls, not tracked finger joints.
// Build each phalanx from its parent joint in grip space, then apply the grip pose.
void Finger(RenderScene& r,const Pose& hand,XMFLOAT3 joint,float yaw,float curl,
            const float* lengths,uint32_t segments,float width,float thickness,
            Material material,uint32_t id){
    float angle=0;
    for(uint32_t segment=0;segment<segments;++segment){
        angle+=curl*(segment==0?1.05f:(segment==1?.85f:.55f));
        const XMMATRIX rotation=XMMatrixRotationX(angle)*XMMatrixRotationY(yaw);
        XMFLOAT3 step;
        XMStoreFloat3(&step,XMVector3TransformNormal(XMVectorSet(0,0,lengths[segment],0),rotation));
        const XMFLOAT3 center=Add(joint,Scale(step,.5f));
        const XMMATRIX local=rotation*XMMatrixTranslation(center.x,center.y,center.z);
        const Pose pose=FromMatrix(local*Matrix(hand));
        const float taper=1.f-.12f*float(segment);
        AddObject(r,pose,{width*taper,thickness*taper,lengths[segment]},Mesh::Box,material,id+segment);
        joint=Add(joint,step);
    }
}
}
void AppendClientHands(RenderScene& r,const FrameInput& input,const std::array<PoseActionState,2>* hands){
    for(uint32_t i=0;i<2;i++){
        const auto& hand=hands?(*hands)[i]:input.PoseState(StandardVrActions().handPose,i);
        if(!hand.tracked)continue;
        const auto material=i?Material::HandRight:Material::HandLeft;
        const float side=i?-1.f:1.f;
        const float grab=std::clamp(input.Analog(StandardVrActions().grab,i),0.f,1.f);
        const float trigger=std::clamp(input.Analog(StandardVrActions().primary,i),0.f,1.f);
        Pose palm=hand.pose;palm.p=Transform({0,0,.012f},hand.pose);
        AddObject(r,palm,{.071f,.035f,.10f},Mesh::Box,material,0xa0000000u+i);
        const float lengths[4][3]={{.025f,.018f,.015f},{.029f,.020f,.016f},{.027f,.019f,.015f},{.022f,.015f,.013f}};
        for(uint32_t finger=0;finger<4;finger++){
            const float x=side*(1.5f-float(finger))*.016f;
            const float curl=finger==0?trigger:grab;
            Finger(r,hand.pose,{x,0,.062f},0,curl,lengths[finger],3,.013f,.016f,material,0xa0000200u+i*32u+finger*3u);
        }
        // The thumbs originate on opposite sides and angle away from the palm.
        const float thumbLengths[2]={.025f,.021f};
        Finger(r,hand.pose,{side*.035f,-.003f,.020f},side*.85f,grab*.55f,
               thumbLengths,2,.017f,.019f,material,0xa0000240u+i*2u);
    }
}
}
