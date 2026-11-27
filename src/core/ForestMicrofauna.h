#pragma once
#include "Assets.h"
#include "SphereGeometry.h"
#include <unordered_map>

namespace OASIS {
// Two stable raster slots; only passive habitat metadata spans the world.
class ForestMicrofauna {
    static constexpr float Units=SpectacleInteriorUnitsPerMeter,Radius=10.f*Units;
    static constexpr uint32_t AmbientId=0x6f610001u,SwarmId=0x6f610002u;
    uint32_t mesh_{Invalid},material_{Invalid},active_{Invalid};
    std::vector<MicrofaunaAttractor> attractors_;
    std::unordered_map<uint64_t,std::vector<uint32_t>> bins_;
    float fade_{},groundY_{},lastFrequency_{-1.f};
    double lastTime_{},nextSearch_{};
    bool groundValid_{};
    static int32_t Cell(float p){return int32_t(std::floor(p/Radius));}
    static uint64_t Key(int32_t x,int32_t z){return (uint64_t(uint32_t(x))<<32)|uint32_t(z);}
    static float Random(uint32_t n){
        n^=n>>16;n*=0x7feb352du;n^=n>>15;n*=0x846ca68bu;n^=n>>16;
        return float(n&0x00ffffffu)/16777215.f;
    }
    static float Distance2(XMFLOAT3 a,XMFLOAT3 b){const auto d=Sub(a,b);return Dot(d,d);}
    bool Eligible(uint32_t i,float frequency)const{
        return frequency>0.f&&(frequency>=1.f||Random(attractors_[i].seed^0x53574152u)<frequency*frequency);
    }
    uint32_t Nearest(XMFLOAT3 p,float frequency)const{
        uint32_t best=Invalid;float bestDistance=Radius*Radius;
        const auto x=Cell(p.x),z=Cell(p.z);
        for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){
            const auto it=bins_.find(Key(x+i,z+j));
            if(it==bins_.end())continue;
            for(const auto index:it->second){
                if(!Eligible(index,frequency))continue;
                const float d=Distance2(p,attractors_[index].position);
                if(d<bestDistance||(d==bestDistance&&index<best)){best=index;bestDistance=d;}
            }
        }
        return best;
    }
    void AppendSlot(RenderScene& scene,uint32_t id,XMFLOAT3 center,float scale,uint32_t data,bool enabled)const{
        RenderObject o{};
        o.current.p=center;o.previous=o.current;o.scale={scale,scale,scale};
        o.mesh=mesh_;o.material=material_;o.id=id;
        o.flags=DynamicObject|NoCollision|NoRayTracing|NoShadow|AlphaBlendObject|
            OASIS_INSTANCE_FOREST|OASIS_INSTANCE_FOREST_INSECTS;
        if(!enabled)o.flags|=128u; // disabled GPU instance retains its address
        o.microfaunaData=data;
        scene.objects.push_back(o);++scene.count;
    }
public:
    uint32_t Mesh()const noexcept{return mesh_;}
    void Configure(const SceneData& scene){
        *this=ForestMicrofauna{};
        mesh_=scene.microfaunaMesh;material_=scene.microfaunaMaterial;
        if(mesh_==Invalid)return;
        attractors_=scene.microfaunaAttractors;
        for(uint32_t i=0;i<attractors_.size();++i){
            const auto p=attractors_[i].position;
            bins_[Key(Cell(p.x),Cell(p.z))].push_back(i);
        }
    }
    void Append(RenderScene& scene,const FrameInput& input,double time,bool exterior,float density,float frequency){
        if(mesh_==Invalid)return;
        const auto player=input.head.p;
        const bool local=exterior&&input.forestExterior&&
            player.y>=input.forestGroundY-.5f*Units&&player.y<input.forestGroundY+10.f*Units;
        const float dt=float(std::clamp(time-lastTime_,0.,.10));lastTime_=time;
        const auto count=local?uint32_t(std::lround(std::clamp(density,0.f,1.f)*OASIS_MICROFAUNA_COUNT)):0u;
        if(!local){groundValid_=false;active_=Invalid;fade_=0.f;nextSearch_=0.;}
        if(count){
            if(!groundValid_||std::abs(input.forestGroundY-groundY_)>2.f*Units)groundY_=input.forestGroundY;
            else groundY_=std::lerp(groundY_,input.forestGroundY,std::min(1.f,dt*5.f));
            groundValid_=true;
        }
        AppendSlot(scene,AmbientId,count?XMFLOAT3{player.x,groundY_,player.z}:XMFLOAT3{},Units,count,count!=0u);
        frequency=local?std::clamp(frequency,0.f,1.f):0.f;
        if(frequency!=lastFrequency_){nextSearch_=0.;lastFrequency_=frequency;}
        if(frequency<=0.f){active_=Invalid;fade_=0.f;}
        else{
            const bool retain=active_!=Invalid&&Eligible(active_,frequency)&&
                Distance2(player,attractors_[active_].position)<Radius*Radius;
            if(active_!=Invalid){
                fade_=std::clamp(fade_+(retain?dt*2.5f:-dt*4.f),0.f,1.f);
                if(!retain&&fade_<=0.f){active_=Invalid;nextSearch_=0.;}
            }
            if(active_==Invalid&&time>=nextSearch_){
                active_=Nearest(player,frequency);nextSearch_=time+.25;
                if(active_!=Invalid)fade_=std::min(1.f,dt*2.5f);
            }
        }
        const bool swarm=active_!=Invalid&&fade_>0.f&&frequency>0.f;
        XMFLOAT3 center{};float scale=Units;
        if(swarm){
            const auto& a=attractors_[active_];center=a.position;
            if(a.habitat==MicrofaunaHabitat::WetBank||a.habitat==MicrofaunaHabitat::Marsh)scale*=1.5f;
        }
        const uint32_t payload=OASIS_MICROFAUNA_SWARM|uint32_t(std::lround(fade_*65535.f));
        AppendSlot(scene,SwarmId,center,scale,payload,swarm);
    }
};
}
