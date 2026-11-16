#include "Destruction.h"
#include "VrInputMapping.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace OASIS {
namespace {
XMFLOAT3 Cross(XMFLOAT3 a,XMFLOAT3 b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
Vertex Interpolate(const Vertex& a,const Vertex& b,float t) {
    Vertex v{};
    v.position=Add(a.position,Scale(Sub(b.position,a.position),t));
    v.normal=Unit(Add(Scale(a.normal,1-t),Scale(b.normal,t)));
    v.uv={a.uv.x+(b.uv.x-a.uv.x)*t,a.uv.y+(b.uv.y-a.uv.y)*t};
    v.uv1={a.uv1.x+(b.uv1.x-a.uv1.x)*t,a.uv1.y+(b.uv1.y-a.uv1.y)*t};
    return v;
}
bool RayTriangle(XMFLOAT3 origin,XMFLOAT3 direction,const Vertex* v,float& distance) {
    auto a=v[0].position,b=v[1].position,c=v[2].position;
    auto e1=Sub(b,a),e2=Sub(c,a),p=Cross(direction,e2);
    float det=Dot(e1,p);
    if(std::abs(det)<1.e-8f)return false;
    float inv=1.f/det;
    auto t=Sub(origin,a);
    float u=Dot(t,p)*inv;
    if(u<0.f||u>1.f)return false;
    auto q=Cross(t,e1);
    float w=Dot(direction,q)*inv;
    if(w<0.f||u+w>1.f)return false;
    distance=Dot(e2,q)*inv;
    return distance>0.0001f;
}
struct SpherePoly {
    std::vector<XMFLOAT3> points;
    std::vector<std::array<uint32_t,3>> faces;
};
const SpherePoly& SphereShape() {
    static const SpherePoly shape=[] {
        SpherePoly s;
        constexpr float t=1.61803398875f;
        for(auto p:std::array<XMFLOAT3,12>{{
            {-1,t,0},{1,t,0},{-1,-t,0},{1,-t,0},
            {0,-1,t},{0,1,t},{0,-1,-t},{0,1,-t},
            {t,0,-1},{t,0,1},{-t,0,-1},{-t,0,1}
        }})s.points.push_back(Unit(p));
        s.faces={{
            {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
            {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
            {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
            {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
        }};
        std::unordered_map<uint64_t,uint32_t> edges;
        auto middle=[&](uint32_t a,uint32_t b) {
            uint64_t key=(uint64_t(std::min(a,b))<<32)|std::max(a,b);
            auto found=edges.find(key);
            if(found!=edges.end())return found->second;
            uint32_t index=uint32_t(s.points.size());
            s.points.push_back(Unit(Scale(Add(s.points[a],s.points[b]),.5f)));
            edges.emplace(key,index);
            return index;
        };
        std::vector<std::array<uint32_t,3>> refined;
        refined.reserve(80);
        for(auto f:s.faces){
            auto ab=middle(f[0],f[1]),bc=middle(f[1],f[2]),ca=middle(f[2],f[0]);
            refined.push_back({f[0],ab,ca});
            refined.push_back({f[1],bc,ab});
            refined.push_back({f[2],ca,bc});
            refined.push_back({ab,bc,ca});
        }
        s.faces=std::move(refined);
        for(auto& f:s.faces){
            auto n=Cross(Sub(s.points[f[1]],s.points[f[0]]),Sub(s.points[f[2]],s.points[f[0]]));
            if(Dot(n,s.points[f[0]])<0)std::swap(f[1],f[2]);
        }
        return s;
    }();
    return shape;
}
Vertex SurfaceVertex(XMFLOAT3 p,XMFLOAT3 n) {
    Vertex v{};v.position=p;v.normal=Unit(n);v.uv={p.x*.32f,p.z*.32f};return v;
}
Pose SegmentPose(XMFLOAT3 a,XMFLOAT3 b) {
    Pose result{};result.p=a;
    auto forward=Unit(Sub(b,a));
    const float cosine=std::clamp(forward.z,-1.f,1.f);
    XMVECTOR rotation;
    if(cosine>0.9999f)rotation=XMQuaternionIdentity();
    else if(cosine<-0.9999f)rotation=XMQuaternionRotationAxis(XMVectorSet(0,1,0,0),XM_PI);
    else {auto axis=Unit(Cross({0,0,1},forward));
        rotation=XMQuaternionRotationAxis(XMLoadFloat3(&axis),std::acos(cosine));}
    XMStoreFloat4(&result.q,rotation);
    return result;
}
}

DestructibleScene::DestructibleScene(SceneData& scene):scene_(&scene) {
    const MeshData input=scene.meshes;
    const auto sourceObjects=scene.objects;
    parts_.reserve(sourceObjects.size()+1);
    for(const auto& source:sourceObjects){
        if(source.mesh>=input.ranges.size()||source.material>=scene.materials.size())
            Fail("Destruction input mesh/material out of range");
        Part part{};part.object=source;part.object.current={};part.object.previous={};
        part.object.scale={1,1,1};
        auto world=XMMatrixScaling(source.scale.x,source.scale.y,source.scale.z)*Matrix(source.current);
        auto normalWorld=XMMatrixTranspose(XMMatrixInverse(nullptr,world));
        const auto& r=input.ranges[source.mesh];
        part.triangles.reserve(r.indexCount/3);
        for(uint32_t i=r.firstIndex;i<r.firstIndex+r.indexCount;i+=3){
            Triangle tri{};
            for(int j=0;j<3;j++){
                uint32_t index=input.indices[i+j];
                if(index>=input.vertices.size())Fail("Destruction source index out of range");
                tri.v[j]=input.vertices[index];
                XMStoreFloat3(&tri.v[j].position,XMVector3TransformCoord(XMLoadFloat3(&tri.v[j].position),world));
                XMStoreFloat3(&tri.v[j].normal,XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&tri.v[j].normal),normalWorld)));
            }
            part.triangles.push_back(tri);
        }
        parts_.push_back(std::move(part));
    }
    soilMaterial_=uint32_t(scene.materials.size());
    AssetMaterial soil{};soil.colorRoughness={.27f,.26f,.23f,.94f};soil.doubleSided=1;
    scene.materials.push_back(soil);
    beamMaterial_=uint32_t(scene.materials.size());
    AssetMaterial beam{};beam.colorRoughness={.1f,.75f,1.f,.24f};beam.emissionMetallic={.3f,1.5f,2.6f,0.f};beam.doubleSided=1;
    scene.materials.push_back(beam);
    ballMaterial_=uint32_t(scene.materials.size());
    AssetMaterial ball{};ball.colorRoughness={.9f,.64f,.25f,.34f};ball.emissionMetallic={.25f,.1f,.02f,.25f};
    scene.materials.push_back(ball);
    AddTerrain();
    Rebuild();
}

void DestructibleScene::AddTerrain() {
    Part part{};part.object.material=soilMaterial_;
    constexpr int cells=36;
    auto ground=[](int i,int j,bool lower){
        float x=-7.2f+14.4f*float(i)/36.f,z=-1.6f+16.f*float(j)/36.f;
        // Irregular lateral silhouette, continuous down to the underside.
        x+=.24f*std::sin(z*.73f)+.11f*std::sin(z*1.91f+x*.27f);
        z+=.19f*std::sin(x*.59f)+.08f*std::sin(x*1.63f+z*.47f);
        float y=-.48f+.14f*std::sin(x*.63f)*std::cos(z*.44f)
                     +.085f*std::sin(x*.19f+z*1.13f)+.045f*std::cos(x*1.8f-z*.6f);
        if(lower)y-=1.4f+.07f*std::cos(x*.4f+z*.7f);
        return XMFLOAT3{x,y,z};
    };
    auto triangle=[&](XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c) {
        auto n=Cross(Sub(b,a),Sub(c,a));
        if(Dot(n,n)<1.e-12f)return;
        Triangle t{};t.v[0]=SurfaceVertex(a,n);t.v[1]=SurfaceVertex(b,n);t.v[2]=SurfaceVertex(c,n);
        part.triangles.push_back(t);
    };
    for(int j=0;j<cells;j++)for(int i=0;i<cells;i++){
        auto a=ground(i,j,false),b=ground(i+1,j,false),c=ground(i,j+1,false),d=ground(i+1,j+1,false);
        triangle(a,c,b);triangle(b,c,d);
        a=ground(i,j,true);b=ground(i+1,j,true);c=ground(i,j+1,true);d=ground(i+1,j+1,true);
        triangle(a,b,c);triangle(b,d,c);
    }
    auto edge=[&](XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 aa,XMFLOAT3 bb) {
        triangle(a,b,aa);triangle(b,bb,aa);
    };
    for(int i=0;i<cells;i++){
        edge(ground(i,0,false),ground(i+1,0,false),ground(i,0,true),ground(i+1,0,true));
        edge(ground(i+1,cells,false),ground(i,cells,false),ground(i+1,cells,true),ground(i,cells,true));
    }
    for(int j=0;j<cells;j++){
        edge(ground(cells,j,false),ground(cells,j+1,false),ground(cells,j,true),ground(cells,j+1,true));
        edge(ground(0,j+1,false),ground(0,j,false),ground(0,j+1,true),ground(0,j,true));
    }
    parts_.push_back(std::move(part));
}

void DestructibleScene::Rebuild(){
    MeshData mesh{};
    scene_->objects.clear();
    auto emitRange=[&](const std::vector<Triangle>& source){
        uint32_t meshId=uint32_t(mesh.ranges.size());
        MeshRange range{};range.firstMeshlet=uint32_t(mesh.meshlets.size());
        range.firstVertex=uint32_t(mesh.vertices.size());range.firstIndex=uint32_t(mesh.indices.size());
        XMFLOAT3 lo{FLT_MAX,FLT_MAX,FLT_MAX},hi{-FLT_MAX,-FLT_MAX,-FLT_MAX};
        Meshlet current{uint32_t(mesh.remap.size()),uint32_t(mesh.triangles.size()),0,0};
        auto flush=[&](){
            if(!current.triangleCount)return;
            mesh.meshlets.push_back(current);
            current={uint32_t(mesh.remap.size()),uint32_t(mesh.triangles.size()),0,0};
        };
        for(const auto& triangle:source){
            if(current.vertexCount+3>63)flush();
            std::array<uint32_t,3> local{};
            for(uint32_t j=0;j<3;j++){
                uint32_t index=uint32_t(mesh.vertices.size());
                mesh.vertices.push_back(triangle.v[j]);
                mesh.indices.push_back(index);mesh.remap.push_back(index);
                mesh.skin.push_back({});
                local[j]=current.vertexCount++;
                const auto& p=triangle.v[j].position;
                lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
                hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
            }
            mesh.triangles.push_back(local);current.triangleCount++;
        }
        flush();
        range.vertexCount=uint32_t(mesh.vertices.size())-range.firstVertex;
        range.indexCount=uint32_t(mesh.indices.size())-range.firstIndex;
        range.meshletCount=uint32_t(mesh.meshlets.size())-range.firstMeshlet;
        if(range.vertexCount){
            auto center=Scale(Add(lo,hi),.5f);float radius=0;
            for(uint32_t i=range.firstVertex;i<mesh.vertices.size();i++)
                radius=std::max(radius,Length(Sub(mesh.vertices[i].position,center)));
            range.bounds={center.x,center.y,center.z,radius};
        }
        mesh.ranges.push_back(range);mesh.deformations.push_back({});mesh.motions.push_back({});
        return meshId;
    };
    for(auto& part:parts_){
        if(part.triangles.empty())continue;
        auto object=part.object;object.mesh=emitRange(part.triangles);
        scene_->objects.push_back(object);
    }
    // Interactive shapes are normal meshes but not part of the destructible static scene.
    std::vector<Triangle> beam;
    for(uint32_t i=0;i<12;i++){
        float a=float(i)*XM_2PI/12.f,b=float(i+1)*XM_2PI/12.f;
        XMFLOAT3 p{.0045f*std::cos(a),.0045f*std::sin(a),0},q{.0045f*std::cos(b),.0045f*std::sin(b),0};
        XMFLOAT3 r{p.x,p.y,1},s{q.x,q.y,1};
        Triangle t{};t.v[0]=SurfaceVertex(p,p);t.v[1]=SurfaceVertex(q,q);t.v[2]=SurfaceVertex(r,p);beam.push_back(t);
        t.v[0]=SurfaceVertex(q,q);t.v[1]=SurfaceVertex(s,s);t.v[2]=SurfaceVertex(r,r);beam.push_back(t);
    }
    beamMesh_=emitRange(beam);
    std::vector<Triangle> ball;const auto& sphere=SphereShape();
    for(auto f:sphere.faces){
        Triangle t{};
        for(uint32_t j=0;j<3;j++){
            auto n=sphere.points[f[j]];t.v[j]=SurfaceVertex(Scale(n,.09f),n);
        }
        ball.push_back(t);
    }
    ballMesh_=emitRange(ball);
    scene_->meshes=std::move(mesh);
}

bool DestructibleScene::Trace(XMFLOAT3 start,XMFLOAT3 direction,float range,Hit& hit)const{
    bool found=false;float best=range;
    for(const auto& part:parts_)for(const auto& triangle:part.triangles){
        float distance=0;
        if(!RayTriangle(start,direction,triangle.v,distance)||distance>=best)continue;
        best=distance;found=true;
        hit.distance=distance;hit.position=Add(start,Scale(direction,distance));
        auto n=Unit(Cross(Sub(triangle.v[1].position,triangle.v[0].position),
                          Sub(triangle.v[2].position,triangle.v[0].position)));
        hit.normal=Dot(n,direction)<0?n:Scale(n,-1.f);
    }
    return found;
}

bool DestructibleScene::Carve(XMFLOAT3 center,float radius){
    if(!(radius>0)||!std::isfinite(radius))return false;
    const auto& sphere=SphereShape();
    struct Plane {XMFLOAT3 n;float offset;};
    std::vector<Plane> planes;planes.reserve(sphere.faces.size());
    for(const auto& f:sphere.faces){
        auto a=sphere.points[f[0]],b=sphere.points[f[1]],c=sphere.points[f[2]];
        auto n=Unit(Cross(Sub(b,a),Sub(c,a)));
        planes.push_back({n,radius*Dot(n,a)});
    }
    auto insideSolid=[&](const Part& part,XMFLOAT3 p){
        const XMFLOAT3 direction{.80431f,.42117f,.41956f};
        uint32_t crossings=0;
        for(const auto& t:part.triangles){
            float distance=0;
            if(RayTriangle(p,direction,t.v,distance))++crossings;
        }
        return (crossings&1)!=0;
    };
    bool changed=false;
    for(size_t partIndex=0;partIndex<parts_.size();partIndex++){
        auto& part=parts_[partIndex];
        std::vector<Triangle> remaining;remaining.reserve(part.triangles.size()+256);
        bool affected=false;
        auto emit=[&](const std::vector<Vertex>& poly){
            if(poly.size()<3)return;
            for(size_t j=1;j+1<poly.size();j++){
                auto n=Cross(Sub(poly[j].position,poly[0].position),
                             Sub(poly[j+1].position,poly[0].position));
                if(Dot(n,n)<1.e-12f)continue;
                Triangle t{};t.v[0]=poly[0];t.v[1]=poly[j];t.v[2]=poly[j+1];
                remaining.push_back(t);
            }
        };
        for(const auto& tri:part.triangles){
            bool intersectsBounds=true;
            for(int axis=0;axis<3;axis++){
                float lo=FLT_MAX,hi=-FLT_MAX;
                for(const auto& v:tri.v){
                    const float* value=&v.position.x;
                    lo=std::min(lo,value[axis]);hi=std::max(hi,value[axis]);
                }
                const float* p=&center.x;
                if(hi<p[axis]-radius||lo>p[axis]+radius){intersectsBounds=false;break;}
            }
            if(!intersectsBounds){remaining.push_back(tri);continue;}
            std::vector<Vertex> inner{tri.v[0],tri.v[1],tri.v[2]};
            for(const auto& plane:planes){
                if(inner.empty())break;
                std::vector<Vertex> next,outside;
                for(size_t i=0;i<inner.size();i++){
                    const Vertex& a=inner[i];const Vertex& b=inner[(i+1)%inner.size()];
                    float da=Dot(plane.n,Sub(a.position,center))-plane.offset;
                    float db=Dot(plane.n,Sub(b.position,center))-plane.offset;
                    bool ina=da<=0.f,inb=db<=0.f;
                    if(ina)next.push_back(a);else outside.push_back(a);
                    if(ina!=inb){
                        auto point=Interpolate(a,b,std::clamp(da/(da-db),0.f,1.f));
                        next.push_back(point);outside.push_back(point);
                    }
                }
                emit(outside);
                inner=std::move(next);
            }
            if(inner.size()>=3){
                auto n=Cross(Sub(inner[1].position,inner[0].position),
                             Sub(inner[2].position,inner[0].position));
                affected |= Dot(n,n)>1.e-12f;
            }
        }
        // A carved closed ground volume exposes a real triangular cavity wall.
        // No voxels, rigid bodies or fragments: just the retained boundary mesh.
        if(partIndex+1==parts_.size() && affected){
            auto addWall=[&](XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c){
                Triangle t{};
                auto n=Unit(Cross(Sub(b,a),Sub(c,a)));
                t.v[0]=SurfaceVertex(a,n);t.v[1]=SurfaceVertex(b,n);t.v[2]=SurfaceVertex(c,n);
                remaining.push_back(t);
            };
            auto patch=[&](auto&& self,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,int depth)->void {
                bool ia=insideSolid(part,a),ib=insideSolid(part,b),ic=insideSolid(part,c);
                if(depth>0&&(ia!=ib||ia!=ic)){
                    auto ab=Scale(Add(a,b),.5f),bc=Scale(Add(b,c),.5f),ca=Scale(Add(c,a),.5f);
                    self(self,a,ab,ca,depth-1);self(self,ab,b,bc,depth-1);
                    self(self,ca,bc,c,depth-1);self(self,ab,bc,ca,depth-1);
                }else if(insideSolid(part,Scale(Add(Add(a,b),c),1.f/3.f))){
                    addWall(a,c,b); // normals into the excavated sphere
                }
            };
            for(const auto& f:sphere.faces){
                auto a=Add(center,Scale(sphere.points[f[0]],radius));
                auto b=Add(center,Scale(sphere.points[f[1]],radius));
                auto c=Add(center,Scale(sphere.points[f[2]],radius));
                patch(patch,a,b,c,2);
            }
        }
        if(affected){part.triangles=std::move(remaining);changed=true;}
    }
    if(changed)Rebuild();
    return changed;
}

DemolitionControls::DemolitionControls(DestructibleScene& geometry,WorldRegistry& world)
    :geometry_(geometry),world_(world){
    beam_.mesh=geometry_.BeamMesh();beam_.material=geometry_.BeamMaterial();
    beam_.flags=NoShadow|NoRayTracing|NoCollision;beam_.boundRadius=1;beam_.enabled=false;
    ball_.mesh=geometry_.BallMesh();ball_.material=geometry_.BallMaterial();
    ball_.flags=NoShadow|NoRayTracing|NoCollision;ball_.boundRadius=.1f;ball_.enabled=false;
    beamId_=world_.Create(beam_);ballId_=world_.Create(ball_);
}
bool DemolitionControls::Tick(const FrameInput& input,bool vr){
    double dt=lastTime_?std::clamp(input.time-lastTime_,0.,.05):0.;
    lastTime_=input.time;
    const auto& actions=StandardVrActions();
    const auto& left=input.PoseState(actions.aimPose,0);
    const auto& right=input.PoseState(actions.aimPose,1);
    const bool leftValid=!vr||left.tracked,rightValid=!vr||right.tracked;
    Pose leftAim=vr?left.pose:input.head,rightAim=vr?right.pose:input.head;
    if(!vr){
        leftAim.p=Transform({-.16f,-.12f,.14f},input.head);
        rightAim.p=Transform({.16f,-.12f,.14f},input.head);
    }
    auto forward=[](const Pose& pose){
        return Unit(Sub(Transform({0,0,1},pose),pose.p));
    };
    beam_.enabled=leftValid;
    if(leftValid){
        auto direction=forward(leftAim);
        DestructibleScene::Hit hit{};
        float length=geometry_.Trace(leftAim.p,direction,25.f,hit)?hit.distance:25.f;
        beam_.previous=beam_.current;
        beam_.current=SegmentPose(leftAim.p,Add(leftAim.p,Scale(direction,length)));
        beam_.scale={1,1,length};
    }
    bool carved=false;
    if(!flying_&&rightValid&&input.Pressed(actions.secondary,1)){
        position_=rightAim.p;
        velocity_=Add(Scale(forward(rightAim),4.3f),XMFLOAT3{0,2.0f,0});
        flightTime_=0;flying_=true;
    }
    if(flying_){
        auto old=position_;
        velocity_.y-=3.6f*float(dt);
        position_=Add(position_,Scale(velocity_,float(dt)));
        flightTime_+=dt;
        const auto step=Sub(position_,old);
        const float distance=Length(step);
        DestructibleScene::Hit hit{};
        if(distance>0.0001f&&geometry_.Trace(old,Scale(step,1.f/distance),distance,hit)){
            flying_=false;
            carved=geometry_.Carve(hit.position,.48f);
        }else if(flightTime_>7.0 || Length(Sub(position_,input.head.p))>30.f){
            flying_=false;
        }
    }
    beam_.mesh=geometry_.BeamMesh();ball_.mesh=geometry_.BallMesh();
    ball_.enabled=flying_;ball_.previous=ball_.current;ball_.current.p=position_;
    world_.Update(beamId_,beam_);world_.Update(ballId_,ball_);
    return carved;
}
}
