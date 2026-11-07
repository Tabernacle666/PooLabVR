#pragma once
#include "InputSystem.h"
namespace lab {
struct LabInputActions {
    LabActionSetId navigationSet{},interactionSet{},trackingSet{},systemSet{};
    LabActionId move{},turn{},recenter{},heightUp{},heightDown{};
    LabActionId grab{},primary{},secondary{},touch{},physical{},proximity{},mode{},next{},feedback{};
    LabActionId handPose{},aimPose{},quit{},menu{};
};
const LabInputActions& StandardVrActions() noexcept;
void DeclareStandardVrInputActions(InputSystem& input);
struct LocomotionSettings {
    enum class Reference {Head,LeftHand,RightHand,World};
    Reference reference{Reference::Head};
    float speed{1.35f},yawSpeed{1.35f};
    float heightSpeed{0.55f},heightMin{-1.25f},heightMax{1.25f};
};
class StandardVrInput {
    LocomotionSettings locomotion_;
    int64_t locomotionTime_{};uint64_t trackingRevision_{};
    float heightOffset_{};
    bool locomotionFocused_{},anchored_{},anchorPending_{true},locomotionEnabled_{},heightAdjustEnabled_{};
    void Anchor(const Pose&,bool,XMFLOAT4X4&);
    void Locomotion(FrameInput&,const Pose&,XMFLOAT4X4&);
public:
    void LoadConfiguration(const std::wstring&);
    void SetNavigationOptions(bool locomotion,bool heightAdjust)noexcept{locomotionEnabled_=locomotion;heightAdjustEnabled_=heightAdjust;if(!locomotion&&!heightAdjust){locomotionTime_=0;locomotionFocused_=false;}}
    void SetLocomotionEnabled(bool enabled)noexcept{SetNavigationOptions(enabled,heightAdjustEnabled_);}
    bool LocomotionEnabled()const noexcept{return locomotionEnabled_;}
    bool HeightAdjustEnabled()const noexcept{return heightAdjustEnabled_;}
    TrackingMapping Mapping();
    bool Frame(FrameInput&,const Pose*,bool,XMFLOAT4X4&);
    void Event(TrackingEvent);
};
}
