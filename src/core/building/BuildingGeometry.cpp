#include "BuildingPlan.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace OASIS::Buildings {
namespace {
void Subtract(std::vector<Rect>& areas,Rect hole){
    std::vector<Rect> remaining;
    for(const Rect r:areas){
        const float l=std::max(r.left,hole.left),z=std::max(r.front,hole.front);
        const float u=std::min(r.right,hole.right),w=std::min(r.back,hole.back);
        if(l>=u||z>=w){remaining.push_back(r);continue;}
        if(r.left<l)remaining.push_back({r.left,r.front,l,r.back});
        if(u<r.right)remaining.push_back({u,r.front,r.right,r.back});
        if(r.front<z)remaining.push_back({l,r.front,u,z});
        if(w<r.back)remaining.push_back({l,w,u,r.back});
    }
    areas=std::move(remaining);
}

struct Emit {
    Sector& target;
    uint64_t next;
    float offset{};
    bool facade{};
    void BoxAt(float x,float y,float z,float u,float v,float w,Material material,bool solid=true){
        if(x<u&&y<v&&z<w)target.parts.push_back({next++,{{x+offset,y,z},{u+offset,v,w}},material,solid,0,{},facade});
    }
    void SolidBox(Box box,Material material){
        BoxAt(box.lo.x,box.lo.y,box.lo.z,box.hi.x,box.hi.y,box.hi.z,material);
    }
    void Layer(Rect r,float lower,float upper,Material material,bool solid=true){
        BoxAt(r.left,lower,r.front,r.right,upper,r.back,material,solid);
    }
    void Opening(Rect r,float y,float top,float middle,float width,Material material){
        Layer({r.left,r.front,middle-width*.5f,r.back},y,top,material);
        Layer({middle+width*.5f,r.front,r.right,r.back},y,top,material);
        Layer({middle-width*.5f,r.front,middle+width*.5f,r.back},y+2.12f,top,material);
    }
};

void Stair(Emit& out,float low,float high){
    constexpr int count=10;
    constexpr float tread=.30f;
    const float half=(high-low)*.5f;
    for(int i=0;i<count;++i){
        const float z=StairOpening.front+tread*i;
        const float y=low+half*float(i+1)/count;
        out.BoxAt(.85f,y-.20f,z,3.13f,y,z+tread,Material::Concrete);
        const float reverse=StairOpening.front+tread*(count-i-1);
        const float top=low+half+half*float(i+1)/count;
        out.BoxAt(3.33f,top-.20f,reverse,5.68f,top,reverse+tread,Material::Concrete);
    }
    out.BoxAt(.72f,low+half-.20f,9.998f,5.82f,low+half,11.47f,Material::Concrete);
    out.BoxAt(3.16f,low+half,6.98f,3.30f,high+.62f,10.f,Material::Metal);
}

void Furnish(Emit& out,const Room& room,float y){
    const auto r=room.bounds;
    const float x=(r.left+r.right)*.5f+float(int(room.variant%7)-3)*.15f;
    const float z=(r.front+r.back)*.5f;
    auto table=[&](float w,float d){
        const auto host=out.next;
        out.BoxAt(x-w,y+.65f,z-d,x+w,y+.78f,z+d,Material::Wood);
        out.target.mounts.push_back({host,room.id,{x-w,z-d,x+w,z+d},y+.78f});
        for(float lx:{x-w+.09f,x+w-.18f})
            out.BoxAt(lx,y,z-d+.07f,lx+.09f,y+.65f,z+d-.07f,Material::Metal);
    };
    switch(room.use){
    case Use::Machine:
        out.BoxAt(r.left+.45f,y,r.front+.26f,r.right-.45f,y+1.48f,r.front+1.15f,Material::Metal);
        for(int i=0;i<3;++i){
            const float a=r.left+.55f+float(i)*.8f;
            out.BoxAt(a,y+1.43f,r.front+.45f,a+.13f,y+2.52f,r.front+.58f,Material::Cable,false);
        }
        break;
    case Use::Meeting:
        table(std::min(1.65f,(r.right-r.left)*.29f),.48f);
        for(float side:{-.81f,.81f})for(int i=-1;i<=1;++i){
            const float lx=x+.66f*i;
            out.BoxAt(lx-.21f,y,z+side-.21f,lx+.21f,y+.47f,z+side+.21f,Material::Metal);
        }
        break;
    case Use::Service:
        out.BoxAt(r.left+.25f,y,r.front+.24f,r.left+.85f,y+1.93f,r.back-.35f,Material::Metal);
        break;
    case Use::Penthouse:
        table(.95f,.42f);
        out.BoxAt(r.left+.50f,y,r.front+.26f,r.right-.55f,y+.57f,r.front+.74f,Material::Wood);
        break;
    default:
        table(.86f,.38f);
        if(room.use!=Use::Research)out.target.mounts.back().detailPlaceholder=out.next;
        out.BoxAt(x-.26f,y+.79f,z-.29f,x+.26f,y+1.15f,z-.21f,
                  room.use==Use::Research?Material::Lamp:Material::Metal,false);
        break;
    }
}

void RoomLighting(Emit& out,Rect room,float y,float ceiling,uint64_t seed,bool penthouse,bool small=false){
    const float x=(room.left+room.right)*.5f,z=(room.front+room.back)*.5f;
    const float half=std::min(small?.25f:.48f,(room.right-room.left)*.13f);
    const float depth=small?.065f:.095f;
    // Color and occupancy are stable across regeneration of unrelated rooms.
    const auto colored=penthouse?Material::LampRed:((seed/5u)&1u)?Material::Lamp:Material::LampAmber;
    const auto lens=seed%5u==0u?Material::LampOff:colored;
    const auto host=out.next;
    out.BoxAt(x-half,ceiling-.08f,z-depth,x+half,ceiling,z+depth,lens);
    if(lens!=Material::LampOff)
        out.target.lights.push_back({host,{{room.left,y,room.front},{room.right,ceiling,room.back}},
            {x,ceiling-.11f,z},lens});
}

void Bathroom(Sector& sector,const Definition& d,int level,unsigned wing,uint64_t id,float y,float top,float ceiling){
    const auto room=BathroomBounds(d,wing);
    const float shaft=wing?d.width-2.75f:9.44f;
    const float door=room.right-.55f/d.xScale;
    Emit bath{sector,id+0x20u},pipes{sector,id+0x80u};
    bath.Layer(room,y+.004f,y+.029f,Material::Ceramic);
    bath.Layer({room.left-.09f,room.front,room.left,room.back},y,ceiling,Material::Ceramic);
    bath.Layer({room.right,room.front,room.right+.09f,room.back},y,ceiling,Material::Ceramic);
    bath.Layer({room.left-.09f,room.back,room.right+.09f,room.back+.09f},y,ceiling,Material::Ceramic);
    bath.Opening({room.left,room.front-.09f,room.right,room.front},y,ceiling,door,1.02f/d.xScale,Material::Ceramic);
    bath.Layer(room,ceiling,ceiling+.045f,Material::Ceiling);
    bath.Layer({shaft-.24f,9.70f,shaft+.24f,room.front},ceiling,ceiling+.045f,Material::Ceiling);
    RoomLighting(bath,room,y,ceiling,d.seed+unsigned(level+1)+wing,false,true);
    const float basin=room.right-.37f/d.xScale,drain=room.left+.38f/d.xScale;
    const float fixtureZ=room.back-.34f;
    bath.BoxAt(drain-.20f/d.xScale,y,fixtureZ-.22f,drain+.20f/d.xScale,y+.42f,fixtureZ+.20f,Material::Ceramic);
    bath.BoxAt(drain-.20f/d.xScale,y+.42f,fixtureZ+.05f,drain+.20f/d.xScale,y+.81f,fixtureZ+.28f,Material::Ceramic);
    const auto pedestal=bath.next;
    bath.BoxAt(basin-.13f/d.xScale,y,fixtureZ-.12f,basin+.13f/d.xScale,y+.76f,fixtureZ+.09f,Material::Ceramic);
    bath.BoxAt(basin-.22f/d.xScale,y+.76f,fixtureZ-.20f,basin+.22f/d.xScale,y+.85f,fixtureZ+.18f,Material::Ceramic);
    bath.BoxAt(basin-.027f,y+.85f,fixtureZ+.09f,basin+.027f,y+1.02f,fixtureZ+.15f,Material::Metal);
    sector.mounts.push_back({pedestal,0,{basin-.22f/d.xScale,fixtureZ-.20f,basin+.22f/d.xScale,fixtureZ+.18f},y+.85f});

    constexpr std::array media{ServiceKind::Drain,ServiceKind::ColdWater,ServiceKind::HotWater};
    for(unsigned i=0;i<media.size();++i){
        const auto material=i==0?Material::DrainPipe:Material::WaterPipe;
        const float x=shaft-.13f+.12f*float(i),z=9.55f+.045f*float(i);
        const float radius=i==0?.047f:.023f;
        const float underSlab=y-d.slab-.14f-.067f*float(i);
        const uint64_t main=id+1u+i*8u;
        auto pipe=[&](uint64_t node,uint64_t previous,Box box){
            sector.services.push_back({node,pipes.next,previous,media[i],box});
            pipes.SolidBox(box,material);
        };
        const float below=level==-1?underSlab:y-d.slab-.49f;
        pipe(main,level>-1?main-0x10000u:0,{{x-radius,below,z-radius},{x+radius,top-d.slab-.10f,z+radius}});
        pipe(main+1,main,{{x-radius,underSlab,z},{x+radius,underSlab+radius*2.f,fixtureZ+radius}});
        for(unsigned endpoint=0;endpoint<2;++endpoint){
            if(i==2&&endpoint==0)continue;
            const float fixture=(endpoint==0?drain:basin)+.075f*float(i);
            const auto branch=main+2+endpoint*2;
            pipe(branch,main+1,{{std::min(x,fixture)-radius,underSlab,fixtureZ-radius},
                               {std::max(x,fixture)+radius,underSlab+radius*2.f,fixtureZ+radius}});
            const float height=endpoint==0?.42f:.76f;
            pipe(branch+1,branch,{{fixture-radius,underSlab,fixtureZ-radius},
                                  {fixture+radius,y+height,fixtureZ+radius}});
        }
    }
}
}

Rect BathroomBounds(const Definition& d,unsigned wing){
    return wing?Rect{d.width-4.64f,10.05f,d.width-.32f,11.92f}:
                Rect{8.65f,10.05f,d.atrium.left-.04f,11.92f};
}

std::array<Rect,2> GlyphBands(const Definition& d,int level){
    auto band=[&](float left,float right){return Rect{left,0.f,right,d.depth};};
    const float w=d.width;
    const int row=level*30/d.floors;
    if(level>=0&&d.letter=='S'){
        if(row>=4&&row<=12)return {band(w*.60f,w),{}};
        if(row>=18&&row<=25)return {band(0.f,w*.40f),{}};
    }
    if(level>=0&&d.letter=='A'){
        const float inset=row>=19?std::min(4.1f,float(row-18)*.38f):0.f;
        if(row<=8)return {band(0.f,w*.35f),band(w*.65f,w)};
        if(row>=14&&row<=24)return {band(inset,w*.425f),band(w*.575f,w-inset)};
        return {band(inset,w-inset),{}};
    }
    return {band(0.f,w),{}};
}

Sector BuildSector(const Definition& d,const Plan& plan,int l){
    Sector sector{};
    sector.level=l;
    const auto& floor=*plan.At(l);
    const float y=d.Elevation(l),top=d.Elevation(l+1),ceiling=top-d.slab-d.plenum;
    const uint64_t id=(d.id<<32)|(uint64_t(l+2)<<16);
    Emit e{sector,id+1};
    const Rect footprint{0,0,d.width,d.depth};
    const bool atrium=d.letter=='O'&&d.HasVoid(l);
    std::vector<Rect> slabs{footprint};
    if(l>-1){Subtract(slabs,StairOpening);Subtract(slabs,StairAt(EastStairOffset));}
    if(l>-1)Subtract(slabs,Shafts[0]);
    if(l>0)Subtract(slabs,Shafts[1]);
    if(l>=0)for(float x:{9.44f,d.width-2.75f})
        Subtract(slabs,{x-.23f,9.26f,x+.23f,9.72f});
    if(atrium){
        Subtract(slabs,d.atrium);
        if(l==15)slabs.push_back({d.atrium.left,4.02f,d.atrium.right,6.02f});
    }
    e.facade=true;
    for(const auto r:slabs){
        e.Layer(r,y-d.slab,y,Material::Concrete);
    }
    e.facade=false;
    // Independent ranges preserve existing slab and landing damage IDs.
    if(l>=0){
        Emit joints{sector,id+0xd400u};
        // Firestop the service penetration around the protected risers.
        for(float x:{9.44f,d.width-2.75f})
            joints.Layer({x-.23f,9.26f,x+.23f,9.72f},y-.038f,y,Material::Wall);
        // Bridge the slab edge to the last tread, including the narrow stair wells.
        for(float x:{0.f,EastStairOffset}){
            joints.offset=x;
            joints.Layer({3.33f,6.94f,5.68f,7.012f},y-.20f,y-.002f,Material::Concrete);
        }
    }
    e.facade=true;
    const std::array<Rect,4> bands{{
        {0,0,d.width,.20f},{0,d.depth-.20f,d.width,d.depth},
        {0,.20f,.20f,d.depth-.20f},{d.width-.20f,.20f,d.width,d.depth-.20f}}};
    auto glaze=[&](Rect bay,float from,float to,bool inset){
        const float shift=inset?(bay.front<d.depth*.5f?.25f:-.25f):0.f;
        if(inset){
            const float front=std::min(bay.front,bay.front+shift),back=std::max(bay.back,bay.back+shift);
            e.Layer({bay.left,front,bay.left+.035f,back},from,to,Material::Metal);
            e.Layer({bay.right-.035f,front,bay.right,back},from,to,Material::Metal);
            e.Layer({bay.left,front,bay.right,back},from-.035f,from,Material::Metal);
            e.Layer({bay.left,front,bay.right,back},to,to+.035f,Material::Metal);
        }
        bay.front+=shift;bay.back+=shift;
        e.Layer(bay,from,to,Material::Glass);
    };
    const float bayWidth=d.letter=='I'?3.25f:d.letter=='A'?2.70f:2.25f;
    auto insetS=[&](float x){return d.letter=='S'&&l>=2&&l<d.floors-2&&
        (l<d.floors/2)==(x<d.width*.5f);};
    for(size_t i=0;i<bands.size();++i){
        if(atrium&&i<2){
            const auto b=bands[i];
            for(const Rect face:{Rect{b.left,b.front,d.atrium.left,b.back},
                                 Rect{d.atrium.right,b.front,b.right,b.back}}){
                e.Layer(face,y,y+.82f,Material::Metal);
                e.Layer(face,y+2.38f,top-d.slab,Material::Metal);
                for(float x=face.left;x<face.right-.01f;x+=bayWidth){
                    e.Layer({x,face.front,std::min(x+.10f,face.right),face.back},
                            y+.82f,y+2.38f,Material::Metal);
                    glaze({x+.10f,face.front+.04f,std::min(x+bayWidth,face.right),face.back-.04f},
                          y+.82f,y+2.38f,insetS(x));
                }
            }
        }else if(i==0&&l==0)e.Opening(bands[i],y,top-d.slab,7.75f,2.3f,Material::Metal);
        else if(i<2&&l>=0){
            e.Layer(bands[i],y,y+.82f,Material::Metal);
            e.Layer(bands[i],y+2.38f,top-d.slab,Material::Metal);
            const auto b=bands[i];
            for(float x=.37f;x<d.width-.8f;x+=bayWidth){
                e.Layer({x,b.front,x+.085f,b.back},y+.82f,y+2.38f,Material::Metal);
                glaze({x+.085f,b.front+.035f,std::min(x+bayWidth,d.width-.2f),b.back-.035f},
                      y+.82f,y+2.38f,insetS(x));
            }
            e.Layer({0,b.front,.37f,b.back},y+.82f,y+2.38f,Material::Metal);
            const float tail=.37f+bayWidth*std::ceil((d.width-.8f-.37f)/bayWidth);
            e.Layer({std::min(tail,d.width-.2f),b.front,d.width,b.back},y+.82f,y+2.38f,Material::Metal);
        }else e.Layer(bands[i],y,top-d.slab,Material::Metal);
    }
    if(atrium){
        const auto a=d.atrium;
        const auto lining=d.letter=='O'?Material::Concrete:Material::Glass;
        const float thickness=d.letter=='O'?.20f:.13f;
        for(float x:{a.left-thickness,a.right}){
            if(l==15){
                e.Layer({x,0,x+thickness,4.02f},y,top-d.slab,lining);
                e.Layer({x,6.02f,x+thickness,d.depth},y,top-d.slab,lining);
            }else e.Layer({x,0,x+thickness,d.depth},y,top-d.slab,lining);
        }
        if(l==15){
            for(float z:{4.02f,5.96f})
                e.Layer({a.left,z,a.right,z+.06f},y,y+1.05f,Material::Glass);
        }
    }
    e.facade=false;
    for(float x:{0.f,EastStairOffset}){
        e.offset=x;
        e.Opening({.72f,6.75f,5.82f,6.91f},y,top-d.slab,2.85f,3.7f,Material::Concrete);
        e.BoxAt(.92f,y+.96f,6.60f,1.09f,y+1.53f,6.76f,Material::Safety,false);
        e.BoxAt(1.12f,y,6.76f,1.21f,y+2.10f,7.76f,Material::Metal,false);
        const auto repaired=plan.jointRepairs[size_t(JointKind::StairReturn)];
        for(unsigned side=0;side<2;++side){
            const float left=side?5.82f:.62f,right=side?5.96f:.72f;
            const Box wall{{left+x,y,6.75f},{right+x,top-d.slab,11.62f}};
            const Box rear{{.62f+x,y,11.47f},{5.96f+x,top-d.slab,11.62f}};
            const Box overlap{{left+x,y,11.47f},{right+x,top-d.slab,11.62f}};
            sector.joints.push_back({e.next,0,JointKind::StairReturn,{wall,rear},overlap,repaired!=0});
            // The rear wall owns the full corner. Keep the previous box
            // as the damage identity for already saved cuts.
            e.Layer({left,6.75f,right,11.47f},y,top-d.slab,Material::Concrete);
            if(!repaired){
                auto& sideWall=sector.parts.back();
                sideWall.source=sideWall.id;
                sideWall.original=wall;
            }
        }
        e.Layer({.62f,11.47f,5.96f,11.62f},y,top-d.slab,Material::Concrete);
        Emit trim{sector,id+0xc000u+(x!=0.f?32u:0u),x};
        if(repaired>=uint8_t(PatchQuality::Balanced)){
            trim.BoxAt(.72f,y,11.435f,.752f,y+.09f,11.47f,Material::Wall);
            trim.BoxAt(5.785f,y,11.435f,5.82f,y+.09f,11.47f,Material::Wall);
        }
        if(repaired==uint8_t(PatchQuality::Detailed)){
            trim.BoxAt(.72f,y+.09f,11.452f,.736f,y+.115f,11.47f,Material::Wall);
            trim.BoxAt(5.802f,y+.09f,11.452f,5.82f,y+.115f,11.47f,Material::Wall);
        }
        if(l+1<d.floors)Stair(e,y,top);
        else e.Layer(StairOpening,y-.15f,y,Material::Concrete);
    }
    e.offset=0;
    for(unsigned bank=0;bank<Shafts.size();++bank){
        if(bank&&l<0)continue;
        const auto r=Shafts[bank];
        e.Layer({r.left,r.front,r.left+.12f,r.back},y,top,Material::Concrete);
        e.Layer({r.right-.12f,r.front,r.right,r.back},y,top,Material::Concrete);
        e.Layer({r.left,r.back-.12f,r.right,r.back},y,top,Material::Concrete);
        if(Serves(bank,l,d.floors))
            e.Opening({r.left,r.front,r.right,r.front+.12f},y,top,(r.left+r.right)*.5f,1.43f,Material::Metal);
        else e.Layer({r.left,r.front,r.right,r.front+.12f},y,top,Material::Metal);
    }
    // Keep the inspection strip, lighting and floor finish within each wing.
    for(const Rect hall:atrium?std::array<Rect,2>{{
             {.22f,3.17f,d.atrium.left,5.66f},{d.atrium.right,3.17f,d.width-.22f,5.66f}}}:
             std::array<Rect,2>{{{.22f,3.17f,d.width-.22f,5.66f},{}}}){
        if(hall.left>=hall.right)continue;
        e.Layer({hall.left,4.12f,hall.right,5.65f},ceiling,ceiling+.045f,Material::Ceiling);
        e.Layer({hall.left,3.97f,hall.right,4.10f},ceiling+.11f,ceiling+.22f,Material::Cable,false);
        e.Layer({hall.left,3.73f,hall.right,3.94f},ceiling+.23f,ceiling+.40f,Material::Metal,false);
        e.Layer(hall,y+.012f,y+.031f,Material::Floor,false);
        for(float x=hall.left+1.25f;x+1.16f<hall.right;x+=9.f)
            e.BoxAt(x,ceiling-.09f,4.91f,x+1.16f,ceiling,5.15f,Material::Lamp,false);
    }
    for(unsigned wing=0;wing<2;++wing){
        Bathroom(sector,d,l,wing,id+0xe400u+wing*0x100u,y,top,ceiling);
        const float x=wing?d.width-2.75f:9.44f;
        const uint64_t serviceBase=id+0xe000u+wing*0x100u;
        const uint64_t chase=serviceBase+1;
        sector.services.push_back({chase,0,l>-1?chase-0x10000u:0,ServiceKind::Chase,
            {{x-.23f,y,9.26f},{x+.23f,top,9.72f}}});
        const float end=wing?d.width-.43f:.49f;
        const Rect route{std::min(x,end),3.32f,std::max(x,end),3.63f};
        sector.services.push_back({serviceBase+2,0,chase,ServiceKind::Plenum,
            {{route.left,ceiling,3.18f},{route.right,top-d.slab,4.10f}}});
        Emit utility{sector,serviceBase+32};
        utility.BoxAt(x-.29f,y,9.20f,x-.23f,top,9.78f,Material::Concrete);
        utility.BoxAt(x+.23f,y,9.20f,x+.29f,top,9.78f,Material::Concrete);
        utility.BoxAt(x-.23f,y,9.72f,x+.23f,top,9.78f,Material::Concrete);
        // The raised branch feeds through the top of the enclosure.
        utility.BoxAt(x-.23f,y,9.20f,x+.23f,ceiling+.06f,9.26f,Material::Concrete);
        constexpr std::array media{ServiceKind::Air,ServiceKind::Data,ServiceKind::Power,ServiceKind::Fluid};
        for(unsigned i=0;i<media.size();++i){
            const float z=3.31f+.13f*float(i);
            const float thickness=i==0?.11f:.046f;
            const float raised=ceiling+.09f+float(i)*.065f;
            const Box riser{{x-.042f+float(i)*.04f,y,9.43f},{x-.015f+float(i)*.04f,top,9.50f}};
            const Box run{{std::min(route.left,riser.lo.x),raised,z},
                          {std::max(route.right,riser.hi.x),raised+thickness,z+thickness}};
            const Box branch{{riser.lo.x,raised,z},{riser.hi.x,raised+thickness,riser.hi.z}};
            const auto trunk=serviceBase+3+i*3;
            sector.services.push_back({trunk,utility.next,l>-1?trunk-0x10000u:0,media[i],riser});
            utility.SolidBox(riser,i==0?Material::Metal:Material::Cable);
            sector.services.push_back({trunk+1,utility.next,trunk,media[i],branch});
            utility.SolidBox(branch,i==0?Material::Metal:Material::Cable);
            sector.services.push_back({trunk+2,utility.next,trunk+1,media[i],run});
            utility.SolidBox(run,i==0?Material::Metal:Material::Cable);
        }
        const float vent=wing?d.width-4.25f:4.08f;
        const Box frame{{vent,ceiling-.05f,3.24f},{vent+.42f,ceiling+.02f,3.75f}};
        const Box drop{{vent+.10f,ceiling-.03f,3.31f},{vent+.34f,ceiling+.20f,3.63f}};
        sector.services.push_back({serviceBase+15,utility.next,serviceBase+5,ServiceKind::Air,drop});
        utility.BoxAt(drop.lo.x,drop.lo.y,drop.lo.z,drop.hi.x,drop.hi.y,drop.lo.z+.026f,Material::Metal);
        utility.BoxAt(drop.lo.x,drop.lo.y,drop.hi.z-.026f,drop.hi.x,drop.hi.y,drop.hi.z,Material::Metal);
        sector.services.push_back({serviceBase+16,utility.next,serviceBase+15,ServiceKind::Grille,frame});
        for(unsigned slot=0;slot<6;++slot){
            const float at=frame.lo.x+.07f*float(slot);
            utility.BoxAt(at,frame.lo.y,frame.lo.z,at+.028f,frame.hi.y,frame.hi.z,Material::Metal);
        }
    }
    if(l==-1){
        const uint64_t idChamber=id+0xe200u;
        const auto shaft=Shafts[0];
        const Box cavity{{shaft.left+.12f,y,shaft.back},
                         {shaft.right-.12f,ceiling,11.32f}};
        const auto [lo,hi]=cavity;
        Emit wall{sector,idChamber+2};
        wall.BoxAt(lo.x-.12f,y,lo.z,lo.x,ceiling,hi.z+.12f,Material::Concrete);
        wall.BoxAt(hi.x,y,lo.z,hi.x+.12f,ceiling,hi.z+.12f,Material::Concrete);
        wall.BoxAt(lo.x,y,hi.z,hi.x,ceiling,hi.z+.12f,Material::Concrete);
        wall.Layer({lo.x-.12f,lo.z,hi.x+.12f,hi.z+.12f},ceiling,ceiling+.12f,Material::Concrete);
        sector.services.push_back({idChamber+1,idChamber+2,id+0xe001u,ServiceKind::Chamber,cavity});
    }
    e.facade=true;
    if(l==d.floors-1)e.Layer(footprint,top-d.slab,top,Material::Metal);
    if(l==-1)e.Layer(footprint,y-d.slab-.35f,y-d.slab,Material::Concrete);
    for(Room room:floor.rooms){
        if(d.letter=='A'||d.letter=='S'){
            auto retained=room.bounds;
            float width=0.f;
            for(const Rect band:GlyphBands(d,l)){
                const float left=std::max(room.bounds.left,band.left),right=std::min(room.bounds.right,band.right);
                if(right-left>width){width=right-left;retained.left=left;retained.right=right;}
            }
            if(width*d.xScale<1.65f)continue;
            room.bounds=retained;
        }
        const auto b=room.bounds;
        const bool back=b.front>6.f;
        const float door=b.left+std::max(.93f,.65f/d.xScale);
        const float wallZ=back?b.front:b.back-.11f;
        const auto roomBase=id+uint64_t(room.id&0xffffu)*1024;
        Emit a{sector,roomBase},fittings{sector,roomBase+512};
        const bool penthouse=room.use==Use::Penthouse;
        const auto tenant=d.seed+unsigned(std::max(0,l)/3)*3u+unsigned(b.left>d.width*.5f);
        constexpr std::array palette{Material::Sage,Material::Sand,Material::Mist,Material::Lilac};
        const auto wall=penthouse?Material::Cherry:palette[tenant%palette.size()];
        // The perpendicular partition owns the last strip of the jamb.
        const auto first=sector.parts.size();
        a.Opening({b.left,wallZ,b.right+.035f,wallZ+.11f},y,ceiling,door,1.02f/d.xScale,wall);
        for(size_t i=first;i<sector.parts.size();++i){
            auto& jamb=sector.parts[i];
            if(jamb.bounds.hi.x<=b.right)continue;
            jamb.source=jamb.id;
            jamb.original=jamb.bounds;
            jamb.bounds.hi.x=b.right;
        }
        a.BoxAt(b.right,y,b.front,b.right+.035f,ceiling,b.back,wall);
        const float edge=back?d.depth-.20f:.20f;
        const float end=back?b.back:b.front;
        const float low=std::min(end,edge),high=std::max(end,edge);
        const Box seam{{b.right,y,low},{b.right+.035f,ceiling,high}};
        const auto repaired=plan.jointRepairs[size_t(JointKind::PartitionFacade)];
        const Box partition{{b.right,y,b.front},{b.right+.035f,ceiling,b.back}};
        const Box facade{{b.right-.18f,y,back?edge:0.f},
                         {b.right+.215f,y+.82f,back?d.depth:edge}};
        if(l>=0&&!(l==0&&!back&&b.right>=6.58f&&b.right<=8.92f)){
            sector.joints.push_back({a.next-1,room.id,JointKind::PartitionFacade,
                {partition,facade},seam,repaired!=0});
            // Separate range keeps furniture and wall IDs across a repair.
            Emit trim{sector,id+uint64_t(room.id&0xffffu)*1024+768};
            if(repaired)trim.SolidBox(seam,wall);
            if(repaired>=uint8_t(PatchQuality::Balanced))
                trim.BoxAt(b.right-.018f,y,low,b.right, y+.09f,high,Material::Wood);
            if(repaired==uint8_t(PatchQuality::Detailed))
                trim.BoxAt(b.right-.009f,y+.09f,low,b.right,y+.112f,high,Material::Wood);
        }
        const float side=door-.51f/d.xScale;
        a.BoxAt(side,y,back?wallZ-.94f:wallZ,side+.05f,y+2.07f,
                back?wallZ:wallZ+.94f,Material::Wood,false);
        const auto floorHost=a.next;
        const auto carpet=penthouse?Material::VioletCarpet:
            (tenant&1u)?Material::BlueCarpet:Material::GreyCarpet;
        a.Layer(b,y+.006f,y+.018f,l<0?Material::Floor:carpet,false);
        sector.mounts.push_back({floorHost,room.id,b,y+.018f});
        a.Layer(b,ceiling,ceiling+.05f,penthouse?Material::BlackCeiling:Material::Ceiling);
        RoomLighting(a,b,y,ceiling,d.seed^room.variant^room.id,penthouse);
        if(l>=0&&!penthouse&&(tenant+room.variant)%4u==0){
            fittings.BoxAt(b.right-.026f,y+.972f,b.front,b.right,y+1.028f,b.back,Material::Moulding);
            const float face=back?wallZ+.11f:wallZ;
            fittings.BoxAt(door+.51f/d.xScale,y+.972f,back?face:face-.026f,
                b.right,y+1.028f,back?face+.026f:face,Material::Moulding);
        }
        Furnish(a,room,y);
    }
    if(d.letter=='A'||d.letter=='S'){
        const auto bands=GlyphBands(d,sector.level);
        const float y=d.Elevation(sector.level),top=d.Elevation(sector.level+1);
        const uint64_t root=(d.id<<32)|(uint64_t(sector.level+2)<<16);
        std::vector<Part> clipped;
        clipped.reserve(sector.parts.size());
        uint64_t split=root+0xa000u;
        std::unordered_map<uint64_t,Box> hosts;
        for(const auto& source:sector.parts){
            bool first=true;
            for(const Rect band:bands){
                const auto original=source.source?source.original:source.bounds;
                const float originalLeft=std::max(band.left,original.lo.x);
                const float originalRight=std::min(band.right,original.hi.x);
                if(originalRight-originalLeft<.001f)continue;
                auto part=source;
                if(!first)part.id=split++;
                first=false;
                part.bounds.lo.x=std::max(band.left,source.bounds.lo.x);
                part.bounds.hi.x=std::min(band.right,source.bounds.hi.x);
                if(part.bounds.hi.x-part.bounds.lo.x<.001f)continue;
                if(part.source){
                    part.source=part.id;
                    part.original.lo.x=originalLeft;
                    part.original.hi.x=originalRight;
                    if(part.original==part.bounds){
                        part.source=0;
                        part.original={};
                    }
                }
                clipped.push_back(part);
                hosts.emplace(part.id,part.bounds);
            }
        }
        sector.parts=std::move(clipped);
        std::erase_if(sector.lights,[&](RoomLight& light){
            const auto host=hosts.find(light.host);
            if(host==hosts.end())return true;
            const float x=(host->second.lo.x+host->second.hi.x)*.5f;
            for(const Rect band:bands)if(x>=band.left&&x<band.right){
                light.room.lo.x=std::max(light.room.lo.x,band.left);
                light.room.hi.x=std::min(light.room.hi.x,band.right);
                light.position.x=x;
                return x<light.room.lo.x||x>light.room.hi.x;
            }
            return true;
        });
        std::erase_if(sector.services,[&](const ServiceNode& node){return node.host&&!hosts.contains(node.host);});
        std::erase_if(sector.mounts,[&](const Mount& mount){return !hosts.contains(mount.host);});
        std::erase_if(sector.joints,[&](const Joint& joint){
            return std::none_of(bands.begin(),bands.end(),[&](Rect band){
                return band.left<band.right&&joint.seam.lo.x>=band.left&&joint.seam.hi.x<=band.right;
            });
        });
        Emit skin{sector,root+0xb000u,0.f,true};
        for(const Rect band:bands){
            if(band.left>=band.right)continue;
            if(band.left>0.f)skin.Layer({band.left,.20f,band.left+.16f,d.depth-.20f},y,top-d.slab,Material::Metal);
            if(band.right<d.width)skin.Layer({band.right-.16f,.20f,band.right,d.depth-.20f},y,top-d.slab,Material::Metal);
            if(sector.level>=d.floors-1)continue;
            std::vector<Rect> terrace{band};
            for(const Rect above:GlyphBands(d,sector.level+1))if(above.left<above.right)Subtract(terrace,above);
            for(const Rect roof:terrace)skin.Layer(roof,top-d.slab,top,Material::Concrete);
        }
    }
    return sector;
}

namespace {
uint64_t FragmentId(uint64_t solid,const Box& bounds){
    uint64_t hash=14695981039346656037ull;
    const auto mix=[&](uint64_t value){hash=(hash^value)*1099511628211ull;};
    mix(solid);
    for(float coordinate:{bounds.lo.x,bounds.lo.y,bounds.lo.z,bounds.hi.x,bounds.hi.y,bounds.hi.z})
        mix(std::bit_cast<uint32_t>(coordinate==0.f?0.f:coordinate));
    // Generated source and moving parts never use the lower word's high nibble 8.
    return (hash&0xffffffff0fffffffull)|0x80000000u;
}
}

void ApplyDamage(const Definition& d,Sector& sector,std::span<const Damage> damage,
                 const Sector* previous,std::span<const uint64_t> changed){
    if(damage.empty())return;
    std::unordered_map<uint64_t,std::vector<const Damage*>> bySolid;
    std::vector<const Damage*> slabCuts;
    for(const auto& cut:damage){
        bySolid[cut.part].push_back(&cut);
        if(cut.original.hi.y!=d.Elevation(sector.level))continue;
        if(std::any_of(sector.parts.begin(),sector.parts.end(),[&](const Part& part){
            return part.id==cut.part&&part.bounds==cut.original&&part.material==Material::Concrete;
        }))slabCuts.push_back(&cut);
    }
    std::unordered_map<uint64_t,std::vector<const Part*>> previousBySource;
    if(previous)for(const auto& part:previous->parts)
        previousBySource[part.source?part.source:part.id].push_back(&part);
    const bool changedSlab=std::any_of(slabCuts.begin(),slabCuts.end(),[&](const Damage* cut){
        return std::find(changed.begin(),changed.end(),cut->part)!=changed.end();
    });
    std::vector<Part> fragments;
    fragments.reserve(previous?previous->parts.size():sector.parts.size());
    for(const auto& source:sector.parts){
        const bool finish=!source.solid&&source.bounds.lo.y>=d.Elevation(sector.level)&&
            source.bounds.hi.y<=d.Elevation(sector.level)+.032f;
        const auto original=source.source?source.original:source.bounds;
        if(previous&&!(finish&&changedSlab)&&
           std::find(changed.begin(),changed.end(),source.id)==changed.end()){
            if(auto old=previousBySource.find(source.id);old!=previousBySource.end())
                for(const auto* part:old->second)fragments.push_back(*part);
            continue;
        }
        std::vector<Box> openings;
        if(const auto cuts=bySolid.find(source.id);cuts!=bySolid.end())
            for(const auto* cut:cuts->second)if(cut->original==original)openings.push_back(cut->opening);
        if(finish)for(const auto* cut:slabCuts){
            const auto& h=cut->opening;
            const auto& b=cut->original;
            const Box hole{{std::max(h.lo.x,b.lo.x),source.bounds.lo.y,std::max(h.lo.z,b.lo.z)},
                           {std::min(h.hi.x,b.hi.x),source.bounds.hi.y,std::min(h.hi.z,b.hi.z)}};
            if(hole.lo.x<source.bounds.hi.x&&hole.hi.x>source.bounds.lo.x&&
               hole.lo.z<source.bounds.hi.z&&hole.hi.z>source.bounds.lo.z)
                openings.push_back(hole);
        }
        if(openings.empty()){fragments.push_back(source);continue;}
        std::vector<Box> pieces{source.bounds};
        for(const auto& h:openings){
            std::vector<Box> remnant;
            for(const auto b:pieces){
                const Point l{std::max(b.lo.x,h.lo.x),std::max(b.lo.y,h.lo.y),std::max(b.lo.z,h.lo.z)};
                const Point u{std::min(b.hi.x,h.hi.x),std::min(b.hi.y,h.hi.y),std::min(b.hi.z,h.hi.z)};
                if(l.x>=u.x||l.y>=u.y||l.z>=u.z){remnant.push_back(b);continue;}
                auto keep=[&](Point lo,Point hi){
                    if(hi.x-lo.x>.0005f&&hi.y-lo.y>.0005f&&hi.z-lo.z>.0005f)
                        remnant.push_back({lo,hi});
                };
                keep(b.lo,{l.x,b.hi.y,b.hi.z});
                keep({u.x,b.lo.y,b.lo.z},b.hi);
                keep({l.x,b.lo.y,b.lo.z},{u.x,l.y,b.hi.z});
                keep({l.x,u.y,b.lo.z},{u.x,b.hi.y,b.hi.z});
                keep({l.x,l.y,b.lo.z},{u.x,u.y,l.z});
                keep({l.x,l.y,u.z},{u.x,u.y,b.hi.z});
            }
            pieces=std::move(remnant);
        }
        for(const auto b:pieces){
            fragments.push_back({FragmentId(source.id,b),b,source.material,source.solid,source.id,original,source.facade});
        }
    }
    sector.parts=std::move(fragments);
}

void BuildContactIndex(Sector& sector){
    sector.contactNodes.clear();
    auto& parts=sector.contactParts;
    parts.resize(sector.parts.size());
    std::iota(parts.begin(),parts.end(),uint32_t{0});
    if(parts.empty())return;
    const auto partBounds=[&](uint32_t index)->const Box&{return sector.parts[index].bounds;};
    const auto build=[&](auto&& self,uint32_t first,uint32_t end)->uint32_t{
        Box bounds=partBounds(parts[first]);
        for(uint32_t i=first+1;i<end;++i){
            const auto& p=partBounds(parts[i]);
            bounds.lo={std::min(bounds.lo.x,p.lo.x),std::min(bounds.lo.y,p.lo.y),std::min(bounds.lo.z,p.lo.z)};
            bounds.hi={std::max(bounds.hi.x,p.hi.x),std::max(bounds.hi.y,p.hi.y),std::max(bounds.hi.z,p.hi.z)};
        }
        const auto node=uint32_t(sector.contactNodes.size());
        sector.contactNodes.push_back({bounds,first,end-first,0});
        if(end-first<=8u)return node;
        const float x=bounds.hi.x-bounds.lo.x,y=bounds.hi.y-bounds.lo.y,z=bounds.hi.z-bounds.lo.z;
        const auto axis=x>=std::max(y,z)?&Point::x:y>=z?&Point::y:&Point::z;
        const auto middle=first+(end-first)/2u;
        std::nth_element(parts.begin()+first,parts.begin()+middle,parts.begin()+end,[&](uint32_t a,uint32_t b){
            const auto& left=partBounds(a);
            const auto& right=partBounds(b);
            const auto p=left.lo.*axis+left.hi.*axis,q=right.lo.*axis+right.hi.*axis;
            return p!=q?p<q:a<b;
        });
        self(self,first,middle);
        const auto right=self(self,middle,end);
        sector.contactNodes[node].right=right;
        sector.contactNodes[node].count=0;
        return node;
    };
    build(build,0,uint32_t(parts.size()));
}

int DamageLevel(const Damage& damage){
    return int((damage.part>>16)&0xffu)-2;
}

std::vector<Sector> Build(const Definition& d,const Plan& plan){
    std::vector<std::vector<Damage>> damageByLevel(size_t(d.floors+1));
    for(const auto& damage:plan.damage){
        const int level=DamageLevel(damage);
        if(level>=-1&&level<d.floors)damageByLevel[size_t(level+1)].push_back(damage);
    }
    std::vector<Sector> result;
    result.reserve(size_t(d.floors+1));
    for(int level=-1;level<d.floors;++level){
        auto& sector=result.emplace_back(BuildSector(d,plan,level));
        ApplyDamage(d,sector,damageByLevel[size_t(level+1)]);
        BuildContactIndex(sector);
    }
    return result;
}
}
