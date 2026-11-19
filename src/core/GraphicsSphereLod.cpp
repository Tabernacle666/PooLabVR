#include "GraphicsInternal.h"

namespace OASIS {

void Graphics::Impl::SetupSphereCache(){
    if(!sphereLod.enabled||sphereLod.relief.resource)return;
    constexpr auto format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto desc=Texture(SphereCacheSize,SphereCacheSize,format,1,SphereCacheLayers,1,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto& texture=sphereLod.relief;
    texture.resource=Placed(desc,D3D12_RESOURCE_STATE_COMMON);
    sphereLod.reliefSRV=TextureSrv(texture.resource.Get(),format,{},true);
    sphereLod.reliefUAV=TextureUav(texture.resource.Get(),format,0,{},true);
    sphereLod.mean=Placed(Buffer(sizeof(XMFLOAT4),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        D3D12_RESOURCE_STATE_COMMON);
    sphereLod.meanSRV=Srv(sphereLod.mean.Get(),1u,sizeof(XMFLOAT4));
    sphereLod.meanUAV=UavBuffer(sphereLod.mean.Get(),1u,sizeof(XMFLOAT4));
}

void Graphics::Impl::ResetSphereCache(){
    auto& texture=sphereLod.relief;
    if(texture.resource)passGraph.Forget(texture.resource.Get());
    ReleaseDescriptor(sphereLod.reliefSRV);
    ReleaseDescriptor(sphereLod.reliefUAV);
    texture={};
    if(sphereLod.mean)passGraph.Forget(sphereLod.mean.Get());
    ReleaseDescriptor(sphereLod.meanSRV);
    ReleaseDescriptor(sphereLod.meanUAV);
    sphereLod.mean.Reset();
    sphereLod.meanReady=false;
    sphereLod.views={};
    sphereLod.current=sphereLod.candidate=SphereRepresentation::Geometry;
    sphereLod.candidateSince=sphereLod.refreshAfter=0.;
    sphereLod.liteSourceAfter=sphereLod.liveCaptureAfter=0.;
    sphereLod.liteSource=Invalid;
    sphereLod.revision=1;
    sphereLod.waveStart=-1.;
    sphereLod.warpRadians=0.f;
}

SphereRepresentation Graphics::Impl::SelectSphereRepresentation(const XRFrame& xr,
    std::array<uint32_t,2>& selected,std::array<XMFLOAT4,MaxViews>& bounds){
    using Level=SphereRepresentation;
    sphereLod.warpRadians=0.f;
    if(!sphereLod.enabled||xr.sphereBakeView!=Invalid||!xr.tracked||!sphereLod.relief.resource||
       atmosphere.radius<=0.f||outputSurface.desktop)return Level::Geometry;

    const auto offset=Sub(xr.input.head.p,atmosphere.center);
    const float distance=Length(offset);
    const float surface=std::max(0.f,distance-atmosphere.radius);
    const bool firing=xr.laserHeld||xr.laserCut||xr.laserAfterimageCount||cutFlightRemaining>0.f||
        std::any_of(xr.shotHands.begin(),xr.shotHands.end(),[](const auto& hand){return hand.fired;});
    const bool passage=xr.input.sceneUnitsPerMeter!=1.f||xr.input.crossingCount!=0u||
        xr.membranePassagePulses[0].w>=1.f;
    if(firing||passage||surface<=1.6f){
        sphereLod.current=sphereLod.candidate=Level::Geometry;
        sphereLod.candidateSince=xr.input.time;
        sphereLod.waveStart=-1.;
        return Level::Geometry;
    }
    const auto& settings=sphereLod.settings;
    const auto current=sphereLod.current;
    Level target=Level::Mean;
    bool onScreen=false,nativeVisible=false;
    for(uint32_t eye=0;eye<xr.viewCount;++eye){
        const auto vp=XMLoadFloat4x4(&xr.vp[eye]);
        if(!SphereIntersectsView(vp,atmosphere.center,atmosphere.radius,false))continue;
        onScreen=true;
        const bool inNativeRange=SphereIntersectsView(vp,atmosphere.center,atmosphere.radius,true);
        nativeVisible|=inNativeRange;
        const auto b=SphereScreenBounds(vp,atmosphere.center,atmosphere.radius);
        bounds[eye]={std::clamp(b.x,0.f,1.f),std::clamp(b.y,0.f,1.f),
                     std::clamp(b.z,0.f,1.f),std::clamp(b.w,0.f,1.f)};
        for(size_t i=0;i<size_t(target);++i){
            const auto level=Level(i);
            const auto& band=settings.bands[i];
            const bool keep=current<=level;
            if(surface<=(keep?band.exitDistance:band.enterDistance)&&
               (level!=Level::Geometry||inNativeRange)&&
               SphereIntersectsView(vp,atmosphere.center,atmosphere.radius,false,
                   keep?band.exitFraction:band.enterFraction)){
                target=level;
                break;
            }
        }
    }
    if(!onScreen){
        sphereLod.current=sphereLod.candidate=Level::Hidden;
        sphereLod.waveStart=-1.;
        return xr.sphereForce==Level::Geometry?Level::Geometry:Level::Hidden;
    }

    const auto towardEye=Unit(offset);
    const float halfAngle=XMConvertToDegrees(std::asin(std::min(1.f,atmosphere.radius/distance)));
    const bool keepPoint=current==Level::Point;
    if(target!=Level::Geometry&&(halfAngle<=(keepPoint?.10f:.075f)||
       (surface>=(keepPoint?settings.pointExit:settings.pointEnter)&&
        halfAngle<=(keepPoint?1.125f:1.f))))target=Level::Point;
    if(xr.sphereForce)target=*xr.sphereForce;

    const auto completed=direct.fence->GetCompletedValue();
    std::array<float,2> scores{-2.f,-2.f};
    float liteScore=-2.f;
    for(uint32_t i=0;i<sphereLod.views.size();++i){
        const auto& view=sphereLod.views[i];
        if(!view.fence||view.fence>completed||view.revision!=sphereLod.revision)continue;
        const float score=Dot(towardEye,view.direction);
        // A live snapshot represents the current damage from a local cone.
        if(i>=SphereBankAngles&&score<.94f)continue;
        if(i==sphereLod.liteSource)liteScore=score;
        if(score>scores[0]){
            selected[1]=selected[0];scores[1]=scores[0];
            selected[0]=i;scores[0]=score;
        }else if(score>scores[1]){selected[1]=i;scores[1]=score;}
    }
    if(SphereUsesRelief(target)&&selected[0]==Invalid)
        target=nativeVisible?Level::Geometry:Level::Mean;
    if(target==Level::Mean&&!sphereLod.meanReady)target=Level::Point;
    const double now=xr.input.time;
    if(target!=sphereLod.candidate){
        sphereLod.candidate=target;
        sphereLod.candidateSince=now;
        sphereLod.waveStart=-1.;
    }
    const bool currentUnavailable=SphereUsesRelief(current)&&selected[0]==Invalid;
    const auto delay=target<sphereLod.current?settings.promoteSeconds:settings.demoteSeconds;
    if(xr.sphereForce||sphereLod.current==Level::Hidden||currentUnavailable||
       (!nativeVisible&&sphereLod.current==Level::Geometry)){
        sphereLod.current=target;
        sphereLod.waveStart=-1.;
    }else if(target!=sphereLod.current&&now-sphereLod.candidateSince>=delay){
        if(settings.warpSeconds<=0.f||settings.warpDegrees<=0.f||
           target==Level::Point||sphereLod.current==Level::Point)sphereLod.current=target;
        else{
            if(sphereLod.waveStart<0.)sphereLod.waveStart=now;
            if(now-sphereLod.waveStart>=.5*settings.warpSeconds)sphereLod.current=target;
        }
    }
    if(sphereLod.waveStart>=0.&&settings.warpSeconds>0.f){
        const double fraction=(now-sphereLod.waveStart)/settings.warpSeconds;
        if(fraction>=1.)sphereLod.waveStart=-1.;
        else{
            const float envelope=std::sin(XM_PI*float(fraction));
            sphereLod.warpRadians=XMConvertToRadians(settings.warpDegrees)*envelope*envelope;
        }
    }
    if(sphereLod.current==Level::DepthLite){
        if(liteScore<-1.f||(now>=sphereLod.liteSourceAfter&&scores[0]>liteScore+.02f)){
            sphereLod.liteSource=selected[0];
            sphereLod.liteSourceAfter=now+1.0/settings.liteRefreshHz;
        }
        selected={sphereLod.liteSource,Invalid};
    }
    return sphereLod.current;
}
}
