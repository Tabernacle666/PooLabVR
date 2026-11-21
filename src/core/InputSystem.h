#pragma once
#include "InputMapping.h"
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace OASIS {
struct PhysicalControlInfo {
    PhysicalControlId id{};
    std::string name;
    InputActionType type{};
    uint32_t handMask{3};
};
struct ActionSetInfo {
    ActionSetId id{};
    std::string name;
    std::string displayName;
};
struct ActionInfo {
    ActionId id{};
    uint32_t handle{};
    std::string name;
    std::string displayName;
    InputActionType type{};
    ActionSetId actionSet{};
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
    std::vector<ActionSetInfo> actionSets_;
    std::vector<ActionInfo> actions_;
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
    bool SetBooleanSources(ActionId,uint32_t,const std::vector<PhysicalControlId>&);

    PhysicalControlId RegisterPhysicalControl(std::string name,InputActionType type,uint32_t handMask=3);
    bool FreezePhysicalControls() noexcept;
    bool PhysicalControlsFrozen() const noexcept{return physicalControlsFrozen_;}
    ActionSetId RegisterActionSet(std::string name,std::string displayName={});
    ActionId RegisterAction(ActionSetId set,std::string name,std::string displayName,InputActionType type);
    PhysicalControlId FindPhysicalControl(std::string_view name)const noexcept;
    ActionSetId FindActionSet(std::string_view name)const noexcept;
    ActionId FindAction(std::string_view name)const noexcept;
    ActionId ActionFromHandle(uint32_t handle)const noexcept;
    uint32_t ActionHandle(ActionId action)const noexcept;
    const PhysicalControlInfo& Physical(PhysicalControlId id)const;
    const ActionSetInfo& ActionSet(ActionSetId id)const;
    const ActionInfo& Action(ActionId id)const;
    uint32_t PhysicalCount()const noexcept{return uint32_t(physical_.size());}
    uint32_t ActionCount()const noexcept{return uint32_t(actions_.size());}
    uint32_t ActionSetCount()const noexcept{return uint32_t(actionSets_.size());}
    const std::vector<PhysicalControlInfo>& PhysicalControls()const noexcept{return physical_;}
    const std::vector<ActionInfo>& Actions()const noexcept{return actions_;}
    const InputMapping& Mapping()const noexcept{return mapping_;}
    bool LoadConfiguration(const std::wstring& path,const std::wstring& userPath={});


    void ObservePhysicalActivity(PhysicalControlId id,uint32_t hand,bool active){if(active&&id&&id.value<physical_.size())available_[id.value][hand&1u]=true;}
    void SetInteractionProfile(uint32_t hand,std::string profile,const std::vector<PhysicalControlId>& available);
    const std::string& InteractionProfile(uint32_t hand)const noexcept{return activeProfiles_[hand&1u];}
    bool PhysicalAvailable(PhysicalControlId id,uint32_t hand)const noexcept{return id&&id.value<physical_.size()&&available_[id.value][hand&1u];}
    void ResolveFrame(const PhysicalFrame& physical,FrameInput& logical);
    void ResolvePoses(const PhysicalFrame& physical,FrameInput& logical)const;
    PhysicalControlId ResolveHaptic(ActionId action,uint32_t hand)const noexcept;
};
}
