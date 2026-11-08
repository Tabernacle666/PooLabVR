#include "GpuParticleField.h"

namespace lab {
namespace {
__device__ __forceinline__ unsigned Mix(unsigned h) {
    h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;return h^(h>>16);
}
__device__ __forceinline__ float Fraction(unsigned h) {
    return float(Mix(h)&0x00ffffffu)*(1.f/16777216.f);
}
__global__ void Initialize(GpuParticleFieldState* particles,unsigned count,unsigned seed) {
    const unsigned id=blockIdx.x*blockDim.x+threadIdx.x;
    if(id>=count)return;
    const unsigned key=Mix(id^seed);
    const float x=(Fraction(key+11u)-.5f)*100.f;
    const float y=.05f+Fraction(key+29u)*12.f;
    const float z=(Fraction(key+47u)-.5f)*100.f;
    particles[id].position=make_float4(x,y,z,Fraction(key+67u));
    particles[id].velocity=make_float4((Fraction(key+73u)-.5f)*.5f,
                                             (Fraction(key+89u)-.5f)*.15f,
                                             (Fraction(key+97u)-.5f)*.5f,
                                             .5f+Fraction(key+103u));
}
__global__ void Advance(GpuParticleFieldState* particles,unsigned count,float dt,float time) {
    const unsigned id=blockIdx.x*blockDim.x+threadIdx.x;
    if(id>=count)return;
    auto s=particles[id];
    // Independent live integration: gravity, bounded wind and ground rebound.
    // No per-particle CPU update, PhysX actor or GPU->CPU readback.
    const float gust=(s.position.w-.5f)*s.velocity.w;
    s.velocity.x=(s.velocity.x+(.12f+gust*.25f-s.position.z*.0008f)*dt)*.9997f;
    s.velocity.z=(s.velocity.z+(.06f-gust*.18f+s.position.x*.0008f)*dt)*.9997f;
    s.velocity.y-=.28f*dt;
    s.position.x+=s.velocity.x*dt;
    s.position.y+=s.velocity.y*dt;
    s.position.z+=s.velocity.z*dt;
    if(s.position.y<.05f){s.position.y=.05f;s.velocity.y=fabsf(s.velocity.y)*.65f+.02f;}
    if(s.position.y>12.f){s.position.y=12.f;s.velocity.y=-fabsf(s.velocity.y);}
    if(s.position.x>=50.f)s.position.x-=100.f;
    if(s.position.x< -50.f)s.position.x+=100.f;
    if(s.position.z>=50.f)s.position.z-=100.f;
    if(s.position.z< -50.f)s.position.z+=100.f;
    // Time is part of the deterministic field phase, not a CPU animation path.
    (void)time;
    particles[id]=s;
}
}
void GpuParticleFieldInitialize(cudaStream_t stream,GpuParticleFieldState* states,
                                std::uint32_t count,std::uint32_t seed){
    if(count)Initialize<<<(count+255)/256,256,0,stream>>>(states,count,seed);
}
void GpuParticleFieldAdvance(cudaStream_t stream,GpuParticleFieldState* states,
                             std::uint32_t count,float dt,float time){
    if(count)Advance<<<(count+255)/256,256,0,stream>>>(states,count,dt,time);
}
}
