#include "PhysicalMaterials.h"
#include "OrganicForest.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <utility>
#include <limits>

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
    void Tube(XMFLOAT3 a,XMFLOAT3 b,float radius0,float radius1,const CanopyPart& part){
        const auto axis=Unit(Sub(b,a));
        const auto helper=std::abs(axis.y)>.92f?XMFLOAT3{1,0,0}:XMFLOAT3{0,1,0};
        const auto tangent=Unit(Cross(axis,helper)),bitangent=Cross(axis,tangent);
        for(unsigned i=0;i<part.sides;++i){
            const auto p=part.ring[i],q=part.ring[i+1];
            const auto at=[&](XMFLOAT3 center,float radius,XMFLOAT2 direction){
                return Add(center,Scale(Add(Scale(tangent,direction.x),
                                            Scale(bitangent,direction.y)),radius));
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
uint32_t InternPart(CanopyLibrary& library,CanopyPartKind kind,unsigned sides){
    for(uint32_t i=0;i<library.parts.size();++i){
        const auto& part=library.parts[i];
        if(part.kind==kind&&part.sides==sides)return i;
    }
    CanopyPart part{};
    part.kind=kind;part.sides=sides;
    if(sides){
        part.ring.reserve(sides+1u);
        for(unsigned i=0;i<=sides;++i){
            const float angle=Tau*float(i)/float(sides);
            part.ring.push_back({std::cos(angle),std::sin(angle)});
        }
    }
    const auto id=uint32_t(library.parts.size());
    library.parts.push_back(std::move(part));
    return id;
}
struct AssemblyBuilder {
    CanopyLibrary& library;
    CanopyAssembly assembly;
    uint32_t Tube(CanopyPartKind kind,XMFLOAT3 root,XMFLOAT3 tip,float radius0,float radius1,unsigned sides,
                  uint32_t parent=Invalid){
        const auto id=uint32_t(assembly.placements.size());
        assembly.placements.push_back({InternPart(library,kind,sides),root,tip,{},radius0,radius1,parent});
        return id;
    }
    void Sprig(CanopyPartKind kind,XMFLOAT3 root,XMFLOAT3 tip,XMFLOAT3 transverse,float width,uint32_t parent){
        assembly.placements.push_back({InternPart(library,kind,0),root,tip,transverse,width,0.f,parent});
    }
};
TreeGeometry CookAssembly(const CanopyLibrary& library,const CanopyAssembly& assembly){
    TreeGeometry geometry;
    for(const auto& placement:assembly.placements){
        const auto& part=library.parts.at(placement.part);
        if(part.kind==CanopyPartKind::Needles||part.kind==CanopyPartKind::Leaves)
            geometry.foliage.Sprig(placement.root,placement.tip,placement.transverse,placement.radius0);
        else
            geometry.bark.Tube(placement.root,placement.tip,placement.radius0,placement.radius1,part);
    }
    return geometry;
}
CanopyAssembly MakeConifer(CanopyLibrary& library,unsigned seed,bool spruce){
    AssemblyBuilder tree{library,{}};
    const float height=spruce?12.6f:11.8f;
    tree.Tube(CanopyPartKind::Trunk,{0,0,0},{0,height,0},spruce?.35f:.40f,.025f,18);
    for(int i=0;i<8;++i){
        const float a=Tau*float(i)/8.f;
        tree.Tube(CanopyPartKind::Root,{0,.15f,0},{.85f*std::cos(a),.05f,.85f*std::sin(a)},.14f,.025f,9,0);
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
            const auto branchId=tree.Tube(CanopyPartKind::Branch,branchBase,branchTip,.052f,.004f,8,0);
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
                tree.Sprig(CanopyPartKind::Needles,origin,tip,side0,.145f,branchId);
                tree.Sprig(CanopyPartKind::Needles,origin,tip,side1,.145f,branchId);
            }
        }
    }
    return std::move(tree.assembly);
}
CanopyAssembly MakeBroadleaf(CanopyLibrary& library,unsigned seed){
    AssemblyBuilder tree{library,{}};
    tree.Tube(CanopyPartKind::Trunk,{0,0,0},{0,10.7f,0},.46f,.10f,20);
    for(unsigned i=0;i<9;++i){
        const float a=Tau*float(i)/9.f;
        tree.Tube(CanopyPartKind::Root,{0,.1f,0},{.88f*std::cos(a),.04f,.88f*std::sin(a)},.16f,.026f,9,0);
    }
    for(unsigned branch=0;branch<18;++branch){
        const float angle=float(branch)*2.3999632f;
        const float y=2.3f+float(branch)*.43f;
        const float length=(2.4f+.8f*Variation(seed,branch))*(.72f+.28f*std::sin(Pi*y/12.f));
        const XMFLOAT3 radial{std::cos(angle),0,std::sin(angle)};
        const XMFLOAT3 side{-radial.z,0,radial.x};
        const XMFLOAT3 root{0,y,0};
        const auto end=Add(Add(root,Scale(radial,length)),{0,.52f+.5f*Variation(seed,branch,7),0});
        const auto branchId=tree.Tube(CanopyPartKind::Branch,root,end,.15f,.028f,11,0);
        for(unsigned twig=0;twig<8u;++twig){
            const float t=.17f+.095f*float(twig);
            const float sign=(twig&1u)?1.f:-1.f;
            const auto start=Lerp(root,end,t);
            const auto twigEnd=Add(Add(start,Scale(side,sign*(.72f+.43f*Variation(seed,branch,twig)))),
                Add(Scale(radial,.38f),{0,.34f+.20f*Variation(seed,branch,twig,9),0}));
            const auto twigId=tree.Tube(CanopyPartKind::Twig,start,twigEnd,.025f,.006f,7,branchId);
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
                tree.Sprig(CanopyPartKind::Leaves,stalk,Add(sprigEnd,Scale(axis,.18f)),side,.19f,twigId);
            }
        }
    }
    return std::move(tree.assembly);
}
// A finite, deterministic morphology library; the individual plant seed
// selects an assembly rather than creating its own CBLAS. The baseline
// recipe is left untouched. Affine shape changes preserve branch junctions.
CanopyAssembly MakeVariant(CanopyLibrary& library,uint32_t species,uint32_t variation){
    constexpr std::array<uint32_t,3> baseSeeds{0x46495231u,0x53505243u,0x4d41504cu};
    const auto seed=baseSeeds.at(species)^(variation*0x9e3779b9u);
    auto assembly=species<2u?MakeConifer(library,seed,species==1u):MakeBroadleaf(library,seed);
    assembly.species=species;assembly.variation=variation;assembly.seed=seed;
    if(!variation)return assembly;
    const float sx=.86f+.14f*Variation(seed,11),sz=.86f+.14f*Variation(seed,12);
    const float shearX=(Variation(seed,13)-.5f)*.018f;
    const float shearZ=(Variation(seed,14)-.5f)*.018f;
    const float foliageWidth=.96f+.10f*Variation(seed,15);
    assembly.rigidity=.75f+.5f*Variation(seed,16);
    const auto shape=[&](XMFLOAT3 p){
        return XMFLOAT3{p.x*sx+p.y*shearX,p.y,p.z*sz+p.y*shearZ};
    };
    for(auto& placement:assembly.placements){
        placement.root=shape(placement.root);placement.tip=shape(placement.tip);
        const auto kind=library.parts[placement.part].kind;
        if(kind==CanopyPartKind::Needles||kind==CanopyPartKind::Leaves){
            placement.transverse=shape(placement.transverse);
            placement.radius0*=foliageWidth;
        }
    }
    return assembly;
}
// Collapse the fine wooden structure into its coarser supporting assembly.
// Error is a conservative local-space displacement bound, including omitted
// twig length and polygonal cross-section deviation. Retain roots and trunk.
CanopyLodNode CookStructuralBark(const CanopyLibrary& library,const CanopyAssembly& assembly,
                                bool collapseTwigs){
    CanopyLodNode node{};
    node.mesh=assembly.meshes[0];
    Triangles geometry;
    std::array<CanopyPart,2> rings{};
    for(size_t k=0;k<rings.size();++k){
        auto& ring=rings[k];ring.sides=k?8u:6u;
        for(unsigned j=0;j<=ring.sides;++j){
            const float angle=Tau*float(j)/float(ring.sides);
            ring.ring.push_back({std::cos(angle),std::sin(angle)});
        }
    }
    const auto distanceToSegment=[](XMFLOAT3 p,XMFLOAT3 a,XMFLOAT3 b){
        const auto axis=Sub(b,a);
        const float l2=Dot(axis,axis);
        const float t=l2>0.f?std::clamp(Dot(Sub(p,a),axis)/l2,0.f,1.f):0.f;
        return Length(Sub(p,Add(a,Scale(axis,t))));
    };
    for(const auto& placement:assembly.placements){
        const auto& part=library.parts.at(placement.part);
        if(part.kind==CanopyPartKind::Needles||part.kind==CanopyPartKind::Leaves)continue;
        const float radius=std::max(placement.radius0,placement.radius1);
        if(collapseTwigs&&part.kind==CanopyPartKind::Twig&&placement.parent!=Invalid){
            const auto& parent=assembly.placements.at(placement.parent);
            // The centreline of the omitted twig is at most this far from
            // the retained parent; include both surface radii in the bound.
            node.error=std::max(node.error,std::max(distanceToSegment(placement.root,parent.root,parent.tip),
                distanceToSegment(placement.tip,parent.root,parent.tip))+radius+
                std::max(parent.radius0,parent.radius1));
            continue;
        }
        const auto& reduced=rings[part.kind==CanopyPartKind::Trunk?1u:0u];
        const auto& ring=part.sides>reduced.sides?reduced:part;
        if(ring.sides!=part.sides){
            node.error=std::max(node.error,radius*(2.f-std::cos(Pi/float(part.sides))-
                std::cos(Pi/float(ring.sides))));
        }
        geometry.Tube(placement.root,placement.tip,placement.radius0,placement.radius1,ring);
        ++node.components;
    }
    node.vertices=std::move(geometry.vertices);node.indices=std::move(geometry.indices);
    return node;
}
// Fixed-orientation optical aggregation. Combine only similarly oriented
// sibling sprigs; their total alpha-weighted surface area is preserved by
// using the same atlas and matching geometric area. The conservative bound
// covers the entire old and new support, so subpixel selection is required.
CanopyLodNode CookStructuralFoliage(const CanopyLibrary& library,const CanopyAssembly& assembly){
    CanopyLodNode node{};
    node.mesh=assembly.meshes[1];
    Triangles geometry;
    struct SprigFrame {XMFLOAT3 center,axis,side,normal;float length,width;};
    const auto frame=[](const CanopyPartPlacement& p){
        const auto delta=Sub(p.tip,p.root);
        const auto axis=Unit(delta);
        const auto side=Unit(Sub(p.transverse,Scale(axis,Dot(axis,p.transverse))));
        return SprigFrame{Lerp(p.root,p.tip,.5f),axis,side,Unit(Cross(side,axis)),
            Length(delta),p.radius0};
    };
    const auto radius=[](const SprigFrame& s){
        return std::hypot(s.length*.5f,s.width);
    };
    const auto emit=[&](const CanopyPartPlacement& p){
        geometry.Sprig(p.root,p.tip,p.transverse,p.radius0);
        ++node.components;
    };
    // Index by botanical parent once at cook time. No per-frame search.
    std::vector<std::vector<uint32_t>> siblings(assembly.placements.size());
    for(uint32_t i=0;i<assembly.placements.size();++i){
        const auto& p=assembly.placements[i];
        const auto kind=library.parts.at(p.part).kind;
        if(kind!=CanopyPartKind::Needles&&kind!=CanopyPartKind::Leaves)continue;
        if(p.parent==Invalid){emit(p);continue;}
        siblings.at(p.parent).push_back(i);
    }
    for(const auto& group:siblings){
        std::vector<uint8_t> used(group.size());
        for(size_t i=0;i<group.size();++i){
            if(used[i])continue;
            const auto& a=assembly.placements[group[i]];
            const auto fa=frame(a);
            size_t match=group.size();
            float bestDistance=std::numeric_limits<float>::max();
            for(size_t j=i+1;j<group.size();++j){
                if(used[j])continue;
                const auto fb=frame(assembly.placements[group[j]]);
                if(Dot(fa.axis,fb.axis)<.94f||std::abs(Dot(fa.normal,fb.normal))<.97f)continue;
                const float distance=Length(Sub(fa.center,fb.center));
                if(distance<.35f*std::min(fa.length,fb.length)||
                   distance>radius(fa)+radius(fb)||distance>=bestDistance)continue;
                match=j;bestDistance=distance;
            }
            if(match==group.size()){emit(a);continue;}
            const auto fb=frame(assembly.placements[group[match]]);
            const auto center=Lerp(fa.center,fb.center,.5f);
            const auto axis=Unit(Add(fa.axis,fb.axis));
            const auto alignedSide=Dot(fa.side,fb.side)<0.f?Scale(fb.side,-1.f):fb.side;
            const auto side0=Add(fa.side,alignedSide);
            const auto side=Unit(Sub(side0,Scale(axis,Dot(axis,side0))));
            const float area=fa.length*fa.width+fb.length*fb.width;
            const float length=std::max(fa.length,fb.length)*std::sqrt(2.f);
            const float width=area/length;
            const float support=std::max(Length(Sub(center,fa.center))+radius(fa),
                Length(Sub(center,fb.center))+radius(fb));
            // Bound old-to-new and new-to-old silhouette/coverage movement
            // even when their alpha masks are not a pointwise match.
            node.error=std::max(node.error,2.f*std::max(support,std::hypot(length*.5f,width)));
            geometry.Sprig(Sub(center,Scale(axis,length*.5f)),Add(center,Scale(axis,length*.5f)),side,width);
            ++node.components;used[match]=1u;
        }
    }
    node.vertices=std::move(geometry.vertices);node.indices=std::move(geometry.indices);
    return node;
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
std::vector<OrganicTreeSource> BuildOrganicTreeSources(SceneData& scene,unsigned variantsPerSpecies){
    if(!variantsPerSpecies||variantsPerSpecies>UINT32_MAX/3u)Fail("Invalid botanical variant count");
    const auto count=size_t(variantsPerSpecies)*3u;
    std::vector<OrganicTreeSource> result(count);
    auto& library=scene.canopy;
    library=CanopyLibrary{};
    library.assemblies.reserve(count);
    for(uint32_t variation=0;variation<variantsPerSpecies;++variation)
        for(uint32_t species=0;species<3u;++species){
            library.species[species].assemblies.push_back(uint32_t(library.assemblies.size()));
            library.assemblies.push_back(MakeVariant(library,species,variation));
        }
    const std::array<XMFLOAT3,3> crowns{{
        {.085f,.205f,.075f},{.075f,.170f,.093f},{.18f,.31f,.095f}
    }};
    SamplerData sampling{};sampling.wrapS=sampling.wrapT=33071u;
    const auto sampler=uint32_t(scene.samplers.size());scene.samplers.push_back(sampling);
    for(size_t i=0;i<library.assemblies.size();++i){
        auto& assembly=library.assemblies[i];
        const auto geometry=CookAssembly(library,assembly);
        const auto species=assembly.species;
        uint32_t bark{},foliage{};
        if(!assembly.variation){
            bark=AppendMaterial(scene,"OrganicForest bark","plant.bark",{.19f,.135f,.085f},.94f);
            foliage=AppendMaterial(scene,"OrganicForest OMM foliage",species<2u?"plant.needle_living":"plant.leaf_living",crowns[species],.84f);
            auto& surface=scene.materials[foliage];
            surface.alphaMode=1u;surface.doubleSided=1u;surface.alpha=1.f;surface.cutoff=.5f;
            surface.albedo=uint32_t(scene.textures.size());surface.samplers[0]=sampler;
            surface.colorRoughness.x=surface.colorRoughness.y=surface.colorRoughness.z=1.f;
            scene.textures.push_back(FoliageAtlas(species<2u,species==1u,crowns[species]));
        }else{
            // Same alpha atlas, UV convention and material. Geometry and
            // acceleration keys still distinguish incompatible shapes.
            const auto& canonical=library.assemblies[species];
            bark=canonical.materials[0];foliage=canonical.materials[1];
        }
        auto& source=result[i];
        source.parts.push_back(AppendGeometry(scene,geometry.bark,bark));
        source.parts.push_back(AppendGeometry(scene,geometry.foliage,foliage));
        source.triangles=(geometry.bark.indices.size()+geometry.foliage.indices.size())/3u;
        assembly.meshes={source.parts[0].mesh,source.parts[1].mesh};
        assembly.materials={bark,foliage};
        auto reduced=CookStructuralBark(library,assembly,false);
        auto coarse=CookStructuralBark(library,assembly,true);
        if(reduced.indices.size()<geometry.bark.indices.size()){
            if(coarse.indices.size()<reduced.indices.size())
                library.lods.push_back(std::move(coarse));
            library.lods.push_back(std::move(reduced));
        }
        auto optical=CookStructuralFoliage(library,assembly);
        if(!optical.indices.empty()&&optical.indices.size()<geometry.foliage.indices.size())
            library.lods.push_back(std::move(optical));
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
