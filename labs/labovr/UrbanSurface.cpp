#include "UrbanSurface.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace lab::urban {
namespace {
constexpr double Pi=3.1415926535897932384626433832795;
constexpr double Pitch=BlockMeters/double(SurfaceTile::Resolution);
constexpr std::array<double,2> Omega{2.*Pi/1200.,2.*Pi/420.};
constexpr std::array<double,2> SnowAmplitude{
    WinterSnowfallMetersPerSecond*.48,WinterSnowfallMetersPerSecond*.26};
constexpr double RainRate=.000000035;
constexpr std::array<double,2> RainAmplitude{RainRate*.58,RainRate*.22};
constexpr std::array<double,8> SnowLoss{
    .0000015,.000010,.000035,.000100,.000300,.000750,.001800,.004500};
constexpr std::array<double,8> WaterLoss{
    .008,.012,.019,.029,.044,.068,.105,.180};

double Phase(unsigned wave,Point p){
    return wave==0?p.x*.012+p.z*.004+.37:-p.x*.029+p.z*.017+1.77;
}
double RainPhase(unsigned wave,Point p){
    return Phase(wave,p)+(wave==0?1.93:-2.2);
}
double Decay(double k,double t){return std::exp(-k*t);}
double Constant(double k,double t){return -std::expm1(-k*t)/k;}
// Exact response of dX/dt = sin(w*t+phase)-k*X.
double SinResponse(double k,double w,double phase,double t){
    const double s0=std::sin(phase),c0=std::cos(phase);
    const double s=std::sin(w*t+phase),c=std::cos(w*t+phase);
    return (k*s-w*c-Decay(k,t)*(k*s0-w*c0))/(k*k+w*w);
}
double CosResponse(double k,double w,double phase,double t){
    const double s0=std::sin(phase),c0=std::cos(phase);
    const double s=std::sin(w*t+phase),c=std::cos(w*t+phase);
    return (k*c+w*s-Decay(k,t)*(k*c0+w*s0))/(k*k+w*w);
}
double Transient(double ks,double kw,double t){
    if(std::abs(kw-ks)<1.e-9)return t*Decay(ks,t);
    return (Decay(ks,t)-Decay(kw,t))/(kw-ks);
}
// Exact two-reservoir cascade: snow -> meltwater -> drainage.
double MeltWave(double ks,double kw,double w,double phase,double t){
    const double a=ks*SinResponse(kw,w,phase,t);
    const double b=w*CosResponse(kw,w,phase,t);
    const double c=ks*std::sin(phase)-w*std::cos(phase);
    return ks*(a-b-c*Transient(ks,kw,t))/(ks*ks+w*w);
}
struct State {double snow{},water{};};
State Evaluate(double s0,double w0,double ks,double kw,Point p,double t){
    const double g=Transient(ks,kw,t);
    State state{
        s0*Decay(ks,t)+WinterSnowfallMetersPerSecond*Constant(ks,t),
        w0*Decay(kw,t)+ks*s0*g+
            WinterSnowfallMetersPerSecond*(Constant(kw,t)-g)+
            RainRate*Constant(kw,t)
    };
    for(unsigned j=0;j<Omega.size();++j){
        const double phase=Phase(j,p),rain=RainPhase(j,p);
        state.snow+=SnowAmplitude[j]*SinResponse(ks,Omega[j],phase,t);
        state.water+=SnowAmplitude[j]*MeltWave(ks,kw,Omega[j],phase,t);
        state.water+=RainAmplitude[j]*SinResponse(kw,Omega[j],rain,t);
    }
    return state;
}
unsigned SnowClass(double heat){
    constexpr std::array<double,7> cuts{.03,.2,.5,1.,1.7,2.6,3.5};
    return unsigned(std::upper_bound(cuts.begin(),cuts.end(),heat)-cuts.begin());
}
unsigned WaterClass(double dist,double grade){
    const double channel=std::exp(-dist/32.);
    return unsigned(std::clamp(int(channel*6.+std::clamp(grade*140.,0.,1.3)),0,7));
}
} // namespace

SurfaceTile::SurfaceTile(Tile key):key_(key){
    const Point origin=Origin(key);
    std::vector<Feature> sources;
    // Neighbour sources keep thermal and drainage fields continuous across
    // tile boundaries; only sources within 24 m of this parcel are needed.
    for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx){
        for(const auto& f:GenerateTile({key.x+dx,key.z+dz})){
            if(f.kind!=Kind::Drain&&f.kind!=Kind::Lamp)continue;
            const double x=std::max({0.,origin.x-f.position.x,
                f.position.x-origin.x-BlockMeters});
            const double z=std::max({0.,origin.z-f.position.z,
                f.position.z-origin.z-BlockMeters});
            if(x*x+z*z<=24.*24.)sources.push_back(f);
        }
    }
    for(unsigned z=0;z<Resolution;++z)for(unsigned x=0;x<Resolution;++x){
        const unsigned i=z*Resolution+x;
        const Point p{origin.x+(x+.5)*Pitch,origin.z+(z+.5)*Pitch};
        Sample& s=values_[i];
        s.height=float(GroundHeight(p));
        double heat=0.,dist2=std::numeric_limits<double>::infinity();
        for(const auto& f:sources){
            const double dx=p.x-f.position.x,dz=p.z-f.position.z;
            const double r=dx*dx+dz*dz;
            if(f.kind==Kind::Drain)dist2=std::min(dist2,r);
            if(f.kind==Kind::Drain&&r<900.)
                heat+=5.*std::exp(-r/(2.*2.7*2.7));
            else if(f.kind==Kind::Lamp&&r<900.)
                heat+=.595*std::exp(-r/(2.*4.2*4.2));
        }
        s.heat=float(heat);
        const double distance=std::sqrt(dist2);
        drainDistance_[i]=float(distance);
        const double snowMask=.5+.5*Perlin(p.x/17.,p.z/17.,WorldSeed^777);
        s.snow=float(.001+.012*std::clamp(snowMask,0.,1.));
        const double hollow=std::clamp(
            .5+.5*Perlin(p.x/8.,p.z/8.,WorldSeed^0x3840),0.,1.);
        s.water=float(.000025+.00018*hollow*
            (.4+.6*std::exp(-distance/24.)));
        s.wetness=.1f;
        const double gx=(GroundHeight({p.x+1,p.z})-
                         GroundHeight({p.x-1,p.z}))*.5;
        const double gz=(GroundHeight({p.x,p.z+1})-
                         GroundHeight({p.x,p.z-1}))*.5;
        const unsigned melt=SnowClass(heat),drain=WaterClass(distance,std::hypot(gx,gz));
        snowRates_[i]=uint8_t(melt);waterRates_[i]=uint8_t(drain);
        Group& group=groups_[melt*Rates+drain];
        ++group.count;group.initialSnow+=s.snow;group.initialWater+=s.water;
        for(unsigned j=0;j<Omega.size();++j){
            const double ps=Phase(j,p),pr=RainPhase(j,p);
            group.snowSin[j]+=SnowAmplitude[j]*std::sin(ps);
            group.snowCos[j]+=SnowAmplitude[j]*std::cos(ps);
            group.rainSin[j]+=RainAmplitude[j]*std::sin(pr);
            group.rainCos[j]+=RainAmplitude[j]*std::cos(pr);
        }
    }
}
void SurfaceTile::SetTime(double t)noexcept{
    seconds_=std::isfinite(t)?std::max(0.,t):0.;
}
SurfaceTile::Sample SurfaceTile::EvaluateCell(unsigned i)const{
    Sample s=values_[i];
    const Point origin=Origin(key_);
    const unsigned x=i%Resolution,z=i/Resolution;
    const Point p{origin.x+(x+.5)*Pitch,origin.z+(z+.5)*Pitch};
    const double ks=SnowLoss[snowRates_[i]],kw=WaterLoss[waterRates_[i]];
    const State state=Evaluate(s.snow,s.water,ks,kw,p,seconds_);
    s.snow=float(std::max(0.,state.snow));
    s.water=float(std::max(0.,state.water));
    const double channel=std::exp(-double(drainDistance_[i])/25.);
    s.wetness=float(std::clamp(.07+state.water*145.+
        ks*state.snow*350.+channel*.10,0.,1.));
    s.runoff=float(std::clamp(
        (kw*state.water*14500.+state.water*50.)*channel,0.,1.));
    return s;
}
SurfaceTile::Sample SurfaceTile::AtWorld(Point p)const{
    const Point origin=Origin(key_);
    const double u=(p.x-origin.x)/Pitch-.5,v=(p.z-origin.z)/Pitch-.5;
    const int x0=std::clamp(int(std::floor(u)),0,int(Resolution)-1);
    const int z0=std::clamp(int(std::floor(v)),0,int(Resolution)-1);
    const int x1=std::min(x0+1,int(Resolution)-1);
    const int z1=std::min(z0+1,int(Resolution)-1);
    const float fx=float(std::clamp(u-double(x0),0.,1.));
    const float fz=float(std::clamp(v-double(z0),0.,1.));
    const Sample a=EvaluateCell(unsigned(z0)*Resolution+unsigned(x0));
    const Sample b=EvaluateCell(unsigned(z0)*Resolution+unsigned(x1));
    const Sample c=EvaluateCell(unsigned(z1)*Resolution+unsigned(x0));
    const Sample d=EvaluateCell(unsigned(z1)*Resolution+unsigned(x1));
    const auto blend=[&](float Sample::*field){
        return std::lerp(std::lerp(a.*field,b.*field,fx),
                         std::lerp(c.*field,d.*field,fx),fz);
    };
    return {blend(&Sample::snow),blend(&Sample::water),
            blend(&Sample::wetness),blend(&Sample::height),
            blend(&Sample::heat),blend(&Sample::runoff)};
}
SurfaceTile::Appearance SurfaceTile::AppearanceAt(Point p)const{
    const auto s=AtWorld(p);
    return {
        std::clamp(s.snow/.012f,0.f,2.4f),
        std::clamp(s.water*120.f+s.wetness*.32f,.035f,1.2f),
        std::clamp(s.wetness*.55f+s.water*40.f+s.heat*.095f,0.f,1.f),
        std::clamp(s.runoff+s.wetness*.35f+s.heat*.06f,.1f,1.f)
    };
}
SurfaceTile::WaterBalance SurfaceTile::Balance()const{
    WaterBalance result{};
    const double t=seconds_;
    for(unsigned a=0;a<Rates;++a)for(unsigned b=0;b<Rates;++b){
        const Group& group=groups_[a*Rates+b];
        if(!group.count)continue;
        const double ks=SnowLoss[a],kw=WaterLoss[b],g=Transient(ks,kw,t);
        double snow=group.initialSnow*Decay(ks,t)+
            double(group.count)*WinterSnowfallMetersPerSecond*Constant(ks,t);
        double water=group.initialWater*Decay(kw,t)+ks*group.initialSnow*g+
            double(group.count)*(RainRate*Constant(kw,t)+
                WinterSnowfallMetersPerSecond*(Constant(kw,t)-g));
        double source=double(group.count)*(WinterSnowfallMetersPerSecond+RainRate)*t;
        for(unsigned j=0;j<Omega.size();++j){
            const double w=Omega[j];
            const double ss=group.snowCos[j],sc=group.snowSin[j];
            const double rs=group.rainCos[j],rc=group.rainSin[j];
            snow+=ss*SinResponse(ks,w,0.,t)+sc*CosResponse(ks,w,0.,t);
            water+=ss*MeltWave(ks,kw,w,0.,t)+
                   sc*MeltWave(ks,kw,w,Pi*.5,t)+
                   rs*SinResponse(kw,w,0.,t)+rc*CosResponse(kw,w,0.,t);
            source+=(ss+rs)*(1.-std::cos(w*t))/w+
                    (sc+rc)*std::sin(w*t)/w;
        }
        const double stored=std::max(0.,snow)+std::max(0.,water);
        result.stored+=stored;
        result.discharged+=std::max(0.,
            group.initialSnow+group.initialWater+source-stored);
    }
    return result;
}
} // namespace lab::urban
