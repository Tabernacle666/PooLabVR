#pragma once

#include "Core.h"
#include "VrInputMapping.h"
#include <xinput.h>

namespace OASIS {

struct DesktopControls {
    FrameInput input;
    XMFLOAT4X4 vp{},inverseVP{};
    XMFLOAT3 eye{};
    bool toggleDeveloper{},toggleOutput{},quit{};
};

class DesktopInput {
    XMFLOAT3 position_{0.f,1.6f,0.f};
    float yaw_{},pitch_{};
    int64_t previousTime_{},firstMenu_{},nextControllerProbe_{};
    DWORD controller_{XUSER_MAX_COUNT};
    WORD previousButtons_{};
    bool previousLeftTrigger_{},previousRightTrigger_{},previousActive_{};

    static XMFLOAT2 Stick(SHORT x,SHORT y,SHORT deadzone) noexcept {
        const float fx=float(x),fy=float(y);
        const float radius=std::sqrt(fx*fx+fy*fy);
        if(radius<=float(deadzone))return {};
        const float magnitude=(std::min)((radius-float(deadzone))/(32767.f-float(deadzone)),1.f);
        return {fx*magnitude/radius,fy*magnitude/radius};
    }

    static float Trigger(BYTE value) noexcept {
        if(value<=XINPUT_GAMEPAD_TRIGGER_THRESHOLD)return 0.f;
        return float(value-XINPUT_GAMEPAD_TRIGGER_THRESHOLD)/float(255-XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
    }

public:
    void SetPose(const Pose& pose) noexcept {
        position_=pose.p;const auto forward=Sub(Transform({0,0,1},pose),pose.p);
        yaw_=std::atan2(forward.x,forward.z);
        pitch_=-std::atan2(forward.y,std::sqrt(forward.x*forward.x+forward.z*forward.z));
        pitch_=std::clamp(pitch_,-1.5f,1.5f);
    }
    DesktopControls Poll(uint64_t frameId,uint32_t width,uint32_t height,uint32_t actionCount,bool focused) {
        if(!width||!height||actionCount>MaxActions)Fail("Invalid desktop frame");
        const int64_t now=QpcCounter();
        const double elapsed=previousTime_?QpcSeconds(now-previousTime_):0.0;
        previousTime_=now;

        XINPUT_STATE gamepad{};bool connected=false;
        if(focused&&controller_<XUSER_MAX_COUNT){
            connected=XInputGetState(controller_,&gamepad)==ERROR_SUCCESS;
            if(!connected){controller_=XUSER_MAX_COUNT;nextControllerProbe_=0;}
        }
        if(focused&&!connected&&now>=nextControllerProbe_){
            for(DWORD index=0;index<XUSER_MAX_COUNT;++index)if(XInputGetState(index,&gamepad)==ERROR_SUCCESS){controller_=index;connected=true;break;}
            if(!connected)nextControllerProbe_=now+QpcFrequency();
        }
        const bool active=focused&&connected;
        const WORD buttons=active?gamepad.Gamepad.wButtons:WORD(0);
        const bool leftTrigger=active&&gamepad.Gamepad.bLeftTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        const bool rightTrigger=active&&gamepad.Gamepad.bRightTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        const bool resumed=active&&!previousActive_;
        if(resumed){previousButtons_=buttons;previousLeftTrigger_=leftTrigger;previousRightTrigger_=rightTrigger;}
        const WORD pressed=WORD(buttons&~previousButtons_);
        const float dt=active&&previousActive_&&elapsed>0.0&&elapsed<=0.25?float(elapsed):0.f;
        const XMFLOAT2 move=active?Stick(gamepad.Gamepad.sThumbLX,gamepad.Gamepad.sThumbLY,XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE):XMFLOAT2{};
        const XMFLOAT2 look=active?Stick(gamepad.Gamepad.sThumbRX,gamepad.Gamepad.sThumbRY,XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE):XMFLOAT2{};
        const float up=active?Trigger(gamepad.Gamepad.bRightTrigger):0.f;
        const float down=active?Trigger(gamepad.Gamepad.bLeftTrigger):0.f;

        yaw_+=look.x*1.35f*dt;
        pitch_=std::clamp(pitch_-look.y*1.35f*dt,-1.5f,1.5f);
        const float sine=std::sin(yaw_),cosine=std::cos(yaw_);
        position_.x+=(cosine*move.x+sine*move.y)*1.35f*dt;
        position_.z+=(-sine*move.x+cosine*move.y)*1.35f*dt;
        position_.y+=(up-down)*0.55f*dt;

        DesktopControls result{};
        result.toggleDeveloper=(pressed&XINPUT_GAMEPAD_A)!=0;
        result.toggleOutput=(pressed&XINPUT_GAMEPAD_B)!=0;
        if(!active)firstMenu_=0;
        else if(pressed&XINPUT_GAMEPAD_BACK){
            if(firstMenu_&&now-firstMenu_<=2*QpcFrequency()){
                result.quit=true;firstMenu_=0;
            }else firstMenu_=now;
        }

        auto& input=result.input;
        input.frameId=frameId;input.targetDisplayQpc=now;input.time=QpcSeconds(now);
        input.actionCount=actionCount;input.focused=focused;input.head.p=position_;
        const XMVECTOR orientation=XMQuaternionRotationRollPitchYaw(pitch_,yaw_,0.f);
        XMStoreFloat4(&input.head.q,orientation);
        const auto& actions=StandardVrActions();
        auto valid=[&](ActionId id){return bool(id)&&id.value<actionCount;};
        auto vector=[&](ActionId id,uint32_t hand,XMFLOAT2 value){
            if(!valid(id))return;auto& state=input.State(id);state.active[hand]=active;state.vector[hand]=value;
        };
        auto button=[&](ActionId id,uint32_t hand,bool held,bool newlyPressed){
            if(!valid(id))return;auto& state=input.State(id);state.active[hand]=active;state.scalar[hand]=held?1.f:0.f;state.down[hand]=held;state.pressed[hand]=newlyPressed;
        };
        vector(actions.move,0,move);vector(actions.turn,1,look);
        button(actions.heightDown,0,leftTrigger,leftTrigger&&!previousLeftTrigger_);
        button(actions.heightUp,1,rightTrigger,rightTrigger&&!previousRightTrigger_);
        button(actions.primary,0,(buttons&XINPUT_GAMEPAD_LEFT_SHOULDER)!=0,(pressed&XINPUT_GAMEPAD_LEFT_SHOULDER)!=0);
        button(actions.secondary,1,(buttons&XINPUT_GAMEPAD_RIGHT_SHOULDER)!=0,(pressed&XINPUT_GAMEPAD_RIGHT_SHOULDER)!=0);

        result.eye=position_;
        const XMVECTOR direction=XMVector3Rotate(XMVectorSet(0.f,0.f,1.f,0.f),orientation);
        const XMMATRIX view=XMMatrixLookToLH(XMLoadFloat3(&position_),direction,XMVectorSet(0.f,1.f,0.f,0.f));
        const XMMATRIX projection=XMMatrixPerspectiveFovLH(XM_PIDIV2,float(width)/float(height),FarZ,NearZ);
        const XMMATRIX matrix=view*projection;
        XMStoreFloat4x4(&result.vp,matrix);XMStoreFloat4x4(&result.inverseVP,XMMatrixInverse(nullptr,matrix));

        previousButtons_=buttons;previousLeftTrigger_=leftTrigger;previousRightTrigger_=rightTrigger;previousActive_=active;
        return result;
    }
};

}
