#include "PhysicalMaterials.h"
#include "OrganicForest.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace OASIS {
namespace {
constexpr float Pi=3.14159265358979323846f;
constexpr float Tau=2.f*Pi;
XMFLOAT3 Cross(XMFLOAT3 a,XMFLOAT3 b){
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
XMFLOAT3 Lerp(XMFLOAT3 a,XMFLOAT3 b,float t){
    return Add(a,Scale(Sub(b,a),t));
}
float Random(uint32_t key){
    key^=key>>16;key*=0x7feb352du;key^=key>>15;
    key*=0x846ca68bu;key^=key>>16;
    return float(key&0x00ffffffu)/16777215.f;
}
float Variation(uint32_t seed,int a,int b=0,int c=0){
    return Random(seed^uint32_t(a+1)*0x9e3779b9u^uint32_t(b+19)*0x85ebca6bu^uint32_t(c+31)*0xc2b2ae35u);
}
struct Triangles {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    void Face(XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c){
        const auto n=Unit(Cross(Sub(b,a),Sub(c,a)));
        const auto begin=uint32_t(vertices.size());
        vertices.push_back({a,n,{0.f,0.f},{}});
        vertices.push_back({b,n,{1.f,0.f},{}});
        vertices.push_back({c,n,{.5f,1.f},{}});
        indices.insert(indices.end(),{begin,begin+1,begin+2});
    }
    void Tube(XMFLOAT3 a,XMFLOAT3 b,float radius0,float radius1,unsigned sides){
        const auto axis=Unit(Sub(b,a));
        const auto helper=std::abs(axis.y)>.92f?XMFLOAT3{1,0,0}:XMFLOAT3{0,1,0};
        const auto tangent=Unit(Cross(axis,helper)),bitangent=Cross(axis,tangent);
        for(unsigned i=0;i<sides;++i){
            const float p=Tau*float(i)/float(sides),q=Tau*float(i+1)/float(sides);
            const auto at=[&](XMFLOAT3 center,float radius,float angle){
                return Add(center,Scale(Add(Scale(tangent,std::cos(angle)),
                                            Scale(bitangent,std::sin(angle))),radius));
            };
            const auto aa=at(a,radius0,p),ab=at(a,radius0,q);
            const auto ba=at(b,radius1,p),bb=at(b,radius1,q);
            Face(aa,ab,bb);Face(aa,bb,ba);
        }
    }
    // A physical, fixed-orientation sprig. No camera-facing geometry.
    void Sprig(XMFLOAT3 root,XMFLOAT3 tip,XMFLOAT3 transverse,float width){
        const auto axis=Unit(Sub(tip,root));
        const auto side=Unit(Sub(transverse,Scale(axis,Dot(axis,transverse))));
        const auto n=Unit(Cross(side,axis));
        const auto base=uint32_t(vertices.size());
        vertices.push_back({Add(root,Scale(side,-width)),n,{0,0},{}});
        vertices.push_back({Add(root,Scale(side,width)),n,{1,0},{}});
        vertices.push_back({Add(tip,Scale(side,width)),n,{1,1},{}});
        vertices.push_back({Add(tip,Scale(side,-width)),n,{0,1},{}});
        indices.insert(indices.end(),{base,base+1,base+2,base,base+2,base+3});
    }
};
struct TreeGeometry {Triangles bark,foliage;};
TreeGeometry MakeConifer(unsigned seed,bool spruce){
    TreeGeometry tree;
    const float height=spruce?12.6f:11.8f;
    tree.bark.Tube({0,0,0},{0,height,0},spruce?.35f:.40f,.025f,18);
    for(int i=0;i<8;++i){
        const float a=Tau*float(i)/8.f;
        tree.bark.Tube({0,.15f,0},{.85f*std::cos(a),.05f,.85f*std::sin(a)},.14f,.025f,9);
    }
    const unsigned tiers=spruce?28u:24u;
    const unsigned arms=spruce?7u:7u;
    for(unsigned tier=0;tier<tiers;++tier){
        const float y=.8f+float(tier)*(height-1.5f)/float(tiers);
        const float r=std::max(.16f,(spruce?3.25f:3.05f)*(1.f-y/height));
        for(unsigned arm=0;arm<arms;++arm){
            const float phase=Tau*float(arm)/float(arms)+float(tier)*1.32f+
                (Variation(seed,tier,arm)-.5f)*.28f;
            const XMFLOAT3 radial{std::cos(phase),0,std::sin(phase)};
            const XMFLOAT3 side{-radial.z,0,radial.x};
            const float branchLength=r*(.78f+.3f*Variation(seed,arm,tier));
            const XMFLOAT3 branchBase{0,y,0};
            const auto branchTip=Add(Add(branchBase,Scale(radial,branchLength)),
                {0,-.12f+.35f*Variation(seed,tier,arm,11),0});
            tree.bark.Tube(branchBase,branchTip,.052f,.004f,8);
            for(unsigned twig=0;twig<4u;++twig){
                const float t=.23f+float(twig)*.19f;
                const auto origin=Lerp(branchBase,branchTip,t);
                const float sideSign=(twig&1u)?1.f:-1.f;
                const auto end=Add(Add(origin,Scale(radial,.32f+.19f*(1.f-t))),
                    Add(Scale(side,sideSign*(.22f+.20f*(1.f-t))),{0,.045f,0}));
                const auto axis=Unit(Sub(end,origin));
                const auto side0=Unit(Cross(axis,XMFLOAT3{0,1,0}));
                const auto side1=Unit(Cross(axis,side0));
                const auto tip=Add(end,Scale(axis,.12f));
                // Each crossed plane carries 12 individual needle silhouettes.
                tree.foliage.Sprig(origin,tip,side0,.145f);
                tree.foliage.Sprig(origin,tip,side1,.145f);
            }
        }
    }
    return tree;
}
TreeGeometry MakeBroadleaf(){
    TreeGeometry tree;
    constexpr unsigned seed=0x4d41504cu;
    tree.bark.Tube({0,0,0},{0,10.7f,0},.46f,.10f,20);
    for(unsigned i=0;i<9;++i){
        const float a=Tau*float(i)/9.f;
        tree.bark.Tube({0,.1f,0},{.88f*std::cos(a),.04f,.88f*std::sin(a)},.16f,.026f,9);
    }
    for(unsigned branch=0;branch<18;++branch){
        const float angle=float(branch)*2.3999632f;
        const float y=2.3f+float(branch)*.43f;
        const float length=(2.4f+.8f*Variation(seed,branch))*(.72f+.28f*std::sin(Pi*y/12.f));
        const XMFLOAT3 radial{std::cos(angle),0,std::sin(angle)};
        const XMFLOAT3 side{-radial.z,0,radial.x};
        const XMFLOAT3 root{0,y,0};
        const auto end=Add(Add(root,Scale(radial,length)),{0,.52f+.5f*Variation(seed,branch,7),0});
        tree.bark.Tube(root,end,.15f,.028f,11);
        for(unsigned twig=0;twig<8u;++twig){
            const float t=.17f+.095f*float(twig);
            const float sign=(twig&1u)?1.f:-1.f;
            const auto start=Lerp(root,end,t);
            const auto twigEnd=Add(Add(start,Scale(side,sign*(.72f+.43f*Variation(seed,branch,twig)))),
                Add(Scale(radial,.38f),{0,.34f+.20f*Variation(seed,branch,twig,9),0}));
            tree.bark.Tube(start,twigEnd,.025f,.006f,7);
            for(unsigned sprig=0;sprig<4u;++sprig){
                const float u=.23f+.18f*float(sprig);
                const auto stalk=Lerp(start,twigEnd,u);
                const float turn=angle+sign*(.6f+.25f*float(sprig));
                const auto sprigEnd=Add(stalk,{.33f*std::cos(turn),.14f,.33f*std::sin(turn)});
                const auto axis=Unit(Sub(sprigEnd,stalk));
                const auto horizontal=Unit(Cross(axis,XMFLOAT3{0,1,0}));
                const auto vertical=Unit(Cross(axis,horizontal));
                const auto side=Unit(Add(horizontal,Scale(vertical,
                    (Variation(seed,branch,twig,sprig)-.5f)*1.2f)));
                // Seven leaf silhouettes per sprig, distributed over the same
                // three-dimensional branch locations as the former solid leaves.
                tree.foliage.Sprig(stalk,Add(sprigEnd,Scale(axis,.18f)),side,.19f);
            }
        }
    }
    return tree;
}
// Author the actual alpha/color texture consumed by raster and the OMM baker.
// Source and hierarchy UV triangles are cooked together in GraphicsFoliageOmm.
TextureData FoliageAtlas(bool conifer,bool spruce,XMFLOAT3 crown){
    constexpr unsigned N=256;
    TextureData t{};t.width=N;t.height=N;t.format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    std::vector<uint8_t> pixels(size_t(N)*N*4);
    auto leaf=[&](float u,float v,float x,float y,float tx,float ty,float width){
        const float dx=tx-x,dy=ty-y,l2=dx*dx+dy*dy;
        const float f=((u-x)*dx+(v-y)*dy)/l2;
        if(f<0.f||f>1.f)return 0.f;
        const float d=std::abs((u-x)*dy-(v-y)*dx)/std::sqrt(l2);
        const float w=width*std::pow(std::max(0.f,std::sin(Pi*f)),conifer?.32f:.75f);
        return std::clamp((w-d)*float(N)+.5f,0.f,1.f);
    };
    for(unsigned y=0;y<N;++y)for(unsigned x=0;x<N;++x){
        const float u=(x+.5f)/N,v=(y+.5f)/N;
        float alpha=0.f;
        const unsigned count=conifer?(spruce?13u:12u):7u;
        for(unsigned k=0;k<count;++k){
            const float f=(k+.55f)/float(count);
            const float sign=(k&1u)?1.f:-1.f;
            const float a=.5f+sign*(conifer?.43f:.40f);
            const float b=std::min(.98f,f*(conifer?.78f:.70f)+(conifer?.16f:.26f));
            const float base=.04f+f*(conifer?.79f:.69f);
            alpha=std::max(alpha,leaf(u,v,.5f,base,a,b,conifer?.018f:.105f));
        }
        const float grain=Variation(conifer?0x4e454544u:0x4c454146u,x,y);
        const float shade=.80f+.15f*grain+.05f*(1.f-std::abs(u-.5f)*2.f);
        // Project the tapering axial twig into the same fixed 3D sprig planes.
        const float stemEnd=conifer?.79f:.72f;
        const float stemWidth=(conifer?.052f:.021f)*std::max(0.f,1.f-v/stemEnd)+.004f;
        const float stem=v<=stemEnd?std::clamp((stemWidth-std::abs(u-.5f))*N+.5f,0.f,1.f):0.f;
        const float barkWeight=stem/std::max(.0001f,alpha+stem);
        alpha=std::max(alpha,stem);
        auto srgb=[](float c){return c<=.0031308f?12.92f*c:1.055f*std::pow(c,1.f/2.4f)-.055f;};
        const float light=shade<=.04045f?shade/12.92f:std::pow((shade+.055f)/1.055f,2.4f);
        const auto color=Lerp(Scale(crown,light),XMFLOAT3{.19f,.135f,.085f},barkWeight);
        const size_t at=(size_t(y)*N+x)*4;
        pixels[at]=uint8_t(std::clamp(srgb(color.x),0.f,1.f)*255.f+.5f);
        pixels[at+1]=uint8_t(std::clamp(srgb(color.y),0.f,1.f)*255.f+.5f);
        pixels[at+2]=uint8_t(std::clamp(srgb(color.z),0.f,1.f)*255.f+.5f);
        pixels[at+3]=uint8_t(alpha*255.f+.5f);
    }
    const auto coverage=[](const std::vector<uint8_t>& p,float scale){
        size_t n=0;for(size_t i=3;i<p.size();i+=4)n+=float(p[i])*scale>=127.5f;
        return double(n)/double(p.size()/4);
    };
    const double target=coverage(pixels,1.f);
    for(unsigned size=N;;size/=2){
        t.offsets.push_back(t.bytes.size());++t.mips;
        const auto* bytes=reinterpret_cast<const std::byte*>(pixels.data());
        t.bytes.insert(t.bytes.end(),bytes,bytes+pixels.size());
        if(size==1)break;
        const unsigned nextSize=size/2;
        std::vector<uint8_t> next(size_t(nextSize)*nextSize*4);
        for(unsigned y=0;y<nextSize;++y)for(unsigned x=0;x<nextSize;++x)
            for(unsigned c=0;c<4;++c){
                unsigned sum=0;
                for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i)
                    sum+=pixels[(size_t(y*2+j)*size+x*2+i)*4+c];
                next[(size_t(y)*nextSize+x)*4+c]=uint8_t((sum+2)/4);
            }
        // Preserve the apparent cutout density under raster mip filtering.
        float lo=.25f,hi=8.f;
        for(int i=0;i<16;++i){const float m=(lo+hi)*.5f;
            if(coverage(next,m)<target)lo=m;else hi=m;}
        for(size_t i=3;i<next.size();i+=4)
            next[i]=uint8_t(std::min(255.f,next[i]*(lo+hi)*.5f+.5f));
        pixels=std::move(next);
    }
    return t;
}
uint32_t AppendMaterial(SceneData& scene,const char* name,const char* materialId,XMFLOAT3 color,float roughness){
    AssetMaterial material{};
    material.colorRoughness={color.x,color.y,color.z,roughness};
    material.emissionMetallic={0,0,0,0};
    material.alphaMode=0;
    material.doubleSided=0;
    ApplyPhysicalMaterial(material,materialId);
    const auto index=uint32_t(scene.materials.size());
    scene.materials.push_back(material);
    scene.materialNames.emplace_back(name);
    return index;
}
WorldObject AppendGeometry(SceneData& scene,const Triangles& soup,uint32_t material){
    auto& mesh=scene.meshes;
    MeshRange range{};
    range.firstMeshlet=uint32_t(mesh.meshlets.size());
    range.firstVertex=uint32_t(mesh.vertices.size());
    range.firstIndex=uint32_t(mesh.indices.size());
    mesh.vertices.insert(mesh.vertices.end(),soup.vertices.begin(),soup.vertices.end());
    mesh.indices.reserve(mesh.indices.size()+soup.indices.size());
    for(const auto index:soup.indices)mesh.indices.push_back(range.firstVertex+index);
    FinishRange(mesh,range);
    const auto sourceIndex=uint32_t(mesh.ranges.size());
    mesh.ranges.push_back(range);
    mesh.deformations.emplace_back();
    mesh.motions.emplace_back();
    scene.mg2ForestParts.push_back({sourceIndex,material});
    WorldObject part{};
    part.mesh=sourceIndex;
    part.material=material;
    part.flags=BuildingObject|NoCollision|OASIS_INSTANCE_FOREST;
    part.boundRadius=std::max(.001f,range.bounds.w);
    return part;
}
}
std::array<OrganicTreeSource,3> BuildOrganicTreeSources(SceneData& scene){
    std::array<OrganicTreeSource,3> result{};
    const std::array<TreeGeometry,3> geometry{
        MakeConifer(0x46495231u,false),
        MakeConifer(0x53505243u,true),
        MakeBroadleaf()
    };
    const std::array<XMFLOAT3,3> crowns{{
        {.085f,.205f,.075f},{.075f,.170f,.093f},{.18f,.31f,.095f}
    }};
    SamplerData sampling{};sampling.wrapS=sampling.wrapT=33071u;
    const auto sampler=uint32_t(scene.samplers.size());scene.samplers.push_back(sampling);
    for(size_t i=0;i<geometry.size();++i){
        const auto bark=AppendMaterial(scene,"OrganicForest bark","plant.bark",{.19f,.135f,.085f},.94f);
        const auto foliage=AppendMaterial(scene,"OrganicForest OMM foliage",i<2u?"plant.needle_living":"plant.leaf_living",crowns[i],.84f);
        auto& surface=scene.materials[foliage];
        surface.alphaMode=1u;surface.doubleSided=1u;surface.alpha=1.f;surface.cutoff=.5f;
        surface.albedo=uint32_t(scene.textures.size());surface.samplers[0]=sampler;
        surface.colorRoughness.x=surface.colorRoughness.y=surface.colorRoughness.z=1.f;
        scene.textures.push_back(FoliageAtlas(i<2u,i==1u,crowns[i]));
        auto& source=result[i];
        source.parts.push_back(AppendGeometry(scene,geometry[i].bark,bark));
        source.parts.push_back(AppendGeometry(scene,geometry[i].foliage,foliage));
        source.triangles=(geometry[i].bark.indices.size()+geometry[i].foliage.indices.size())/3u;
    }
    // Forest primitives have no skeletons and no cut source. Keep the general
    // scene buffer bookkeeping coherent for later meshlet and DXR setup.
    scene.meshes.skin.resize(scene.meshes.vertices.size());
    // This scene normally has no solid-ID table. Do not create one for
    // non-collidable forest geometry: BuildingCampus::Install appends rigid
    // building meshes later, and the optional table must remain absent.
    if(!scene.triangleSolids.empty())
        scene.triangleSolids.resize(scene.meshes.indices.size()/3u,Invalid);
    return result;
}
} // namespace OASIS
