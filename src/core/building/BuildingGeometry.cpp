#include "BuildingPlan.h"
#include <algorithm>
#include <cmath>

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
        out.BoxAt(x-w,y+.65f,z-d,x+w,y+.78f,z+d,Material::Wood);
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
        result.push_back({l,{}});
        Emit e{result.back(),id+1};
        const Rect footprint{0,0,d.width,d.depth};
        const bool atrium=l>=d.voidStart&&l<=d.voidEnd;
        std::vector<Rect> slabs{footprint};
        if(l>-1){Subtract(slabs,StairOpening);Subtract(slabs,StairAt(EastStairOffset));}
        if(l>-1)Subtract(slabs,Shafts[0]);
        if(l>0)Subtract(slabs,Shafts[1]);
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
        for(size_t i=0;i<bands.size();++i){
            if(atrium&&i<2){
                const auto b=bands[i];
                for(const Rect face:{Rect{b.left,b.front,d.atrium.left,b.back},
                                     Rect{d.atrium.right,b.front,b.right,b.back}}){
                    e.Layer(face,y,y+.82f,Material::Metal);
                    e.Layer(face,y+2.38f,top-d.slab,Material::Metal);
                    for(float x=face.left;x<face.right-.01f;x+=2.25f){
                        e.Layer({x,face.front,std::min(x+.10f,face.right),face.back},
                                y+.82f,y+2.38f,Material::Metal);
                        e.Layer({x+.10f,face.front+.04f,std::min(x+2.25f,face.right),face.back-.04f},
                                y+.82f,y+2.38f,Material::Glass);
                    }
                }
            }else if(i==0&&l==0)e.Opening(bands[i],y,top-d.slab,7.75f,2.3f,Material::Metal);
            else if(i<2&&l>=0){
                e.Layer(bands[i],y,y+.82f,Material::Metal);
                e.Layer(bands[i],y+2.38f,top-d.slab,Material::Metal);
                const auto b=bands[i];
                for(float x=.37f;x<d.width-.8f;x+=2.25f){
                    e.Layer({x,b.front,x+.085f,b.back},y+.82f,y+2.38f,Material::Metal);
                    e.Layer({x+.085f,b.front+.035f,std::min(x+2.25f,d.width-.2f),b.back-.035f},
                            y+.82f,y+2.38f,Material::Glass);
                }
                e.Layer({0,b.front,.37f,b.back},y+.82f,y+2.38f,Material::Metal);
                const float tail=.37f+2.25f*std::ceil((d.width-.8f-.37f)/2.25f);
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
            e.Layer({.62f,6.75f,.72f,11.62f},y,top-d.slab,Material::Concrete);
            e.Layer({5.82f,6.75f,5.96f,11.62f},y,top-d.slab,Material::Concrete);
            e.Layer({.62f,11.47f,5.96f,11.62f},y,top-d.slab,Material::Concrete);
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
        if(l==d.floors-1)e.Layer(footprint,top-d.slab,top,Material::Metal);
        if(l==-1)e.Layer(footprint,y-d.slab-.35f,y-d.slab,Material::Concrete);
        for(const Room& room:floor.rooms){
            const auto b=room.bounds;
            const bool back=b.front>6.f;
            const float door=b.left+.93f;
            const float wallZ=back?b.front:b.back-.11f;
            Emit a{result.back(),id+uint64_t(room.id&0xffffu)*1024};
            a.Opening({b.left,wallZ,b.right+.035f,wallZ+.11f},y,ceiling,door,1.02f,Material::Wall);
            a.BoxAt(b.right,y,b.front,b.right+.035f,ceiling,b.back,Material::Wall);
            const float side=door-.51f;
            a.BoxAt(side,y,back?wallZ-.94f:wallZ,side+.05f,y+2.07f,
                    back?wallZ:wallZ+.94f,Material::Wood,false);
            a.Layer(b,y+.006f,y+.018f,Material::Floor,false);
            a.Layer(b,ceiling,ceiling+.05f,Material::Wall);
            a.BoxAt(b.left+1.62f,ceiling-.08f,(b.front+b.back)*.5f,b.left+2.58f,ceiling,
                    (b.front+b.back)*.5f+.21f,Material::Lamp,false);
            Furnish(a,room,y);
        }
    }
    return result;
}
}
