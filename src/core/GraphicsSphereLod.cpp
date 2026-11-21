#include "GraphicsInternal.h"

namespace OASIS {

void Graphics::Impl::SetupSphereCache(SphereLodCache& sphereLod,uint32_t layers,uint32_t size){
    if(!sphereLod.enabled||sphereLod.relief.resource)return;
    constexpr auto format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto desc=Texture(size,size,format,1,layers,1,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto& texture=sphereLod.relief;
    texture.resource=Placed(desc,D3D12_RESOURCE_STATE_COMMON);
    sphereLod.reliefSRV=TextureSrv(texture.resource.Get(),format,{},true);
    sphereLod.reliefUAV=TextureUav(texture.resource.Get(),format,0,{},true);
    if(solarSystem){
        constexpr auto normalFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
        auto& normals=sphereLod.reliefNormals;
        normals.resource=Placed(Texture(size,size,normalFormat,1,layers,1,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
        sphereLod.reliefNormalsSRV=TextureSrv(normals.resource.Get(),normalFormat,{},true);
        sphereLod.reliefNormalsUAV=TextureUav(normals.resource.Get(),normalFormat,0,{},true);
    }
    if(DenseSphereField())return;
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
    auto& normals=sphereLod.reliefNormals;
    if(normals.resource)passGraph.Forget(normals.resource.Get());
    ReleaseDescriptor(sphereLod.reliefNormalsSRV);
    ReleaseDescriptor(sphereLod.reliefNormalsUAV);
    normals={};
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
    sphereLod.transitionViews={Invalid,Invalid};
    sphereLod.zone=SphereZone::Far;
    sphereLod.revision=1;
    sphereLod.waveStart=-1.;
    sphereLod.warpRadians=0.f;
}

const Graphics::Impl::SphereLodCache::View& Graphics::Impl::SphereView(uint32_t owner,uint32_t slot){
    const uint32_t source=DenseSphereField()&&!solarSystem&&slot<SphereBankAngles?uint32_t(owner!=0u):owner;
    return SphereCache(source).views[slot];
}

uint32_t Graphics::Impl::SphereViewLayer(uint32_t owner,uint32_t slot)const{
    if(slot==Invalid||!DenseSphereField())return slot;
    if(solarSystem){
        if(HasOasisRelief())return owner==0u?slot:(owner-1u)*SphereCacheLayers+slot;
        return owner*SphereCacheLayers+slot;
    }
    if(slot<SphereBankAngles)return uint32_t(owner!=0u)*SphereBankAngles+slot;
    return SpectaclePairCount*SphereBankAngles+2u*owner+(slot-SphereBankAngles);
}

SphereRepresentation Graphics::Impl::SelectSphereRepresentation(const XRFrame& xr,uint32_t owner,
    SphereSelection& selection,std::optional<SphereRepresentation> force,bool ellipse){
    using Level=SphereRepresentation;
    auto& sphereLod=SphereCache(owner);
    auto atmosphere=SphereAtmosphere(owner);
    atmosphere.center=SphereWorldCenter(owner);
    auto& selected=selection.views;
    auto& bounds=selection.bounds;
    sphereLod.warpRadians=0.f;
    if(xr.sphereBakeView!=Invalid)return owner==xr.sphereBakeOwner?Level::Geometry:Level::Hidden;
    if(!sphereLod.enabled||!xr.tracked||!SphereAtlas(owner).relief.resource||
       atmosphere.radius<=0.f||outputSurface.desktop)return Level::Geometry;
    if(force==Level::Hidden){
        sphereLod.current=sphereLod.candidate=Level::Hidden;
        sphereLod.waveStart=-1.;
        return Level::Hidden;
    }

    const auto offset=Sub(xr.input.head.p,atmosphere.center);
    const float distance=Length(offset);
    const float surface=std::max(0.f,distance-atmosphere.radius);
    const bool firing=xr.laserHeld||xr.laserCut||xr.laserAfterimageCount||
        cutFlightRemaining[owner]>0.f||cutDrainPending[owner]||
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
    const bool dense=DenseSphereField();
    Level target=dense?Level::Depth:Level::Mean;
    SphereZone zone=SphereZone::Far;
    bool onScreen=false,nativeVisible=false;
    for(uint32_t eye=0;eye<xr.viewCount;++eye){
        const auto vp=XMLoadFloat4x4(&xr.vp[eye]);
        if(!SphereIntersectsView(vp,atmosphere.center,atmosphere.radius,false))continue;
        onScreen=true;
        const bool inNativeRange=SphereIntersectsView(vp,atmosphere.center,atmosphere.radius,true);
        nativeVisible|=inNativeRange;
        const auto b=SphereScreenBounds(vp,atmosphere.center,atmosphere.radius);
        if(ellipse&&eye==0u&&xr.input.sceneUnitsPerMeter==1.f){
            const float z=XMVectorGetW(XMVector3Transform(XMLoadFloat3(&atmosphere.center),vp));
            selection.captureVisible=b.x>=0.f&&b.y>=0.f&&b.z<=1.f&&b.w<=1.f&&
                b.z>b.x&&b.w>b.y&&z>atmosphere.radius&&z+atmosphere.radius<xr.input.SceneFarZ();
        }
        bounds[eye]={std::clamp(b.x,0.f,1.f),std::clamp(b.y,0.f,1.f),
                     std::clamp(b.z,0.f,1.f),std::clamp(b.w,0.f,1.f)};
        for(size_t i=0;i<(dense?2u:size_t(target));++i){
            const auto level=Level(i);
            const auto& band=settings.bands[i];
            const bool keep=dense?sphereLod.zone<=SphereZone(i):current<=level;
            const bool solarLocal=solarBodies&&level==Level::Geometry;
            const float limit=solarLocal?SolarLocalSurfaceRange(settings,keep,owner):
                keep?band.exitDistance:band.enterDistance;
            if(((!ellipse||solarLocal)&&surface>limit)||(level==Level::Geometry&&!inNativeRange))continue;
            bool within=false;
            if(solarLocal)within=true;
            else if(ellipse){
                const auto columns=XMMatrixTranspose(vp);
                const auto delta=Sub(atmosphere.center,xr.eyes[eye]);
                const XMFLOAT3 forward=Unit({XMVectorGetX(columns.r[3]),
                    dense?XMVectorGetY(columns.r[3]):0.f,XMVectorGetZ(columns.r[3])});
                const float ahead=Dot(delta,forward);
                const float z=ahead/(limit+atmosphere.radius);
                const float lateral=Dot(delta,{forward.z,0.f,-forward.x});
                const float perpendicular=dense?std::max(0.f,Dot(delta,delta)-ahead*ahead):lateral*lateral;
                const float width=keep?band.exitWidth:band.width;
                within=z>=0.f&&perpendicular/(width*width)+z*z<=1.f;
            }else within=SphereIntersectsView(vp,atmosphere.center,atmosphere.radius,false,
                keep?band.exitFraction:band.enterFraction);
            if(within){
                if(dense){
                    zone=std::min(zone,SphereZone(i));
                    if(i==0u)target=Level::Geometry;
                }else target=level;
                break;
            }
        }
    }
    selection.nativeVisible=nativeVisible;
    if(!onScreen){
        sphereLod.current=sphereLod.candidate=Level::Hidden;
        sphereLod.zone=SphereZone::Far;
        sphereLod.waveStart=-1.;
        return force==Level::Geometry&&(!ellipse||!xr.sphereForces[owner])?Level::Geometry:Level::Hidden;
    }
    sphereLod.zone=zone;

    auto towardEye=Unit(Sub(xr.viewCount==2u?Scale(Add(xr.eyes[0],xr.eyes[1]),.5f):
        xr.eyes[0],atmosphere.center));
    if(solarBodies){
        const auto rotation=XMLoadFloat4(&(*solarBodies)[owner].q);
        XMStoreFloat3(&towardEye,XMVector3Rotate(XMLoadFloat3(&towardEye),XMQuaternionConjugate(rotation)));
    }
    const float halfAngle=XMConvertToDegrees(std::asin(std::min(1.f,atmosphere.radius/distance)));
    const bool keepPoint=current==Level::Point;
    if(!dense&&target!=Level::Geometry&&(halfAngle<=(keepPoint?.10f:.075f)||
       (surface>=(keepPoint?settings.pointExit:settings.pointEnter)&&
        halfAngle<=(keepPoint?1.125f:1.f))))target=Level::Point;
    if(dense&&xr.input.insideSphere<sphereCount&&xr.input.insideSphere!=owner)
        target=Level::DepthLite;
    const bool oasisApproach=solarBodies&&owner==0u&&HasOasisRelief()&&
        xr.input.insideSphere==Invalid;
    if(oasisApproach){
        // Stay in relief in peripheral vision. Centered attention and
        // proximity must persist before allocating the native scene.
        const bool keepNative=current==Level::Geometry;
        const auto forward=Unit(Sub(Transform({0.f,0.f,1.f},xr.input.head),xr.input.head.p));
        const float alignment=distance>.001f?Dot(forward,Scale(offset,-1.f/distance)):1.f;
        const float focus=keepNative?.88f:.955f;
        const float limit=std::min(keepNative?4.8f:3.8f,SolarLocalSurfaceRange(settings,keepNative,owner));
        if(nativeVisible&&surface<=limit&&alignment>=focus)target=Level::Geometry;
        else target=surface<=(current==Level::DepthUltra?17.f:14.f)?Level::DepthUltra:Level::Depth;
    }
    if(force)target=*force;

    const auto completed=direct.fence->GetCompletedValue();
    std::array<float,2> scores{-2.f,-2.f};
    float liteScore=-2.f;
    uint64_t newest=0;
    for(uint32_t i=0;i<sphereLod.views.size();++i){
        const auto& view=SphereView(owner,i);
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
        target=nativeVisible?Level::Geometry:dense?Level::Hidden:Level::Mean;
    if(target==Level::Mean&&!sphereLod.meanReady)target=Level::Point;
    const double now=xr.input.time;
    if(target!=sphereLod.candidate){
        sphereLod.candidate=target;
        sphereLod.candidateSince=now;
        sphereLod.waveStart=-1.;
    }
    const bool currentUnavailable=SphereUsesRelief(current)&&selected[0]==Invalid;
    const auto detail=[](Level level){return level==Level::DepthUltra?1u:level==Level::Geometry?0u:uint32_t(level)+1u;};
    const bool promoting=detail(target)<detail(sphereLod.current);
    const double delay=oasisApproach&&
        (target==Level::Geometry||current==Level::Geometry)?
        (promoting?std::max(.16, double(settings.promoteSeconds)):std::max(.38,double(settings.demoteSeconds))):
        (promoting?settings.promoteSeconds:settings.demoteSeconds);
    const bool insideRelief=dense&&SphereUsesRelief(target)&&
        (target==Level::DepthLite||current==Level::DepthLite);
    if(force||insideRelief||sphereLod.current==Level::Hidden||currentUnavailable||
       (!nativeVisible&&sphereLod.current==Level::Geometry)){
        sphereLod.current=target;
        sphereLod.waveStart=-1.;
    }else if(target!=sphereLod.current&&now-sphereLod.candidateSince>=delay){
        if(oasisApproach||settings.warpSeconds<=0.f||settings.warpDegrees<=0.f||
           target==Level::Point||sphereLod.current==Level::Point)sphereLod.current=target;
        else{
            if(sphereLod.waveStart<0.){
                sphereLod.waveStart=now;
                sphereLod.transitionViews=selected;
            }
            if(now-sphereLod.waveStart>=.5*settings.warpSeconds)sphereLod.current=target;
        }
    }
    if(sphereLod.waveStart>=0.&&settings.warpSeconds>0.f){
        const double fraction=(now-sphereLod.waveStart)/settings.warpSeconds;
        if(fraction>=1.)sphereLod.waveStart=-1.;
        else{
            const float envelope=std::sin(XM_PI*float(fraction));
            sphereLod.warpRadians=XMConvertToRadians(settings.warpDegrees)*envelope*envelope;
            if(dense&&sphereLod.transitionViews[0]!=Invalid&&selected[0]!=Invalid&&
               SphereView(owner,sphereLod.transitionViews[0]).fence<=completed&&
               (sphereLod.transitionViews[1]==Invalid||
                SphereView(owner,sphereLod.transitionViews[1]).fence<=completed))
                selected=sphereLod.transitionViews;
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
    if(xr.input.insideSphere<sphereCount)return xr.input.insideSphere;
    if(xr.input.crossingCount)return xr.input.crossings[xr.input.crossingCount-1u].sphere;
    if(xr.membranePassagePulses[0].w>=1.f)return xr.membranePassageSphere;
    float nearest=std::numeric_limits<float>::max();
    uint32_t hit=Invalid;
    auto trace=[&](XMFLOAT3 origin,XMFLOAT3 direction,float reach){
        for(uint32_t i=0;i<sphereCount;++i){
            const auto& profile=SphereAtmosphere(i);
            const auto offset=Sub(origin,SphereWorldCenter(i));
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
    if(nativeSphere<sphereCount&&
       (cutFlightRemaining[nativeSphere]>0.f||cutDrainPending[nativeSphere]))return nativeSphere;
    for(uint32_t i=0;i<sphereCount;++i){
        const auto& profile=SphereAtmosphere(i);
        if(Length(Sub(xr.input.head.p,SphereWorldCenter(i)))<=profile.radius+1.6f)return i;
    }
    if(fired||laser)return nativeSphere<sphereCount?nativeSphere:0u;
    return Invalid;
}

void Graphics::Impl::SelectSpherePair(XRFrame& xr,std::array<SphereSelection,SpectacleSphereCount>& slots){
    using Level=SphereRepresentation;
    const bool scripted=xr.sphereForces[0].has_value()||xr.sphereForces[1].has_value();
    const uint32_t preferred=scripted?Invalid:SelectInteractiveSphere(xr);
    const auto forward=Unit(Sub(Transform({0.f,0.f,1.f},xr.input.head),xr.input.head.p));
    auto score=[&](uint32_t owner){
        return Dot(forward,Unit(Sub(SphereWorldCenter(owner),xr.input.head.p)));
    };
    uint32_t winner=Invalid;
    for(uint32_t owner=0;owner<sphereCount;++owner){
        auto forced=xr.sphereForces[owner];
        if(solarBodies&&xr.input.insideSphere==SolarMoon&&owner!=SolarMoon)forced=Level::Hidden;
        if(owner==preferred)forced=Level::Geometry;
        auto& slot=slots[owner];
        slot.level=SelectSphereRepresentation(xr,owner,slot,forced,true);
        if(slot.level==Level::Geometry&&(winner==Invalid||owner==preferred||
           (winner!=preferred&&score(owner)>score(winner))))winner=owner;
    }
    if(winner==Invalid&&nativeSphere!=Invalid&&preferred==Invalid&&!scripted){
        auto& slot=slots[nativeSphere];
        const auto& cache=SphereCache(nativeSphere);
        // Retain the current native view long enough to publish its changed
        // capture, while yielding immediately to another native target.
        if(slot.captureVisible&&!slot.fresh&&cache.revision>1u&&xr.input.time<cache.refreshAfter+.35){
            winner=nativeSphere;
            slot.level=Level::Geometry;
        }
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
    const bool moonWithOasis=solarBodies&&winner==0u&&!xr.sphereForces[SolarMoon]&&
        slots[SolarMoon].nativeVisible&&slots[SolarMoon].level!=Level::Hidden;
    xr.lunarGeometry=solarBodies&&(winner==SolarMoon||moonWithOasis);
    for(uint32_t owner=0;owner<sphereCount;++owner){
        auto& slot=slots[owner];
        if(owner==SolarMoon&&moonWithOasis)slot.level=Level::Geometry;
        else if(slot.level==Level::Geometry&&owner!=winner){
            slot.level=slot.views[0]!=Invalid?
                (DenseSphereField()&&xr.input.insideSphere<sphereCount?Level::DepthLite:Level::Depth):
                SphereCache(owner).meanReady?Level::Mean:Level::Hidden;
        }
        xr.sphereLevels[owner]=slot.level;
        xr.sphereZones[owner]=SphereCache(owner).zone;
    }
    nativeSphere=winner;
    xr.sphereNative=winner;
    renderSphere=winner!=Invalid?winner:0u;
    if(winner==Invalid)for(uint32_t owner=0;owner<sphereCount;++owner){
        if(slots[owner].level==Level::Depth||slots[owner].level==Level::DepthUltra){
            renderSphere=owner;
            break;
        }
    }
}

void Graphics::Impl::BinSphereField(Constants& c,XRFrame& xr,
    const std::array<SphereSelection,SpectacleSphereCount>& slots){
    constexpr uint32_t orderStride=(SpectacleSphereCount+3u)/4u;
    auto component=[](XMUINT4& v,uint32_t i)->uint32_t&{
        switch(i){case 0u:return v.x;case 1u:return v.y;case 2u:return v.z;default:return v.w;}
    };
    auto tile=[](float uv){return std::min(SphereTileAxis-1u,uint32_t(uv*SphereTileAxis));};
    for(uint32_t eye=0;eye<xr.viewCount;++eye){
        struct Entry {uint32_t owner;float order;};
        std::array<Entry,SpectacleSphereCount> visible;
        uint32_t count=0,reliefs=0;
        for(uint32_t owner=0;owner<sphereCount;++owner){
            const auto& slot=slots[owner];
            if(!SphereUsesRelief(slot.level)&&c.sphereProjections[owner].optics.y==0.f)continue;
            const auto box=slot.bounds[eye];
            if(box.x>=box.z||box.y>=box.w)continue;
            const auto& sphere=c.sphereProjections[owner].sphere;
            const auto offset=Sub({sphere.x,sphere.y,sphere.z},xr.eyes[eye]);
            // For disjoint volumes, entry * exit equals this key on every
            // ray through the sphere. It gives one consistent order per eye.
            visible[count++]={owner,Dot(offset,offset)-sphere.w*sphere.w};
            reliefs+=SphereUsesRelief(slot.level);
        }
        std::sort(visible.begin(),visible.begin()+count,[](const Entry& a,const Entry& b){
            return a.order==b.order?a.owner<b.owner:a.order>b.order;
        });
        xr.visibleSpheres[eye]=reliefs;
        for(uint32_t rank=0;rank<count;++rank){
            const auto owner=visible[rank].owner;
            component(c.sphereOrder[eye*orderStride+rank/4u],rank%4u)=owner;
            const auto box=slots[owner].bounds[eye];
            for(uint32_t y=tile(box.y);y<=tile(box.w);++y)
                for(uint32_t x=tile(box.x);x<=tile(box.z);++x){
                    auto& mask=c.sphereTiles[(eye*SphereTileAxis+y)*SphereTileAxis+x];
                    component(mask,rank/32u)|=1u<<(rank%32u);
                }
        }
    }
}
}
