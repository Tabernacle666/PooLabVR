#pragma once
#include "BuildingCampus.h"

namespace OASIS::TargetCalibration {
inline constexpr float U=SpectacleInteriorUnitsPerMeter;
inline constexpr std::array<int,7> Distances{2,5,10,20,40,80,160};
// The O rooftop has a full upper floor. Start clear of the central pad,
// facing +X along the roof, parallel to the campus frontage.
inline XMFLOAT3 Origin(const BuildingSession& building){
    const auto& d=building.Definition();
    return building.Scene({8.f/d.xScale,d.Elevation(d.floors),2.25f});
}
inline XMFLOAT4 Ruler(const BuildingSession& building){
    const auto p=Origin(building);
    return {p.x,p.y+.008f*U,p.z,U};
}
inline Pose Spawn(const BuildingSession& building){
    const auto eye=Add(Origin(building),{0.f,1.52f*U,0.f});
    return SolarLookAt(eye,Add(eye,{U,0.f,0.f}));
}
inline XMFLOAT3 Parking(const BuildingSession& building){
    const auto& d=building.Definition();
    return building.Scene({d.width*.5f,d.Elevation(d.floors)+.105f+.925f,d.depth*.53f});
}
// Native scene geometry, not HUD text: resolution scaling and raster
// visibility are the same as for other world objects.
inline uint32_t LetterMesh(SceneData& scene){
    constexpr const char* glyphs[5][7]={
        {"10001","10001","10001","11111","10001","10001","10001"},
        {"11111","10000","10000","11110","10000","10000","11111"},
        {"10000","10000","10000","10000","10000","10000","11111"},
        {"10000","10000","10000","10000","10000","10000","11111"},
        {"01110","10001","10001","10001","10001","10001","01110"}
    };
    auto& mesh=scene.meshes;
    MeshRange range{};
    range.firstVertex=uint32_t(mesh.vertices.size());
    range.firstIndex=uint32_t(mesh.indices.size());
    range.firstMeshlet=uint32_t(mesh.meshlets.size());
    constexpr float cell=.045f,z=-.018f;
    for(unsigned letter=0;letter<5;++letter)for(unsigned y=0;y<7;++y)for(unsigned x=0;x<5;++x){
        if(glyphs[letter][y][x]!='1')continue;
        const float l=(float(letter*6+x)-14.5f)*cell,r=l+cell;
        const float t=(3.5f-float(y))*cell,b=t-cell;
        const uint32_t i=uint32_t(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),{{{l,b,z},{0,0,-1},{0,0}},{{l,t,z},{0,0,-1},{0,1}},
                                                  {{r,t,z},{0,0,-1},{1,1}},{{r,b,z},{0,0,-1},{1,0}}});
        mesh.indices.insert(mesh.indices.end(),{i,i+1,i+2,i,i+2,i+3});
    }
    FinishRange(mesh,range);
    const auto index=uint32_t(mesh.ranges.size());
    mesh.ranges.push_back(range);mesh.deformations.emplace_back();mesh.motions.emplace_back();
    mesh.skin.resize(mesh.vertices.size());
    return index;
}
inline void Install(SceneData& scene,WorldRegistry& world,const BuildingSession& building){
    const auto box=AppendBuildingBox(scene),letters=LetterMesh(scene);
    if(!scene.triangleSolids.empty())scene.triangleSolids.resize(scene.meshes.indices.size()/3u,Invalid);
    auto material=[&](const char* name,float value){
        AssetMaterial m{};
        m.colorRoughness={value,value,value,1.f};m.unlit=1u;m.doubleSided=1u;
        const auto index=uint32_t(scene.materials.size());
        scene.materials.push_back(m);scene.materialNames.emplace_back(name);
        return index;
    };
    const auto black=material("Target board",.018f),white=material("Target HELLO",1.f);
    const auto origin=Origin(building);
    std::vector<WorldObject> objects;objects.reserve(Distances.size()*2u);
    for(const int distance:Distances){
        WorldObject panel{};
        panel.current.p=Add(origin,{float(distance)*U,.55f*U,0.f});
        XMStoreFloat4(&panel.current.q,XMQuaternionRotationRollPitchYaw(0.f,XM_PIDIV2,0.f));
        panel.previous=panel.current;
        panel.mesh=box;panel.material=black;panel.scale={1.5f*U,.6f*U,.03f*U};
        panel.flags=NoShadow|NoRayTracing|NoCollision;
        panel.boundRadius=scene.meshes.ranges[box].bounds.w;
        objects.push_back(panel);
        panel.mesh=letters;panel.material=white;panel.scale={U,U,U};
        panel.boundRadius=scene.meshes.ranges[letters].bounds.w;
        objects.push_back(panel);
    }
    world.CreateBatch(objects);
}
}
