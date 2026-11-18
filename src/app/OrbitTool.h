#pragma once
#include "PhosphorOrbit.h"
#include <filesystem>
#include <string>

namespace OASIS {
class Graphics;
struct AtmosphereSettings;
struct DlssScaleControl;

class OrbitProfile {
    std::filesystem::path path_;
    std::array<std::optional<float>,2> values_;
    bool dirty_{};
public:
    explicit OrbitProfile(const std::wstring& runtimeConfig);
    std::optional<float> Get(OrbitFeature)const;
    void Set(OrbitFeature,std::optional<float>);
    bool Save()const;
};

class OrbitTool {
    OrbitProfile& profile_;
    OrbitDisplay display_;
    float factoryFog_;
    float dlssMinimum_{},dlssMaximum_{};
    bool logo_{},basaltEquipped_{true};
    bool awaitNeutral_{},gestureArmed_{},turning_{};
    float previousAngle_{},accumulatedAngle_{};
    double colorReturn_{};
    int steps_{},snapDirection_{};

    void UpdatePointer(const PoseActionState& left,const PoseActionState& aim);
    void Drop();
    void ReadGesture(XMFLOAT2 stick,double time);
    void Refresh(const DlssScaleControl&,const AtmosphereSettings&);
    std::optional<float> UserValue(OrbitFeature)const;
public:
    OrbitTool(OrbitProfile& profile,bool logo,float factoryFog)
        :profile_(profile),factoryFog_(factoryFog),logo_(logo){}
    void Restore(Graphics&,AtmosphereSettings&);
    bool ReadInput(const FrameInput&);
    void Apply(Graphics&,AtmosphereSettings&);
    bool BasaltEquipped()const{return basaltEquipped_;}
    const OrbitDisplay& Display()const{return display_;}
};
}
