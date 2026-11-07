#include "VisualAssets.h"
#include "ConsoleSpec.h"
#include <fstream>
#include <sstream>
#include <string_view>
#include <cfloat>
namespace lab {
namespace {
struct V3{float x{},y{},z{};};
struct V2{float x{},y{};};
V3 operator-(V3 a,V3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
V3 operator+(V3 a,V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
V3 operator*(V3 a,float s){return {a.x*s,a.y*s,a.z*s};}
float Dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
V3 Cross(V3 a,V3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
V3 Normalize(V3 v){float l=std::sqrt(std::max(1e-20f,Dot(v,v)));return v*(1.0f/l);}
V3 QuatRotate(V3 v,const float q[4]){V3 u{q[0],q[1],q[2]};V3 t=Cross(u,v)*2.0f;return v+t*q[3]+Cross(u,t);}
std::string Narrow(const std::filesystem::path& p){auto u=p.u8string();return std::string(reinterpret_cast<const char*>(u.data()),u.size());}
struct FaceIndex{int p{},t{},n{};};
FaceIndex ParseFace(std::string_view token){FaceIndex r{};size_t a=token.find('/'),b=a==std::string_view::npos?std::string_view::npos:token.find('/',a+1);auto parse=[](std::string_view s)->int{if(s.empty())return 0;int sign=1,v=0;size_t i=0;if(s[0]=='-'){sign=-1;i=1;}for(;i<s.size()&&s[i]>='0'&&s[i]<='9';++i)v=v*10+(s[i]-'0');return v*sign;};if(a==std::string_view::npos){r.p=parse(token);return r;}r.p=parse(token.substr(0,a));if(b==std::string_view::npos){r.t=parse(token.substr(a+1));return r;}r.t=parse(token.substr(a+1,b-a-1));r.n=parse(token.substr(b+1));return r;}
int ResolveIndex(int idx,size_t size){if(idx>0)return idx-1;if(idx<0)return int(size)+idx;return -1;}
bool LoadObjFile(const std::filesystem::path& path,std::vector<VisualMeshVertex>& vertices,std::vector<uint32_t>& indices,std::string& error){
    std::ifstream f(path);if(!f){error="Missing visual mesh: "+Narrow(path);return false;}std::vector<V3> pos,nrm;std::vector<V2> uv;std::string line;
    while(std::getline(f,line)){
        if(line.size()<2)continue;std::istringstream in(line);std::string tag;in>>tag;
        if(tag=="v"){V3 v;in>>v.x>>v.y>>v.z;pos.push_back(v);}else if(tag=="vn"){V3 v;in>>v.x>>v.y>>v.z;nrm.push_back(v);}else if(tag=="vt"){V2 v;in>>v.x>>v.y;uv.push_back(v);}else if(tag=="f"){
            std::vector<FaceIndex> face;std::string token;while(in>>token)face.push_back(ParseFace(token));if(face.size()<3)continue;
            for(size_t tri=1;tri+1<face.size();++tri){FaceIndex fi[3]={face[0],face[tri],face[tri+1]};uint32_t base=uint32_t(vertices.size());V3 p3[3]{};V2 t3[3]{};V3 n3[3]{};bool normals=true;
                for(int k=0;k<3;k++){int pi=ResolveIndex(fi[k].p,pos.size()),ti=ResolveIndex(fi[k].t,uv.size()),ni=ResolveIndex(fi[k].n,nrm.size());if(pi<0||size_t(pi)>=pos.size()){error="Invalid OBJ position index in "+Narrow(path);return false;}p3[k]=pos[size_t(pi)];if(ti>=0&&size_t(ti)<uv.size())t3[k]=uv[size_t(ti)];if(ni>=0&&size_t(ni)<nrm.size())n3[k]=nrm[size_t(ni)];else normals=false;}
                V3 e1=p3[1]-p3[0],e2=p3[2]-p3[0];V3 faceN=Normalize(Cross(e1,e2));float du1=t3[1].x-t3[0].x,dv1=t3[1].y-t3[0].y,du2=t3[2].x-t3[0].x,dv2=t3[2].y-t3[0].y;float den=du1*dv2-du2*dv1;V3 tangent=std::abs(den)>1e-7f?Normalize((e1*dv2-e2*dv1)*(1.0f/den)):Normalize(e1);
                for(int k=0;k<3;k++){V3 nn=normals?Normalize(n3[k]):faceN;vertices.push_back({p3[k].x,p3[k].y,p3[k].z,nn.x,nn.y,nn.z,tangent.x,tangent.y,tangent.z,t3[k].x,1.0f-t3[k].y});indices.push_back(base+uint32_t(k));}
            }
        }
    }
    if(vertices.empty()||indices.empty()){error="HERO mesh contains no triangles: "+Narrow(path);return false;}return true;
}

static void FinishRange(MeshData& mesh,MeshRange& range) {
    range.vertexCount=uint32_t(mesh.vertices.size())-range.firstVertex;
    range.indexCount=uint32_t(mesh.indices.size())-range.firstIndex;
    if(!range.vertexCount || !range.indexCount) Fail("empty glTF primitive");
    XMFLOAT3 lo{FLT_MAX,FLT_MAX,FLT_MAX},hi{-FLT_MAX,-FLT_MAX,-FLT_MAX};
    for(uint32_t i=range.firstVertex;i<range.firstVertex+range.vertexCount;i++) {
        const auto& p=mesh.vertices[i].position;
        lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
        hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
    }
    auto center=Scale(Add(lo,hi),.5f); float radius=0;
    for(uint32_t i=range.firstVertex;i<range.firstVertex+range.vertexCount;i++) radius=std::max(radius,Length(Sub(mesh.vertices[i].position,center)));
    range.bounds={center.x,center.y,center.z,radius};
    std::array<uint32_t,64> remap{}; uint32_t vertices=0,triangles=0;
    Meshlet current{uint32_t(mesh.remap.size()),uint32_t(mesh.triangles.size()),0,0};
    auto flush=[&] {
        if(!triangles) return;
        current.vertexCount=vertices; current.triangleCount=triangles; mesh.meshlets.push_back(current);
        mesh.remap.insert(mesh.remap.end(),remap.begin(),remap.begin()+vertices); vertices=triangles=0;
        current={uint32_t(mesh.remap.size()),uint32_t(mesh.triangles.size()),0,0};
    };
    for(uint32_t t=range.firstIndex;t<range.firstIndex+range.indexCount;t+=3) {
        uint32_t needed=0;
        for(uint32_t j=0;j<3;j++) if(std::find(remap.begin(),remap.begin()+vertices,mesh.indices[t+j])==remap.begin()+vertices) needed++;
        if(vertices+needed>64 || triangles==124) flush();
        std::array<uint32_t,3> local{};
        for(uint32_t j=0;j<3;j++) {
            uint32_t index=mesh.indices[t+j]; auto found=std::find(remap.begin(),remap.begin()+vertices,index);
            if(found==remap.begin()+vertices) remap[vertices++]=index;
            local[j]=uint32_t(found-remap.begin());
        }
        mesh.triangles.push_back(local); triangles++;
    }
    flush(); range.meshletCount=uint32_t(mesh.meshlets.size())-range.firstMeshlet;
}

}
std::filesystem::path ResolveVisualAsset(const std::filesystem::path& root,const char* path) {
    if(!path||!*path)Fail("Missing visual asset path");
    auto relative=std::filesystem::u8path(path);
    if(relative.is_absolute()||relative.has_root_name())Fail("Visual assets must be relative to package root");
    auto base=std::filesystem::weakly_canonical(root),full=std::filesystem::weakly_canonical(base/relative);
    auto local=full.lexically_relative(base);
    if(local.empty()||local.is_absolute())Fail("Invalid visual asset path");
    for(const auto& component:local)if(component=="..")Fail("Visual asset escapes package root");
    return full;
}
VisualMeshData LoadVisualObj(const std::filesystem::path& path) {
    VisualMeshData out;std::string error;
    if(!LoadObjFile(path,out.vertices,out.indices,error))Fail(error.c_str());
    return out;
}
TextureData LoadVisualImage(const std::filesystem::path& path,LabU32 use) {
    TextureUse mappedUse{};
    switch(use){
    case LABOVR_VISUAL_TEXTURE_COLOR_V1:mappedUse=TextureUse::Color;break;
    case LABOVR_VISUAL_TEXTURE_NORMAL_V1:case LABOVR_VISUAL_TEXTURE_DETAIL_V1:mappedUse=TextureUse::Normal;break;
    case LABOVR_VISUAL_TEXTURE_HEIGHT_V1:mappedUse=TextureUse::Height;break;
    case LABOVR_VISUAL_TEXTURE_ORM_V1:case LABOVR_VISUAL_TEXTURE_LINEAR_V1:mappedUse=TextureUse::Linear;break;
    default:Fail("Invalid visual texture use");
    }
    auto out=LoadTexture(path.wstring(),mappedUse);
    if(out.width>ConsoleSpec::MaxTextureEdge||out.height>ConsoleSpec::MaxTextureEdge)Fail("Visual texture exceeds console size");
    return out;
}
VisualMeshData VisualPrimitive(LabU32 source) {
    VisualMeshData out;
    auto vertex=[&](XMFLOAT3 p,XMFLOAT3 n,float u,float v) {
        XMFLOAT3 t=Unit(std::abs(n.y)>.999f?XMFLOAT3{1,0,0}:XMFLOAT3{n.z,0,-n.x});
        out.vertices.push_back({p.x,p.y,p.z,n.x,n.y,n.z,t.x,t.y,t.z,u,v});
    };
    if(source==LABOVR_VISUAL_MESH_BOX_V1){
        const XMFLOAT3 normals[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        const XMFLOAT3 right[]={{0,0,-1},{0,0,1},{1,0,0},{1,0,0},{1,0,0},{-1,0,0}};
        const XMFLOAT3 up[]={{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
        for(int f=0;f<6;++f){uint32_t b=uint32_t(out.vertices.size());
            for(int k=0;k<4;++k)vertex(Add(Scale(normals[f],.5f),Add(Scale(right[f],(k&1)?.5f:-.5f),Scale(up[f],(k&2)?.5f:-.5f))),normals[f],float(k&1),float((k>>1)&1));
            out.indices.insert(out.indices.end(),{b,b+1,b+2,b+2,b+1,b+3});}
    }else if(source==LABOVR_VISUAL_MESH_SPHERE_V1){
        constexpr uint32_t longitude=64,latitude=32;
        for(uint32_t y=0;y<=latitude;++y)for(uint32_t x=0;x<=longitude;++x){float a=float(x)*XM_2PI/longitude,b=float(y)*XM_PI/latitude;XMFLOAT3 n{std::sin(b)*std::cos(a),std::cos(b),std::sin(b)*std::sin(a)};vertex(Scale(n,.5f),n,float(x)/longitude,float(y)/latitude);}
        for(uint32_t y=0;y<latitude;++y)for(uint32_t x=0;x<longitude;++x){uint32_t b=y*(longitude+1)+x;out.indices.insert(out.indices.end(),{b,b+longitude+1,b+1,b+1,b+longitude+1,b+longitude+2});}
    }else if(source==LABOVR_VISUAL_MESH_CYLINDER_V1){
        constexpr uint32_t sides=32;
        for(uint32_t x=0;x<=sides;++x){float a=float(x)*XM_2PI/sides;XMFLOAT3 n{std::cos(a),0,std::sin(a)};vertex({n.x*.5f,-.5f,n.z*.5f},n,float(x)/sides,0);vertex({n.x*.5f,.5f,n.z*.5f},n,float(x)/sides,1);}
        for(uint32_t x=0;x<sides;++x){uint32_t b=x*2;out.indices.insert(out.indices.end(),{b,b+1,b+2,b+2,b+1,b+3});}
        for(int cap=0;cap<2;++cap){float h=cap?.5f:-.5f;XMFLOAT3 n{0,cap?1.f:-1.f,0};uint32_t b=uint32_t(out.vertices.size());vertex({0,h,0},n,.5f,.5f);
            for(uint32_t x=0;x<=sides;++x){float a=float(x)*XM_2PI/sides;vertex({std::cos(a)*.5f,h,std::sin(a)*.5f},n,std::cos(a)*.5f+.5f,std::sin(a)*.5f+.5f);}
            for(uint32_t x=0;x<sides;++x)if(cap)out.indices.insert(out.indices.end(),{b,b+x+2,b+x+1});else out.indices.insert(out.indices.end(),{b,b+x+1,b+x+2});}
    }else Fail("Unknown visual primitive");
    return out;
}
uint32_t AppendVisualMesh(MeshData& mesh,const VisualMeshData& input) {
    MeshRange range{};range.firstVertex=uint32_t(mesh.vertices.size());range.firstIndex=uint32_t(mesh.indices.size());range.firstMeshlet=uint32_t(mesh.meshlets.size());
    for(const auto& v:input.vertices)mesh.vertices.push_back({{v.px,v.py,v.pz},{v.nx,v.ny,v.nz},{v.u,v.v}});
    for(auto i:input.indices)mesh.indices.push_back(i+range.firstVertex);
    FinishRange(mesh,range);auto id=uint32_t(mesh.ranges.size());mesh.ranges.push_back(range);mesh.deformations.push_back({});mesh.motions.push_back({});mesh.skin.resize(mesh.vertices.size());return id;
}
}
