#pragma once
#include "InputSystem.h"
#include "SolarPassage.h"
#include <filesystem>
namespace OASIS {
struct VrInputActions {
    ActionSetId navigationSet{},interactionSet{},trackingSet{},shuttleSet{};
    ActionId move{},turn{},recenter{},heightUp{},heightDown{},gravity{};
    ActionId grab{},primary{},secondary{},touch{},physical{},proximity{},equipRightTool{},shuttle{},capture{},next{},quit{},feedback{};
    ActionId handPose{},aimPose{},shuttleMixReset{},shuttleOptions{},shuttleOptionsPage{},shuttleSystems{},shuttleCalibrate{},shuttleBrake{};
};
const VrInputActions& StandardVrActions() noexcept;
void DeclareStandardVrInputActions(InputSystem& input);
struct LocomotionSettings {
    enum class Reference {Head,LeftHand,RightHand,World};
    Reference reference{Reference::Head};
    float speed{1.35f},yawSpeed{1.35f};
    float heightSpeed{0.55f},heightMin{-1.25f},heightMax{1.25f};
};
class OrbitTool;
class BuildingCampus;
class ShuttlePilot;
class StandardVrInput {
    LocomotionSettings locomotion_;
    OrbitTool* orbit_{};
    BuildingCampus* building_{};
    bool buildingGravity_{true};
    int liftStick_{};
    int64_t buildingTime_{};
    ShuttlePilot* shuttle_{};
    int64_t locomotionTime_{};uint64_t trackingRevision_{};
    float heightOffset_{};
    int stance_{3};
    int64_t nextStanceStep_{};
    Pose initialPose_{};
    bool initialPosePending_{};
    bool locomotionFocused_{},anchored_{},anchorPending_{true},locomotionEnabled_{},heightAdjustEnabled_{};
    bool spectacleEnabled_{},trackingContinuous_{};
    bool hasLastHead_{};
    XMFLOAT3 lastHead_{};
    bool solarSystem_{},solarRecenterPending_{},solarEntryBrake_{};
    int64_t solarOrigin_{};
    double lastSolarTime_{-1.};
    Pose lastSolarRaw_{};
    SolarPassage solarPassage_;
    SpectaclePassage spectacle_;
    void Anchor(const Pose&,bool,XMFLOAT4X4&);
    VrTravel Locomotion(const FrameInput&,const Pose&,FXMMATRIX,XMFLOAT2 turn);
    void SolarFrame(FrameInput&,const Pose*,XMFLOAT4X4&,XMFLOAT2 turn,bool recenter,bool boardShuttle,int liftStep);
public:
    bool LoadConfiguration(const std::wstring&,const std::filesystem::path& user={});
    void SetNavigationOptions(bool locomotion,bool heightAdjust)noexcept{locomotionEnabled_=locomotion;heightAdjustEnabled_=heightAdjust;if(!locomotion&&!heightAdjust){locomotionTime_=0;locomotionFocused_=false;}}
    void SetLocomotionEnabled(bool enabled)noexcept{SetNavigationOptions(enabled,heightAdjustEnabled_);}
    void SetInitialPose(const Pose& pose)noexcept{
        initialPose_=pose;initialPosePending_=anchorPending_=true;
    }
    void StartSolarSystem(int64_t origin=0)noexcept{
        solarSystem_=true;solarOrigin_=origin;solarRecenterPending_=solarEntryBrake_=false;
        anchored_=false;trackingContinuous_=false;
    }
    void SetSpectaclePassage(bool enabled,uint32_t count=1u)noexcept{
        spectacleEnabled_=enabled;spectacle_.SetCount(count);
    }
    void SetBuilding(BuildingCampus* building)noexcept{building_=building;}
    void SetOrbitTool(OrbitTool* orbit)noexcept{orbit_=orbit;}
    void SetShuttlePilot(ShuttlePilot* pilot) noexcept {shuttle_=pilot;}
    bool LocomotionEnabled()const noexcept{return locomotionEnabled_;}
    bool HeightAdjustEnabled()const noexcept{return heightAdjustEnabled_;}
    TrackingMapping Mapping();
    bool Frame(FrameInput&,const Pose*,bool,XMFLOAT4X4&);
    void Event(TrackingEvent);
};
}
