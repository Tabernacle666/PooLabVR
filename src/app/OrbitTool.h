#pragma once
#include "PhosphorOrbit.h"
#include "LaserSweep.h"
#include <filesystem>
#include <string>

namespace OASIS {
class Graphics;
struct AtmosphereSettings;
struct DlssScaleControl;

class OrbitProfile {
    std::filesystem::path path_;
    std::array<std::optional<float>,OrbitFeatureCount> values_{},live_{};
    bool Save()const;
public:
    explicit OrbitProfile(const std::wstring& runtimeConfig);
    std::optional<float> Get(OrbitFeature)const;
    std::optional<float> Effective(OrbitFeature)const;
    void Remember(OrbitFeature,float);
    bool Set(OrbitFeature,std::optional<float>);
    void RestoreAtmosphere(AtmosphereSettings&)const;
};

class OrbitTool {
    OrbitProfile& profile_;
    OrbitDisplay display_;
    float factoryFog_;
    float dlssMinimum_{},dlssMaximum_{};
    bool logo_{};
    RightHandTool rightTool_{RightHandTool::Basalt};
    bool awaitNeutral_{},gestureArmed_{},turning_{};
    float previousAngle_{},accumulatedAngle_{};
    double colorReturn_{};
    int steps_{},snapDirection_{};

    void UpdatePointer(const PoseActionState& aim,FXMMATRIX model);
    void Drop();
    void ReadGesture(XMFLOAT2 stick,double time);
    void Refresh(const DlssScaleControl&,const AtmosphereSettings&);
    std::optional<float> UserValue(OrbitFeature)const;
public:
    OrbitTool(OrbitProfile& profile,bool logo,float factoryFog)
        :profile_(profile),factoryFog_(factoryFog),logo_(logo){}
    void Restore(Graphics&,AtmosphereSettings&);
    bool ReadInput(const FrameInput&,const XMFLOAT4X4* mounted=nullptr);
    void Suspend(){Drop();display_.hover=OrbitControl::None;display_.pointer.reset();}
    bool Apply(Graphics&,AtmosphereSettings&);
    RightHandTool EquippedTool()const{return rightTool_;}
    const OrbitDisplay& Display()const{return display_;}
};
}
