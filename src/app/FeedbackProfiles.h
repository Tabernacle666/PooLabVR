#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace OASIS {

enum class FeedbackCue : uint32_t {
    CaptureSaved,SurfaceEnter,SurfaceSlide,SurfaceExit,ShuttleBoot,ShuttleIgnition,
    RotorReverse,ShuttleJet,ShuttleFan,DustGlass,DustHull,MenuDetent,
    LunarRifle,LunarCannon,LunarSurface,LunarMembrane,LunarMotor,Count
};
inline constexpr uint32_t FeedbackCueCount=uint32_t(FeedbackCue::Count);
enum class LunarCue : uint32_t;
FeedbackCue LunarFeedback(LunarCue cue);

class FeedbackProfiles {
public:
    enum class HapticMode : int {Off, Toc, Shake, Drive};
    struct Audio {int gain{100},pitch{100};std::string file;};
    struct Haptic {int mode{},strength{},duration{32},delay{},echo{100},echoDuration{32},interval{65};};
    const Audio& Sound(FeedbackCue cue) const {return audio_[uint32_t(cue)];}
    const Haptic& Pulse(FeedbackCue cue) const {return haptic_[uint32_t(cue)];}
    static const char* Key(FeedbackCue cue);
    static bool Spatial(FeedbackCue cue);
    static constexpr int AudioFields=2,HapticFields=7;

    FeedbackProfiles();
    void Load(const std::filesystem::path&);
    bool Save();
    void OverwriteFactory();
    int Value(bool haptic,uint32_t cue,int field) const;
    void Edit(bool haptic,uint32_t cue,int field,int steps,bool reset);

private:
    std::filesystem::path folder_;
    std::array<Audio,FeedbackCueCount> audio_,factoryAudio_;
    std::array<Haptic,FeedbackCueCount> haptic_,factoryHaptic_;
    bool audioDirty_{},hapticDirty_{};
};

}
