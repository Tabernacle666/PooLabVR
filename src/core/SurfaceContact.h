#pragma once
#include "Core.h"
#include <algorithm>
#include <optional>

namespace OASIS {
struct SurfaceCrossing {
    XMFLOAT3 position;
    bool entering;
};
struct SurfaceStroke {
    XMFLOAT3 start,end;
    float strength;
};
struct SurfaceContactSample {
    std::optional<SurfaceCrossing> crossing;
    std::optional<SurfaceStroke> stroke;
};

struct SurfaceContactTuning {
    float hysteresis{.025f},band{.26f},maxStrokeLength{.38f};
    double strokeLifetime{.42};
};

class SurfaceContact {
    XMFLOAT3 previous_{},anchor_{},tail_{},head_{};
    double lastTime_{},lastMotion_{};
    float strokeStrength_{};
    int side_{};
public:
    void Reset(){*this={};}

    // Query supplies signed distance (negative inside) and closest surface point.
    template<class Surface>
    SurfaceContactSample Update(XMFLOAT3 point,double time,const Surface& surface,
                                const SurfaceContactTuning& tuning={}){
        const auto [hysteresis,band,maxLength,fadeTime]=tuning;
        SurfaceContactSample result;
        const float distance=surface.Distance(point);
        const auto projected=surface.Project(point);
        const double dt=time-lastTime_;
        if(!side_||dt<=0||dt>.15||Length(Sub(point,previous_))>.75f){
            Reset();
            side_=distance<0.f?-1:1;
            previous_=anchor_=point;
            tail_=head_=projected;
            lastTime_=time;
            return result;
        }

        const int confirmed=distance>hysteresis?1:distance<-hysteresis?-1:0;
        if(confirmed&&confirmed!=side_){
            auto first=anchor_,last=point;
            for(int step=0;step<8;++step){
                const auto middle=Scale(Add(first,last),.5f);
                if((surface.Distance(middle)<0.f)==(side_<0))first=middle;
                else last=middle;
            }
            result.crossing=SurfaceCrossing{surface.Project(Scale(Add(first,last),.5f)),confirmed<0};
            side_=confirmed;
        }
        if(confirmed)anchor_=point;
        const bool sliding=std::abs(distance)<=band || std::abs(surface.Distance(previous_))<=band;
        const float moved=Length(Sub(projected,head_));
        if(sliding&&moved>.0006f){
            if(time-lastMotion_>=fadeTime)tail_=head_;
            tail_=surface.Project(Add(tail_,Scale(Sub(head_,tail_),std::clamp(float(dt)*9.f,0.f,1.f))));
            const auto offset=Sub(tail_,projected);
            if(Length(offset)>maxLength)tail_=surface.Project(Add(projected,Scale(Unit(offset),maxLength)));
            head_=projected;
            strokeStrength_=(.58f+.42f*std::clamp(1.f-std::abs(distance)/band,0.f,1.f))*
                std::clamp(moved/float(dt)*8.f,.22f,1.f);
            lastMotion_=time;
        }else if(!sliding&&time-lastMotion_>=fadeTime){
            tail_=head_=projected;
        }
        const float fade=std::clamp(float(1.-(time-lastMotion_)/fadeTime),0.f,1.f);
        if(fade>0.f&&strokeStrength_>0.f)result.stroke=SurfaceStroke{tail_,head_,fade*strokeStrength_};
        previous_=point;
        lastTime_=time;
        return result;
    }
};
}
