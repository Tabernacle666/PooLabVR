#pragma once


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
#include "RuntimeConfig.h"
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <thread>
#include <vector>
#include <limits>

namespace OASIS {
using namespace DirectX;
inline constexpr uint32_t MaxLights=RuntimeConfig::MaxLights;
constexpr float PhysicsDt=1.0f/240.0f,NearZ=0.04f,FarZ=100.0f;
constexpr uint32_t Invalid=0xffffffffu;
using FailureSink=void(*)(const char*) noexcept;
inline FailureSink failureSink{};
[[noreturn]] inline void Fail(const char* text){if(failureSink){failureSink(text);ExitProcess(1);}MessageBoxA(nullptr,text,"OASIS",MB_OK|MB_ICONERROR);ExitProcess(1);}
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
inline XMFLOAT3 TransformPoint(XMFLOAT3 v,FXMMATRIX m){XMFLOAT3 o;XMStoreFloat3(&o,XMVector3TransformCoord(XMLoadFloat3(&v),m));return o;}
inline XMFLOAT3 TransformDirection(XMFLOAT3 v,FXMMATRIX m){XMFLOAT3 o;XMStoreFloat3(&o,XMVector3TransformNormal(XMLoadFloat3(&v),m));return o;}
inline XMFLOAT3 Transform(XMFLOAT3 v,const Pose& p){return TransformPoint(v,Matrix(p));}
inline Pose Blend(const Pose& a,const Pose& b,float t){Pose r;r.p=Add(a.p,Scale(Sub(b.p,a.p),t));XMStoreFloat4(&r.q,XMQuaternionSlerp(XMLoadFloat4(&a.q),XMLoadFloat4(&b.q),t));return r;}
inline uint32_t GrowCapacity(uint32_t current,uint64_t required){
    uint64_t capacity=(std::max)(16u,current);
    while(capacity<required)capacity*=2;
    if(capacity>uint64_t(Invalid))Fail("Resource address space exhausted");
    return uint32_t(capacity);
}


constexpr uint32_t InputSourceCount=2;
constexpr uint32_t MaxPhysicalControls=32;
constexpr uint32_t MaxActions=64;
constexpr uint32_t MaxActionSets=16;
struct PhysicalControlId {uint32_t value{Invalid};explicit operator bool()const{return value!=Invalid;}bool operator==(const PhysicalControlId&)const=default;};
struct ActionId {uint32_t value{Invalid};explicit operator bool()const{return value!=Invalid;}bool operator==(const ActionId&)const=default;};
struct ActionSetId {uint32_t value{Invalid};explicit operator bool()const{return value!=Invalid;}bool operator==(const ActionSetId&)const=default;};
struct PoseActionState {Pose pose;XMFLOAT3 velocity{},angularVelocity{};bool tracked{};};
struct InputActionState {
    std::array<XMFLOAT2,InputSourceCount> vector{};std::array<float,InputSourceCount> scalar{};std::array<bool,InputSourceCount> active{},down{},pressed{};std::array<PoseActionState,InputSourceCount> pose{};
};
struct MembraneCrossing {XMFLOAT3 position{};bool entering{};uint32_t sphere{};double solarTime{-1.};};
struct FrameInput {
    uint64_t frameId{},trackingRevision{};int64_t targetDisplayTime{},targetDisplayQpc{};double time{};Pose head;uint32_t actionCount{};std::array<InputActionState,MaxActions> actions{};bool focused{};
    float sceneUnitsPerMeter{1.f},farClipMeters{FarZ};
    double solarTime{-1.};
    std::array<MembraneCrossing,4> crossings{};
    uint32_t crossingCount{},insideSphere{Invalid};
    float SceneNearZ() const noexcept {return NearZ*sceneUnitsPerMeter;}
    float SceneFarZ() const noexcept {return farClipMeters*sceneUnitsPerMeter;}
    const InputActionState& State(ActionId action)const{if(!action||action.value>=actionCount)Fail("Logical input action outside frame registry");return actions[action.value];}
    InputActionState& State(ActionId action){if(!action||action.value>=actionCount)Fail("Logical input action outside frame registry");return actions[action.value];}
    XMFLOAT2 Vector(ActionId action)const{const auto& s=State(action);return s.active[0]?s.vector[0]:(s.active[1]?s.vector[1]:XMFLOAT2{});}
    float Analog(ActionId action,uint32_t hand)const{return State(action).scalar[hand&1];}
    bool Down(ActionId action,uint32_t hand)const{return State(action).down[hand&1];}
    bool Pressed(ActionId action,uint32_t hand)const{return State(action).pressed[hand&1];}
    bool Pressed(ActionId action)const{const auto& s=State(action);return s.pressed[0]||s.pressed[1];}
    const PoseActionState& PoseState(ActionId action,uint32_t hand)const{return State(action).pose[hand&1];}
};
enum class Mesh:uint32_t {Box,Sphere,Cylinder,Count};
enum ObjectFlags:uint32_t { DynamicObject=1, DeformedObject=2, NoShadow=4, NoCollision=8, AlphaBlendObject=16, NoRayTracing=32, CompanionObject=1u<<13u };
struct SceneLight { XMFLOAT3 position{}; float radius{7}; XMFLOAT3 color{1,1,1}; float power{1}; XMFLOAT3 direction{0,0,1}; float outerCos{-1}; float innerCos{1}; uint32_t id{}, revision{}, flags{1}, animationNode{Invalid}; };
struct RenderObject {Pose previous,current;XMFLOAT3 scale{1,1,1};uint32_t mesh{},material{},id{},flags{},visualPose{};};
struct RenderScene {uint64_t inputFrameId{},simulationTick{};double time{};uint32_t count{},lightCount{};std::vector<SceneLight> lights;std::vector<RenderObject> objects;};
struct HapticEvent {uint32_t hand{};float amplitude{},duration{};uint64_t inputFrameId{};ActionId action{};};

}
