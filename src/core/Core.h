#pragma once

// Core owns the Win32 min/max policy for its public API. Defining NOMINMAX here
// protects standalone consumers; the explicit undef also sanitizes the hostile
// include order where Windows.h was included first without NOMINMAX.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <DirectXMath.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "ConsoleSpec.h"
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <thread>
#include <vector>
#include <limits>

namespace lab {
using namespace DirectX;
inline constexpr uint32_t MaxLights=ConsoleSpec::MaxLights;
constexpr uint32_t MaxAudioEmitters=16;
constexpr float PhysicsDt=1.0f/240.0f,NearZ=0.04f,FarZ=40.0f;
constexpr uint32_t Invalid=0xffffffffu;
struct CoreIdentity {
    const char* applicationName{"CoreClient"};
    const char* engineName{"Core"};
    const char* version{"0"};
    const wchar_t* versionW{L"0"};
    const wchar_t* cacheNamespaceW{L"CoreClient"};
    const char* diagnosticLabel{"CoreClient"};
    const wchar_t* diagnosticLabelW{L"CoreClient"};
    const char* pipelineCacheIdentity{"Core"};
    const void* telemetryProvider{};
};
inline CoreIdentity coreIdentity{};
inline void ConfigureCore(const CoreIdentity& identity) noexcept { coreIdentity=identity; ConsoleSpec::ValidateCpu(); }
inline const CoreIdentity& GetCoreIdentity() noexcept { return coreIdentity; }
using FailureSink=void(*)(const char*) noexcept;
inline FailureSink failureSink{};
[[noreturn]] inline void Fail(const char* text){if(failureSink)failureSink(text);MessageBoxA(nullptr,text,GetCoreIdentity().applicationName,MB_OK|MB_ICONERROR);ExitProcess(1);}
inline void Hr(HRESULT h,const char* text){if(FAILED(h)){char message[768];snprintf(message,sizeof(message),"%s (HRESULT 0x%08lX)",text,static_cast<unsigned long>(h));Fail(message);}}
inline int64_t QpcCounter(){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart;}
inline int64_t QpcFrequency(){static const int64_t frequency=[] {LARGE_INTEGER q;QueryPerformanceFrequency(&q);return q.QuadPart;}();return frequency;}
inline double QpcSeconds(int64_t counter){return double(counter)/double(QpcFrequency());}
inline double QpcNowSeconds(){return QpcSeconds(QpcCounter());}
inline double Clock(){return QpcNowSeconds();}
struct Pose {XMFLOAT3 p{};XMFLOAT4 q{0,0,0,1};};
inline XMMATRIX Matrix(const Pose& p){return XMMatrixRotationQuaternion(XMLoadFloat4(&p.q))*XMMatrixTranslation(p.p.x,p.p.y,p.p.z);}
inline Pose FromMatrix(FXMMATRIX m){Pose p;XMVECTOR s,q,t;XMMatrixDecompose(&s,&q,&t,m);XMStoreFloat3(&p.p,t);XMStoreFloat4(&p.q,q);return p;}
inline XMFLOAT3 Add(XMFLOAT3 a,XMFLOAT3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline XMFLOAT3 Sub(XMFLOAT3 a,XMFLOAT3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline XMFLOAT3 Scale(XMFLOAT3 a,float b){return {a.x*b,a.y*b,a.z*b};}
inline float Dot(XMFLOAT3 a,XMFLOAT3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline float Length(XMFLOAT3 a){return std::sqrt(Dot(a,a));}
inline XMFLOAT3 Unit(XMFLOAT3 a){return Scale(a,1.0f/(std::max)(Length(a),1e-6f));}
inline XMFLOAT3 Transform(XMFLOAT3 v,const Pose& p){XMFLOAT3 o;XMStoreFloat3(&o,XMVector3TransformCoord(XMLoadFloat3(&v),Matrix(p)));return o;}
inline Pose Blend(const Pose& a,const Pose& b,float t){Pose r;r.p=Add(a.p,Scale(Sub(b.p,a.p),t));XMStoreFloat4(&r.q,XMQuaternionSlerp(XMLoadFloat4(&a.q),XMLoadFloat4(&b.q),t));return r;}
inline uint32_t GrowCapacity(uint32_t current,uint64_t required){
    uint64_t capacity=(std::max)(16u,current);
    while(capacity<required)capacity*=2;
    if(capacity>uint64_t(Invalid))Fail("Resource address space exhausted");
    return uint32_t(capacity);
}
struct Handle {uint64_t value{UINT64_MAX};uint32_t index()const{return uint32_t(value);}uint32_t generation()const{return uint32_t(value>>32);}explicit operator bool()const{return value!=UINT64_MAX;}bool operator==(const Handle&)const=default;};
// Input has three deliberately separate identities. PhysicalControlId names a
// device signal, LabActionId names a logical gameplay intent, and LabActionSetId
// names the logical context that owns an action. None of these are OpenXR indices
// or plugin ABI handles.
constexpr uint32_t InputSourceCount=2;
constexpr uint32_t MaxPhysicalControls=32;
constexpr uint32_t MaxLabActions=64;
constexpr uint32_t MaxLabActionSets=16;
struct PhysicalControlId {uint32_t value{Invalid};explicit operator bool()const{return value!=Invalid;}bool operator==(const PhysicalControlId&)const=default;};
struct LabActionId {uint32_t value{Invalid};explicit operator bool()const{return value!=Invalid;}bool operator==(const LabActionId&)const=default;};
struct LabActionSetId {uint32_t value{Invalid};explicit operator bool()const{return value!=Invalid;}bool operator==(const LabActionSetId&)const=default;};
struct PoseActionState {Pose pose;XMFLOAT3 velocity{},angularVelocity{};bool tracked{};};
struct InputActionState {
    std::array<XMFLOAT2,InputSourceCount> vector{};std::array<float,InputSourceCount> scalar{};std::array<bool,InputSourceCount> active{},down{},pressed{};std::array<PoseActionState,InputSourceCount> pose{};
};
struct FrameInput {
    uint64_t frameId{},trackingRevision{};int64_t targetDisplayTime{},targetDisplayQpc{};double time{};Pose head;uint32_t actionCount{};std::array<InputActionState,MaxLabActions> actions{};bool focused{};
    const InputActionState& State(LabActionId action)const{if(!action||action.value>=actionCount)Fail("Logical input action outside frame registry");return actions[action.value];}
    InputActionState& State(LabActionId action){if(!action||action.value>=actionCount)Fail("Logical input action outside frame registry");return actions[action.value];}
    XMFLOAT2 Vector(LabActionId action)const{const auto& s=State(action);return s.active[0]?s.vector[0]:(s.active[1]?s.vector[1]:XMFLOAT2{});}
    float Analog(LabActionId action,uint32_t hand)const{return State(action).scalar[hand&1];}
    bool Down(LabActionId action,uint32_t hand)const{return State(action).down[hand&1];}
    bool Pressed(LabActionId action,uint32_t hand)const{return State(action).pressed[hand&1];}
    bool Pressed(LabActionId action)const{const auto& s=State(action);return s.pressed[0]||s.pressed[1];}
    const PoseActionState& PoseState(LabActionId action,uint32_t hand)const{return State(action).pose[hand&1];}
};
enum class Mesh:uint32_t {Box,Sphere,Cylinder,Count};
enum ObjectFlags:uint32_t { DynamicObject=1, DeformedObject=2, NoShadow=4, NoCollision=8, AlphaBlendObject=16, NoRayTracing=32 };
struct SceneLight { XMFLOAT3 position{}; float radius{7}; XMFLOAT3 color{1,1,1}; float power{1}; XMFLOAT3 direction{0,0,1}; float outerCos{-1}; float innerCos{1}; uint32_t id{}, revision{}, flags{1}, animationNode{Invalid}; };
struct RenderObject {Pose previous,current;XMFLOAT3 scale{1,1,1};uint32_t mesh{},material{},id{},flags{};};
struct RenderScene {uint64_t inputFrameId{},simulationTick{};double time{};uint32_t count{},lightCount{};std::array<SceneLight,MaxLights> lights;std::vector<RenderObject> objects;};
using EventKind=uint32_t; // Open identity supplied by the client; wire values stay 32-bit.
struct PhysicsEvent {EventKind kind{};Handle a{},b{};XMFLOAT3 position{};float strength{};uint32_t hand{};uint64_t inputFrameId{};};
struct AudioEvent {EventKind kind{};XMFLOAT3 position{};float gain{};uint64_t inputFrameId{};float frequency{},decay{};uint32_t wave{},flags{};};
using SoundWave=uint32_t;
struct AudioEmitter {uint32_t id{Invalid},flags{};uint64_t owner{},generation{};SoundWave wave{};XMFLOAT3 position{},velocity{};float gain{},frequency{},occlusion{};};
struct AudioFrame {uint64_t inputFrameId{};double time{};Pose listener;XMFLOAT3 velocity{};uint32_t count{};std::array<AudioEmitter,MaxAudioEmitters> emitters;};
struct HapticEvent {uint32_t hand{};float amplitude{},duration{};uint64_t inputFrameId{};LabActionId action{};};
template<class T,uint32_t N> class SpscQueue {
    static_assert((N&(N-1))==0);
    std::array<T,N> data_{};alignas(64) std::atomic<uint32_t> write_{};alignas(64) std::atomic<uint32_t> read_{};
public:
    bool push(const T& t)noexcept{auto w=write_.load(std::memory_order_relaxed);if(w-read_.load(std::memory_order_acquire)==N)return false;data_[w&(N-1)]=t;write_.store(w+1,std::memory_order_release);return true;}
    bool pop(T& t)noexcept{auto r=read_.load(std::memory_order_relaxed);if(r==write_.load(std::memory_order_acquire))return false;t=data_[r&(N-1)];read_.store(r+1,std::memory_order_release);return true;}
};
template<class T> class Mailbox {
    std::array<T,3> data_{};alignas(64) std::atomic<uint32_t> middle_{1};uint32_t back_=2,front_=0;
public:
    T& write()noexcept{return data_[back_];}
    void publish()noexcept{back_=middle_.exchange(back_|4u,std::memory_order_acq_rel)&3u;}
    const T& read()noexcept{if(middle_.load(std::memory_order_acquire)&4u)front_=middle_.exchange(front_,std::memory_order_acq_rel)&3u;return data_[front_];}
};
struct Channels {
    Mailbox<FrameInput> input;
    Mailbox<AudioFrame> audio;
    SpscQueue<AudioEvent,256> audioEvents;
    SpscQueue<HapticEvent,256> haptics;
    SpscQueue<PhysicsEvent,1024> physicsEvents;
    std::atomic<bool> stop{};
};
}

