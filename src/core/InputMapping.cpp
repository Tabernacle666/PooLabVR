#include "InputMapping.h"
#include "InputSystem.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cctype>
#include <map>
#include <utility>

namespace lab {
namespace {
std::string Trim(std::string value){auto first=value.find_first_not_of(" \t"),last=value.find_last_not_of(" \t");return first==std::string::npos?std::string{}:value.substr(first,last-first+1);}
bool ParseBool(const std::string& value,bool& out){if(value=="1"||value=="true"||value=="on"){out=true;return true;}if(value=="0"||value=="false"||value=="off"){out=false;return true;}return false;}
float ParseFloat(const std::string& value,const char* error){char* tail=nullptr;float number=std::strtof(value.c_str(),&tail);if(value.empty()||*tail||!std::isfinite(number))Fail(error);return number;}
bool HandToken(std::string_view token,uint32_t& hand){if(token=="left"){hand=0;return true;}if(token=="right"){hand=1;return true;}return false;}
bool TransformProperty(std::string_view p){return p=="deadzone"||p=="min"||p=="max"||p=="curve"||p=="invert"||p=="invert_x"||p=="invert_y"||p=="press"||p=="release"||p=="smoothing";}
bool PropertyCompatible(InputActionType source,InputActionType destination,std::string_view p){
    if(p=="smoothing")return source==InputActionType::Float||source==InputActionType::Vector2;
    if(p=="deadzone"||p=="curve"||p=="min"||p=="max")return source==InputActionType::Float||source==InputActionType::Vector2;
    if(p=="press"||p=="release")return source==InputActionType::Float&&(destination==InputActionType::Float||destination==InputActionType::Boolean);
    if(p=="invert")return source==InputActionType::Float;
    if(p=="invert_x"||p=="invert_y")return source==InputActionType::Vector2;
    return false;
}
struct PendingBinding {std::string action,source;uint32_t hand{};std::map<std::string,std::string> properties;};
}

const char* InputActionTypeName(InputActionType type) noexcept {switch(type){case InputActionType::Boolean:return "Boolean";case InputActionType::Float:return "Float";case InputActionType::Vector2:return "Vector2";case InputActionType::Pose:return "Pose";case InputActionType::Haptic:return "Haptic";}return "Invalid";}
const char* InputActionErrorName(InputActionError error) noexcept {switch(error){case InputActionError::None:return "none";case InputActionError::InvalidName:return "invalid input name";case InputActionError::DuplicateName:return "duplicate input name";case InputActionError::InvalidType:return "invalid input type";case InputActionError::CapacityExceeded:return "input registry capacity exceeded";case InputActionError::InvalidHandle:return "invalid input handle";case InputActionError::IncompatibleBinding:return "binding types are incompatible";case InputActionError::UnknownPhysicalControl:return "unknown physical control";}return "unknown input error";}
InputActionError InputMapping::ValidateBinding(InputActionType source,InputActionType destination)const noexcept{
    if(source==InputActionType::Boolean&&destination==InputActionType::Boolean)return InputActionError::None;
    if(source==InputActionType::Float&&(destination==InputActionType::Float||destination==InputActionType::Boolean))return InputActionError::None;
    if(source==InputActionType::Vector2&&destination==InputActionType::Vector2)return InputActionError::None;
    if(source==InputActionType::Pose&&destination==InputActionType::Pose)return InputActionError::None;
    if(source==InputActionType::Haptic&&destination==InputActionType::Haptic)return InputActionError::None;
    return InputActionError::IncompatibleBinding;
}
const InputBinding* InputMapping::HapticBinding(LabActionId action,uint32_t hand)const noexcept{for(const auto& b:bindings_)if(b.action==action&&b.hand==(hand&1u)&&b.sourceType==InputActionType::Haptic&&b.actionType==InputActionType::Haptic)return &b;return nullptr;}
void InputMapping::SetBooleanSources(LabActionId action,uint32_t mask,const std::vector<PhysicalControlId>& sources){
    std::erase_if(bindings_,[&](const auto& b){return b.action==action&&(mask&(1u<<b.hand));});
    for(auto source:sources)for(uint32_t h=0;h<2;h++)if(mask&(1u<<h)){InputBinding b;b.source=source;b.action=action;b.hand=h;b.sourceType=b.actionType=InputActionType::Boolean;bindings_.push_back(b);}
}
void InputMapping::LoadConfiguration(const std::wstring& path,const InputSystem& inputSystem){
    std::ifstream file{std::filesystem::path(path)};if(!file)Fail("input.ini: cannot open input configuration");std::string line;std::vector<PendingBinding> pending;
    auto findPending=[&](std::string_view action,uint32_t hand)->PendingBinding&{for(auto& p:pending)if(p.action==action&&p.hand==hand)return p;pending.push_back({std::string(action),{},hand,{}});return pending.back();};
    while(std::getline(file,line)){
        auto end=line.find_first_of("#;\r");if(end!=std::string::npos)line.resize(end);line=Trim(line);if(line.empty())continue;auto equals=line.find('=');if(equals==std::string::npos)Fail("input.ini: expected key=value");auto key=Trim(line.substr(0,equals)),value=Trim(line.substr(equals+1));if(key.rfind("bind.",0)!=0)continue;key.erase(0,5);
        std::string property;auto last=key.rfind('.');if(last==std::string::npos)Fail("input.ini: binding key requires hand");std::string tail=key.substr(last+1);uint32_t hand{};
        if(TransformProperty(tail)){property=tail;key.resize(last);last=key.rfind('.');if(last==std::string::npos)Fail("input.ini: binding transform requires hand");tail=key.substr(last+1);}
        if(!HandToken(tail,hand))Fail("input.ini: binding hand must be left or right");std::string action=key.substr(0,last);if(action.empty())Fail("input.ini: binding action is empty");auto& p=findPending(action,hand);if(property.empty())p.source=value;else p.properties[property]=value;
    }
    std::vector<InputBinding> next;next.reserve(pending.size());
    for(const auto& p:pending){LabActionId action=inputSystem.FindAction(p.action);if(!action)Fail("input.ini: unknown logical action");size_t start=0;do{auto end=p.source.find('|',start);auto sourceName=Trim(p.source.substr(start,end==std::string::npos?std::string::npos:end-start));PhysicalControlId source=inputSystem.FindPhysicalControl(sourceName);if(!source)Fail("input.ini: unknown physical control");const auto& sourceInfo=inputSystem.Physical(source);const auto& actionInfo=inputSystem.Action(action);if(!(sourceInfo.handMask&(1u<<p.hand)))Fail("input.ini: physical control unavailable for selected hand");if(ValidateBinding(sourceInfo.type,actionInfo.type)!=InputActionError::None)Fail("input.ini: incompatible physical/logical binding");InputBinding b; b.source=source;b.action=action;b.hand=p.hand;b.sourceType=sourceInfo.type;b.actionType=actionInfo.type;b.transform.minimum=sourceInfo.type==InputActionType::Vector2?-1.f:0.f;b.transform.maximum=1.f;
        for(const auto& [property,value]:p.properties){if(!PropertyCompatible(sourceInfo.type,actionInfo.type,property))Fail("input.ini: transform incompatible with binding types");auto& t=b.transform;if(property=="invert"||property=="invert_x"||property=="invert_y"){bool flag{};if(!ParseBool(value,flag))Fail("input.ini: invalid boolean value");if(property=="invert")t.invert=flag;else if(property=="invert_x")t.invertX=flag;else t.invertY=flag;continue;}float number=ParseFloat(value,"input.ini: invalid numeric value");if(property=="deadzone"&&number>=0&&number<1)t.deadzone=number;else if(property=="curve"&&number>0&&number<=8)t.curve=number;else if(property=="min")t.minimum=number;else if(property=="max")t.maximum=number;else if(property=="press"&&number>=0&&number<=1)t.press=number;else if(property=="release"&&number>=0&&number<=1)t.release=number;else if(property=="smoothing"&&number>=0&&number<1)t.smoothing=number;else Fail("input.ini: input transform value outside bounds");}
        if(b.transform.minimum>b.transform.maximum)Fail("input.ini: binding min exceeds max");if(b.transform.release>b.transform.press)Fail("input.ini: binding release exceeds press threshold");next.push_back(std::move(b));if(end==std::string::npos)break;if(actionInfo.type!=InputActionType::Boolean)Fail("input.ini: OR requires a boolean action");start=end+1;}while(start<=p.source.size());
    }
    bindings_=std::move(next);
}
}
