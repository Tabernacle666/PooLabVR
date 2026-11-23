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
    // Four-sided closed solid, oriented in 3D around a twig; not a leaf card.
    void Needle(XMFLOAT3 base,XMFLOAT3 tip,float width){
        const auto axis=Unit(Sub(tip,base));
        const auto helper=std::abs(axis.y)>.93f?XMFLOAT3{1,0,0}:XMFLOAT3{0,1,0};
        const auto u=Unit(Cross(axis,helper)),v=Unit(Cross(axis,u));
        const auto a=Add(base,Scale(u,width));
        const auto b=Add(base,Scale(Add(Scale(u,-.5f),Scale(v,.8660254f)),width));
        const auto c=Add(base,Scale(Add(Scale(u,-.5f),Scale(v,-.8660254f)),width));
        Face(a,b,tip);Face(b,c,tip);Face(c,a,tip);Face(a,c,b);
    }
    // Convex, cambered eight-triangle leaf with distinct front/back ridges.
    // The two surfaces enclose a real volume, even when seen edge-on.
    void Leaf(XMFLOAT3 root,XMFLOAT3 tip,float halfWidth,float thickness){
        const auto direction=Unit(Sub(tip,root));
        const auto helper=std::abs(direction.y)>.95f?XMFLOAT3{1,0,0}:XMFLOAT3{0,1,0};
        const auto side=Unit(Cross(direction,helper));
        const auto up=Unit(Cross(side,direction));
        const auto middle=Lerp(root,tip,.48f);
        const auto l=Add(middle,Scale(side,halfWidth));
        const auto r=Add(middle,Scale(side,-halfWidth));
        const auto top=Add(middle,Scale(up,thickness));
        const auto bottom=Add(middle,Scale(up,-thickness*.65f));
        Face(root,l,top);Face(root,top,r);
        Face(l,tip,top);Face(top,tip,r);
        Face(root,bottom,l);Face(root,r,bottom);
        Face(l,bottom,tip);Face(bottom,r,tip);
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
                tree.bark.Tube(origin,end,.015f,.0022f,6);
                const unsigned clusters=spruce?13u:12u;
                for(unsigned q=0;q<clusters;++q){
                    const float u=(float(q)+.35f)/float(clusters);
                    const auto center=Lerp(origin,end,u);
                    for(unsigned needle=0;needle<2u;++needle){
                        const float angle=phase+float(q)*2.3999632f+Pi*float(needle);
                        const float length=.085f+.065f*Variation(seed,tier*8u+arm,twig*19u+q,needle);
                        const XMFLOAT3 ray{std::cos(angle),.31f+Variation(seed,q,twig,tier)*.55f,std::sin(angle)};
                        const auto tip=Add(center,Scale(Unit(Add(ray,Scale(Unit(Sub(end,origin)),.3f))),length));
                        tree.foliage.Needle(center,tip,.0065f);
                    }
                }
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
                tree.bark.Tube(stalk,sprigEnd,.008f,.002f,5);
                for(unsigned leaf=0;leaf<7u;++leaf){
                    const float f=.16f+.115f*float(leaf);
                    const auto base=Lerp(stalk,sprigEnd,f);
                    const float dir=turn+((leaf&1u)?1.f:-1.f)*(.55f+.23f*Variation(seed,branch,twig,sprig*7u+leaf));
                    const float elev=.06f+.09f*Variation(seed,twig,sprig,leaf);
                    const auto tip=Add(base,{.18f*std::cos(dir),elev,.18f*std::sin(dir)});
                    tree.foliage.Leaf(base,tip,.065f+.016f*Variation(seed,sprig,leaf,twig),.024f);
                }
            }
        }
    }
    return tree;
}
uint32_t AppendMaterial(SceneData& scene,const char* name,XMFLOAT3 color,float roughness){
    AssetMaterial material{};
    material.colorRoughness={color.x,color.y,color.z,roughness};
    material.emissionMetallic={0,0,0,0};
    material.alphaMode=0;
    material.doubleSided=0;
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
    for(size_t i=0;i<geometry.size();++i){
        const auto bark=AppendMaterial(scene,"OrganicForest bark",{.19f,.135f,.085f},.94f);
        const auto foliage=AppendMaterial(scene,"OrganicForest solid foliage",crowns[i],.84f);
        auto& source=result[i];
        source.parts.push_back(AppendGeometry(scene,geometry[i].bark,bark));
        source.parts.push_back(AppendGeometry(scene,geometry[i].foliage,foliage));
        source.triangles=(geometry[i].bark.indices.size()+geometry[i].foliage.indices.size())/3u;
    }
    // Forest primitives have no skeletons and no cut source. Keep the general
    // scene buffer bookkeeping coherent for later meshlet and DXR setup.
    scene.meshes.skin.resize(scene.meshes.vertices.size());
    scene.triangleSolids.resize(scene.meshes.indices.size()/3u,Invalid);
    return result;
}
