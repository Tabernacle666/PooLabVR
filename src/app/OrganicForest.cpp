#include "PhysicalMaterials.h"
#include "OrganicForest.h"
#include "BotanicalBake.h"
#include "AppSession.h"
#include <cstdio>
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
// Parallel-transported ring frames keep contiguous woody segments welded
// geometrically, including at LOD changes. No extra render objects.
struct WoodFrame {
    XMFLOAT3 axis0{},axis1{},side0{},side1{};
    float distance0{},distance1{};
};
XMFLOAT3 Perpendicular(XMFLOAT3 axis){
    return Unit(Cross(axis,std::abs(axis.y)>.92f?XMFLOAT3{1,0,0}:XMFLOAT3{0,1,0}));
}
XMFLOAT3 Transport(XMFLOAT3 side,XMFLOAT3 axis){
    const auto p=Sub(side,Scale(axis,Dot(side,axis)));
    return Dot(p,p)>1.e-8f?Unit(p):Perpendicular(axis);
}
std::vector<WoodFrame> WoodFrames(const CanopyLibrary& library,const CanopyAssembly& assembly){
    const auto& p=assembly.placements;
    std::vector<WoodFrame> frames(p.size());
    std::vector<uint32_t> next(p.size(),Invalid);
    const auto wood=[&](size_t i){return library.parts.at(p[i].part).sides!=0;};
    for(uint32_t i=0;i<p.size();++i){
        const auto parent=p[i].parent;
        if(!wood(i)||parent==Invalid||!wood(parent)||next[parent]!=Invalid)continue;
        if(library.parts[p[i].part].kind!=library.parts[p[parent].part].kind)continue;
        const auto gap=Sub(p[i].root,p[parent].tip);
        if(Dot(gap,gap)<1.e-10f&&std::abs(p[i].radius0-p[parent].radius1)<1.e-5f)next[parent]=i;
    }
    for(uint32_t i=0;i<p.size();++i){
        if(!wood(i))continue;
        auto& f=frames[i];
        const auto delta=Sub(p[i].tip,p[i].root);
        const auto axis=Unit(delta);
        const auto parent=p[i].parent;
        if(parent!=Invalid&&next[parent]==i){
            f.axis0=frames[parent].axis1;f.side0=frames[parent].side1;
            f.distance0=frames[parent].distance1;
        }else{
            f.axis0=axis;f.side0=Perpendicular(axis);
        }
        f.axis1=axis;
        if(next[i]!=Invalid){
            const auto child=Unit(Sub(p[next[i]].tip,p[next[i]].root));
            const auto average=Add(axis,child);
            if(Dot(average,average)>1.e-8f)f.axis1=Unit(average);
        }
        f.side1=Transport(f.side0,f.axis1);
        f.distance1=f.distance0+Length(delta);
    }
    return frames;
}
struct Triangles {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    void Tube(XMFLOAT3 a,XMFLOAT3 b,float radius0,float radius1,const CanopyPart& part,const WoodFrame& f){
        const auto base=uint32_t(vertices.size());
        const auto stride=part.sides+1u;
        for(unsigned end=0;end<2;++end){
            const auto center=end?b:a;
            const auto axis=end?f.axis1:f.axis0;
            const auto tangent=end?f.side1:f.side0;
            const auto bitangent=Cross(axis,tangent);
            const float radius=end?radius1:radius0;
            const float v=end?f.distance1:f.distance0;
            for(unsigned i=0;i<stride;++i){
                const auto p=part.ring[i];
                const auto n=Unit(Add(Scale(tangent,p.x),Scale(bitangent,p.y)));
                vertices.push_back({Add(center,Scale(n,radius)),n,{float(i)/float(part.sides),v},{}});
            }
        }
        for(unsigned i=0;i<part.sides;++i){
            const auto aa=base+i,ab=aa+1,ba=aa+stride,bb=ba+1;
            indices.insert(indices.end(),{aa,ab,bb,aa,bb,ba});
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
    const auto frames=WoodFrames(library,assembly);
    for(size_t i=0;i<assembly.placements.size();++i){
        const auto& placement=assembly.placements[i];
        const auto& part=library.parts.at(placement.part);
        if(part.kind==CanopyPartKind::Needles||part.kind==CanopyPartKind::Leaves)
            geometry.foliage.Sprig(placement.root,placement.tip,placement.transverse,placement.radius0);
        else
            geometry.bark.Tube(placement.root,placement.tip,placement.radius0,placement.radius1,part,frames[i]);
    }
    return geometry;
}
// Species-authored growth curves, deterministic per specimen. All growth is
// performed only on an ARB-1 recipe miss; placements remain assembly-local.
struct AxisSample {XMFLOAT3 position,direction;float radius;uint32_t parent;};
struct GrowthAxis {
    std::vector<XMFLOAT3> points;
    std::vector<float> radii;
    std::vector<uint32_t> segments;
    AxisSample At(float t)const{
        const float u=std::clamp(t,0.f,1.f)*float(segments.size());
        const auto i=std::min(size_t(u),segments.size()-1u);
        const float f=u-float(i);
        return {Lerp(points[i],points[i+1],f),Unit(Sub(points[i+1],points[i])),
            radii[i]+(radii[i+1]-radii[i])*f,segments[i]};
    }
};
GrowthAxis Grow(AssemblyBuilder& tree,CanopyPartKind kind,XMFLOAT3 start,XMFLOAT3 control,
                XMFLOAT3 end,float r0,float r1,unsigned sides,unsigned steps,uint32_t parent,float taper=1.f){
    GrowthAxis path;
    path.points.reserve(steps+1u);path.radii.reserve(steps+1u);path.segments.reserve(steps);
    path.points.push_back(start);path.radii.push_back(r0);
    for(unsigned i=1;i<=steps;++i){
        const float t=float(i)/float(steps);
        const auto p=Lerp(Lerp(start,control,t),Lerp(control,end,t),t);
        const float r=r1+(r0-r1)*std::pow(1.f-t,taper);
        parent=tree.Tube(kind,path.points.back(),p,path.radii.back(),r,sides,parent);
        path.segments.push_back(parent);path.points.push_back(p);path.radii.push_back(r);
    }
    return path;
}
XMFLOAT3 Radial(float a){return {std::cos(a),0,std::sin(a)};}
float Range(uint32_t seed,int a,int b,int c,float lo,float hi){
    return lo+(hi-lo)*Variation(seed,a,b,c);
}
GrowthAxis GrowTrunk(AssemblyBuilder& tree,uint32_t seed,float height,float radius,bool broadleaf){
    const float a=Tau*Variation(seed,90,0),b=a+Range(seed,90,1,0,1.2f,3.8f);
    const auto bend=Scale(Radial(a),height*Range(seed,90,2,0,.012f,broadleaf?.12f:.055f));
    const auto tip=Scale(Radial(b),height*Range(seed,90,3,0,.015f,broadleaf?.11f:.055f));
    return Grow(tree,CanopyPartKind::Trunk,{0,0,0},Add(bend,{0,height*.49f,0}),
        Add(tip,{0,height,0}),radius,broadleaf?.045f:.012f, broadleaf?20u:18u,12u,Invalid,1.55f);
}
void GrowRoots(AssemblyBuilder& tree,const GrowthAxis& trunk,uint32_t seed,float radius,bool broadleaf){
    const unsigned count=5u+unsigned(Variation(seed,91,0)*4.f);
    const float phase=Tau*Variation(seed,91,1);
    for(unsigned i=0;i<count;++i){
        const float a=phase+Tau*float(i)/float(count)+Range(seed,91,i,2,-.25f,.25f);
        const auto radial=Radial(a),side=Radial(a+Pi*.5f);
        const auto base=trunk.At(Range(seed,91,i,3,.006f,.022f));
        const float length=radius*Range(seed,91,i,4,1.8f,broadleaf?4.2f:3.6f);
        const auto end=Add(Scale(radial,length),{0,-.035f,0});
        const auto control=Add(Add(base.position,Scale(radial,length*.55f)),
            Add(Scale(side,length*Range(seed,91,i,5,-.20f,.20f)),{0,-base.position.y*.65f,0}));
        Grow(tree,CanopyPartKind::Root,base.position,control,end,radius*Range(seed,91,i,6,.30f,.52f),
            .012f,9u,3u,base.parent,1.4f);
    }
}
void NeedleSprigs(AssemblyBuilder& tree,const GrowthAxis& twig,uint32_t seed,int branch,int index,float size,bool spruce){
    // One actual woody twig, with two differently oriented needle-bearing
    // sprays attached to its terminal growth, plus a continuation spray.
    const auto at=twig.At(.48f),tip=twig.At(.88f);
    const auto transverse=Perpendicular(at.direction);
    const auto other=Cross(at.direction,transverse);
    const float angle=Range(seed,branch,index,70,-.55f,.55f);
    const auto side=Unit(Add(Scale(transverse,std::cos(angle)),Scale(other,std::sin(angle))));
    const auto side2=Unit(Cross(at.direction,side));
    const auto end=Add(tip.position,Scale(tip.direction,size*.65f));
    tree.Sprig(CanopyPartKind::Needles,at.position,end,side,size*(spruce?.53f:.62f),at.parent);
    tree.Sprig(CanopyPartKind::Needles,at.position,end,side2,size*(spruce?.42f:.36f),at.parent);
}
// Authored life-stage morphology, not a uniform scale of a finished adult.
struct GrowthForm {
    float height,radius,crown,firstConifer,firstBroadleaf,branchCount,asymmetry,loss;
};
constexpr std::array<GrowthForm,4> GrowthForms{{
    {1.f,1.f,1.f,.30f,.38f,1.f,.20f,0.f},       // established adult
    {.58f,.40f,.62f,.19f,.28f,.74f,.12f,0.f},   // young, narrow, vigorous
    {1.06f,1.24f,1.08f,.40f,.44f,.90f,.27f,0.f}, // mature, clear bole
    {.91f,1.09f,.94f,.34f,.40f,.95f,.40f,.20f}  // exposed, damaged crown
}};
CanopyAssembly MakeConifer(CanopyLibrary& library,unsigned seed,bool spruce,const GrowthForm& form){
    AssemblyBuilder tree{library,{}};
    const float height=(spruce?12.6f:11.8f)*form.height*Range(seed,80,0,0,.91f,1.09f);
    const float radius=(spruce?.35f:.40f)*form.radius*Range(seed,80,1,0,.87f,1.13f);
    const float crown=(spruce?3.25f:3.05f)*form.crown*Range(seed,80,2,0,.85f,1.15f);
    const auto trunk=GrowTrunk(tree,seed,height,radius,false);
    GrowRoots(tree,trunk,seed,radius,false);
    const unsigned whorls=unsigned(float((spruce?22u:19u)+unsigned(Variation(seed,80,3)*4.f))*form.branchCount);
    const float first=form.firstConifer+Range(seed,80,4,0,-.035f,.035f);
    const float exposed=Tau*Variation(seed,80,5);
    float phase=Tau*Variation(seed,80,6);
    for(unsigned w=0;w<whorls;++w){
        const float h=std::clamp(first+(.955f-first)*(float(w)+Range(seed,w,80,0,.12f,.88f))/float(whorls),first,.97f);
        phase+=Range(seed,w,80,1,1.05f,2.1f);
        const unsigned arms=4u+unsigned(Variation(seed,w,80,2)*3.f);
        for(unsigned arm=0;arm<arms;++arm){
            const int id=int(w*8u+arm);
            const float a=phase+Tau*float(arm)/float(arms)+Range(seed,id,81,0,-.27f,.27f);
            const auto radial=Radial(a),side=Radial(a+Pi*.5f);
            const auto base=trunk.At(std::clamp(h+Range(seed,id,81,1,-.008f,.008f),.05f,.98f));
            const float envelope=std::pow(std::max(.025f,1.f-h),spruce?.88f:1.12f);
            const float exposure=std::cos(a-exposed);
            if(exposure<-.25f&&Variation(seed,id,81,9)<form.loss)continue;
            const float asymmetry=1.f+form.asymmetry*exposure;
            const float length=crown*envelope*asymmetry*Range(seed,id,81,2,.70f,1.23f);
            const float rise=length*(spruce?Range(seed,id,81,3,-.09f,.16f):Range(seed,id,81,3,.03f,.23f));
            const auto end=Add(Add(base.position,Scale(radial,length)),Add(Scale(side,length*Range(seed,id,81,4,-.13f,.13f)),{0,rise,0}));
            const auto mid=Add(Add(base.position,Scale(radial,length*.54f)),{0,length*(spruce?-.23f:-.06f),0});
            const float r=std::min(base.radius*.38f,.025f+length*.021f);
            const auto branch=Grow(tree,CanopyPartKind::Branch,base.position,mid,end,r,.004f,8u,3u,base.parent,1.15f);
            const unsigned twigs=5u+unsigned(Variation(seed,id,81,5)*3.f);
            for(unsigned j=0;j<twigs;++j){
                const float t=.20f+.72f*(float(j)+Range(seed,id,j,82,.18f,.82f))/float(twigs);
                const auto at=branch.At(t);
                const float sign=(j&1u)?1.f:-1.f;
                const float spread=std::min(.69f,length*(.31f-.14f*t))*Range(seed,id,j,83,.75f,1.15f);
                const auto delta=Add(Scale(at.direction,spread*.8f),Add(Scale(side,sign*spread),{0,spread*(spruce?-.17f:.12f),0}));
                const auto twigEnd=Add(at.position,delta);
                const auto twig=Grow(tree,CanopyPartKind::Twig,at.position,
                    Add(at.position,Add(Scale(delta,.5f),{0,-spread*.13f,0})),twigEnd,
                    std::min(at.radius*.55f,.013f),.002f,5u,1u,at.parent);
                NeedleSprigs(tree,twig,seed,id,j,std::max(.065f,spread*.65f),spruce);
            }
            const auto last=branch.At(.92f);
            const float spray=std::max(.07f,std::min(.36f,length*.22f));
            const auto tip=Add(last.position,Scale(last.direction,spray));
            tree.Sprig(CanopyPartKind::Needles,last.position,tip,side,spray*.48f,last.parent);
            tree.Sprig(CanopyPartKind::Needles,last.position,tip,Perpendicular(last.direction),spray*.43f,last.parent);
        }
    }
    // Living leader at the crown apex, not a bare pointed pole.
    for(unsigned i=0;i<5u;++i){
        const auto at=trunk.At(.94f+.011f*float(i));
        const auto radial=Radial(phase+2.3999632f*float(i));
        const auto tip=Add(at.position,Add(Scale(radial,.22f*(1.f-.12f*i)),{0,.19f,0}));
        tree.Sprig(CanopyPartKind::Needles,at.position,tip,Perpendicular(Unit(Sub(tip,at.position))),.085f,at.parent);
    }
    return std::move(tree.assembly);
}
CanopyAssembly MakeBroadleaf(CanopyLibrary& library,unsigned seed,const GrowthForm& form){
    AssemblyBuilder tree{library,{}};
    const float height=10.7f*form.height*Range(seed,85,0,0,.91f,1.12f);
    const float radius=.46f*form.radius*Range(seed,85,1,0,.84f,1.18f);
    const float crown=3.45f*form.crown*Range(seed,85,2,0,.86f,1.14f);
    const auto trunk=GrowTrunk(tree,seed,height,radius,true);
    GrowRoots(tree,trunk,seed,radius,true);
    const float phase=Tau*Variation(seed,85,3);
    const unsigned primaries=unsigned(float(9u+unsigned(Variation(seed,85,4)*4.f))*form.branchCount);
    const float first=form.firstBroadleaf+Range(seed,85,5,0,-.04f,.04f);
    const float exposed=Tau*Variation(seed,85,6);
    for(unsigned i=0;i<primaries;++i){
        const int id=int(i);
        const float h=first+(.88f-first)*(float(i)+Range(seed,id,86,0,.10f,.9f))/float(primaries);
        const auto base=trunk.At(h);
        const float angle=phase+2.3999632f*float(i)+Range(seed,id,86,1,-.40f,.40f);
        const auto radial=Radial(angle),side=Radial(angle+Pi*.5f);
        const float exposure=std::cos(angle-exposed);
        if(exposure<-.25f&&Variation(seed,id,86,9)<form.loss)continue;
        const float length=crown*std::pow(std::sin(Pi*(.12f+.82f*h)),.72f)*
            (1.f+form.asymmetry*exposure)*Range(seed,id,86,2,.72f,1.16f);
        const float rise=std::min(height*(1.015f-h),length*Range(seed,id,86,3,.56f,1.1f));
        const auto end=Add(Add(base.position,Scale(radial,length)),Add(Scale(side,length*Range(seed,id,86,4,-.19f,.19f)),{0,rise,0}));
        const auto control=Add(base.position,Add(Scale(radial,length*.38f),{0,rise*.21f,0}));
        const auto branch=Grow(tree,CanopyPartKind::Branch,base.position,control,end,
            std::min(base.radius*.72f,.10f+length*.025f),.014f,12u,4u,base.parent,1.25f);
        const unsigned forks=4u+unsigned(Variation(seed,id,86,5)*2.f);
        for(unsigned j=0;j<forks;++j){
            const float t=.25f+.71f*(float(j)+Range(seed,id,j,87,.12f,.92f))/float(forks);
            const auto at=branch.At(t);
            const float turn=angle+((j&1u)?1.f:-1.f)*Range(seed,id,j,88,.6f,1.4f);
            const auto out=Radial(turn);
            const float len=length*(.54f-.22f*t)*Range(seed,id,j,89,.75f,1.15f);
            const auto delta=Add(Scale(out,len),{0,len*Range(seed,id,j,90,.52f,1.05f),0});
            const auto fork=Grow(tree,CanopyPartKind::Branch,at.position,
                Add(at.position,Add(Scale(at.direction,len*.35f),Scale(delta,.27f))),
                Add(at.position,delta),std::min(at.radius*.65f,.075f),.005f,8u,3u,at.parent,1.1f);
            const unsigned shoots=3u+unsigned(Variation(seed,id,j,91)*2.f);
            for(unsigned k=0;k<shoots;++k){
                const int key=int(j*8u+k);
                const float u=.29f+.68f*(float(k)+Range(seed,id,key,92,.15f,.9f))/float(shoots);
                const auto origin=fork.At(u);
                const float a=turn+((k&1u)?1.f:-1.f)*Range(seed,id,key,93,.55f,1.25f);
                const float shoot=Range(seed,id,key,94,.37f,.74f)*(.65f+.35f*len);
                const auto d=Add(Scale(Radial(a),shoot),{0,shoot*Range(seed,id,key,95,.22f,.65f),0});
                const auto twig=Grow(tree,CanopyPartKind::Twig,origin.position,
                    Add(origin.position,Scale(d,.40f)),Add(origin.position,d),
                    std::min(origin.radius*.67f,.017f),.0025f,6u,2u,origin.parent,1.15f);
                for(unsigned m=0;m<4u;++m){
                    const auto stalk=twig.At(.22f+.70f*(float(m)+Variation(seed,id,key,100+m))/4.f);
                    const float az=a+((m&1u)?1.f:-1.f)*Range(seed,id,key,110+m,.55f,1.45f);
                    const float sprig=Range(seed,id,key,120+m,.32f,.51f);
                    const auto tip=Add(stalk.position,Add(Scale(Radial(az),sprig),{0,sprig*Range(seed,id,key,130+m,-.16f,.48f),0}));
                    const auto axis=Unit(Sub(tip,stalk.position));
                    const auto across=Perpendicular(axis),up=Cross(axis,across);
                    const float roll=Range(seed,id,key,140+m,-.82f,.82f);
                    const auto transverse=Unit(Add(Scale(across,std::cos(roll)),Scale(up,std::sin(roll))));
                    tree.Sprig(CanopyPartKind::Leaves,stalk.position,tip,transverse,sprig*Range(seed,id,key,150+m,.34f,.46f),stalk.parent);
                }
            }
        }
    }
    // A small asymmetric terminal crown prevents an exposed straight leader.
    for(unsigned i=0;i<8u;++i){
        const auto at=trunk.At(.87f+.105f*float(i)/8.f);
        const auto radial=Radial(phase+2.3999632f*float(i));
        const auto delta=Add(Scale(radial,Range(seed,87,i,0,.27f,.63f)),{0,.17f,0});
        const auto twig=Grow(tree,CanopyPartKind::Twig,at.position,Add(at.position,Scale(delta,.42f)),
            Add(at.position,delta),std::min(at.radius*.5f,.015f),.002f,6u,2u,at.parent);
        for(unsigned j=0;j<3u;++j){
            const auto p=twig.At(.35f+.27f*j);
            const auto d=Add(Scale(Radial(phase+2.3999632f*i+(j&1u?1.f:-1.f)),.37f),{0,.10f,0});
            tree.Sprig(CanopyPartKind::Leaves,p.position,Add(p.position,d),Perpendicular(Unit(d)),.15f,p.parent);
        }
    }
    return std::move(tree.assembly);
}
CanopyAssembly MakeVariant(CanopyLibrary& library,uint32_t species,uint32_t variation){
    constexpr std::array<uint32_t,3> baseSeeds{0x46495231u,0x53505243u,0x4d41504cu};
    const auto seed=baseSeeds.at(species)^(variation*0x9e3779b9u);
    const auto& form=GrowthForms[variation%GrowthForms.size()];
    auto assembly=species<2u?MakeConifer(library,seed,species==1u,form):MakeBroadleaf(library,seed,form);
    assembly.species=species;assembly.variation=variation;assembly.seed=seed;
    assembly.rigidity=(variation%GrowthForms.size()==1u?.60f:variation%GrowthForms.size()==2u?1.25f:1.f)*
        (.90f+.20f*Variation(seed,16));
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
    const auto frames=WoodFrames(library,assembly);
    for(size_t i=0;i<assembly.placements.size();++i){
        const auto& placement=assembly.placements[i];
        const auto& part=library.parts.at(placement.part);
        if(part.kind==CanopyPartKind::Needles||part.kind==CanopyPartKind::Leaves)continue;
        const float radius=std::max(placement.radius0,placement.radius1);
        if(collapseTwigs&&part.kind==CanopyPartKind::Twig&&placement.parent!=Invalid){
            // A multi-segment twig may have an omitted parent. Walk up to
            // the retained support before measuring the full displacement.
            auto ancestor=placement.parent;
            while(ancestor!=Invalid&&library.parts.at(assembly.placements.at(ancestor).part).kind==CanopyPartKind::Twig)
                ancestor=assembly.placements.at(ancestor).parent;
            if(ancestor==Invalid){
                geometry.Tube(placement.root,placement.tip,placement.radius0,placement.radius1,part,frames[i]);
                ++node.components;continue;
            }
            const auto& parent=assembly.placements.at(ancestor);
            // Include both surface radii in the conservative bound.
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
        geometry.Tube(placement.root,placement.tip,placement.radius0,placement.radius1,ring,frames[i]);
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
        float alpha=0.f,vein=0.f;
        const unsigned count=conifer?(spruce?13u:12u):7u;
        for(unsigned k=0;k<count;++k){
            const float f=(k+.55f)/float(count);
            const float sign=(k&1u)?1.f:-1.f;
            const float a=.5f+sign*(conifer?.43f:.40f);
            const float b=std::min(.98f,f*(conifer?.78f:.70f)+(conifer?.16f:.26f));
            const float base=.04f+f*(conifer?.79f:.69f);
            const float coverage=leaf(u,v,.5f,base,a,b,conifer?.018f:.105f);
            alpha=std::max(alpha,coverage); // unchanged OMM coverage
            if(coverage>0.f){
                const float dx=a-.5f,dy=b-base,l2=dx*dx+dy*dy;
                const float along=((u-.5f)*dx+(v-base)*dy)/l2;
                const float across=std::abs((u-.5f)*dy-(v-base)*dx)/std::sqrt(l2);
                const float midrib=std::max(0.f,1.f-across/(conifer?.009f:.013f));
                const float ribs=conifer?0.f:.15f*std::max(0.f,1.f-std::abs(std::sin(36.f*along-48.f*across))*5.f);
                vein=std::max(vein,coverage*(.14f*midrib+ribs));
            }
        }
        const float grain=Variation(conifer?0x4e454544u:0x4c454146u,x,y);
        const float shade=std::clamp(.76f+.12f*grain+.07f*(1.f-std::abs(u-.5f)*2.f)+vein,.55f,1.f);
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
// Repeatable bark pigment and relief authored once per species. U wraps
// around each transported woody ring; V follows its continuous axial length.
// These maps do not alter topology, alpha coverage, LOD or acceleration data.
float BarkHeight(unsigned species,float u,float v){
    const float x=Tau*u,y=Tau*v;
    const float warp=.39f*std::sin(3.f*x+2.f*y)+.22f*std::sin(7.f*x-y);
    const float phase=(species==2u?11.f:species==1u?19.f:15.f)*x+warp+
        .48f*std::sin(2.f*y+3.f*x);
    const float plate=.5f+.5f*std::sin(phase);
    const float ridge=1.f-std::pow(plate,species==1u?7.f:5.f);
    const float fissures=.12f*std::sin(37.f*x+3.f*y+warp)*
        std::sin(9.f*y+2.f*x);
    const float scales=.08f*std::sin(13.f*x-5.f*y)*std::sin(17.f*y+5.f*x);
    return .68f*ridge+fissures+scales+.09f*std::sin(5.f*x+2.f*y);
}
TextureData BarkAtlas(unsigned species,bool normal){
    constexpr unsigned N=256;
    TextureData t{};t.width=N;t.height=N;t.normal=normal;
    t.format=normal?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    std::vector<uint8_t> pixels(size_t(N)*N*4);
    const std::array<XMFLOAT3,3> pigments{{
        {.115f,.082f,.054f},{.135f,.088f,.055f},{.125f,.103f,.077f}
    }};
    const auto byte=[](float c){return uint8_t(std::clamp(c,0.f,1.f)*255.f+.5f);};
    const auto srgb=[](float c){return c<=.0031308f?12.92f*c:1.055f*std::pow(c,1.f/2.4f)-.055f;};
    const auto linear=[](float c){return c<=.04045f?c/12.92f:std::pow((c+.055f)/1.055f,2.4f);};
    for(unsigned y=0;y<N;++y)for(unsigned x=0;x<N;++x){
        const float u=(x+.5f)/N,v=(y+.5f)/N,step=1.f/N;
        const float h=BarkHeight(species,u,v);
        const size_t at=(size_t(y)*N+x)*4;
        if(normal){
            const float dx=BarkHeight(species,u+step,v)-BarkHeight(species,u-step,v);
            const float dy=BarkHeight(species,u,v+step)-BarkHeight(species,u,v-step);
            const auto n=Unit(XMFLOAT3{-dx*2.7f,-dy*2.7f,1.f});
            pixels[at]=byte(n.x*.5f+.5f);pixels[at+1]=byte(n.y*.5f+.5f);pixels[at+2]=byte(n.z*.5f+.5f);
        }else{
            const float lichen=std::max(0.f,std::sin(Tau*(3.f*u+2.f*v))*
                std::sin(Tau*(5.f*u-3.f*v))-.62f)*.55f;
            const auto color=Lerp(Scale(pigments[species],.53f+.72f*std::clamp(h,0.f,1.f)),
                                  XMFLOAT3{.16f,.18f,.11f},lichen);
            pixels[at]=byte(srgb(color.x));pixels[at+1]=byte(srgb(color.y));pixels[at+2]=byte(srgb(color.z));
        }
        pixels[at+3]=255u;
    }
    for(unsigned size=N;;size/=2u){
        t.offsets.push_back(t.bytes.size());++t.mips;
        const auto* bytes=reinterpret_cast<const std::byte*>(pixels.data());
        t.bytes.insert(t.bytes.end(),bytes,bytes+pixels.size());
        if(size==1u)break;
        const unsigned nextSize=size/2u;
        std::vector<uint8_t> next(size_t(nextSize)*nextSize*4);
        for(unsigned y=0;y<nextSize;++y)for(unsigned x=0;x<nextSize;++x){
            const size_t at=(size_t(y)*nextSize+x)*4;
            for(unsigned c=0;c<3;++c){
                float sum=0.f;
                for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i){
                    const float sample=pixels[(size_t(y*2+j)*size+x*2+i)*4+c]/255.f;
                    sum+=normal?sample:linear(sample);
                }
                next[at+c]=byte(normal?sum*.25f:srgb(sum*.25f));
            }
            next[at+3]=255u;
        }
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
// Cook each unique mesh in its own address space. The same FinishRange
// implementation is used as before; global offsets are assigned only on import.
MeshData CookMesh(Triangles&& soup){
    MeshData mesh{};
    mesh.vertices=std::move(soup.vertices);mesh.indices=std::move(soup.indices);
    MeshRange range{};FinishRange(mesh,range);mesh.ranges.push_back(range);
    return mesh;
}
BotanicalBake::Geometry CookGeometry(const CanopyLibrary& library,CanopyAssembly& assembly){
    auto geometry=CookAssembly(library,assembly);
    const auto barkCount=geometry.bark.indices.size(),foliageCount=geometry.foliage.indices.size();
    BotanicalBake::Geometry result;
    // LOD routes are local to this asset, not the current scene.
    assembly.meshes={0u,1u};
    result.parts[0]=CookMesh(std::move(geometry.bark));
    result.parts[1]=CookMesh(std::move(geometry.foliage));
    auto reduced=CookStructuralBark(library,assembly,false);
    auto coarse=CookStructuralBark(library,assembly,true);
    if(reduced.indices.size()<barkCount){
        if(coarse.indices.size()<reduced.indices.size())result.lods.push_back(std::move(coarse));
        result.lods.push_back(std::move(reduced));
    }
    auto optical=CookStructuralFoliage(library,assembly);
    if(!optical.indices.empty()&&optical.indices.size()<foliageCount)result.lods.push_back(std::move(optical));
    return result;
}
WorldObject AppendGeometry(SceneData& scene,const MeshData& cooked,uint32_t material){
    auto& mesh=scene.meshes;
    const auto fits=[](size_t a,size_t b){return a<=UINT32_MAX&&b<=UINT32_MAX-a;};
    if(!fits(mesh.vertices.size(),cooked.vertices.size())||!fits(mesh.indices.size(),cooked.indices.size())||
       !fits(mesh.meshlets.size(),cooked.meshlets.size())||!fits(mesh.remap.size(),cooked.remap.size())||
       !fits(mesh.triangles.size(),cooked.triangles.size())||mesh.ranges.size()>=Invalid)
        Fail("Botanical mesh address space exhausted");
    auto range=cooked.ranges.front();
    range.firstVertex=uint32_t(mesh.vertices.size());range.firstIndex=uint32_t(mesh.indices.size());
    range.firstMeshlet=uint32_t(mesh.meshlets.size());
    const auto remapBase=uint32_t(mesh.remap.size()),triangleBase=uint32_t(mesh.triangles.size());
    mesh.vertices.insert(mesh.vertices.end(),cooked.vertices.begin(),cooked.vertices.end());
    for(auto index:cooked.indices)mesh.indices.push_back(range.firstVertex+index);
    for(auto index:cooked.remap)mesh.remap.push_back(range.firstVertex+index);
    mesh.triangles.insert(mesh.triangles.end(),cooked.triangles.begin(),cooked.triangles.end());
    for(auto m:cooked.meshlets){
        m.vertexOffset+=remapBase;m.triangleOffset+=triangleBase;mesh.meshlets.push_back(m);
    }
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
BotanicalBake::Recipe LoadRecipe(unsigned species,unsigned variant,BotanicalBake::Stats& stats){
    const auto key=BotanicalBake::RecipeKey(species,variant);
    BotanicalBake::Recipe recipe;
    if(!BotanicalBake::Load(key,recipe,stats,[&](const auto& r){return BotanicalBake::Valid(r,species,variant);})){
        CanopyLibrary local;
        recipe.assembly=MakeVariant(local,species,variant);
        recipe.parts=std::move(local.parts);
        BotanicalBake::Save(key,recipe,stats);
    }
    return recipe;
}
CanopyAssembly ImportRecipe(CanopyLibrary& library,BotanicalBake::Recipe&& recipe){
    std::vector<uint32_t> remap;
    remap.reserve(recipe.parts.size());
    for(auto& source:recipe.parts){
        uint32_t id=0;
        for(;id<library.parts.size();++id){
            const auto& p=library.parts[id];
            if(p.kind!=source.kind||p.sides!=source.sides||p.ring.size()!=source.ring.size())continue;
            if(std::equal(p.ring.begin(),p.ring.end(),source.ring.begin(),
                          [](auto a,auto b){return a.x==b.x&&a.y==b.y;}))break;
        }
        if(id==library.parts.size())library.parts.push_back(std::move(source));
        remap.push_back(id);
    }
    for(auto& placement:recipe.assembly.placements)placement.part=remap.at(placement.part);
    return std::move(recipe.assembly);
}
}
std::vector<OrganicTreeSource> BuildOrganicTreeSources(SceneData& scene,unsigned variantsPerSpecies){
    if(!variantsPerSpecies||variantsPerSpecies>UINT32_MAX/3u)Fail("Invalid botanical variant count");
    const auto count=size_t(variantsPerSpecies)*3u;
    std::vector<OrganicTreeSource> result(count);
    auto& library=scene.canopy;
    library=CanopyLibrary{};
    library.assemblies.reserve(count);
    const auto started=GetTickCount64();
    BotanicalBake::Stats recipes,meshes,atlases;
    for(uint32_t variation=0;variation<variantsPerSpecies;++variation)
        for(uint32_t species=0;species<3u;++species){
            library.species[species].assemblies.push_back(uint32_t(library.assemblies.size()));
            library.assemblies.push_back(ImportRecipe(library,LoadRecipe(species,variation,recipes)));
        }
    const std::array<XMFLOAT3,3> crowns{{
        {.085f,.205f,.075f},{.075f,.170f,.093f},{.18f,.31f,.095f}
    }};
    SamplerData sampling{};sampling.wrapS=sampling.wrapT=33071u;
    const auto sampler=uint32_t(scene.samplers.size());scene.samplers.push_back(sampling);
    const auto barkSampler=uint32_t(scene.samplers.size());scene.samplers.push_back(SamplerData{});
    for(size_t i=0;i<library.assemblies.size();++i){
        auto& assembly=library.assemblies[i];
        const auto species=assembly.species;
        const auto geometryKey=BotanicalBake::GeometryKey(species,assembly.variation);
        BotanicalBake::Geometry geometry;
        if(!BotanicalBake::Load(geometryKey,geometry,meshes,[](const auto& g){return BotanicalBake::Valid(g);})){
            geometry=CookGeometry(library,assembly);
            BotanicalBake::Save(geometryKey,geometry,meshes);
        }
        uint32_t bark{},foliage{};
        if(!assembly.variation){
            bark=AppendMaterial(scene,"OrganicForest bark","plant.bark",{1.f,1.f,1.f},.94f);
            // Independent texture products; existing geometry/recipe revisions
            // remain valid. All variants share the species' two bark maps.
            for(unsigned channel=0;channel<2u;++channel){
                const bool normal=channel==1u;
                const auto key=BotanicalBake::BarkKey(species,normal);
                const auto format=normal?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                TextureData map;
                if(!BotanicalBake::Load(key,map,atlases,[&](const auto& t){return BotanicalBake::Valid(t,format)&&t.normal==normal;})){
                    map=BarkAtlas(species,normal);
                    BotanicalBake::Save(key,map,atlases);
                }
                auto& wood=scene.materials[bark];
                (normal?wood.normal:wood.albedo)=uint32_t(scene.textures.size());
                wood.samplers[channel]=barkSampler;
                scene.textures.push_back(std::move(map));
            }
            foliage=AppendMaterial(scene,"OrganicForest OMM foliage",species<2u?"plant.needle_living":"plant.leaf_living",crowns[species],.84f);
            auto& surface=scene.materials[foliage];
            surface.alphaMode=1u;surface.doubleSided=1u;surface.alpha=1.f;surface.cutoff=.5f;
            surface.albedo=uint32_t(scene.textures.size());surface.samplers[0]=sampler;
            surface.colorRoughness.x=surface.colorRoughness.y=surface.colorRoughness.z=1.f;
            const auto atlasKey=BotanicalBake::AtlasKey(species);
            TextureData atlas;
            if(!BotanicalBake::Load(atlasKey,atlas,atlases,[](const auto& t){return BotanicalBake::Valid(t);})){
                atlas=FoliageAtlas(species<2u,species==1u,crowns[species]);
                BotanicalBake::Save(atlasKey,atlas,atlases);
            }
            scene.textures.push_back(std::move(atlas));
        }else{
            // Same alpha atlas, UV convention and material. Geometry and
            // acceleration keys still distinguish incompatible shapes.
            const auto& canonical=library.assemblies[species];
            bark=canonical.materials[0];foliage=canonical.materials[1];
        }
        auto& source=result[i];
        source.parts.push_back(AppendGeometry(scene,geometry.parts[0],bark));
        source.parts.push_back(AppendGeometry(scene,geometry.parts[1],foliage));
        source.triangles=(geometry.parts[0].indices.size()+geometry.parts[1].indices.size())/3u;
        assembly.meshes={source.parts[0].mesh,source.parts[1].mesh};
        assembly.materials={bark,foliage};
        for(auto& lod:geometry.lods){
            lod.mesh=assembly.meshes.at(lod.mesh);
            library.lods.push_back(std::move(lod));
        }
        MarkDiagnosticStartupProgress();
    }
    char line[512];
    std::snprintf(line,sizeof(line),
        "arb1_botanical recipes_hits=%llu recipes_misses=%llu recipes_writes=%llu "
        "geometry_hits=%llu geometry_misses=%llu geometry_writes=%llu "
        "atlas_hits=%llu atlas_misses=%llu atlas_writes=%llu read_bytes=%llu written_bytes=%llu total_ms=%llu\r\n",
        (unsigned long long)recipes.hits,(unsigned long long)recipes.misses,(unsigned long long)recipes.writes,
        (unsigned long long)meshes.hits,(unsigned long long)meshes.misses,(unsigned long long)meshes.writes,
        (unsigned long long)atlases.hits,(unsigned long long)atlases.misses,(unsigned long long)atlases.writes,
        (unsigned long long)(recipes.readBytes+meshes.readBytes+atlases.readBytes),
        (unsigned long long)(recipes.writtenBytes+meshes.writtenBytes+atlases.writtenBytes),
        (unsigned long long)(GetTickCount64()-started));
    DiagnosticLine(line);
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
