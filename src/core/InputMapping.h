#pragma once
#include "Core.h"
#include <string>
#include <string_view>
#include <vector>

namespace lab {
class InputSystem;

enum class TrackingEvent {SessionStopped,ReferenceSpaceChanged};
enum class InputActionType:uint8_t {Boolean,Float,Vector2,Pose,Haptic};
enum class InputActionError:uint8_t {None,InvalidName,DuplicateName,InvalidType,CapacityExceeded,InvalidHandle,IncompatibleBinding,UnknownPhysicalControl};

struct TrackingMapping {
    void* context{};
    bool (*frame)(void*,FrameInput&,const Pose*,bool,XMFLOAT4X4&){};
    void (*event)(void*,TrackingEvent){};
};

struct InputTransform {
    float deadzone{},curve{1.f},minimum{},maximum{1.f},press{.5f},release{.5f},smoothing{};
    bool invert{},invertX{},invertY{};
};

struct InputBinding {
    PhysicalControlId source{};
    LabActionId action{};
    uint32_t hand{};
    InputActionType sourceType{};
    InputActionType actionType{};
    InputTransform transform{};
};

class InputMapping {
    std::vector<InputBinding> bindings_;
public:
    InputActionError ValidateBinding(InputActionType source,InputActionType destination)const noexcept;
    void SetBooleanSources(LabActionId,uint32_t,const std::vector<PhysicalControlId>&);
    void LoadConfiguration(const std::wstring& path,const InputSystem& inputSystem);
    uint32_t Count()const noexcept{return uint32_t(bindings_.size());}
    const std::vector<InputBinding>& Bindings()const noexcept{return bindings_;}
    const InputBinding* HapticBinding(LabActionId action,uint32_t hand)const noexcept;
};

const char* InputActionTypeName(InputActionType type) noexcept;
const char* InputActionErrorName(InputActionError error) noexcept;
}
