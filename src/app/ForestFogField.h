#pragma once
#include "Assets.h"
#include "OasisMetric.h"
#include "SphereGeometry.h"

namespace OASIS {
// A single immutable atlas; no per-frame ecology work. Terrain altitude and
// three independently interpretable habitat channels share each bilinear read.
// R = baseline density/retention, G = water proximity, B = exposure, A = height.
// Future weather producers may replace this bake without changing the ray path.
struct ForestFogField {
    static constexpr int N=512;
    static constexpr float Units=OasisSceneUnitsPerMeter;
    TextureData texture;
    XMFLOAT4 bounds{};
    XMFLOAT2 height{};
    static uint32_t Hash(uint32_t n){
        n^=n>>16;n*=0x7feb352du;n^=n>>15;n*=0x846ca68bu;n^=n>>16;return n;
    }
    static float Noise(float x,float z,uint32_t salt){
        const auto ix=int32_t(std::floor(x)),iz=int32_t(std::floor(z));
        float a=x-float(ix),b=z-float(iz);a=a*a*(3.f-2.f*a);b=b*b*(3.f-2.f*b);
        const auto sample=[&](int32_t i,int32_t j){
            return float(Hash(uint32_t(i)*0x9e3779b9u^uint32_t(j)*0x85ebca6bu^salt)&0xffffffu)/16777215.f;
        };
        return std::lerp(std::lerp(sample(ix,iz),sample(ix+1,iz),a),
                         std::lerp(sample(ix,iz+1),sample(ix+1,iz+1),a),b);
    }
    void Build(const BuildingSiteTerrain& terrain,std::span<const XMFLOAT3> roots){
        *this=ForestFogField{};
        if(terrain.soil.empty())return;
        float minX=std::numeric_limits<float>::max(),minZ=minX,minY=minX;
        float maxX=-minX,maxZ=-minX,maxY=-minX;
        for(const auto* triangles:{&terrain.soil,&terrain.banks,&terrain.waterway})
            for(const auto& t:*triangles)for(const auto& v:t){
                minX=std::min(minX,v.x);maxX=std::max(maxX,v.x);
                minZ=std::min(minZ,v.z);maxZ=std::max(maxZ,v.z);
                minY=std::min(minY,v.y);maxY=std::max(maxY,v.y);
            }
        const float padding=4.f*Units;
        minX-=padding;maxX+=padding;minZ-=padding;maxZ+=padding;
        const float dx=(maxX-minX)/N,dz=(maxZ-minZ)/N;
        if(!(dx>0.f&&dz>0.f))return;
        bounds={minX,minZ,1.f/(maxX-minX),1.f/(maxZ-minZ)};
        height={minY,std::max(maxY-minY,.01f*Units)};
        constexpr size_t Count=size_t(N)*N;
        const float missing=-std::numeric_limits<float>::max(),inf=1.e9f;
        std::vector<float> ground(Count,missing),landDistance(Count,inf),wetDistance(Count,inf),canopy(Count,0.f);
        const auto idx=[](int x,int z){return size_t(z)*N+size_t(x);};
        const auto clampX=[&](float x){return std::clamp(int(std::floor((x-minX)/dx)),0,N-1);};
        const auto clampZ=[&](float z){return std::clamp(int(std::floor((z-minZ)/dz)),0,N-1);};
        // Rasterize the actual terrain once, in its native coordinate system.
        const auto raster=[&](const auto& triangles,bool wet){
            for(const auto& t:triangles){
                const auto& [a,b,c]=t;
                const auto ab=Sub(b,a),ac=Sub(c,a);
                const float det=ab.x*ac.z-ab.z*ac.x;
                if(std::abs(det)<1.e-12f)continue;
                const int x0=clampX(std::min({a.x,b.x,c.x})),x1=clampX(std::max({a.x,b.x,c.x}));
                const int z0=clampZ(std::min({a.z,b.z,c.z})),z1=clampZ(std::max({a.z,b.z,c.z}));
                for(int z=z0;z<=z1;++z)for(int x=x0;x<=x1;++x){
                    const float px=minX+(float(x)+.5f)*dx-a.x,pz=minZ+(float(z)+.5f)*dz-a.z;
                    const float u=(px*ac.z-pz*ac.x)/det,v=(ab.x*pz-ab.z*px)/det;
                    if(u<0.f||v<0.f||u+v>1.f)continue;
                    const auto i=idx(x,z);
                    ground[i]=std::max(ground[i],a.y+u*ab.y+v*ac.y);
                    landDistance[i]=0.f;if(wet)wetDistance[i]=0.f;
                }
            }
        };
        raster(terrain.soil,false);raster(terrain.banks,true);raster(terrain.waterway,true);
        // Chamfer distance propagation fills altitude under narrow gaps and
        // gives wet edges a smooth influence instead of a triangle pattern.
        const float sx=dx/Units,sz=dz/Units,diag=std::hypot(sx,sz);
        const auto propagate=[&](std::vector<float>& distance,bool carryHeight){
            const auto visit=[&](int x,int z,int nx,int nz,float step){
                if(nx<0||nx>=N||nz<0||nz>=N)return;
                const auto i=idx(x,z),j=idx(nx,nz);
                const float d=distance[j]+step;
                if(d<distance[i]){
                    distance[i]=d;
                    if(carryHeight)ground[i]=ground[j];
                }
            };
            for(int z=0;z<N;++z)for(int x=0;x<N;++x){
                visit(x,z,x-1,z,sx);visit(x,z,x,z-1,sz);
                visit(x,z,x-1,z-1,diag);visit(x,z,x+1,z-1,diag);
            }
            for(int z=N-1;z>=0;--z)for(int x=N-1;x>=0;--x){
                visit(x,z,x+1,z,sx);visit(x,z,x,z+1,sz);
                visit(x,z,x+1,z+1,diag);visit(x,z,x-1,z+1,diag);
            }
        };
        propagate(landDistance,true);propagate(wetDistance,false);
        // Scatter the actual tree roots into a broad retention/exposure field.
        // This is static canopy proximity, not a running wind simulation.
        constexpr float Reach=9.f*Units;
        for(const auto& root:roots){
            const int x0=clampX(root.x-Reach),x1=clampX(root.x+Reach);
            const int z0=clampZ(root.z-Reach),z1=clampZ(root.z+Reach);
            for(int z=z0;z<=z1;++z)for(int x=x0;x<=x1;++x){
                const float a=(minX+(float(x)+.5f)*dx-root.x)/Reach;
                const float b=(minZ+(float(z)+.5f)*dz-root.z)/Reach;
                const float w=std::max(0.f,1.f-a*a-b*b);
                canopy[idx(x,z)]+=w*w;
            }
        }
        texture.width=texture.height=N;texture.mips=1;
        texture.format=DXGI_FORMAT_R16G16B16A16_UNORM;
        texture.offsets={0};texture.bytes.resize(Count*8u);
        const auto encode=[](float v){return uint16_t(std::lround(std::clamp(v,0.f,1.f)*65535.f));};
        for(int z=0;z<N;++z)for(int x=0;x<N;++x){
            const auto i=idx(x,z);
            const float px=(minX+(float(x)+.5f)*dx)/Units,pz=(minZ+(float(z)+.5f)*dz)/Units;
            const float broad=Noise(px/18.f,pz/12.f,0x41544d31u);
            const float fine=Noise(px/5.f,pz/8.f,0x464f4732u);
            const float moisture=std::exp(-wetDistance[i]/9.f);
            const float retention=1.f-std::exp(-canopy[i]*.36f);
            const float exposure=std::clamp((1.f-retention)*(.55f+.45f*broad),0.f,1.f);
            const float edge=std::clamp(1.f-landDistance[i]/3.f,0.f,1.f);
            const float density=edge*(.12f+.34f*broad+.16f*fine+.38f*retention);
            const float y=ground[i]==missing?minY:ground[i];
            const std::array<uint16_t,4> channels{encode(density),encode(moisture*edge),encode(exposure),
                encode((y-height.x)/height.y)};
            std::memcpy(texture.bytes.data()+i*8u,channels.data(),8u);
        }
    }
    void Install(SceneData& scene)const{
        if(texture.bytes.empty())return;
        scene.forestFogTexture=uint32_t(scene.textures.size());
        scene.forestFogBounds=bounds;scene.forestFogHeight=height;
        scene.textures.push_back(texture);
    }
};
}
