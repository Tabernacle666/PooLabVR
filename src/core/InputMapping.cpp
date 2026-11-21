#include "InputMapping.h"
#include "InputSystem.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cctype>
#include <map>
#include <utility>
#include <iterator>

namespace OASIS {
namespace {
std::string Trim(std::string value){auto first=value.find_first_not_of(" \t"),last=value.find_last_not_of(" \t");return first==std::string::npos?std::string{}:value.substr(first,last-first+1);}
bool ParseBool(const std::string& value,bool& out){if(value=="1"||value=="true"||value=="on"){out=true;return true;}if(value=="0"||value=="false"||value=="off"){out=false;return true;}return false;}
bool ParseFloat(const std::string& value,float& number){
    char* tail=nullptr;
    number=std::strtof(value.c_str(),&tail);
    return !value.empty()&&!*tail&&std::isfinite(number);
}
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
const InputBinding* InputMapping::HapticBinding(ActionId action,uint32_t hand)const noexcept{for(const auto& b:bindings_)if(b.action==action&&b.hand==(hand&1u)&&b.sourceType==InputActionType::Haptic&&b.actionType==InputActionType::Haptic)return &b;return nullptr;}
void InputMapping::SetBooleanSources(ActionId action,uint32_t mask,const std::vector<PhysicalControlId>& sources){
    std::erase_if(bindings_,[&](const auto& b){return b.action==action&&(mask&(1u<<b.hand));});
    for(auto source:sources)for(uint32_t h=0;h<2;h++)if(mask&(1u<<h)){InputBinding b;b.source=source;b.action=action;b.hand=h;b.sourceType=b.actionType=InputActionType::Boolean;bindings_.push_back(b);}
}
bool InputMapping::LoadConfiguration(const std::wstring& path,const InputSystem& inputSystem,
                                     const std::wstring& userPath){
    std::ifstream factory{std::filesystem::path(path)};
    if(!factory)Fail("input.ini: cannot open input configuration");
    std::vector<PendingBinding> pending;
    auto read=[&](std::istream& file){
        for(std::string line;std::getline(file,line);){
            if(line.starts_with("\xEF\xBB\xBF"))line.erase(0,3);
            if(const auto comment=line.find_first_of("#;\r");comment!=std::string::npos)line.resize(comment);
            line=Trim(line);
            if(line.empty())continue;
            const auto equals=line.find('=');
            if(equals==std::string::npos)return false;
            auto key=Trim(line.substr(0,equals));
            if(!key.starts_with("bind."))continue;
            key.erase(0,5);
            auto last=key.rfind('.');
            if(last==std::string::npos)return false;
            std::string property,tail=key.substr(last+1);
            if(TransformProperty(tail)){
                property=tail;
                key.resize(last);
                last=key.rfind('.');
                if(last==std::string::npos)return false;
                tail=key.substr(last+1);
            }
            uint32_t hand{};
            if(!HandToken(tail,hand)||last==0)return false;
            const auto action=key.substr(0,last);
            auto entry=std::find_if(pending.begin(),pending.end(),[&](const auto& binding){
                return binding.action==action&&binding.hand==hand;
            });
            if(entry==pending.end()){
                pending.push_back({action,{},hand,{}});
                entry=std::prev(pending.end());
            }
            auto value=Trim(line.substr(equals+1));
            if(property.empty())entry->source=std::move(value);
            else entry->properties[property]=std::move(value);
        }
        return !file.bad();
    };
    auto resolve=[&](std::vector<InputBinding>& next){
        next.reserve(pending.size());
        for(const auto& p:pending){
            const auto action=inputSystem.FindAction(p.action);
            if(!action)return false;
            if(p.source=="none")continue;
            const auto actionType=inputSystem.Action(action).type;
            size_t start=0;
            for(;;){
                const auto end=p.source.find('|',start);
                const auto name=Trim(p.source.substr(start,end==std::string::npos?end:end-start));
                const auto source=inputSystem.FindPhysicalControl(name);
                if(!source)return false;
                const auto& info=inputSystem.Physical(source);
                if(!(info.handMask&(1u<<p.hand))||ValidateBinding(info.type,actionType)!=InputActionError::None)
                    return false;
                InputBinding binding{source,action,p.hand,info.type,actionType};
                auto& t=binding.transform;
                t.minimum=info.type==InputActionType::Vector2?-1.f:0.f;
                for(const auto& [property,value]:p.properties){
                    if(!PropertyCompatible(info.type,actionType,property))return false;
                    if(property=="invert"||property=="invert_x"||property=="invert_y"){
                        bool flag{};
                        if(!ParseBool(value,flag))return false;
                        if(property=="invert")t.invert=flag;
                        else if(property=="invert_x")t.invertX=flag;
                        else t.invertY=flag;
                        continue;
                    }
                    float number{};
                    if(!ParseFloat(value,number))return false;
                    if(property=="deadzone"&&number>=0&&number<1)t.deadzone=number;
                    else if(property=="curve"&&number>0&&number<=8)t.curve=number;
                    else if(property=="min")t.minimum=number;
                    else if(property=="max")t.maximum=number;
                    else if(property=="press"&&number>=0&&number<=1)t.press=number;
                    else if(property=="release"&&number>=0&&number<=1)t.release=number;
                    else if(property=="smoothing"&&number>=0&&number<1)t.smoothing=number;
                    else return false;
                }
                if(t.minimum>t.maximum||t.release>t.press)return false;
                next.push_back(binding);
                if(end==std::string::npos)break;
                if(actionType!=InputActionType::Boolean)return false;
                start=end+1;
            }
        }
        return true;
    };
    std::vector<InputBinding> next;
    if(!read(factory)||!resolve(next))Fail("input.ini: invalid factory binding");
    bindings_=std::move(next);
    if(userPath.empty())return true;
    std::ifstream user{std::filesystem::path(userPath)};
    if(!user){
        std::error_code error;
        return !std::filesystem::exists(std::filesystem::path(userPath),error)&&!error;
    }
    std::vector<InputBinding> custom;
    if(!read(user)||!resolve(custom))return false;
    bindings_=std::move(custom);
    return true;
}
}
