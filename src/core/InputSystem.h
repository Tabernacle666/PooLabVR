#pragma once
#include "InputMapping.h"
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace lab {
struct PhysicalControlInfo {
    PhysicalControlId id{};
    std::string name;
    InputActionType type{};
    uint32_t handMask{3};
};
struct LabActionSetInfo {
    LabActionSetId id{};
    std::string name;
    std::string displayName;
};
struct LabActionInfo {
    LabActionId id{};
    uint32_t handle{};
    std::string name;
    std::string displayName;
    InputActionType type{};
    LabActionSetId actionSet{};
};
struct PhysicalControlState {
    std::array<XMFLOAT2,InputSourceCount> vector{};
    std::array<float,InputSourceCount> scalar{};
    std::array<bool,InputSourceCount> active{},down{};
    std::array<PoseActionState,InputSourceCount> pose{};
};
struct PhysicalFrame {
    uint64_t frameId{};
    int64_t targetDisplayTime{},targetDisplayQpc{};
    double time{};
    uint32_t controlCount{};
    bool focused{};
    std::array<PhysicalControlState,MaxPhysicalControls> controls{};
    PhysicalControlState& State(PhysicalControlId id){if(!id||id.value>=controlCount)Fail("Physical input outside registry");return controls[id.value];}
    const PhysicalControlState& State(PhysicalControlId id)const{if(!id||id.value>=controlCount)Fail("Physical input outside registry");return controls[id.value];}
};

class InputSystem {
    struct BindingRuntime {
        bool initialized{},down{};
        float scalar{};
        XMFLOAT2 vector{};
    };
    std::vector<PhysicalControlInfo> physical_;
    bool physicalControlsFrozen_{};
    std::vector<LabActionSetInfo> actionSets_;
    std::vector<LabActionInfo> actions_;
    InputMapping mapping_;
    PhysicalFrame lastPhysical_{};
    std::vector<BindingRuntime> bindingRuntime_;
    std::array<std::string,InputSourceCount> activeProfiles_{};
    std::array<std::array<bool,InputSourceCount>,MaxPhysicalControls> available_{};
    uint32_t nextActionHandle_{0x4c410101u};
    void RegisterStandardPhysicalControls();
    static bool ValidName(std::string_view) noexcept;
public:
    InputSystem();
    const PhysicalFrame& LastPhysical()const{return lastPhysical_;}
    bool SetBooleanSources(LabActionId,uint32_t,const std::vector<PhysicalControlId>&);

    PhysicalControlId RegisterPhysicalControl(std::string name,InputActionType type,uint32_t handMask=3);
    bool FreezePhysicalControls() noexcept;
    bool PhysicalControlsFrozen() const noexcept{return physicalControlsFrozen_;}
    LabActionSetId RegisterActionSet(std::string name,std::string displayName={});
    LabActionId RegisterAction(LabActionSetId set,std::string name,std::string displayName,InputActionType type);
    PhysicalControlId FindPhysicalControl(std::string_view name)const noexcept;
    LabActionSetId FindActionSet(std::string_view name)const noexcept;
    LabActionId FindAction(std::string_view name)const noexcept;
    LabActionId ActionFromHandle(uint32_t handle)const noexcept;
    uint32_t ActionHandle(LabActionId action)const noexcept;
    const PhysicalControlInfo& Physical(PhysicalControlId id)const;
    const LabActionSetInfo& ActionSet(LabActionSetId id)const;
    const LabActionInfo& Action(LabActionId id)const;
    uint32_t PhysicalCount()const noexcept{return uint32_t(physical_.size());}
    uint32_t ActionCount()const noexcept{return uint32_t(actions_.size());}
    uint32_t ActionSetCount()const noexcept{return uint32_t(actionSets_.size());}
    const std::vector<PhysicalControlInfo>& PhysicalControls()const noexcept{return physical_;}
    const std::vector<LabActionInfo>& Actions()const noexcept{return actions_;}
    const InputMapping& Mapping()const noexcept{return mapping_;}
    void LoadConfiguration(const std::wstring& path);
    // Actual OpenXR activity proves support even if a runtime reports a remapped
    // profile name. Profile changes clear this observation; no values are invented.
    void ObservePhysicalActivity(PhysicalControlId id,uint32_t hand,bool active){if(active&&id&&id.value<physical_.size())available_[id.value][hand&1u]=true;}
    void SetInteractionProfile(uint32_t hand,std::string profile,const std::vector<PhysicalControlId>& available);
    const std::string& InteractionProfile(uint32_t hand)const noexcept{return activeProfiles_[hand&1u];}
    bool PhysicalAvailable(PhysicalControlId id,uint32_t hand)const noexcept{return id&&id.value<physical_.size()&&available_[id.value][hand&1u];}
    void ResolveFrame(const PhysicalFrame& physical,FrameInput& logical);
    void ResolvePoses(const PhysicalFrame& physical,FrameInput& logical)const;
    PhysicalControlId ResolveHaptic(LabActionId action,uint32_t hand)const noexcept;
};
}
