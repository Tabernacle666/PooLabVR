#include "GraphicsInternal.h"

namespace OASIS {

void Graphics::Impl::SetupSphereCache(SphereLodCache& sphereLod){
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

void Graphics::Impl::ResetSphereCache(SphereLodCache& sphereLod){
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

SphereRepresentation Graphics::Impl::SelectSphereRepresentation(const XRFrame& xr,uint32_t owner,
    SphereSelection& selection,std::optional<SphereRepresentation> force,bool ellipse){
    using Level=SphereRepresentation;
    auto& sphereLod=SphereCache(owner);
    const auto& atmosphere=SphereAtmosphere(owner);
    auto& selected=selection.views;
    auto& bounds=selection.bounds;
    sphereLod.warpRadians=0.f;
    if(xr.sphereBakeView!=Invalid)return owner==xr.sphereBakeOwner?Level::Geometry:Level::Hidden;
    if(!sphereLod.enabled||!xr.tracked||!sphereLod.relief.resource||
       atmosphere.radius<=0.f||outputSurface.desktop)return Level::Geometry;
    if(force==Level::Hidden){
        sphereLod.current=sphereLod.candidate=Level::Hidden;
        sphereLod.waveStart=-1.;
        return Level::Hidden;
    }

    const auto offset=Sub(xr.input.head.p,atmosphere.center);
    const float distance=Length(offset);
    const float surface=std::max(0.f,distance-atmosphere.radius);
    const bool firing=xr.laserHeld||xr.laserCut||xr.laserAfterimageCount||cutFlightRemaining>0.f||
        std::any_of(xr.shotHands.begin(),xr.shotHands.end(),[](const auto& hand){return hand.fired;});
    const bool passage=xr.input.sceneUnitsPerMeter!=1.f||xr.input.crossingCount!=0u||
        xr.membranePassagePulses[0].w>=1.f;
    if(!ellipse&&(firing||passage||surface<=1.6f)){
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
            const float limit=keep?band.exitDistance:band.enterDistance;
            if((!ellipse&&surface>limit)||(level==Level::Geometry&&!inNativeRange))continue;
            bool within=false;
            if(ellipse){
                const auto columns=XMMatrixTranspose(vp);
                const XMFLOAT3 forward=Unit({XMVectorGetX(columns.r[3]),0.f,XMVectorGetZ(columns.r[3])});
                const auto delta=Sub(atmosphere.center,xr.eyes[eye]);
                const float z=Dot(delta,forward)/(limit+atmosphere.radius);
                const float x=Dot(delta,{forward.z,0.f,-forward.x})/(keep?band.exitWidth:band.width);
                within=z>=0.f&&x*x+z*z<=1.f;
            }else within=SphereIntersectsView(vp,atmosphere.center,atmosphere.radius,false,
                keep?band.exitFraction:band.enterFraction);
            if(within){
                target=level;
                break;
            }
        }
    }
    if(!onScreen){
        sphereLod.current=sphereLod.candidate=Level::Hidden;
        sphereLod.waveStart=-1.;
        return force==Level::Geometry&&(!ellipse||!xr.sphereForces[owner])?Level::Geometry:Level::Hidden;
    }

    const auto towardEye=Unit(offset);
    const float halfAngle=XMConvertToDegrees(std::asin(std::min(1.f,atmosphere.radius/distance)));
    const bool keepPoint=current==Level::Point;
    if(target!=Level::Geometry&&(halfAngle<=(keepPoint?.10f:.075f)||
       (surface>=(keepPoint?settings.pointExit:settings.pointEnter)&&
        halfAngle<=(keepPoint?1.125f:1.f))))target=Level::Point;
    if(force)target=*force;

    const auto completed=direct.fence->GetCompletedValue();
    std::array<float,2> scores{-2.f,-2.f};
    float liteScore=-2.f;
    uint64_t newest=0;
    for(uint32_t i=0;i<sphereLod.views.size();++i){
        const auto& view=sphereLod.views[i];
        if(!view.fence||view.fence>completed||view.revision<newest||
           (!ellipse&&view.revision!=sphereLod.revision))continue;
        const float score=Dot(towardEye,view.direction);
        // A live snapshot represents the current damage from a local cone.
        if(i>=SphereBankAngles&&score<.94f)continue;
        if(view.revision>newest){
            selected={Invalid,Invalid};scores={-2.f,-2.f};liteScore=-2.f;
            newest=view.revision;
        }
        if(i==sphereLod.liteSource)liteScore=score;
        if(score>scores[0]){
            selected[1]=selected[0];scores[1]=scores[0];
            selected[0]=i;scores[0]=score;
        }else if(score>scores[1]){selected[1]=i;scores[1]=score;}
    }
    selection.fresh=newest==sphereLod.revision;
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
    if(force||sphereLod.current==Level::Hidden||currentUnavailable||
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

uint32_t Graphics::Impl::SelectInteractiveSphere(const XRFrame& xr)const{
    if(xr.input.insideSphere<SpectacleSphereCount)return xr.input.insideSphere;
    if(xr.input.crossingCount)return xr.input.crossings[xr.input.crossingCount-1u].sphere;
    if(xr.membranePassagePulses[0].w>=1.f)return xr.membranePassageSphere;
    float nearest=std::numeric_limits<float>::max();
    uint32_t hit=Invalid;
    auto trace=[&](XMFLOAT3 origin,XMFLOAT3 direction,float reach){
        for(uint32_t i=0;i<SpectacleSphereCount;++i){
            const auto& profile=SphereAtmosphere(i);
            const auto offset=Sub(origin,profile.center);
            const float along=Dot(offset,direction),r=profile.radius;
            const float disc=along*along+r*r-Dot(offset,offset);
            if(disc<=0.f)continue;
            const float entry=std::max(0.f,-along-std::sqrt(disc));
            if(-along+std::sqrt(disc)>=0.f&&entry<=reach&&entry<nearest){nearest=entry;hit=i;}
        }
    };
    const bool laser=xr.laserHeld||xr.laserCut;
    if(laser&&xr.basaltRay){
        const auto delta=Sub(xr.basaltRay->end,xr.basaltRay->origin);
        const float reach=Length(delta);
        if(reach>0.f)trace(xr.basaltRay->origin,Scale(delta,1.f/reach),reach);
    }
    bool fired=false;
    for(const auto& hand:xr.shotHands)
        if(hand.fired&&hand.aim.tracked){
            fired=true;
            trace(hand.aim.pose.p,Unit(Sub(Transform({0.f,0.f,1.f},hand.aim.pose),hand.aim.pose.p)),FarZ);
        }
    if(hit!=Invalid)return hit;
    if(cutFlightRemaining>0.f&&nativeSphere<SpectacleSphereCount)return nativeSphere;
    for(uint32_t i=0;i<SpectacleSphereCount;++i){
        const auto& profile=SphereAtmosphere(i);
        if(Length(Sub(xr.input.head.p,profile.center))<=profile.radius+1.6f)return i;
    }
    if(fired||laser)return nativeSphere<SpectacleSphereCount?nativeSphere:0u;
    return Invalid;
}

void Graphics::Impl::SelectSpherePair(XRFrame& xr,std::array<SphereSelection,SpectacleSphereCount>& slots){
    using Level=SphereRepresentation;
    const bool scripted=xr.sphereForces[0].has_value()||xr.sphereForces[1].has_value();
    const uint32_t preferred=scripted?Invalid:SelectInteractiveSphere(xr);
    const auto forward=Unit(Sub(Transform({0.f,0.f,1.f},xr.input.head),xr.input.head.p));
    auto score=[&](uint32_t owner){
        return Dot(forward,Unit(Sub(SphereAtmosphere(owner).center,xr.input.head.p)));
    };
    uint32_t winner=Invalid;
    for(uint32_t owner=0;owner<SpectacleSphereCount;++owner){
        auto forced=xr.sphereForces[owner];
        if(owner==preferred)forced=Level::Geometry;
        auto& slot=slots[owner];
        slot.level=SelectSphereRepresentation(xr,owner,slot,forced,true);
        if(slot.level==Level::Geometry&&(winner==Invalid||owner==preferred||
           (winner!=preferred&&score(owner)>score(winner))))winner=owner;
    }
    const bool contested=winner!=Invalid&&nativeSphere!=Invalid&&winner!=nativeSphere&&
        slots[nativeSphere].level==Level::Geometry&&preferred==Invalid&&!scripted&&xr.sphereBakeView==Invalid;
    if(contested){
        const double now=xr.input.time;
        if(score(winner)<=score(nativeSphere)+.02f){
            pendingNativeSphere=Invalid;winner=nativeSphere;
        }else{
            if(pendingNativeSphere!=winner){pendingNativeSphere=winner;pendingNativeSince=now;}
            if(now-pendingNativeSince<sphereLod.settings.promoteSeconds)winner=nativeSphere;
        }
    }else pendingNativeSphere=Invalid;
    for(uint32_t owner=0;owner<SpectacleSphereCount;++owner){
        auto& slot=slots[owner];
        if(slot.level==Level::Geometry&&owner!=winner){
            slot.level=slot.views[0]!=Invalid?Level::Depth:
                SphereCache(owner).meanReady?Level::Mean:Level::Hidden;
        }
        xr.sphereLevels[owner]=slot.level;
    }
    nativeSphere=winner;
    xr.sphereNative=winner;
    renderSphere=winner!=Invalid?winner:slots[0].level==Level::Depth?0u:
        slots[1].level==Level::Depth?1u:0u;
}
}
