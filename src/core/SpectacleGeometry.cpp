#include "PhysicalMaterials.h"
#include "Assets.h"
#include "GpuSceneCache.h"
#include "SphereGeometry.h"
#include "SolarSystem.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <numeric>
#include <unordered_map>

namespace OASIS {
namespace {
// The size controls publication granularity, not the total scene complexity.
constexpr uint32_t TrianglesPerRegion=512;
uint32_t SpreadBits(uint32_t value) {
    value&=1023u;
    value=(value|(value<<16))&0x030000ffu;
    value=(value|(value<<8))&0x0300f00fu;
    value=(value|(value<<4))&0x030c30c3u;
    return (value|(value<<2))&0x09249249u;
}
uint32_t SpatialKey(XMFLOAT3 p,XMFLOAT3 lo,XMFLOAT3 hi) {
    auto q=[](float x,float a,float b){return b>a?uint32_t(std::clamp((x-a)/(b-a),0.f,1.f)*1023.f):0u;};
    return SpreadBits(q(p.x,lo.x,hi.x))|(SpreadBits(q(p.y,lo.y,hi.y))<<1)|(SpreadBits(q(p.z,lo.z,hi.z))<<2);
}
struct OrderedTriangle {uint32_t first{},key{};int64_t tileX{},tileZ{};};

}

void AppendSpectacleCity(SceneData& scene) {
    auto& mesh=scene.meshes;
    const auto center=SpectacleCenters[1];
    auto triangle=[&](XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c){
        const auto ab=Sub(b,a),ac=Sub(c,a);
        XMFLOAT3 normal;
        XMStoreFloat3(&normal,XMVector3Normalize(XMVector3Cross(XMLoadFloat3(&ab),XMLoadFloat3(&ac))));
        for(const auto point:{a,b,c}){
            mesh.indices.push_back(uint32_t(mesh.vertices.size()));
            mesh.vertices.push_back({Add(center,point),normal,{}});
        }
    };
    auto quad=[&](XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,XMFLOAT3 d){
        triangle(a,b,c);triangle(a,c,d);
    };
    auto box=[&](XMFLOAT3 lo,XMFLOAT3 hi){
        const auto [x,y,z]=lo;
        const auto [u,v,w]=hi;
        quad({x,y,z},{x,v,z},{u,v,z},{u,y,z});
        quad({u,y,w},{u,v,w},{x,v,w},{x,y,w});
        quad({x,y,w},{x,v,w},{x,v,z},{x,y,z});
        quad({u,y,z},{u,v,z},{u,v,w},{u,y,w});
        quad({x,v,z},{x,v,w},{u,v,w},{u,v,z});
        quad({x,y,w},{x,y,z},{u,y,z},{u,y,w});
    };
    auto surface=[&](const char* name,const char* materialId,XMFLOAT4 color,XMFLOAT4 emission,uint32_t flags,auto build){
        MeshRange range;
        range.firstIndex=uint32_t(mesh.indices.size());range.firstVertex=uint32_t(mesh.vertices.size());
        build();
        range.indexCount=uint32_t(mesh.indices.size())-range.firstIndex;
        range.vertexCount=uint32_t(mesh.vertices.size())-range.firstVertex;
        RenderObject object;
        object.mesh=uint32_t(mesh.ranges.size());object.material=uint32_t(scene.materials.size());
        object.flags=CompanionObject|flags;
        AssetMaterial material;material.colorRoughness=color;material.emissionMetallic=emission;
        material.doubleSided=(flags&NoCollision)!=0u;
        ApplyPhysicalMaterial(material,materialId);
        scene.materials.push_back(material);scene.materialNames.emplace_back(name);
        mesh.ranges.push_back(range);mesh.deformations.emplace_back();mesh.motions.emplace_back();
        scene.objects.push_back(object);
    };
    constexpr float ground=-.67f;
    surface("Terraced sandstone","mineral.sandstone",{.31f,.168f,.084f,.89f},{},0u,[&]{
        constexpr std::array<XMFLOAT2,6> rings{{{-1.93f,.81f},{-1.37f,1.9f},
            {-.99f,2.52f},{-.89f,2.52f},{-.89f,2.2f},{ground,2.14f}}};
        constexpr std::array<float,12> outline{1.f,.91f,1.04f,.96f,1.02f,.94f,1.f,.91f,.99f,1.04f,.93f,1.01f};
        auto point=[&](size_t layer,size_t side){
            const float angle=2.f*XM_PI*float(side)/float(outline.size());
            const float radius=rings[layer].y*outline[side%outline.size()];
            return XMFLOAT3{std::cos(angle)*radius,rings[layer].x,std::sin(angle)*radius};
        };
        for(size_t side=0;side<outline.size();++side){
            const auto next=(side+1u)%outline.size();
            for(size_t layer=1;layer<rings.size();++layer)
                quad(point(layer-1u,side),point(layer,side),point(layer,next),point(layer-1u,next));
            triangle({0.f,rings.front().x,0.f},point(0,side),point(0,next));
            triangle({0.f,ground,0.f},point(rings.size()-1u,next),point(rings.size()-1u,side));
        }
    });
    struct Tower {float x,z,w,d,h;};
    constexpr std::array<Tower,6> towers{{{-.96f,-.42f,.59f,.79f,1.66f},
        {-.14f,.49f,.66f,.58f,2.46f},{.83f,-.26f,.64f,.62f,2.08f},
        {-.85f,.72f,.48f,.54f,1.31f},{.18f,-.91f,.57f,.48f,1.21f},
        {.97f,.69f,.40f,.50f,1.53f}}};
    surface("Graphite tower shells","metal.alloy_matte",{.12f,.135f,.16f,.65f},{0,0,0,.2f},0u,[&]{
        for(const auto& t:towers){
            box({t.x-.5f*t.w,ground,t.z-.5f*t.d},{t.x+.5f*t.w,ground+t.h,t.z+.5f*t.d});
            box({t.x-.32f*t.w,ground+t.h,t.z-.29f*t.d},
                {t.x+.32f*t.w,ground+t.h+.23f,t.z+.29f*t.d});
        }
    });
    surface("Amber window bands","glass.tinted",{.32f,.17f,.07f,.42f},{.88f,.33f,.075f,.18f},NoCollision|NoShadow,[&]{
        for(const auto& t:towers){
            for(float level=.22f;level<t.h-.1f;level+=.235f){
                const float y=ground+level,top=y+.065f;
                const float x=t.x-.38f*t.w,u=t.x+.38f*t.w;
                const float z=t.z-.38f*t.d,w=t.z+.38f*t.d;
                for(const float face:{t.z-.501f*t.d,t.z+.501f*t.d})
                    quad({x,y,face},{x,top,face},{u,top,face},{u,y,face});
                for(const float face:{t.x-.501f*t.w,t.x+.501f*t.w})
                    quad({face,y,z},{face,top,z},{face,top,w},{face,y,w});
            }
        }
    });
    if(!mesh.skin.empty())mesh.skin.resize(mesh.vertices.size());
}

void InstantiateSpectacleCities(SceneData& scene,uint32_t count){
    const auto first=std::find_if(scene.objects.begin(),scene.objects.end(),[](const auto& object){
        return (object.flags&CompanionObject)!=0u;
    });
    if(first==scene.objects.begin()||first==scene.objects.end()||count<SpectaclePairCount||
       count>SpectacleSphereCount||std::any_of(first,scene.objects.end(),[](const auto& object){
           return (object.flags&CompanionObject)==0u;
       }))Fail("Spectacle object ranges");
    const std::vector<RenderObject> city(first,scene.objects.end());
    scene.sphereObjectOffsets={0u,uint32_t(first-scene.objects.begin()),uint32_t(scene.objects.size())};
    scene.objects.reserve(scene.objects.size()+city.size()*(count-SpectaclePairCount));
    for(uint32_t owner=SpectaclePairCount;owner<count;++owner){
        const auto translation=Sub(SpectacleCenters[owner],SpectacleCenters[1]);
        for(auto object:city){
            object.current.p=Add(object.current.p,translation);
            object.previous.p=Add(object.previous.p,translation);
            object.id=uint32_t(scene.objects.size());
            scene.objects.push_back(object);
        }
        scene.sphereObjectOffsets.push_back(uint32_t(scene.objects.size()));
    }
}

void AppendSolarPlanets(SceneData& scene){
    auto& mesh=scene.meshes;
    scene.solarSystem=true;
    scene.lights.clear();
    scene.sphereObjectOffsets={0u,uint32_t(scene.objects.size())};
    for(uint32_t owner=1;owner<SolarPlanetCount;++owner){
        const auto center=SpectacleCenters[owner];
        const auto color=SolarTerrainColor(owner);
        uint32_t seed=SolarSeed^(owner*0x85ebca6bu);
        const uint32_t sides=12u+2u*(owner%5u);
        const float elongation=.72f+.25f*SolarRandom(seed);
        const float shear=.5f*(SolarRandom(seed)-.5f);
        const float ground=-.45f-.35f*SolarRandom(seed);
        const float floor=-1.5f-.6f*SolarRandom(seed);
        const float peak=.2f+.65f*SolarRandom(seed);
        const float radius=1.85f+.6f*SolarRandom(seed);
        constexpr uint32_t layers=4;
        const std::array<float,layers> heights{floor,ground-.55f,ground,ground+.18f};
        const std::array<float,layers> radii{radius*.38f,radius*.88f,radius,radius*.52f};
        std::array<std::array<XMFLOAT3,20>,layers> rings{};
        for(uint32_t side=0;side<sides;++side){
            const float angle=XM_2PI*float(side)/float(sides);
            const float x=std::cos(angle),z=std::sin(angle)*elongation;
            const float roughness=.82f+.32f*SolarRandom(seed);
            for(uint32_t layer=0;layer<layers;++layer){
                const float y=heights[layer]+.28f*(SolarRandom(seed)-.5f);
                rings[layer][side]={radii[layer]*roughness*(x+shear*z),y,radii[layer]*roughness*z};
            }
        }
        MeshRange range{};
        range.firstIndex=uint32_t(mesh.indices.size());range.firstVertex=uint32_t(mesh.vertices.size());
        auto triangle=[&](XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c){
            const auto ab=Sub(b,a),ac=Sub(c,a);
            XMFLOAT3 normal;
            XMStoreFloat3(&normal,XMVector3Normalize(XMVector3Cross(XMLoadFloat3(&ab),XMLoadFloat3(&ac))));
            for(const auto point:{a,b,c}){
                mesh.indices.push_back(uint32_t(mesh.vertices.size()));
                mesh.vertices.push_back({Add(center,point),normal,{}});
            }
        };
        for(uint32_t side=0;side<sides;++side){
            const uint32_t next=(side+1u)%sides;
            for(uint32_t layer=1;layer<layers;++layer){
                const auto& a=rings[layer-1u];
                const auto& b=rings[layer];
                triangle(a[side],b[side],b[next]);
                triangle(a[side],b[next],a[next]);
            }
            triangle({0.f,floor,0.f},rings.front()[side],rings.front()[next]);
            triangle({shear,peak,-.25f},rings.back()[next],rings.back()[side]);
        }
        range.indexCount=uint32_t(mesh.indices.size())-range.firstIndex;
        range.vertexCount=uint32_t(mesh.vertices.size())-range.firstVertex;
        RenderObject object{};
        object.mesh=uint32_t(mesh.ranges.size());object.material=uint32_t(scene.materials.size());
        object.id=uint32_t(scene.objects.size());object.flags=CompanionObject;
        AssetMaterial material{};
        material.colorRoughness={color.x,color.y,color.z,.89f};
        ApplyPhysicalMaterial(material,"mineral.rock_generic");
        scene.materials.push_back(material);scene.materialNames.emplace_back("Orbital rock");
        mesh.ranges.push_back(range);mesh.deformations.emplace_back();mesh.motions.emplace_back();
        scene.objects.push_back(object);
        scene.sphereObjectOffsets.push_back(uint32_t(scene.objects.size()));
    }
    if(!mesh.skin.empty())mesh.skin.resize(mesh.vertices.size());
}

void PrepareSpectacleGeometry(SceneData& scene) {
    // The prepared path is for static opaque geometry; keep other scenes unchanged.
    if(scene.objects.empty())return;
    const auto& original=scene.meshes;
    for(const auto& object:scene.objects){
        if(object.mesh>=original.ranges.size()||object.material>=scene.materials.size())Fail("spectacle object mesh/material");
        if(object.mesh>=original.motions.size()||object.mesh>=original.deformations.size())Fail("spectacle mesh metadata");
        if(original.motions[object.mesh].node!=Invalid||original.deformations[object.mesh].axisAmplitude.w!=0 ||
           scene.materials[object.material].alphaMode==2 || (object.flags&(DynamicObject|DeformedObject|AlphaBlendObject)))return;
    }
    MeshData source=std::move(scene.meshes), result;
    // Fixed campus geometry is rigid and already baked in world metres.
    // Compact every prepared region; the legacy paths keep their original
    // vertex addresses. Source vertices stay available until packing ends.
    const bool packTerritory=scene.solarSystem&&scene.sphereObjectOffsets.empty()&&source.morphs.empty();
    if(!packTerritory){
        result.vertices=std::move(source.vertices);
        result.skin=std::move(source.skin);
    }
    result.morphs=std::move(source.morphs);
    const auto& sourceVertices=packTerritory?source.vertices:result.vertices;
    scene.terrainSpans.clear();
    const bool trackSolids=!scene.triangleSolids.empty();
    if(trackSolids&&scene.triangleSolids.size()*3ull!=source.indices.size())
        Fail("Spectacle triangle solid range");
    std::vector<uint32_t> triangleSolids;
    std::vector<RenderObject> objects;
    std::vector<Mg2ForestPart> remappedForestParts;
    size_t nextSphere=0;
    for(size_t index=0;index<scene.objects.size();++index){
        if(nextSphere<scene.sphereObjectOffsets.size()&&index==scene.sphereObjectOffsets[nextSphere])
            scene.sphereObjectOffsets[nextSphere++]=uint32_t(objects.size());
        const auto& object=scene.objects[index];
        const auto& src=source.ranges[object.mesh];
        const bool mg2Source=std::any_of(scene.mg2ForestParts.begin(),scene.mg2ForestParts.end(),
            [&](const auto& part){return part.mesh==object.mesh&&part.material==object.material;});
        const auto& name=scene.materialNames[object.material];
        const bool territorial=packTerritory&&(name=="Island soil"||name=="Island stone"||
            name=="River banks"||name=="Bioluminescent green water");
        if(src.indexCount%3||uint64_t(src.firstIndex)+src.indexCount>source.indices.size())Fail("spectacle index range");
        std::vector<OrderedTriangle> order;order.reserve(src.indexCount/3);
        XMFLOAT3 lo{FLT_MAX,FLT_MAX,FLT_MAX},hi{-FLT_MAX,-FLT_MAX,-FLT_MAX};
        for(uint32_t i=0;i<src.indexCount;i++){
            const auto index=source.indices[src.firstIndex+i];if(index>=sourceVertices.size())Fail("spectacle vertex index");
            const auto& p=sourceVertices[index].position;
            lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
            hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
        }
        for(uint32_t i=0;i<src.indexCount;i+=3){
            auto a=sourceVertices[source.indices[src.firstIndex+i]].position;
            auto b=sourceVertices[source.indices[src.firstIndex+i+1]].position;
            auto c=sourceVertices[source.indices[src.firstIndex+i+2]].position;
            const auto center=Scale(Add(Add(a,b),c),1.f/3.f);
            order.push_back({src.firstIndex+i,SpatialKey(center,lo,hi),
                territorial?int64_t(std::floor(double(center.x)/TerritorialTileMeters)):0,
                territorial?int64_t(std::floor(double(center.z)/TerritorialTileMeters)):0});
        }
        std::stable_sort(order.begin(),order.end(),[](const auto& a,const auto& b){
            if(a.tileZ!=b.tileZ)return a.tileZ<b.tileZ;
            if(a.tileX!=b.tileX)return a.tileX<b.tileX;
            return a.key<b.key;
        });
        for(size_t begin=0;begin<order.size();begin+=TrianglesPerRegion){
            const size_t end=std::min(order.size(),begin+TrianglesPerRegion);
            MeshRange range{};
            range.firstVertex=packTerritory?uint32_t(result.vertices.size()):src.firstVertex;
            range.vertexCount=packTerritory?0u:src.vertexCount;
            range.firstIndex=uint32_t(result.indices.size());range.firstMeshlet=uint32_t(result.meshlets.size());
            const uint32_t outputMesh=uint32_t(result.ranges.size());
            std::unordered_map<uint32_t,uint32_t> packedVertices;
            if(packTerritory)packedVertices.reserve((end-begin)*3u);
            TerrainTileSpan span{};
            auto beginSpan=[&](const OrderedTriangle& t){
                span={};span.tileX=t.tileX;span.tileZ=t.tileZ;span.mesh=outputMesh;
                span.firstVertex=uint32_t(result.vertices.size());
                span.firstIndex=uint32_t(result.indices.size());
                span.firstMeshlet=uint32_t(result.meshlets.size());
                span.firstRemap=uint32_t(result.remap.size());
                span.firstTriangle=uint32_t(result.triangles.size());
            };
            auto endSpan=[&]{
                span.vertexCount=uint32_t(result.vertices.size())-span.firstVertex;
                span.indexCount=uint32_t(result.indices.size())-span.firstIndex;
                span.meshletCount=uint32_t(result.meshlets.size())-span.firstMeshlet;
                span.remapCount=uint32_t(result.remap.size())-span.firstRemap;
                span.triangleCount=uint32_t(result.triangles.size())-span.firstTriangle;
                if(span.indexCount)scene.terrainSpans.push_back(span);
            };
            if(territorial)beginSpan(order[begin]);
            XMFLOAT3 min{FLT_MAX,FLT_MAX,FLT_MAX},max{-FLT_MAX,-FLT_MAX,-FLT_MAX};
            std::array<uint32_t,64> remap{};uint32_t vertices=0,triangles=0;
            Meshlet current{uint32_t(result.remap.size()),uint32_t(result.triangles.size()),0,0};
            auto flush=[&]{if(!triangles)return;current.vertexCount=vertices;current.triangleCount=triangles;result.meshlets.push_back(current);result.remap.insert(result.remap.end(),remap.begin(),remap.begin()+vertices);vertices=triangles=0;current={uint32_t(result.remap.size()),uint32_t(result.triangles.size()),0,0};};
            for(size_t k=begin;k<end;k++){
                // Meshlet ownership follows territory, but geometry regions
                // and BLAS retain their independent 512-triangle packing.
                if(territorial&&k>begin&&(order[k].tileX!=order[k-1].tileX||order[k].tileZ!=order[k-1].tileZ)){
                    flush();endSpan();packedVertices.clear();beginSpan(order[k]);
                }
                uint32_t ids[3]={source.indices[order[k].first],source.indices[order[k].first+1],source.indices[order[k].first+2]};
                if(packTerritory)for(auto& id:ids){
                    const auto [found,added]=packedVertices.try_emplace(id,uint32_t(result.vertices.size()));
                    if(added){
                        result.vertices.push_back(sourceVertices[id]);
                        if(!source.skin.empty())result.skin.push_back(source.skin[id]);
                    }
                    id=found->second;
                }
                if(trackSolids)triangleSolids.push_back(scene.triangleSolids[order[k].first/3u]);
                uint32_t needed=0;for(auto id:ids)if(std::find(remap.begin(),remap.begin()+vertices,id)==remap.begin()+vertices)++needed;
                if(vertices+needed>64||triangles==124)flush();
                std::array<uint32_t,3> local{};
                for(uint32_t j=0;j<3;j++){
                    const uint32_t id=ids[j];auto found=std::find(remap.begin(),remap.begin()+vertices,id);
                    if(found==remap.begin()+vertices)remap[vertices++]=id;
                    local[j]=uint32_t(found-remap.begin());result.indices.push_back(id);
                    const auto& p=result.vertices[id].position;
                    min={std::min(min.x,p.x),std::min(min.y,p.y),std::min(min.z,p.z)};
                    max={std::max(max.x,p.x),std::max(max.y,p.y),std::max(max.z,p.z)};
                }
                result.triangles.push_back(local);++triangles;
            }
            flush();if(territorial)endSpan();
            if(packTerritory)range.vertexCount=uint32_t(result.vertices.size())-range.firstVertex;
            range.indexCount=uint32_t(result.indices.size())-range.firstIndex;
            range.meshletCount=uint32_t(result.meshlets.size())-range.firstMeshlet;
            auto center=Scale(Add(min,max),.5f);float radius=0;
            for(uint32_t i=range.firstIndex;i<range.firstIndex+range.indexCount;i++)radius=std::max(radius,Length(Sub(result.vertices[result.indices[i]].position,center)));
            range.bounds={center.x,center.y,center.z,radius};
            RenderObject part=object;part.mesh=uint32_t(result.ranges.size());part.id=uint32_t(objects.size());
            result.ranges.push_back(range);result.deformations.push_back(source.deformations[object.mesh]);result.motions.push_back(source.motions[object.mesh]);objects.push_back(part);
            if(mg2Source)remappedForestParts.push_back({part.mesh,part.material});
        }
    }
    if(!scene.sphereObjectOffsets.empty())scene.sphereObjectOffsets.back()=uint32_t(objects.size());
    if(trackSolids)scene.triangleSolids=std::move(triangleSolids);
    scene.meshes=std::move(result);scene.objects=std::move(objects);scene.writableStaticGeometry=true;
    scene.mg2ForestParts=std::move(remappedForestParts);
    constexpr std::array<XMFLOAT4,2> wallColors{{{.084f,.074f,.062f,.84f},
                                                  {.17f,.115f,.068f,.96f}}};
    for(size_t i=0;i<wallColors.size();++i){
        scene.laserWallMaterials[i]=uint32_t(scene.materials.size());
        AssetMaterial wall{};
        wall.colorRoughness=wallColors[i];
        wall.doubleSided=1u;
        ApplyPhysicalMaterial(wall,i?"mineral.rock_generic":"metal.alloy_matte");
        scene.materials.push_back(wall);
        scene.materialNames.emplace_back(i?"Laser wall stone":"Laser wall metal");
    }
}
}
