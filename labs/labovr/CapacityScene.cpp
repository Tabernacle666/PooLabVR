#include "CapacityScene.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace lab {
namespace {
struct MeshBuilder {
    MeshData& mesh;
    MeshRange range{};
    XMFLOAT3 low{1e30f,1e30f,1e30f},high{-1e30f,-1e30f,-1e30f};
    explicit MeshBuilder(MeshData& m):mesh(m) {
        range.firstVertex=uint32_t(m.vertices.size());
        range.firstIndex=uint32_t(m.indices.size());
        range.firstMeshlet=uint32_t(m.meshlets.size());
    }
    void Triangle(XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c) {
        if(mesh.meshlets.size()==range.firstMeshlet||
           mesh.meshlets.back().vertexCount+3>64||
           mesh.meshlets.back().triangleCount>=124)
            mesh.meshlets.push_back({uint32_t(mesh.remap.size()),
                                      uint32_t(mesh.triangles.size()),0,0});
        auto& ml=mesh.meshlets.back();
        const uint32_t local=ml.vertexCount;
        const auto edge1=Sub(b,a),edge2=Sub(c,a);
        XMFLOAT3 normal{};
        XMStoreFloat3(&normal,XMVector3Cross(XMLoadFloat3(&edge1),XMLoadFloat3(&edge2)));
        normal=Unit(normal);
        for(auto p:{a,b,c}) {
            low={std::min(low.x,p.x),std::min(low.y,p.y),std::min(low.z,p.z)};
            high={std::max(high.x,p.x),std::max(high.y,p.y),std::max(high.z,p.z)};
            const uint32_t index=uint32_t(mesh.vertices.size());
            const XMFLOAT2 uv{p.x*.5f+.5f,p.z*.5f+.5f};
            mesh.vertices.push_back({p,normal,uv,uv});
            mesh.indices.push_back(index);
            mesh.remap.push_back(index);
            mesh.skin.push_back({});
        }
        mesh.triangles.push_back({local,local+1,local+2});
        ml.vertexCount+=3;ml.triangleCount+=1;
    }
    void Quad(XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,XMFLOAT3 d) {
        Triangle(a,b,c);Triangle(a,c,d);
    }
    void Finish() {
        range.vertexCount=uint32_t(mesh.vertices.size())-range.firstVertex;
        range.indexCount=uint32_t(mesh.indices.size())-range.firstIndex;
        range.meshletCount=uint32_t(mesh.meshlets.size())-range.firstMeshlet;
        const XMFLOAT3 middle=Scale(Add(low,high),.5f);
        float radius=0.f;
        for(size_t i=range.firstVertex;i<mesh.vertices.size();++i)
            radius=std::max(radius,Length(Sub(mesh.vertices[i].position,middle)));
        range.bounds={middle.x,middle.y,middle.z,radius};
        mesh.ranges.push_back(range);
        mesh.deformations.push_back({});
        mesh.motions.push_back({});
    }
};
void Box(MeshData& mesh) {
    MeshBuilder b(mesh);
    constexpr std::array<XMFLOAT3,8> c={{
        {-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,.5f,-.5f},{-.5f,.5f,-.5f},
        {-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f}
    }};
    constexpr uint32_t faces[6][4]={
        {0,3,2,1},{4,5,6,7},{0,4,7,3},
        {1,2,6,5},{0,1,5,4},{3,7,6,2}
    };
    for(const auto& f:faces)b.Quad(c[f[0]],c[f[1]],c[f[2]],c[f[3]]);
    b.Finish();
}
void Canopy(MeshData& mesh) {
    MeshBuilder b(mesh);
    const XMFLOAT3 t{0,1,0},a{-.75f,0,-.75f},
                   c{.75f,0,.75f},d{-.75f,0,.75f},e{.75f,0,-.75f};
    b.Triangle(a,t,e);b.Triangle(e,t,c);b.Triangle(c,t,d);b.Triangle(d,t,a);
    b.Quad(a,d,c,e);b.Finish();
}
void Stone(MeshData& mesh) {
    MeshBuilder b(mesh);
    const XMFLOAT3 top{0,.8f,0},bottom{0,-.8f,0},
                   a{1,0,0},c{-1,0,0},d{0,0,1},e{0,0,-1};
    for(const auto& pair:std::array<std::array<XMFLOAT3,2>,4>{{
        {{a,d}},{{d,c}},{{c,e}},{{e,a}}
    }}){
        b.Triangle(pair[0],top,pair[1]);
        b.Triangle(pair[1],bottom,pair[0]);
    }
    b.Finish();
}
RenderObject Instance(uint32_t mesh,uint32_t material,
                      XMFLOAT3 position,XMFLOAT3 scale){
    RenderObject o{};
    o.mesh=mesh;o.material=material;o.scale=scale;
    o.current.p=position;o.previous=o.current;
    return o;
}
}
SceneData CreateCapacityScene(){
    SceneData scene;
    Box(scene.meshes);Canopy(scene.meshes);Stone(scene.meshes);
    AssetMaterial bark{};bark.colorRoughness={.22f,.13f,.055f,.92f};
    AssetMaterial foliage{};foliage.colorRoughness={.05f,.29f,.09f,.85f};
    AssetMaterial rock{};rock.colorRoughness={.40f,.43f,.45f,.92f};
    AssetMaterial ground{};ground.colorRoughness={.14f,.19f,.16f,.96f};
    scene.materials={bark,foliage,rock,ground};
    // Repeated prototype cluster: trunk + canopy + stone.
    scene.objects.push_back(Instance(0,0,{0,1.1f,0},{.24f,2.2f,.24f}));
    scene.objects.push_back(Instance(1,1,{0,2.3f,0},{1.1f,2.f,1.1f}));
    scene.objects.push_back(Instance(2,2,{1.2f,.35f,.3f},{.48f,.32f,.55f}));
    // Last prototype is extracted as a single static ground at launch.
    scene.objects.push_back(Instance(0,3,{0,-.22f,-46.f},{86.f,.4f,96.f}));
    return scene;
}
}
