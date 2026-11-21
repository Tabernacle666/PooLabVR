#include "BuildingPlan.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

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
    void BoxAt(float x,float y,float z,float u,float v,float w,Material material,bool solid=true){
        if(x<u&&y<v&&z<w)target.parts.push_back({next++,{{x+offset,y,z},{u+offset,v,w}},material,solid});
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
    out.BoxAt(.72f,low+half-.20f,10.f,5.82f,low+half,11.45f,Material::Concrete);
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
        out.BoxAt(x-.26f,y+.79f,z-.29f,x+.26f,y+1.15f,z-.21f,
                  room.use==Use::Research?Material::Lamp:Material::Metal,false);
        break;
    }
}
}

std::vector<Sector> Build(const Definition& d,const Plan& plan){
    std::vector<Sector> result;
    for(int l=-1;l<d.floors;++l){
        const auto& floor=*plan.At(l);
        const float y=d.Elevation(l),top=d.Elevation(l+1),ceiling=top-d.slab-d.plenum;
        const uint64_t id=(d.id<<32)|(uint64_t(l+2)<<16);
        result.emplace_back().level=l;
        Emit e{result.back(),id+1};
        const Rect footprint{0,0,d.width,d.depth};
        const bool atrium=d.HasVoid(l);
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
        for(const auto r:slabs){
            e.Layer(r,y-d.slab,y,Material::Concrete);
        }
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
            for(float x:{a.left-.13f,a.right}){
                if(l==15){
                    e.Layer({x,0,x+.13f,4.02f},y,top-d.slab,Material::Glass);
                    e.Layer({x,6.02f,x+.13f,d.depth},y,top-d.slab,Material::Glass);
                }else e.Layer({x,0,x+.13f,d.depth},y,top-d.slab,Material::Glass);
            }
            if(l==15){
                for(float z:{4.02f,5.96f})
                    e.Layer({a.left,z,a.right,z+.06f},y,y+1.05f,Material::Glass);
            }
        }
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
                result.back().joints.push_back({e.next,0,JointKind::StairReturn,{wall,rear},overlap,repaired!=0});
                e.Layer({left,6.75f,right,repaired?11.47f:11.62f},y,top-d.slab,Material::Concrete);
            }
            e.Layer({.62f,11.47f,5.96f,11.62f},y,top-d.slab,Material::Concrete);
            Emit trim{result.back(),id+0xc000u+(x!=0.f?32u:0u),x};
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
            e.Layer({hall.left,4.12f,hall.right,5.65f},ceiling,ceiling+.045f,Material::Wall);
            e.Layer({hall.left,3.97f,hall.right,4.10f},ceiling+.11f,ceiling+.22f,Material::Cable,false);
            e.Layer({hall.left,3.73f,hall.right,3.94f},ceiling+.23f,ceiling+.40f,Material::Metal,false);
            e.Layer(hall,y+.012f,y+.031f,Material::Floor,false);
            for(float x=hall.left+1.25f;x+1.16f<hall.right;x+=4.5f)
                e.BoxAt(x,ceiling-.09f,4.91f,x+1.16f,ceiling,5.15f,Material::Lamp,false);
        }
        auto& sector=result.back();
        for(unsigned wing=0;wing<2;++wing){
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
        if(l==d.floors-1)e.Layer(footprint,top-d.slab,top,Material::Metal);
        if(l==-1)e.Layer(footprint,y-d.slab-.35f,y-d.slab,Material::Concrete);
        for(const Room& room:floor.rooms){
            const auto b=room.bounds;
            const bool back=b.front>6.f;
            const float door=b.left+std::max(.93f,.65f/d.xScale);
            const float wallZ=back?b.front:b.back-.11f;
            Emit a{result.back(),id+uint64_t(room.id&0xffffu)*1024};
            a.Opening({b.left,wallZ,b.right+.035f,wallZ+.11f},y,ceiling,door,1.02f/d.xScale,Material::Wall);
            a.BoxAt(b.right,y,b.front,b.right+.035f,ceiling,b.back,Material::Wall);
            const float edge=back?d.depth-.20f:.20f;
            const float end=back?b.back:b.front;
            const float low=std::min(end,edge),high=std::max(end,edge);
            const Box seam{{b.right,y,low},{b.right+.035f,ceiling,high}};
            const auto repaired=plan.jointRepairs[size_t(JointKind::PartitionFacade)];
            const Box partition{{b.right,y,b.front},{b.right+.035f,ceiling,b.back}};
            const Box facade{{b.right-.18f,y,back?edge:0.f},
                             {b.right+.215f,y+.82f,back?d.depth:edge}};
            if(l>=0&&!(l==0&&!back&&b.right>=6.58f&&b.right<=8.92f)){
                result.back().joints.push_back({a.next-1,room.id,JointKind::PartitionFacade,
                    {partition,facade},seam,repaired!=0});
                // Separate range keeps furniture and wall IDs across a repair.
                Emit trim{result.back(),id+uint64_t(room.id&0xffffu)*1024+768};
                if(repaired)trim.SolidBox(seam,Material::Wall);
                if(repaired>=uint8_t(PatchQuality::Balanced))
                    trim.BoxAt(b.right-.018f,y,low,b.right, y+.09f,high,Material::Wood);
                if(repaired==uint8_t(PatchQuality::Detailed))
                    trim.BoxAt(b.right-.009f,y+.09f,low,b.right,y+.112f,high,Material::Wood);
            }
            const float side=door-.51f/d.xScale;
            a.BoxAt(side,y,back?wallZ-.94f:wallZ,side+.05f,y+2.07f,
                    back?wallZ:wallZ+.94f,Material::Wood,false);
            const auto floorHost=a.next;
            a.Layer(b,y+.006f,y+.018f,Material::Floor,false);
            result.back().mounts.push_back({floorHost,room.id,b,y+.018f});
            a.Layer(b,ceiling,ceiling+.05f,Material::Wall);
            a.BoxAt(b.left+1.62f,ceiling-.08f,(b.front+b.back)*.5f,b.left+2.58f,ceiling,
                    (b.front+b.back)*.5f+.21f,Material::Lamp,false);
            Furnish(a,room,y);
        }
    }
    for(auto& sector:result){
        if(plan.damage.empty())break;
        std::vector<Part> fragments;
        uint64_t next=(d.id<<32)|(uint64_t(sector.level+2)<<16)|0xf000u;
        for(const auto& source:sector.parts){
            std::vector<Box> pieces{source.bounds};
            bool edited=false;
            for(const auto& cut:plan.damage){
                if(cut.part!=source.id||cut.original!=source.bounds)continue;
                edited=true;
                std::vector<Box> remnant;
                for(const auto b:pieces){
                    const auto& h=cut.opening;
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
            if(!edited){fragments.push_back(source);continue;}
            for(const auto b:pieces){
                if((next&0xffffu)==0xffffu)throw std::runtime_error("Building fragment capacity");
                fragments.push_back({next++,b,source.material,source.solid,source.id,source.bounds});
            }
        }
        sector.parts=std::move(fragments);
    }
    return result;
}
}
