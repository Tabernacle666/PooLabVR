#include "GraphicsInternal.h"
#include "../../shaders/debris_layout.hlsli"
#include <cmath>
#include <cstring>

namespace OASIS {
namespace {
float Halton(uint32_t index,uint32_t base){
    float sample=0.f,weight=1.f;
    for(;index;index/=base){
        weight/=float(base);
        sample+=weight*float(index%base);
    }
    return sample;
}
}
    void Graphics::Impl::Render(XRFrame& outputFrame,const GpuProducerCoordinator& coordinator){
        if(!active)return;
        const bool measureRender=developerMode||dlssDiagnosticEnabled||sphereBaselineTiming;
        const int64_t cpuRenderStart=measureRender?QpcCounter():0;
        if(measureRender){
            if(!timerInitialized){
                timerInitialized=true;
                D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
                const bool copyTiming=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&options,sizeof(options)))&&options.CopyQueueTimestampQueriesSupported;
                std::array<QueueState*,GraphQueueCount> queues{&copy,&compute,&direct};
                for(uint32_t q=0;q<GraphQueueCount;++q){
                    if(q==QI(GraphQueue::Copy)&&!copyTiming)continue;
                    auto& t=timerQueues[q];
                    if(FAILED(queues[q]->queue->GetTimestampFrequency(&t.frequency))||!t.frequency)continue;
                    D3D12_QUERY_HEAP_DESC heap{};
                    heap.Type=q==QI(GraphQueue::Copy)?D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP:D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                    heap.Count=GpuBatchTimer::Slots*GpuBatchTimer::QueriesPerSlot;
                    if(FAILED(device->CreateQueryHeap(&heap,IID_PPV_ARGS(&t.queries))))continue;
                    D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_READBACK;
                    D3D12_RESOURCE_DESC buffer{};
                    buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
                    buffer.Width=UINT64(heap.Count)*sizeof(UINT64);
                    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;
                    buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    if(FAILED(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&buffer,
                        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&t.readback))))t.queries.Reset();
                }
            }
            CollectGpuTimings();
        }
        if(outputFrame.viewCount!=viewCount||!outputFrame.viewCount||outputFrame.viewCount>MaxViews)Fail("XR view topology changed after Graphics configuration");
        if(outputFrame.color>=xrRTV.size()||outputFrame.depth>=xrDSV.size())Fail("Output image index out of range");
        publicationCatalog.Clear();publications.clear();descriptorAllocator.BeginRecording();managedMemory.BeginFrame();CollectManaged();residency.Begin();vramCache.RequireActive();for(auto* domain:domainRegistry)domain->BeginFrame(outputFrame.input.frameId);
        auto endDomains=[&]{for(auto* domain:domainRegistry)domain->EndFrame();managedMemory.EndFrame(SubmittedFences());descriptorAllocator.EndRecording(SubmittedFences());};
        for(auto* domain:domainRegistry){auto publication=domain->Publish();if(!publication)continue;if(publication.groups.empty()||publication.domain!=domain->Identity()||!domain->ValidatePublication(publication))Fail("GPU domain rejected its own publication");DescribePublication(publication);publications.push_back({nullptr,domain,std::move(publication)});domain->BindPublication(publications.back().publication);}
        auto releasePublications=[&](bool immediateSignal){
            for(auto& bound:publications){
                if(!bound.stream)continue;auto& publication=bound.publication;
                if(immediateSignal&&publication.consumerFence&&publication.consumerValue){
                    if(bound.deferred){
                        if(!coordinator.finishFallback)Fail("Deferred GPU producer cancellation without worker join");
                        coordinator.finishFallback();
                    }

                    ReturnUnusedPublication(publication);
                }
                bound.stream->Release(publication);bound.stream=nullptr;bound.publication={};
            }
        };
        for(auto* stream:streams){
            auto* domain=FindDomain(stream->Domain());if(!domain)Fail("GPU stream domain is not registered");
            auto reserved=std::find_if(reservedStreams.begin(),reservedStreams.end(),[&](const auto& r){return r.stream==stream;});
            GpuPublication publication;
            const bool deferred=reserved!=reservedStreams.end();
            if(deferred){publication=std::move(reserved->publication);reserved->stream=nullptr;}
            else {


                if(coordinator.finishFallback)coordinator.finishFallback();
                publication=stream->Acquire();
            }


            if(!publication){continue;}
            if(publication.groups.empty())Fail("GPU publication missing resource groups");if(publication.domain!=stream->Domain())Fail("GPU publication changed stream domain");if(!publication.producerFence||!publication.producerValue||!publication.consumerFence||!publication.consumerValue)Fail("GPU stream publication missing producer/consumer fence");if(!domain->ValidatePublication(publication))Fail("GPU publication rejected by domain contract");
            DescribePublication(publication);publications.push_back({stream,domain,std::move(publication),deferred});domain->BindPublication(publications.back().publication);
        }
        auto& f=*active;
        const bool paired=sphereCount>1u&&!sphereObjectOffsets.empty();
        const bool dense=paired&&DenseSphereField();
        std::array<SphereSelection,SpectacleSphereCount> selections{};
        const uint32_t previousNative=nativeSphere;
        const bool sphereBake=outputFrame.sphereBakeView!=Invalid;
        if(solarSystem&&!sphereBake){
            auto& bodies=solarBodies.emplace();
            for(uint32_t i=0;i<SolarBodyCount;++i)
                bodies[i]=SolarBody(i,std::max(0.,outputFrame.input.solarTime));
        }else solarBodies.reset();
        if(paired)SelectSpherePair(outputFrame,selections);
        else{
            renderSphere=0u;
            selections[0].level=SelectSphereRepresentation(outputFrame,0u,selections[0],outputFrame.sphereForce,false);
            outputFrame.sphereLevels[0]=selections[0].level;outputFrame.sphereLevels[1]=SphereRepresentation::Hidden;
            outputFrame.sphereNative=selections[0].level==SphereRepresentation::Geometry?0u:Invalid;
        }
        std::optional<XRFrame> local;
        auto renderToCosmos=XMMatrixIdentity(),cosmosToRender=XMMatrixIdentity();
        XMFLOAT4 renderOrientation{0.f,0.f,0.f,1.f};
        if(solarBodies&&outputFrame.sphereNative!=Invalid){
            const auto& body=(*solarBodies)[renderSphere];
            renderOrientation=body.q;
            renderToCosmos=SolarBodyToCosmos(renderSphere,body);
            cosmosToRender=XMMatrixInverse(nullptr,renderToCosmos);
            RebaseSolarFrame(f,local.emplace(outputFrame),renderToCosmos,cosmosToRender);
        }
        PublishFrameObjects(f);
        auto& xr=local?*local:outputFrame;
        const auto sun=TransformPoint({},cosmosToRender);
        const bool sphereProxy=xr.sphereNative==Invalid;
        const bool drawCompanionMoon=solarBodies&&xr.lunarGeometry&&xr.sphereNative==0u;
        const bool lunarScene=solarSystem&&renderSphere==SolarMoon;
        auto& sphereLod=SphereCache(renderSphere);
        auto& atlas=SphereAtlas(renderSphere);
        const auto atmosphere=SphereAtmosphere(renderSphere);
        auto& membrane=RenderMembrane();
        auto& debris=RenderDebris();
        const auto& selection=selections[renderSphere];
        const auto& sphereViews=selection.views;
        xr.sphereRepresentation=selection.level;
        const bool sphereRelief=xr.sphereRepresentation==SphereRepresentation::Depth;
        const bool sphereProjected=SphereUsesRelief(xr.sphereRepresentation);
        const bool sphereMean=xr.sphereRepresentation==SphereRepresentation::Mean;
        const bool visibleSphere=std::any_of(selections.begin(),selections.end(),[](const auto& slot){
            return slot.level!=SphereRepresentation::Hidden;
        });
        // A local vehicle can be in front of a field rendered entirely as sphere proxies.
        const bool coreSceneWork=f.snapshotCount!=0||
            (!sphereProxy&&(f.cpuCount!=0||!meshDomain->Bindings().empty()));
        const bool proxyOnly=sphereProxy&&!coreSceneWork;
        if(dense){
            if(renderSphere!=0u||sphereProxy)
                std::erase_if(visualFrame.lights,[](const SceneLight& light){return (light.flags&BuildingLightFlag)!=0u;});
            if(buildingRevision&&!solarSystem)
                std::erase_if(visualFrame.lights,[](const SceneLight& light){return light.id==0x70000001u;});
            if(coreSceneWork){
                if(solarBodies){
                    SceneLight light{};
                    light.position=sun;light.radius=SunLightRange;light.color=SunColor;light.power=SunPower;
                    light.id=0x70000000u;light.flags=sphereProxy?0u:1u|4u;
                    visualFrame.lights.push_back(light);
                }else if(!solarSystem){
                    if(buildingRevision){
                        SceneLight sun{};
                        sun.direction=Unit({.52f,-.74f,.40f});
                        sun.color=SunColor;sun.power=2.1f;sun.flags=1u|2u|4u;
                        sun.id=0x70000000u;
                        visualFrame.lights.push_back(sun);
                    }else
                        visualFrame.lights.push_back(SpectacleLight(renderSphere,xr.input.head.p,f.simulationTime));
                }
            }
            visualFrame.lightCount=uint32_t(visualFrame.lights.size());
            PrepareLights(f,visualFrame);
        }
        f.instanceCount=meshDomain->TotalInstances();
        if(coreSceneWork) {
            EnsureInstances(f,sceneCache.StaticCount()+f.snapshotCount,f.cpuCount,f.instanceCount);
            EnsureLightBuffers(f);
            EnsureLightBvhBuffers(f);
        }
        const GpuSceneCache* tileCatalog=contributions.residentTiles?
            contributions.residentTiles(contributions.context):nullptr;
        const auto* tiles=tileCatalog?&tileCatalog->ResidentTiles():nullptr;
        auto residentBatches=PrepareResidentBatches(f,tileCatalog,coreSceneWork);
        auto& tileBatchDispatch=residentBatches.tileBatchDispatch;
        auto& opaqueRuns=residentBatches.opaqueRuns;
        const uint32_t tileBatchBlocks=residentBatches.tileBatchBlocks;
        EnsureStreamControl(f,uint32_t(meshDomain->Bindings().size()));uint32_t frameSlot=uint32_t(&f-frames.data());auto pendingStatic=sceneCache.Pending(frameSlot);const bool copyStatic=coreSceneWork&&!pendingStatic.empty();
        auto rayFrame=coreSceneWork?PrepareRayScene(f,tileCatalog,frameSlot,!sphereProxy&&renderSphere==0u):RayFrameData{};
        auto& rayTileAcks=rayFrame.acks;
        const auto& rayCpuDirty=rayFrame.cpuDirty;
        const bool rayDirty=rayFrame.dirty;
        if(coreSceneWork||!publications.empty()){
            if(!PrepareResidency(xr,f,coreSceneWork)){releasePublications(true);publications.clear();endDomains();return;}
        }else{
            std::fill(visibleMaterialBits.begin(),visibleMaterialBits.end(),0);requiredResidencyPages.clear();
        }
        const bool trackingChanged=xr.input.trackingRevision!=lastTrackingRevision||
                                   (paired&&previousNative!=nativeSphere);
        const bool displayGap=lastDisplayTime&&xr.displayTime-lastDisplayTime>xr.displayPeriod*2;
        if(trackingChanged||displayGap)historyReady=false;
        lastTrackingRevision=xr.input.trackingRevision;
        lastDisplayTime=xr.displayTime;

        const bool dlssUsable=dlss.available&&MsaaSamples==1u&&xr.viewCount==2u&&
            coreSceneWork&&f.instanceCount==f.cpuCount&&f.snapshotCount==0u&&
            f.residentCount==0u&&dynamicMeshes.empty()&&staticTransparent.empty();
        const bool useDlss=!sphereBake&&dlssUsable&&dlss.requested>0.f;
        const uint32_t nextWidth=useDlss?std::clamp(uint32_t(std::lround(float(width)*dlss.requested)),
                                                   dlss.minimum.x,dlss.maximum.x):(sphereBake?SphereCacheSize:width);
        const uint32_t nextHeight=useDlss?std::clamp(uint32_t(std::lround(float(height)*dlss.requested)),
                                                    dlss.minimum.y,dlss.maximum.y):(sphereBake?SphereCacheSize:height);
        if(renderWidth!=nextWidth||renderHeight!=nextHeight)historyReady=false;
        renderWidth=nextWidth;
        renderHeight=nextHeight;
        renderMips=std::bit_width(std::max(renderWidth,renderHeight));
        const bool resetDlssHistory=trackingChanged||!dlss.historyReady||
            dlss.lastRenderSize.x!=renderWidth||dlss.lastRenderSize.y!=renderHeight;
        constexpr uint32_t quality=3u;
        uint32_t output=uint32_t(frameNumber&1),previous=1-output;
        auto* shadingRateImage=coreSceneWork&&xr.tracked&&xr.viewCount==2u&&MsaaSamples==1?
            vrsImage.resource.Get():nullptr;
        const bool refreshVrs=shadingRateImage&&(!vrsImageReady||
            vrsWidth!=renderWidth||vrsHeight!=renderHeight||
            std::memcmp(vrsFov.data(),xr.fov.data(),sizeof(vrsFov))!=0);
        auto bindVrs=[shadingRateImage](ID3D12GraphicsCommandList7* l){
            if(!shadingRateImage)return;
            constexpr D3D12_SHADING_RATE_COMBINER combiners[]{
                D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                D3D12_SHADING_RATE_COMBINER_OVERRIDE};
            l->RSSetShadingRate(D3D12_SHADING_RATE_1X1,combiners);
            l->RSSetShadingRateImage(shadingRateImage);
        };
        uint32_t staticCount=sceneCache.StaticCount();f.transparentCount=0;bool hasReflections=false;
        if(coreSceneWork){
            auto alpha=[&](uint32_t index,uint32_t mesh,const auto& p){
                f.transparent[f.transparentCount++]={index,mesh,{p.x,p.y,p.z}};
            };
            if(!sphereProxy)for(uint32_t index:staticTransparent){
                const auto& instance=CpuInstance(f,index);
                alpha(index,instance.mesh,instance.positionRadius);
            }
            hasReflections=!sphereProxy&&staticReflectionCandidates[quality];
            for(uint32_t i=0;i<f.dynamicCount;i++){const auto& object=f.objects[i];
                if(i<f.snapshotCount&&(object.flags&AlphaBlendObject))
                    alpha(staticCount+i,object.mesh,object.positionRadius);
                if(!sphereProxy&&!(object.flags&128u)&&!hasReflections&&object.material<surfaceData.size())
                    hasReflections=SurfaceMayReflect(surfaceData[object.material],quality);
            }
            if(!sphereProxy&&tiles){
                uint32_t first=staticCount+f.snapshotCount;
                for(const auto& [key,tile]:*tiles){
                    for(const auto& object:tile.objects){
                        if(object.enabled&&(object.flags&AlphaBlendObject)){
                            bool visible=true;
                            if(object.flags&BuildingObject){
                                visible=false;
                                if(renderSphere==0u){
                                    const XMFLOAT3 center{object.position.x,object.position.y,object.position.z};
                                    const float radius=.87f*std::max({object.scale.x,object.scale.y,object.scale.z});
                                    for(uint32_t eye=0;eye<xr.viewCount&&!visible;++eye)
                                        visible=SphereIntersectsView(XMLoadFloat4x4(&xr.vp[eye]),center,radius,true);
                                }
                            }
                            if(visible)alpha(first,object.mesh,object.position);
                        }
                        ++first;
                    }
                }
            }
            if(!sphereProxy&&!hasReflections&&tileCatalog)
                for(const auto& [key,tile]:tileCatalog->ResidentTiles()){
                    for(const auto& [material,count]:tile.materialCounts){
                        if(!count)continue;
                        if(material>=surfaceData.size())
                            Fail("Resident tile reflection material out of range");
                        if(SurfaceMayReflect(surfaceData[material],quality)){
                            hasReflections=true;break;
                        }
                    }
                    if(hasReflections)break;
                }
            if(!sphereProxy&&!hasReflections)for(uint32_t material:contributions.streamMaterials){if(material>=surfaceData.size()){hasReflections=true;break;}if(SurfaceMayReflect(surfaceData[material],quality)){hasReflections=true;break;}}


            if(!sphereProxy&&!hasReflections&&!meshDomain->Bindings().empty()&&contributions.streamMaterials.empty())hasReflections=true;
        }else{
            hasReflections=false;historyReady=false;
        }
        const UINT64 completedImpactFence=direct.fence->GetCompletedValue();
        for(auto& frame:frames){
            if(!frame.cutImpactReadbackFence||frame.cutImpactReadbackFence>completedImpactFence)continue;
            frame.cutImpactReadbackFence=0;
            const uint32_t owner=frame.cutImpactReadbackOwner;
            if(frame.cutImpactReadbackEpoch!=cutFlightEpoch[owner])continue;
            const D3D12_RANGE read{0,sizeof(uint32_t)};
            const D3D12_RANGE noWrite{};
            void* mapped{};
            Hr(frame.cutImpactReadback->Map(0,&read,&mapped),"read pending cut impacts");
            cutDrainPending[owner]=*static_cast<const uint32_t*>(mapped)!=0u;
            frame.cutImpactReadback->Unmap(0,&noWrite);
        }
        auto& flightRemaining=cutFlightRemaining[renderSphere];
        // The guide and afterimages have no collision work of their own.
        bool aimVisible=false;
        if(!sphereBake&&f.instanceCount){
            aimVisible=xr.basaltRay.has_value()||xr.laserAfterimageCount!=0u;
            for(uint32_t hand=0;hand<2;++hand){
                const auto& shot=xr.shotHands[hand];
                aimVisible|=shot.aim.tracked;
                if(shot.fired&&shot.aim.tracked)
                    flightRemaining=std::max(flightRemaining,CutFlightDuration(hand,xr));
            }
        }
        const float cutDt=std::clamp(float(double(xr.displayPeriod)*1e-9),.00001f,.05f);
        const bool flyingProjectiles=flightRemaining>0.f;
        cutActive=flyingProjectiles||cutDrainPending[renderSphere];
        RenderPassPlan passPlan{};
        if(coreSceneWork){
            passPlan.occlusion=!sphereProxy&&f.instanceCount>SmallSceneOcclusionThreshold;
            passPlan.depthHierarchy=passPlan.occlusion||hasReflections;
            passPlan.clusteredLights=f.lightCount>DirectLightLimit;
            passPlan.reflections=hasReflections;
            passPlan.shadowAtlas=f.hasShadowAtlasLights;
            passPlan.rayQueries=!sphereProxy&&(passPlan.reflections||f.hasShadowedLights||cutActive);
            // The first user shot must build TLAS even when the ray snapshot
            // did not change while no ray tracing passes were active.
            passPlan.rayWork=(rayDirty||(cutActive&&!f.tlasBuilt))&&
                passPlan.rayQueries&&f.rayInstanceCount>0;
        }
        auto& c=*f.constants;c={};
        c.sphereWave.x=sphereLod.warpRadians;
        const bool blendSky=dense&&f.transparentCount!=0u;
        c.sphereWave.y=blendSky?1.f:0.f;
        if(paired){
            c.spherePair={dense?2u:1u,renderSphere,sphereProxy?0u:sphereObjectOffsets[renderSphere],staticCount};
            c.sphereScene.x=sphereProxy?0u:sphereObjectOffsets[renderSphere+1u];
        }
        if(solarSystem)c.sphereScene.y=1u;
        if(solarBodies){
            c.solarSun={sun.x,sun.y,sun.z,SunRadius};
            c.solarOrientation=renderOrientation;
            c.sphereScene.z=Physical(atlas.reliefNormalsSRV);
            if(xr.lunarBattle){
                c.lunarRipples=xr.lunarBattle->ripples;
                if(xr.lunarGeometry)c.lunarPoses=xr.lunarBattle->poses;
            }
            if(xr.lunarGeometry){
                const auto moon=TransformPoint((*solarBodies)[SolarMoon].p,cosmosToRender);
                c.lunarOrigin={moon.x,moon.y,moon.z,xr.lunarBattle?(lunarScene?2.f:1.f):0.f};
                if(drawCompanionMoon)c.sphereScene.w=sphereObjectOffsets[SolarMoon];
            }
        }
        if(xr.spherePerception){
            const auto& rig=xr.input.head.p;
            c.sphereGuideOrigin={rig.x,rig.y,rig.z,1.6f};
            c.sphereGuideDistances={SpherePerceptionDistances[0],SpherePerceptionDistances[1],
                                    SpherePerceptionDistances[2],0.f};
            c.sphereGuide={xr.spherePerceptionZone,xr.spherePerceptionDistance,
                           uint32_t(xr.sphereRepresentation),0u};
        }
        if(xr.sphereOrbitBand!=Invalid)
            c.sphereGuide={xr.sphereOrbitBand,xr.sphereOrbitOwner,uint32_t(xr.sphereLevels[xr.sphereOrbitOwner]),1u};
        if(tileCatalog){
            const auto camera=solarBodies||sphereBake?xr.input.head.p:
                XMFLOAT3{float(tileCatalog->TileViewerX()),0.f,float(tileCatalog->TileViewerZ())};
            c.tileViewer={camera.x,camera.z,0,0};
        }
        bool reflectionHistoryReady=historyReady&&passPlan.reflections;
        if(useDlss){
            const float scale=float(renderWidth)/float(width);
            const auto phases=uint32_t(std::ceil(8.f/(scale*scale)));
            const auto sample=uint32_t(frameNumber%phases)+1u;
            c.dlssJitter={Halton(sample,2)-.5f,Halton(sample,3)-.5f,0.f,0.f};
        }
        for(uint32_t view=0;view<xr.viewCount;view++){
            c.vp[view]=xr.vp[view];
            c.inverseVP[view]=xr.inverseVP[view];
            if(useDlss){
                const auto jitter=XMMatrixTranslation(2.f*c.dlssJitter.x/float(renderWidth),
                    -2.f*c.dlssJitter.y/float(renderHeight),0.f);
                const auto jittered=XMLoadFloat4x4(&xr.vp[view])*jitter;
                XMStoreFloat4x4(&c.vp[view],jittered);
                XMStoreFloat4x4(&c.inverseVP[view],XMMatrixInverse(nullptr,jittered));
                c.prevDlssVP[view]=resetDlssHistory?xr.vp[view]:dlss.previousVP[view];
            }
            c.previousVP[view]=reflectionHistoryReady?lastVP[view]:c.vp[view];
            c.eye[view]={xr.eyes[view].x,xr.eyes[view].y,xr.eyes[view].z,1};
            lastVP[view]=c.vp[view];
        }
        for(uint32_t i=0;i<ShadowSlots*6;i++)c.shadowVP[i]=shadowMatrices[i];
        SetDomain(f,ResourceDomain::Meshes,{verticesSRV,meshletsSRV,remapSRV,trianglesSRV,rangesSRV,materialsSRV,indicesSRV,meshletRegionsSRV,geometryUAVs[0],geometryUAVs[1],geometryUAVs[2],geometryUAVs[3],geometryUAVs[4],geometryUAVs[5],triangleSolidsSRV});
        SetDomain(f,ResourceDomain::Frame,{f.worldInstanceSRV,f.opaqueOrderSRV,{},f.worldInstanceUAV,{}, {},{}, {},f.cpuVisibility.depthArgumentsUAV,f.cpuVisibility.depthCountUAV,f.publishedVisibility.depthArgumentsUAV,f.publishedVisibility.depthCountUAV,f.cpuVisibility.cullArgumentsUAV,f.cpuVisibility.cullCountUAV,f.publishedVisibility.cullArgumentsUAV,f.publishedVisibility.cullCountUAV,f.cpuVisibility.shadowArgumentsUAV,f.cpuVisibility.shadowCountUAV,f.publishedInstanceSRV,f.instanceRangesSRV,
            f.cpuVisibility.batchVisibilityUAV,f.cpuVisibility.batchVisibilitySRV,
            f.cpuVisibility.batchArgumentsUAV,f.cpuVisibility.batchCountUAV,
            f.cpuVisibility.tileMarksUAV,f.cpuVisibility.tileMarksSRV,
            f.cpuVisibility.tilePrefixesUAV,f.cpuVisibility.tilePrefixesSRV,
            f.cpuVisibility.tileTotalsUAV,f.cpuVisibility.tileTotalsSRV,
            f.cpuVisibility.tileOffsetsUAV,f.cpuVisibility.tileOffsetsSRV});
        SetDomain(f,ResourceDomain::Lighting,{
            f.lightsSRV,f.clusterSRV,f.clusterUAV,shadowSRV,
            f.clusterIndicesSRV,f.clusterIndicesUAV,
            f.clusterGroupCountsSRV,f.clusterGroupCountsUAV,
            f.clusterGroupOffsetsSRV,f.clusterGroupOffsetsUAV,
            f.clusterDirectionalIndicesSRV,f.clusterDirectionalIndicesUAV,
            f.clusterDirectionalCountSRV,f.clusterDirectionalCountUAV,
            f.clusterBoundsSRV,f.clusterBoundsUAV,
            f.lightBvhNodesSRV,f.lightBvhNodesUAV,
            f.lightBvhPairsSRV,f.lightBvhPairsUAV,
            f.lightBvhBoundsSRV,f.lightBvhBoundsUAV});
        SetDomain(f,ResourceDomain::History,{hierarchySRV,depthSRV,hdrResolvedSRV,reflectionsResolvedSRV,historySRV[previous],historyDepthSRV[previous],historyUAV[output],historyDepthUAV[output],historySRV[output],hierarchySRV});
        for(uint32_t mip=0;mip<hierarchyUAV.size();mip++)f.resourceTableData[uint32_t(ResourceDomain::History)][HistoryHiZMipBase+mip]={Physical(hierarchyUAV[mip]),Generation(hierarchyUAV[mip])};
        SetDomain(f,ResourceDomain::Animation,{f.dynamicSRV,f.dynamicUAV,deformationsSRV,f.motionSRV,f.boneSRV,f.weightSRV,skinSRV,morphSRV,f.deformedBoundsSRV,f.deformedBoundsUAV});
        SetDomain(f,ResourceDomain::Visibility,{f.hotBoundsSRV,f.hotBoundsUAV,f.hotMeshesSRV,f.hotMeshesUAV});
        SetDomain(f,ResourceDomain::RayTracing,{f.tlasSRV,f.rayUAV,f.blasAddressesSRV,f.rayMapSRV});
        c.resourceAbi={DescriptorIndex(f.resourceDirectorySRV),ResourceAbiVersion,0,0};c.dimensions={renderWidth,renderHeight,renderMips,0};c.outputDimensions={width,height,renderWidth,renderHeight};c.counts={coreSceneWork?f.instanceCount:0u,coreSceneWork?f.additionalCount:0u,coreSceneWork?f.lightCount:0u,coreSceneWork&&reflectionHistoryReady?1u:0u};c.topology={xr.viewCount,MsaaSamples,passPlan.reflections?1u:0u,f.rayInstanceCount};c.timing={xr.input.SceneNearZ(),xr.input.SceneFarZ(),float(f.simulationTime),float(quality)};
        c.lightBvhLayout={f.lightBvhLeaves,0,0,0};
        const auto hudMemory=managedMemory.Budget();
        constexpr double gibibyte=1024.0*1024.0*1024.0;
        const float estimatedFps=potentialFrameMs>0?1000.f/potentialFrameMs:-1.f;
        const float margin=estimatedFps>0.f?
            100.f*std::max(0.f,(estimatedFps-xr.headsetTargetHz)/xr.headsetTargetHz):-1000.f;
        c.hud={hudFps,estimatedFps,margin,developerMode?1.f:0.f};
        c.atmosphereSphere={atmosphere.center.x,atmosphere.center.y,atmosphere.center.z,atmosphere.radius};
        c.atmosphereColor={atmosphere.color.x,atmosphere.color.y,atmosphere.color.z,atmosphere.density};
        c.atmosphereParams={-1.f,1.f,0,0};
        auto lightFor=[&](const AtmosphereSettings& profile){
            uint32_t best=0u;
            if(paired){
                auto distance=[&](uint32_t i){
                    const auto& p=f.lightData[i].positionRadius;
                    const auto d=Sub({p.x,p.y,p.z},profile.center);
                    return Dot(d,d);
                };
                for(uint32_t i=1;i<f.lightCount;++i)if(distance(i)<distance(best))best=i;
            }
            return best;
        };
        if(f.lightCount){
            const auto& light=f.lightData[lightFor(atmosphere)];
            c.atmosphereLight=light.positionRadius;c.atmosphereEmission=light.colorPower;
            c.atmosphereParams.x=light.parameters.y;
            if(!solarSystem&&atmosphere.emissionReferencePower>0)c.atmosphereParams.y=light.colorPower.w/atmosphere.emissionReferencePower;
        }
        if(sphereRelief&&c.atmosphereParams.x>=0.f){
            // The relief can read an up-to-date shadow tile without redrawing it.
            const auto slot=uint32_t(c.atmosphereParams.x);
            if(cutShadowsPending||(f.shadowDirty&(1u<<slot)))c.atmosphereParams.x=-1.f;
        }
        if(coreSceneWork){
            XMFLOAT3 center{};for(uint32_t view=0;view<xr.viewCount;view++)center=Add(center,xr.eyes[view]);center=Scale(center,1.0f/float(xr.viewCount));
            std::sort(f.transparent.begin(),f.transparent.begin()+f.transparentCount,[&](const auto& a,const auto& b){
                const auto da=Sub(a.center,center),db=Sub(b.center,center);
                const float left=Dot(da,da),right=Dot(db,db);
                return left!=right?left>right:a.index<b.index;
            });
        }

        const bool drawMembrane=(!sphereProxy||(!dense&&sphereRelief))&&!sphereBake&&
            atmosphere.membrane&&atmosphere.radius>0.f;
        if(drawMembrane)EnsureMembraneResources();
        c.membraneField={drawMembrane?Physical(membrane.fieldSRV):Invalid,MembraneFieldSize,0u,0u};
        static_assert(HandCount==2u&&MembranePassagePulseCount==4u);
        const bool resetMembrane=drawMembrane&&membrane.resetHistory;
        if(drawMembrane&&(!paired||xr.membraneHandSphere==renderSphere)){
            std::copy(xr.membraneHandPulses.begin(),xr.membraneHandPulses.end(),c.membraneHandPulses);
            std::copy(xr.membraneStrokeStart.begin(),xr.membraneStrokeStart.end(),c.membraneStrokeStart);
            std::copy(xr.membraneStrokeEnd.begin(),xr.membraneStrokeEnd.end(),c.membraneStrokeEnd);
        }
        if(drawMembrane&&(!paired||xr.membranePassageSphere==renderSphere)){
            std::copy(xr.membranePassagePulses.begin(),xr.membranePassagePulses.end(),c.membranePassagePulses);
        }
        const auto membraneDelta=drawMembrane&&!resetMembrane?
            std::max(0.0,f.simulationTime-membrane.lastTime):0.0;
        const auto patternTime=membrane.patternTime+membraneDelta*atmosphere.film.patternSpeed;
        const auto waveTime=membrane.waveTime+membraneDelta*atmosphere.film.waveSpeed;
        c.membraneAnimation={float(patternTime),float(waveTime),
                             atmosphere.film.waveDuration,atmosphere.film.waveStrength};
        c.membraneOptics={atmosphere.membrane?atmosphere.film.opacity:0.f,atmosphere.film.deformation,
                          atmosphere.film.glitter,std::cos(XMConvertToRadians(atmosphere.film.glitterWidth))};
        c.membraneTint={atmosphere.film.tint.x,atmosphere.film.tint.y,atmosphere.film.tint.z,1.f};
        const bool runCut=!lunarScene&&!sphereProxy&&cutActive&&coreSceneWork&&f.instanceCount&&passPlan.rayQueries;
        const bool runOverlay=runCut||(aimVisible&&f.instanceCount&&(coreSceneWork||sphereProxy));
        const bool drawDebris=!lunarScene&&coreSceneWork&&cutInitialCount!=0u&&atmosphere.radius>0.f;
        if(drawDebris)EnsureDebrisResources();
        const bool resetDebris=drawDebris&&debris.needsReset;
        c.debrisBindings={drawDebris?Physical(debris.eventSRV):Invalid,
                          drawDebris?Physical(debris.floorSRV):Invalid,
                          cutSpatialRoot==0u?Physical(cutSpatialNodesSRV):Invalid,
                          sphereCutRanges[renderSphere].root};
        // Prepare only selects a Frame whose fences have completed on all queues.
        f.retiredCutResources.clear();
        ComPtr<ID3D12Resource> previousPool,previousScratch;
        DescriptorHandle previousPoolSRV,previousPoolUAV,previousScratchSRV,previousScratchUAV;
        const uint32_t previousPoolCapacity=cutCachePoolCapacity,previousScratchCapacity=cutCapacity;
        if(cutInitialCount&&(runCut||xr.laserCut)){
            const UINT64 completed=direct.fence->GetCompletedValue();
            for(auto& frame:frames){
                if(!frame.cutCacheReadbackFence||frame.cutCacheReadbackFence>completed)continue;
                frame.cutCacheReadbackFence=0;
                const D3D12_RANGE read{0,2u*sizeof(uint32_t)};
                void* mapped{};
                Hr(frame.cutCacheReadback->Map(0,&read,&mapped),"read cut buffer capacity");
                const auto* errors=static_cast<const uint32_t*>(mapped);
                for(uint32_t tool=0;tool<2u;++tool){
                    if(!(frame.cutCacheReadbackMask&(1u<<tool)))continue;
                    if(frame.cutCacheReadbackCapacity==cutCachePoolCapacity&&errors[tool]==CutPoolFull)
                        cutCacheNeedsGrowth=true;
                    if(frame.cutScratchReadbackCapacity==cutCapacity&&errors[tool]==CutScratchFull)
                        cutScratchNeedsGrowth=true;
                }
                const D3D12_RANGE noWrite{};
                frame.cutCacheReadback->Unmap(0,&noWrite);
            }
            if(cutCacheNeedsGrowth){
                const uint64_t grown=uint64_t(cutCachePoolCapacity)+
                    std::max(cutCapacity,cutCachePoolCapacity/2u);
                if(grown<=UINT32_MAX){
                    auto larger=Temporary(Buffer(grown*sizeof(CutTriangle),
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON,false);
                    if(larger){
                        const auto capacity=uint32_t(grown);
                        const auto newSRV=Srv(larger.Get(),capacity,sizeof(CutTriangle));
                        const auto newUAV=UavBuffer(larger.Get(),capacity,sizeof(CutTriangle));
                        previousPool=std::move(cutCachePool);
                        previousPoolSRV=cutCachePoolSRV;
                        previousPoolUAV=cutCachePoolUAV;
                        cutCachePool=std::move(larger);
                        cutCachePoolSRV=newSRV;
                        cutCachePoolUAV=newUAV;
                        cutCachePoolCapacity=capacity;
                    }
                }
            }
            if(cutScratchNeedsGrowth&&cutCapacity<=UINT32_MAX/2u){
                const uint32_t grown=cutCapacity*2u;
                auto larger=Temporary(Buffer(uint64_t(grown)*sizeof(CutTriangle),
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON,false);
                if(larger){
                    const auto newSRV=Srv(larger.Get(),grown,sizeof(CutTriangle));
                    const auto newUAV=UavBuffer(larger.Get(),grown,sizeof(CutTriangle));
                    previousScratch=std::move(cutScratch);
                    previousScratchSRV=cutScratchSRV;
                    previousScratchUAV=cutScratchUAV;
                    cutScratch=std::move(larger);
                    cutScratchSRV=newSRV;
                    cutScratchUAV=newUAV;
                    cutCapacity=grown;
                }
            }
        }
        const uint32_t previousBrushCapacity=cutBrushCapacity;
        ComPtr<ID3D12Resource> previousBrushes,previousCandidates;
        DescriptorHandle previousBrushUAV,previousCandidatesUAV;
        if(runOverlay&&cutBrushes){
            if(cutBrushSampleFence&&direct.fence->GetCompletedValue()>=cutBrushSampleFence){
                D3D12_RANGE read{0,sizeof(uint32_t)};
                void* data{};
                Hr(cutBrushSample->Map(0,&read,&data),"read completed cut generation");
                uint32_t generation{};
                std::memcpy(&generation,data,sizeof(generation));
                const D3D12_RANGE noWrite{0,0};
                cutBrushSample->Unmap(0,&noWrite);
                cutBrushBound=generation+(cutBrushBound-cutBrushSampleBound);
                cutBrushSampleFence=0;
            }
            // At most two transactions can commit in this frame. Reserve space
            // before publishing either set of GPU parameters.
            if(cutBrushBound>=cutBrushCapacity-1u){
                const uint32_t grown=cutBrushCapacity+cutBrushCapacity/2u;
                if(grown<=cutBrushCapacity||grown==UINT32_MAX)Fail("cut history address capacity");
                constexpr auto uav=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                auto larger=Temporary(Buffer(UINT64(grown)*CutBrushStride,uav),
                    D3D12_RESOURCE_STATE_COMMON,false);
                auto candidates=larger?Temporary(Buffer((UINT64(grown)+1u)*sizeof(uint32_t),uav),
                    D3D12_RESOURCE_STATE_COMMON,false):ComPtr<ID3D12Resource>{};
                if(larger&&candidates){
                    previousBrushes=std::move(cutBrushes);
                    previousCandidates=std::move(cutBrushCandidates);
                    previousBrushUAV=cutBrushesUAV;
                    previousCandidatesUAV=cutBrushCandidatesUAV;
                    cutBrushes=std::move(larger);
                    cutBrushCandidates=std::move(candidates);
                    cutBrushesUAV=UavBuffer(cutBrushes.Get(),grown,CutBrushStride);
                    cutBrushCandidatesUAV=UavBuffer(cutBrushCandidates.Get(),grown+1u,0u);
                    cutBrushCapacity=grown;
                }
            }
        }
        CutDispatch cutDispatch{};
        if(runOverlay)cutDispatch=PrepareCutDispatch(f,xr,runCut,sphereProxy);
        const bool stepProjectiles=cutDispatch.step!=0;
        uint32_t projectileStepNode=UINT32_MAX;
        if(sphereProxy&&!paired){
            c.sphereAtlas={sphereProjected?Physical(sphereLod.reliefSRV):Invalid,
                (sphereViews[0]&0xffffu)|(sphereViews[1]<<16u),
                runOverlay?Physical(cutOverlaySRV):Invalid,
                sphereMean?Physical(sphereLod.meanSRV):Invalid};
            std::copy(selection.bounds.begin(),selection.bounds.end(),c.sphereBounds);
            for(unsigned i=0;i<2;++i)if(sphereViews[i]!=Invalid){
                const auto& view=sphereLod.views[sphereViews[i]];
                c.sphereSourceVP[i]=view.vp;
                c.sphereSourceRect[i]=view.rect;
                c.sphereSourceDepth[i]=view.depth;
            }
        }
        bool pairShadows=false;
        if(paired&&!sphereBake){
            c.sphereAtlas.z=runOverlay?Physical(cutOverlaySRV):Invalid;
            for(uint32_t owner=0;owner<sphereCount;++owner){
                const auto& slot=selections[owner];
                auto& sprite=c.sphereProjections[owner];
                sprite.atlas.w=uint32_t(slot.level);
                if(slot.level==SphereRepresentation::Hidden&&!xr.spherePerception)continue;
                auto profile=SphereAtmosphere(owner);
                if(solarBodies)profile.center=TransformPoint(SphereWorldCenter(owner),cosmosToRender);
                const auto& cache=SphereCache(owner);
                const auto& sourceAtlas=SphereAtlas(owner);
                sprite.sphere={profile.center.x,profile.center.y,profile.center.z,profile.radius};
                sprite.fog={profile.color.x,profile.color.y,profile.color.z,profile.density};
                const auto first=SphereViewLayer(owner,slot.views[0]);
                const auto second=SphereViewLayer(owner,slot.views[1]);
                sprite.atlas={SphereUsesRelief(slot.level)?Physical(sourceAtlas.reliefSRV):Invalid,
                    (first&0xffffu)|(second<<16u),
                    slot.level==SphereRepresentation::Mean?Physical(cache.meanSRV):Invalid,uint32_t(slot.level)};
                sprite.optics.x=profile.membrane?profile.film.opacity:0.f;
                sprite.optics.y=drawCompanionMoon&&owner==SolarMoon?1.f:0.f;
                sprite.optics.z=owner==SolarMoon&&xr.lunarBattle?1.f:0.f;
                sprite.tint={profile.film.tint.x,profile.film.tint.y,profile.film.tint.z,1.f};
                sprite.parameters={-1.f,cache.warpRadians,float(f.simulationTime*profile.film.patternSpeed),0.f};
                if(solarBodies){
                    sprite.light=c.solarSun;sprite.light.w=SunLightRange;
                    sprite.emission={SunColor.x,SunColor.y,SunColor.z,SunPower};
                    sprite.parameters.w=float(owner*SphereCacheLayers+SphereBankAngles);
                }else if(dense){
                    const auto light=SpectacleLight(owner,xr.input.head.p,f.simulationTime);
                    sprite.light={light.position.x,light.position.y,light.position.z,light.radius};
                    sprite.emission={light.color.x,light.color.y,light.color.z,light.power};
                }else if(f.lightCount){
                    const auto& light=f.lightData[lightFor(profile)];
                    sprite.light=light.positionRadius;sprite.emission=light.colorPower;
                    const float index=light.parameters.y;
                    if(slot.level==SphereRepresentation::Depth&&index>=0.f&&!cutShadowsPending&&
                       !(f.shadowDirty&(1u<<uint32_t(index)))){
                        sprite.parameters.x=index;pairShadows|=profile.density>0.f;
                    }
                }
                for(uint32_t eye=0;eye<xr.viewCount;++eye){
                    const auto box=slot.bounds[eye];
                    sprite.bounds[eye]=box;
                    if(box.z<=box.x||box.w<=box.y||slot.level==SphereRepresentation::Hidden)continue;
                    auto& bounds=c.sphereBounds[eye];
                    if(bounds.z<=bounds.x)bounds=box;
                    else bounds={std::min(bounds.x,box.x),std::min(bounds.y,box.y),
                                 std::max(bounds.z,box.z),std::max(bounds.w,box.w)};
                }
                for(uint32_t j=0;j<2u;++j)if(slot.views[j]!=Invalid){
                    const auto& view=SphereView(owner,slot.views[j]);
                    sprite.sourceVP[j]=view.vp;sprite.sourceRect[j]=view.rect;sprite.sourceDepth[j]=view.depth;
                    if(solarBodies){
                        const auto bodyToCosmos=SolarBodyToCosmos(owner,(*solarBodies)[owner]);
                        XMStoreFloat4x4(&sprite.sourceVP[j],renderToCosmos*XMMatrixInverse(nullptr,bodyToCosmos)*
                            XMLoadFloat4x4(&view.vp));
                    }else if(dense&&owner>=SpectaclePairCount&&slot.views[j]<SphereBankAngles){
                        const auto offset=Sub(SpectacleCenters[1],profile.center);
                        XMStoreFloat4x4(&sprite.sourceVP[j],XMMatrixTranslation(offset.x,offset.y,offset.z)*
                            XMLoadFloat4x4(&view.vp));
                    }
                }
                if(solarBodies){
                    auto& metadata=sprite.sourceDepth[0];
                    const auto& body=(*solarBodies)[owner];
                    const auto toSun=Scale(Unit(body.p),-1.f);
                    XMFLOAT3 localSun;
                    XMStoreFloat3(&localSun,XMVector3Rotate(XMLoadFloat3(&toSun),
                        XMQuaternionConjugate(XMLoadFloat4(&body.q))));
                    metadata.y=localSun.x;metadata.z=localSun.y;metadata.w=localSun.z;
                }
            }
            if(dense)BinSphereField(c,xr,selections);
        }
        if(useDlss)c.dlssOutput={Physical(dlss.eyes[0].outputSrv),
            Physical(dlss.eyes[1].outputSrv),runOverlay&&!paired?Physical(cutOverlaySRV):Invalid,0u};
        const bool drawCut=!lunarScene&&cutInitialCount!=0&&!sphereProxy;
        const auto& cutRange=sphereCutRanges[renderSphere];
        // Before the first edit, the ordinary scene mesh is the only draw.
        const bool drawCutBlocks=drawCut&&cutWorkSubmitted&&cutRange.count!=0u;
        const UINT64 cutDrawOffset=UINT64(cutRange.first)*5u*sizeof(uint32_t);
        uint32_t cutShadowSlots=0;
        if(drawCut&&passPlan.shadowAtlas){
            for(uint32_t slot=0;slot<ShadowSlots;++slot)
                if(f.shadowIDs[slot]!=Invalid)cutShadowSlots|=1u<<slot;
            f.shadowDirty|=cutShadowSlots&~lastCutShadowSlots;
        }
        const uint32_t predicatedShadowSlots=cutShadowsPending?
            cutShadowSlots&~f.shadowDirty:0u;
        const uint32_t shadowWork=f.shadowDirty|predicatedShadowSlots;
        c.resourceAbi.w=drawCut?Physical(cutStateSRV):Invalid;
        c.resourceAbi.z=Invalid;
        c.cutRaster={Invalid,Invalid,Invalid,Invalid};
        if(drawCut||drawDebris){
            c.cutGeometry={Physical(cutOriginalSRV),Invalid,Invalid,cutStaticCount};
            c.cutRaster={Physical(cutCacheStateSRV),
                Physical(cutCacheBlockSRVs[0]),Physical(cutCacheBlockSRVs[1]),
                Physical(cutCachePoolSRV)};
        }
        frameGraph.BeginFrame();f.queueFences={};uint32_t params[4]{};
        auto dispatchScene64=[](ID3D12GraphicsCommandList7* list,uint32_t count){
            const uint32_t groups=uint32_t((uint64_t(count)+63u)/64u);
            if(groups)list->Dispatch(std::min(groups,65535u),(groups+65534u)/65535u,1);
        };
        auto useBuffer=[&](uint32_t node,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess){frameGraph.Buffer(node,resource,mode,firstSync,firstAccess,finalSync,finalAccess);};
        auto useCutMesh=[&](uint32_t node){
            if(!drawCut)return;
            useBuffer(node,cutState.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(!drawCutBlocks)return;
            for(auto* resource:{cutOriginal.Get(),cutCacheState.Get(),cutCacheBlocks[0].Get(),
                                cutCacheBlocks[1].Get(),cutCachePool.Get()})
                useBuffer(node,resource,GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cutDrawCount.Get(),cutCacheDrawArguments.Get()})
                useBuffer(node,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                    D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                    D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
        };
        auto useTexture=[&](uint32_t node,TextureResource& resource,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access){frameGraph.Texture(node,&resource,resource.resource.Get(),resource.layout,mode,layout,sync,access);};
        auto useVrs=[&](uint32_t node){
            if(shadingRateImage)useTexture(node,vrsImage,GraphMode::Read,
                D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,
                D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE);
        };
        auto useTileSources=[&](uint32_t node,D3D12_BARRIER_SYNC sync){
            for(const auto& tile:f.heldTileSources)
                useBuffer(node,tile.Get(),GraphMode::Read,sync,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE,sync,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        };

        if(previousPool){
            const auto node=frameGraph.AddPass("Cut.GrowPool",GraphQueue::Direct,19,
                [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    l->CopyBufferRegion(cutCachePool.Get(),0,previousPool.Get(),0,
                                        previousPool->GetDesc().Width);
                });
            useBuffer(node,previousPool.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COPY,
                D3D12_BARRIER_ACCESS_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);
            useBuffer(node,cutCachePool.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,
                D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
        }
        if(previousBrushes){
            const auto node=frameGraph.AddPass("Cut.GrowHistory",GraphQueue::Direct,43,
                [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    l->CopyBufferRegion(cutBrushes.Get(),0,previousBrushes.Get(),0,
                                        previousBrushes->GetDesc().Width);
                });
            useBuffer(node,previousBrushes.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COPY,
                D3D12_BARRIER_ACCESS_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);
            useBuffer(node,cutBrushes.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,
                D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
        }
        uint32_t cutBrushReadNode=UINT32_MAX;
        uint32_t cutBrushAttempts=0;

        if(resetDebris){
            const auto node=frameGraph.AddPass("Debris.Reset",GraphQueue::Direct,43,
                [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);
                    const uint32_t bindings[]{Physical(debris.eventUAV),Physical(debris.floorUAV)};
                    l->SetComputeRoot32BitConstants(1,2,bindings,0);
                    l->SetPipelineState(debrisResetPSO.Get());
                    l->Dispatch((DEBRIS_FLOOR_CELLS+63u)/64u,1,1);
                });
            for(auto* buffer:{debris.events.Get(),debris.floor.Get()})
                useBuffer(node,buffer,GraphMode::Write,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        }

        if(resetMembrane){
            const auto offset=Reserve(f,sizeof(XMUINT4),16);
            std::memset(f.uploadGeneration->mapped+offset,0,sizeof(XMUINT4));
            const auto node=frameGraph.AddPass("Membrane.Reset",GraphQueue::Direct,43,
                [&,offset](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    l->CopyBufferRegion(membrane.events.Get(),0,f.uploadGeneration->resource.Get(),
                                        offset,sizeof(XMUINT4));
                });
            useBuffer(node,membrane.events.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,
                D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
        }

        if(!dirtySurfaces.empty()&&materials){
            uint64_t count=0;
            for(const auto& range:dirtySurfaces)count+=range.count;
            const auto full=uint32_t(surfaceData.size());
            const bool denseCopy=count*2u>=full;
            const auto slice=StageFrameTransfer(f,(denseCopy?full:count)*sizeof(Surface));
            if(denseCopy)std::memcpy(slice.data,surfaceData.data(),size_t(slice.bytes));
            else{
                UINT64 offset=0;
                for(const auto& range:dirtySurfaces){
                    const UINT64 bytes=uint64_t(range.count)*sizeof(Surface);
                    std::memcpy(slice.data+offset,surfaceData.data()+range.first,size_t(bytes));
                    offset+=bytes;
                }
            }
            auto node=frameGraph.AddPass("Materials.Update",GraphQueue::Direct,9,
                [&,slice,denseCopy](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    if(denseCopy)l->CopyBufferRegion(materials.Get(),0,slice.resource,slice.offset,slice.bytes);
                    else{
                        UINT64 offset=slice.offset;
                        for(const auto& range:dirtySurfaces){
                            const UINT64 bytes=uint64_t(range.count)*sizeof(Surface);
                            l->CopyBufferRegion(materials.Get(),uint64_t(range.first)*sizeof(Surface),
                                               slice.resource,offset,bytes);
                            offset+=bytes;
                        }
                    }
                });
            useBuffer(node,materials.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,
                D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_ALL_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(compute.serial)frameGraph.ExternalWait(node,compute.fence.Get(),compute.serial);
        }
        uint32_t coreColorNode=UINT32_MAX;
        if(coreSceneWork){
            auto uploadNode=frameGraph.AddPass("Upload",GraphQueue::Copy,10,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                if(!f.uploadGeneration)Fail("frame upload generation missing");auto uploadResource=f.uploadGeneration->resource.Get();auto copyAnimation=[&](ID3D12Resource* destination,UINT64 offset){l->CopyBufferRegion(destination,0,uploadResource,offset,destination->GetDesc().Width);};copyAnimation(f.motionBuffer.Get(),f.motionOffset);copyAnimation(f.boneBuffer.Get(),f.boneOffset);copyAnimation(f.weightBuffer.Get(),f.weightOffset);if(f.lightCount)l->CopyBufferRegion(f.lights.Get(),0,uploadResource,f.lightsOffset,UINT64(f.lightCount)*sizeof(Light));
                if(copyStatic){if(!residentStaticInstances)Fail("persistent static scene source missing");for(const auto& range:pendingStatic){UINT64 offset=UINT64(range.first)*sizeof(GpuInstance),bytes=UINT64(range.count)*sizeof(GpuInstance);l->CopyBufferRegion(f.worldInstances.Get(),offset,residentStaticInstances.Get(),offset,bytes);}}if(f.snapshotCount)l->CopyBufferRegion(f.worldInstances.Get(),UINT64(staticCount)*sizeof(GpuInstance),uploadResource,f.objectsOffset,UINT64(f.snapshotCount)*sizeof(GpuInstance));


                for(const auto& transfer:f.tileTransfers){
                    UINT64 sourceOffset=transfer.staging.offset;
                    for(const auto& range:transfer.ranges){
                        const UINT64 destinationOffset=UINT64(range.first)*
                            sizeof(GpuResidentTileGpuInstance);
                        const UINT64 bytes=UINT64(range.count)*
                            sizeof(GpuResidentTileGpuInstance);
                        l->CopyBufferRegion(transfer.buffer.Get(),destinationOffset,
                            transfer.staging.resource,sourceOffset,bytes);
                        sourceOffset+=bytes;
                    }
                }
                for(const auto& transfer:f.tileRunTransfers)
                    l->CopyBufferRegion(transfer.buffer.Get(),0,
                        transfer.staging.resource,transfer.staging.offset,transfer.staging.bytes);

            });
            for(auto* resource:{f.motionBuffer.Get(),f.boneBuffer.Get(),f.weightBuffer.Get()})if(resource)useBuffer(uploadNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);if(copyStatic||f.snapshotCount)useBuffer(uploadNode,f.worldInstances.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);if(copyStatic)useBuffer(uploadNode,residentStaticInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);
            for(const auto& transfer:f.tileTransfers)
                useBuffer(uploadNode,transfer.buffer.Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            for(const auto& transfer:f.tileRunTransfers)
                useBuffer(uploadNode,transfer.buffer.Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
 if(f.lightCount)useBuffer(uploadNode,f.lights.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
        }

        frameResources.Clear();auto frameAddress=f.uploadGeneration->resource->GetGPUVirtualAddress()+f.constantsOffset;
        frameResources.Set({GpuFrameResourceId::SceneDepth,depth.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,depth.layout,depthSRV,{}, {},Dsv(depthDSV),0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::DepthResolved,hierarchy.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,hierarchy.layout,hierarchySRV,{}, {},{},0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::HdrColor,hdr.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,hdr.layout,MsaaSamples==1?hdrResolvedSRV:GpuResourceHandle{},MsaaSamples==1?hdrResolvedUAV:GpuResourceHandle{},Rtv(hdrRTV),{},0,0,0,renderWidth,renderHeight,viewCount});
        auto& resolvedColor=MsaaSamples>1?hdrResolved:hdr;frameResources.Set({GpuFrameResourceId::ResolvedColor,resolvedColor.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,resolvedColor.layout,hdrResolvedSRV,hdrResolvedUAV,MsaaSamples==1?Rtv(hdrRTV):Rtv(hdrResolvedRTV), {},0,0,0,renderWidth,renderHeight,viewCount});
        auto& resolvedReflection=MsaaSamples>1?reflectionsResolved:reflections;frameResources.Set({GpuFrameResourceId::Reflections,resolvedReflection.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,resolvedReflection.layout,reflectionsResolvedSRV,{}, {},{},0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::ShadowAtlas,shadow.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,shadow.layout,shadowSRV,{}, {},Dsv(shadowDSV),0,0,0,3072,2048,1});
        frameResources.Set({GpuFrameResourceId::Lighting,f.lights.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.lightsSRV,{}, {},{},0,f.lights?f.lights->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::Clusters,f.clusters.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.clusterSRV,f.clusterUAV,{}, {},0,f.clusters?f.clusters->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::ClusterIndices,f.clusterIndices.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.clusterIndicesSRV,f.clusterIndicesUAV,{}, {},0,f.clusterIndices?f.clusterIndices->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::LightBvhNodes,f.lightBvhNodes.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.lightBvhNodesSRV,f.lightBvhNodesUAV,{}, {},0,f.lightBvhNodes?f.lightBvhNodes->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::TLAS,f.tlas.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.tlasSRV,{}, {},{},0,f.tlas?f.tlas->GetDesc().Width:0,f.tlas?f.tlas->GetGPUVirtualAddress():0,0,0,0});
        GpuFrameResource constants{GpuFrameResourceId::FrameConstants,f.uploadGeneration->resource.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,{}, {}, {},{},f.constantsOffset,sizeof(Constants),frameAddress,0,0,0};frameResources.Set(constants);constants.id=GpuFrameResourceId::ViewConstants;frameResources.Set(constants);
        for(const auto& bound:publications)publicationCatalog.Add(bound.publication);
        GpuFrameContext domainContext{&frameResources,xr.input.frameId,renderWidth,renderHeight,xr.viewCount,frameAddress,&publicationCatalog,frameSlot,uint32_t(frames.size()),f.simulationTime};


        if(coreSceneWork&&!f.deformationWork.Empty()){
            auto deformNode=frameGraph.AddPass("Deform",GraphQueue::Compute,15,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);
                l->SetPipelineState(deformPSO.Get());
                for(uint32_t dynamicIndex:f.deformationWork.Active()){uint32_t mesh=dynamicMeshes[dynamicIndex];l->SetComputeRoot32BitConstant(1,mesh,2);l->Dispatch((meshes.ranges[mesh].vertexCount+63)/64,1,1);}
                BBarrier(l,f.dynamicVertices.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                l->SetPipelineState(deformBoundsPSO.Get());
                for(uint32_t dynamicIndex:f.deformationWork.Active()){uint32_t mesh=dynamicMeshes[dynamicIndex];l->SetComputeRoot32BitConstant(1,mesh,2);l->Dispatch(1,1,1);}
                BBarrier(l,f.deformedBounds.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            });
            for(auto* resource:{f.motionBuffer.Get(),f.boneBuffer.Get(),f.weightBuffer.Get()})useBuffer(deformNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.dynamicVertices.Get(),f.deformedBounds.Get()})useBuffer(deformNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }


        if(coreSceneWork&&f.cpuCount){
            auto cpuVisibilityNode=frameGraph.AddPass("CpuScene.Visibility",GraphQueue::Compute,20,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                Bind(l,f,false);
                l->SetComputeRoot32BitConstants(1,4,params,0);
                l->SetComputeRoot32BitConstant(1,0,0);
                l->SetComputeRoot32BitConstant(1,f.cpuCount,2);
                l->SetPipelineState(hotDataPSO.Get());
                dispatchScene64(l,f.cpuCount);
                for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                    BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                ClearCount(l,f.cpuVisibility.depthCount.Get(),f.cpuVisibility.depthCountUAV);
                l->SetPipelineState(cullPSO.Get());
                l->SetComputeRoot32BitConstant(1,3,1);
                dispatchScene64(l,f.cpuCount);
                ArgumentsReady(l,f.cpuVisibility.depthArguments.Get(),f.cpuVisibility.depthCount.Get());
            });
            useTileSources(cpuVisibilityNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
            useBuffer(cpuVisibilityNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuVisibilityNode,f.deformedBounds.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                useBuffer(cpuVisibilityNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.cpuVisibility.depthArguments.Get(),f.cpuVisibility.depthCount.Get()})
                useBuffer(cpuVisibilityNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useBuffer(cpuVisibilityNode,f.cpuVisibility.batchVisibility.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }


        if(coreSceneWork&&f.cpuCount){
            auto cpuDepthNode=frameGraph.AddPass("CpuScene.Depth",GraphQueue::Direct,21,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                Bind(l,f,true);
                Viewport(l,renderWidth,renderHeight);
                auto depthHandle=Dsv(depthDSV);
                l->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
                l->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);
                l->SetPipelineState(depthPSO.Get());
                l->SetGraphicsRoot32BitConstants(1,4,params,0);
                l->SetGraphicsRoot32BitConstant(1,0,4);
                l->ExecuteIndirect(indirect.Get(),f.cpuCount,f.cpuVisibility.depthArguments.Get(),0,f.cpuVisibility.depthCount.Get(),0);
                if(drawCutBlocks){
                    l->SetPipelineState(cutDepthPSO.Get());
                    l->SetGraphicsRoot32BitConstant(1,2,4);
                    l->ExecuteIndirect(cutBlockIndirect.Get(),cutRange.count,
                        cutCacheDrawArguments.Get(),cutDrawOffset,cutDrawCount.Get(),0);
                    l->SetGraphicsRoot32BitConstant(1,0,4);
                }
            });
            useCutMesh(cpuDepthNode);
            useTileSources(cpuDepthNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            useBuffer(cpuDepthNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuDepthNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuDepthNode,f.dynamicVertices.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.cpuVisibility.depthArguments.Get(),f.cpuVisibility.depthCount.Get()})
                useBuffer(cpuDepthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useTexture(cpuDepthNode,depth,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
        }


        DomainPassBuilder domainBuilder(*this);domainBuilder.SetDomain(meshDomain->Identity());meshDomain->BuildPasses(domainBuilder,domainContext);domainBuilder.SetDomain(GpuDomainId::Invalid);




        const bool bvhSortNeeded=AddLightBvhPass(f,coreSceneWork);

        if(coreSceneWork){


            if(f.instanceCount>f.cpuCount){
                auto publishedNode=frameGraph.AddPass("MeshSolid.Visibility",GraphQueue::Compute,25,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);
                    l->SetComputeRoot32BitConstants(1,4,params,0);
                    auto dispatchGpu=[&](uint32_t passIndex){
                        l->SetComputeRoot32BitConstant(1,passIndex,1);
                        for(const auto& bound:meshDomain->Bindings()){
                            l->SetComputeRoot32BitConstant(1,bound.base,0);
                            l->SetComputeRoot32BitConstant(1,Invalid,2);
                            uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;
                            l->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::ActiveDispatchOffset,nullptr,0);
                        }
                        l->SetComputeRoot32BitConstants(1,4,params,0);
                    };
                    l->SetPipelineState(hotDataPSO.Get());
                    dispatchGpu(0);
                    for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                        BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    ClearCount(l,f.publishedVisibility.depthCount.Get(),f.publishedVisibility.depthCountUAV);
                    l->SetPipelineState(cullPSO.Get());
                    dispatchGpu(4);
                    ArgumentsReady(l,f.publishedVisibility.depthArguments.Get(),f.publishedVisibility.depthCount.Get());
                });
                useBuffer(publishedNode,f.streamControl.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                useBuffer(publishedNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useBuffer(publishedNode,f.deformedBounds.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                    useBuffer(publishedNode,resource,GraphMode::ReadWrite,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto* resource:{f.publishedVisibility.depthArguments.Get(),f.publishedVisibility.depthCount.Get()})
                    useBuffer(publishedNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            }

            if(passPlan.rayWork){
            auto computeNode=frameGraph.AddPass("DeformCull",GraphQueue::Compute,30,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);if(passPlan.rayWork){
                if(f.rayMapBuild){
                    l->SetPipelineState(rayMapFillPSO.Get());
                    for(const auto& segment:f.dirtyRayMapSegments){
                        uint32_t mapArgs[4]{
                            segment.compactFirst,segment.renderFirst,segment.count,
                            Physical(f.rayMapUAV)};
                        l->SetComputeRoot32BitConstants(1,4,mapArgs,0);
                        const uint32_t groups=(segment.count+63u)/64u;
                        l->Dispatch(std::min(groups,65535u),
                                    (groups+65534u)/65535u,1);
                    }
                    BBarrier(l,f.rayInstanceMap.Get(),
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                }
                l->SetPipelineState(rayBuildPSO.Get());for(const auto& range:rayCpuDirty){l->SetComputeRoot32BitConstant(1,range.first,0);l->SetComputeRoot32BitConstant(1,range.count,2);l->Dispatch(std::min((range.count+63u)/64u,65535u),
                    (((range.count+63u)/64u)+65534u)/65535u,1);}l->SetComputeRoot32BitConstants(1,4,params,0);}});
            if(passPlan.rayWork)useBuffer(computeNode,f.rayInstanceMap.Get(),
                f.rayMapBuild?GraphMode::ReadWrite:GraphMode::Read,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                f.rayMapBuild?(D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                               D3D12_BARRIER_ACCESS_SHADER_RESOURCE):
                              D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayWork)useBuffer(computeNode,materials.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useTileSources(computeNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
            if(passPlan.rayWork&&f.cpuCount)
                useBuffer(computeNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayWork&&f.instanceCount>f.cpuCount)
                useBuffer(computeNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayWork)useBuffer(computeNode,f.rayDescriptors.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            }

            auto depthNode=frameGraph.AddPass("Depth",GraphQueue::Direct,40,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){
                Bind(l,f,true);
                l->SetGraphicsRoot32BitConstants(1,4,params,0);
                if(passPlan.rayQueries&&!f.blasWork.Empty())BuildDynamic(l,f);
                if(passPlan.rayWork){
                    AliasMemory(l);
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                    build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                    build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                    build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
                    build.Inputs.NumDescs=f.rayInstanceCount;
                    build.Inputs.InstanceDescs=f.rayDescriptors->GetGPUVirtualAddress();
                    build.DestAccelerationStructureData=f.tlas->GetGPUVirtualAddress();
                    build.ScratchAccelerationStructureData=f.scratch->GetGPUVirtualAddress();
                    if(f.tlasBuilt&&f.lastTlasCount==f.rayInstanceCount){
                        build.SourceAccelerationStructureData=build.DestAccelerationStructureData;
                        build.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
                    }
                    l->BuildRaytracingAccelerationStructure(&build,0,nullptr);
                    BBarrier(l,f.tlas.Get(),D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
                }
                if(shadowWork){
                    auto dsv=Dsv(shadowDSV);l->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
                    if(f.cpuCount)Bind(l,f,false);
                    bool firstShadow=true;
                    for(uint32_t slot=0;slot<ShadowSlots;++slot){
                        const uint32_t slotBit=1u<<slot;
                        if(!(shadowWork&slotBit))continue;
                        const bool predicated=(predicatedShadowSlots&slotBit)!=0;
                        const UINT64 predicateBase=UINT64(slot)*CutShadowPredicatesPerSlot*sizeof(uint64_t);
                        uint32_t x=(slot%2)*1536,y=(slot/2)*1024;
                        if(!predicated){
                            D3D12_RECT rectangle{LONG(x),LONG(y),LONG(x+1536),LONG(y+1024)};
                            l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0.f,0,1,&rectangle);
                        }
                        if(!f.cpuCount)continue;
                        if(!firstShadow)for(auto* resource:{f.cpuVisibility.shadowArguments.Get(),f.cpuVisibility.shadowCount.Get()})
                            BBarrier(l,resource,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        firstShadow=false;
                        if(predicated)
                            l->SetPredication(cutShadowPredicates.Get(),predicateBase+6u*sizeof(uint64_t),
                                D3D12_PREDICATION_OP_EQUAL_ZERO);
                        ClearCount(l,f.cpuVisibility.shadowCount.Get(),f.cpuVisibility.shadowCountUAV);
                        const uint32_t shadowParams[4]={0,7,f.cpuCount,slot};
                        l->SetComputeRoot32BitConstants(1,4,shadowParams,0);
                        l->SetPipelineState(cullPSO.Get());
                        dispatchScene64(l,f.cpuCount);
                        ArgumentsReady(l,f.cpuVisibility.shadowArguments.Get(),f.cpuVisibility.shadowCount.Get());
                        l->SetPipelineState(shadowPSO.Get());
                        l->SetGraphicsRoot32BitConstant(1,0,4);
                        for(uint32_t face=0;face<6;face++){
                            const uint32_t faceX=x+(face%3)*ShadowFace;
                            const uint32_t faceY=y+(face/3)*ShadowFace;
                            if(predicated){
                                l->SetPredication(cutShadowPredicates.Get(),predicateBase+face*sizeof(uint64_t),
                                    D3D12_PREDICATION_OP_EQUAL_ZERO);
                                D3D12_RECT rectangle{LONG(faceX),LONG(faceY),
                                    LONG(faceX+ShadowFace),LONG(faceY+ShadowFace)};
                                l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0.f,0,1,&rectangle);
                            }
                            Viewport(l,ShadowFace,ShadowFace,faceX,faceY);
                            l->SetGraphicsRoot32BitConstant(1,slot*6+face,1);
                            l->ExecuteIndirect(indirect.Get(),f.cpuCount,f.cpuVisibility.shadowArguments.Get(),
                                uint64_t(face)*f.cpuCount*16u,f.cpuVisibility.shadowCount.Get(),uint64_t(face)*4u);
                            if(drawCutBlocks){
                                l->SetPipelineState(cutShadowPSO.Get());
                                l->SetGraphicsRoot32BitConstant(1,2,4);
                                l->ExecuteIndirect(cutBlockIndirect.Get(),cutRange.count,
                                    cutCacheDrawArguments.Get(),cutDrawOffset,cutDrawCount.Get(),0);
                                l->SetPipelineState(shadowPSO.Get());
                                l->SetGraphicsRoot32BitConstant(1,0,4);
                            }
                        }
                        if(predicated)l->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
                    }
                    EmitBarriers(l,compiled.barriersDuring[0]);
                }
                Viewport(l,renderWidth,renderHeight);
                auto depthHandle=Dsv(depthDSV);
                if(!f.cpuCount)l->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
                l->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);
                l->SetPipelineState(depthPSO.Get());
                l->SetGraphicsRoot32BitConstants(1,4,params,0);
                l->SetGraphicsRoot32BitConstant(1,0,4);
                if(f.instanceCount>f.cpuCount)l->ExecuteIndirect(indirect.Get(),f.instanceCount-f.cpuCount,f.publishedVisibility.depthArguments.Get(),0,f.publishedVisibility.depthCount.Get(),0);
            });
            useCutMesh(depthNode);
            useTileSources(depthNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            useBuffer(depthNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(predicatedShadowSlots)
                useBuffer(depthNode,cutShadowPredicates.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PREDICATION,D3D12_BARRIER_ACCESS_PREDICATION,
                    D3D12_BARRIER_SYNC_PREDICATION,D3D12_BARRIER_ACCESS_PREDICATION);
            if(shadowWork&&f.cpuCount)useBuffer(depthNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(depthNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useBuffer(depthNode,f.dynamicVertices.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayWork)useBuffer(depthNode,f.rayDescriptors.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(f.instanceCount>f.cpuCount)for(auto* resource:{f.publishedVisibility.depthArguments.Get(),f.publishedVisibility.depthCount.Get()})useBuffer(depthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(shadowWork&&f.cpuCount)for(auto* resource:{f.cpuVisibility.shadowArguments.Get(),f.cpuVisibility.shadowCount.Get()})useBuffer(depthNode,resource,GraphMode::ReadWrite,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(passPlan.rayWork)useBuffer(depthNode,f.tlas.Get(),GraphMode::ReadWrite,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ|D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);useTexture(depthNode,depth,f.cpuCount?GraphMode::ReadWrite:GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);if(shadowWork)useTexture(depthNode,shadow,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);

            if(shadowWork)frameGraph.TransitionTexture(depthNode,shadow,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,true);
            frameGraph.TransitionTexture(depthNode,depth,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(stepProjectiles){
                auto& cf=cutFrames[frameSlot];
                auto stepNode=frameGraph.AddPass("Projectile.Step",GraphQueue::Direct,44,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};l->SetDescriptorHeaps(2,heaps);
                    const UINT cleared[4]{};
                    l->ClearUnorderedAccessViewUint(Gpu(Physical(cf.controlUAV)),Cpu(Physical(cf.controlUAV)),cf.control.Get(),cleared,0,nullptr);
                    BBarrier(l,cf.control.Get(),D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    BindCutPipeline(l,CutKernel::ProjectileBounds,cutDispatch.step);
                    dispatchScene64(l,cf.collisionLeaves);
                    auto* bounds=cf.collisionBounds.Get();
                    for(uint32_t level=cf.collisionLeaves/2u;level;level/=2u){
                        BBarrier(l,bounds,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        BindCutPipeline(l,CutKernel::ProjectileRefit,cutDispatch.step);
                        l->SetComputeRoot32BitConstant(1,level,0);
                        dispatchScene64(l,level);
                    }
                    BBarrier(l,bounds,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    BindCutPipeline(l,CutKernel::Projectile,cutDispatch.step);
                    l->Dispatch(CutProjectileCapacity,1,1);
                    if(cutDispatch.buildingReadback){
                        BBarrier(l,cf.buildingHits.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_COPY_SOURCE);
                        l->CopyBufferRegion(cf.buildingReadback.Get(),0,cf.buildingHits.Get(),0,
                            CutProjectileCapacity*sizeof(BuildingImpact));
                    }
                    BBarrier(l,cf.control.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                });
                projectileStepNode=stepNode;
                frameGraph.Dependency(depthNode,stepNode);
                if(cutDispatch.buildingReadback)
                    useBuffer(stepNode,cf.buildingHits.Get(),GraphMode::ReadWrite,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);
                useBuffer(stepNode,cf.collisionBounds.Get(),GraphMode::ReadWrite,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTileSources(stepNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
                for(auto* resource:{ranges.Get(),vertices.Get(),indices.Get(),
                                    f.worldInstances.Get(),f.publishedInstances.Get()})
                    useBuffer(stepNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                if(drawCut)
                    // 4c reads both the cut's authoritative generation and
                    // the 4b cache generation as one already-published frame.
                    // Both producers execute later on this same Direct queue.
                    for(auto* resource:{cutState.Get(),
                                       cutOriginal.Get(),cutSpatialNodes.Get(),cutSpatialBlocks.Get(),
                                       cutCacheState.Get(),cutCacheBlocks[0].Get(),
                                       cutCacheBlocks[1].Get(),cutCachePool.Get()})
                        useBuffer(stepNode,resource,GraphMode::Read,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                if(f.rayInstanceCount){
                    useBuffer(stepNode,f.rayInstanceMap.Get(),GraphMode::Read,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    useBuffer(stepNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
                }
                useBuffer(stepNode,cutProjectiles[cutDispatch.source].Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useBuffer(stepNode,cutProjectiles[cutDispatch.destination].Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                if(drawMembrane)
                    useBuffer(stepNode,membrane.events.Get(),GraphMode::ReadWrite,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useBuffer(stepNode,cf.control.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                if(cutPendingImpacts)useBuffer(stepNode,cutPendingImpacts.Get(),GraphMode::ReadWrite,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            }

            uint32_t visibilityNode=UINT32_MAX;
            if(passPlan.depthHierarchy||passPlan.clusteredLights){
                visibilityNode=frameGraph.AddPass("Visibility",GraphQueue::Compute,50,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);
                    if(passPlan.depthHierarchy){
                        l->SetPipelineState(depthResolvePSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);
                        if(passPlan.occlusion){
                            auto mipReady=[&](uint32_t mip){EmitBarriers(l,compiled.barriersDuring[mip]);};
                            mipReady(0);l->SetPipelineState(hizPSO.Get());for(uint32_t mip=1;mip<renderMips;mip++){l->SetComputeRoot32BitConstant(1,mip,1);l->Dispatch((std::max(1u,renderWidth>>mip)+7)/8,(std::max(1u,renderHeight>>mip)+7)/8,viewCount);mipReady(mip);}for(uint32_t mip=renderMips;mip<mips;mip++)mipReady(mip);
                            ClearCount(l,f.cpuVisibility.cullCount.Get(),f.cpuVisibility.cullCountUAV);
                            ClearCount(l,f.publishedVisibility.cullCount.Get(),f.publishedVisibility.cullCountUAV);
                            l->SetPipelineState(cullPSO.Get());
                            if(f.cpuCount){
                                l->SetComputeRoot32BitConstant(1,0,0);
                                l->SetComputeRoot32BitConstant(1,5,1);
                                l->SetComputeRoot32BitConstant(1,f.cpuCount,2);
                                dispatchScene64(l,f.cpuCount);
                            }
                            if(f.instanceCount>f.cpuCount){
                                l->SetComputeRoot32BitConstant(1,6,1);
                                for(const auto& bound:meshDomain->Bindings()){
                                    l->SetComputeRoot32BitConstant(1,bound.base,0);
                                    l->SetComputeRoot32BitConstant(1,Invalid,2);
                                    uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;
                                    l->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::ActiveDispatchOffset,nullptr,0);
                                }
                            }
                            l->SetComputeRoot32BitConstants(1,4,params,0);
                            ArgumentsReady(l,f.cpuVisibility.cullArguments.Get(),f.cpuVisibility.cullCount.Get());
                            ArgumentsReady(l,f.publishedVisibility.cullArguments.Get(),f.publishedVisibility.cullCount.Get());
                        }else EmitBarriers(l,compiled.barriersDuring[0]);
                    }


                });
                if(passPlan.depthHierarchy){useTexture(visibilityNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(visibilityNode,hierarchy,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);}
                if(passPlan.occlusion){useTileSources(visibilityNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
                if(f.cpuCount)useBuffer(visibilityNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                if(f.instanceCount>f.cpuCount)useBuffer(visibilityNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(f.streamControl)useBuffer(visibilityNode,f.streamControl.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);for(auto* resource:{f.cpuVisibility.cullArguments.Get(),f.cpuVisibility.cullCount.Get(),f.publishedVisibility.cullArguments.Get(),f.publishedVisibility.cullCount.Get()})useBuffer(visibilityNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);}
                if(passPlan.occlusion&&f.cpuCount)
                    useBuffer(visibilityNode,f.cpuVisibility.batchVisibility.Get(),GraphMode::Write,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                if(passPlan.clusteredLights){


                    const uint64_t groups=(uint64_t(clustersCount)+63u)/64u;
                    if(groups>65535u)
                        Fail("Light spatial clusters exceed DX12 dispatch X");
                    const auto clusterNode=frameGraph.AddPass(
                        "Light.Clusters",GraphQueue::Compute,51,
                        [&,groups](const CompiledNode&,
                                   ID3D12GraphicsCommandList7* l){
                            Bind(l,f,false);
                            const uint32_t clusterParams[]{0u,0u,f.clusterIndexCapacity,uint32_t(groups)};
                            l->SetComputeRoot32BitConstants(1,4,clusterParams,0);
                            auto written=[&](ID3D12Resource* resource){
                                BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE|D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            };
                            l->SetPipelineState(clusterCountPSO.Get());
                            l->Dispatch(uint32_t(groups),1u,1u);
                            written(f.clusterGroupCounts.Get());
                            written(f.clusters.Get());
                            l->SetPipelineState(clusterOffsetsPSO.Get());
                            l->Dispatch(1u,1u,1u);
                            written(f.clusterGroupOffsets.Get());
                            l->SetPipelineState(clusterFillPSO.Get());
                            l->Dispatch(uint32_t(groups),1u,1u);
                        });
                    for(auto* resource:{f.lights.Get(),f.lightBvhNodes.Get()})
                        useBuffer(clusterNode,resource,GraphMode::Read,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    for(auto* resource:{f.clusterGroupCounts.Get(),f.clusterGroupOffsets.Get()})
                        useBuffer(clusterNode,resource,GraphMode::ReadWrite,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    for(auto* resource:{f.clusters.Get(),f.clusterBounds.Get(),
                        f.clusterIndices.Get(),
                        f.clusterDirectionalIndices.Get(),
                        f.clusterDirectionalCount.Get()})
                        useBuffer(clusterNode,resource,GraphMode::Write,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                }
                if(passPlan.depthHierarchy){
                    if(passPlan.occlusion)for(uint32_t mip=0;mip<mips;++mip){
                        D3D12_BARRIER_SUBRESOURCE_RANGE range{};range.IndexOrFirstMipLevel=mip;range.NumMipLevels=1;range.NumArraySlices=viewCount;range.NumPlanes=1;
                        frameGraph.TransitionTextureSubresource(visibilityNode,hierarchy,range,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,mip+1==mips);
                    }
                    else frameGraph.TransitionTexture(visibilityNode,hierarchy,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,true);
                }
            }


            if(f.cpuCount){
                auto batchNode=frameGraph.AddPass("CpuScene.BatchOpaque",GraphQueue::Compute,59,
                    [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                        Bind(l,f,false);
                        ClearCount(l,f.cpuVisibility.batchCount.Get(),f.cpuVisibility.batchCountUAV);
                        l->SetPipelineState(batchOpaquePSO.Get());
                        for(uint32_t runIndex=0;runIndex<opaqueRuns.size();++runIndex){
                            const auto& run=opaqueRuns[runIndex];
                            const uint32_t batchParams[4]{run.first,0u,run.count,runIndex};
                            l->SetComputeRoot32BitConstants(1,4,batchParams,0);
                            const uint32_t groups=uint32_t((uint64_t(run.count)+1023u)/1024u);
                            l->Dispatch(std::min(groups,65535u),(groups+65534u)/65535u,1);
                        }


                        auto uavBarrier=[&](ID3D12Resource* resource){
                            BBarrier(l,resource,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE|
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        };
                        for(const auto& tileBatch:tileBatchDispatch){
                            if(!tileBatch.count)continue;
                            const uint32_t args[5]{
                                tileBatch.first,tileBatch.runDescriptor,
                                tileBatch.count,tileBatch.blockFirst,
                                tileBatch.runFirst};
                            l->SetComputeRoot32BitConstants(1,5,args,0);
                            const uint32_t groups=(tileBatch.count+63u)/64u;
                            l->SetPipelineState(tileBatchMarkPSO.Get());
                            l->Dispatch(std::min(groups,65535u),
                                (groups+65534u)/65535u,1);
                            uavBarrier(f.cpuVisibility.tileBatchMarks.Get());
                            l->SetPipelineState(tileBatchScanPSO.Get());
                            l->Dispatch(std::min(tileBatch.blockCount,65535u),
                                (tileBatch.blockCount+65534u)/65535u,1);
                            uavBarrier(f.cpuVisibility.tileBatchPrefixes.Get());
                            uavBarrier(f.cpuVisibility.tileBatchTotals.Get());
                            const uint32_t superFirst=tileBatchBlocks+
                                tileBatch.superFirst;
                            l->SetComputeRoot32BitConstant(1,superFirst,1);
                            l->SetComputeRoot32BitConstant(1,tileBatch.blockCount,2);
                            l->SetPipelineState(tileBatchOffsetScanPSO.Get());
                            l->Dispatch(tileBatch.superCount,1,1);
                            uavBarrier(f.cpuVisibility.tileBatchTotals.Get());
                            uavBarrier(f.cpuVisibility.tileBatchOffsets.Get());
                            l->SetPipelineState(tileBatchOffsetFinalizePSO.Get());
                            l->Dispatch((tileBatch.blockCount+63u)/64u,1,1);
                            uavBarrier(f.cpuVisibility.tileBatchOffsets.Get());
                            l->SetComputeRoot32BitConstants(1,5,args,0);
                            l->SetPipelineState(tileBatchEmitPSO.Get());
                            l->Dispatch(std::min(groups,65535u),
                                (groups+65534u)/65535u,1);
                        }
                        ArgumentsReady(l,f.cpuVisibility.batchArguments.Get(),f.cpuVisibility.batchCount.Get());
                    });
                useTileSources(batchNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
                useBuffer(batchNode,f.worldInstances.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useBuffer(batchNode,f.hotMeshes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useBuffer(batchNode,f.cpuVisibility.batchVisibility.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto* resource:{f.cpuVisibility.tileBatchMarks.Get(),
                    f.cpuVisibility.tileBatchPrefixes.Get(),
                    f.cpuVisibility.tileBatchTotals.Get(),
                    f.cpuVisibility.tileBatchOffsets.Get()})
                    useBuffer(batchNode,resource,GraphMode::ReadWrite,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto* resource:{f.cpuVisibility.batchArguments.Get(),f.cpuVisibility.batchCount.Get()})
                    useBuffer(batchNode,resource,GraphMode::Write,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            }
            ID3D12Resource* cpuOpaqueArguments=f.cpuVisibility.batchArguments.Get();
            ID3D12Resource* cpuOpaqueCount=f.cpuVisibility.batchCount.Get();
            ID3D12Resource* publishedOpaqueArguments=passPlan.occlusion?f.publishedVisibility.cullArguments.Get():f.publishedVisibility.depthArguments.Get();
            ID3D12Resource* publishedOpaqueCount=passPlan.occlusion?f.publishedVisibility.cullCount.Get():f.publishedVisibility.depthCount.Get();


            for(auto it=f.tileDrawCache.begin();it!=f.tileDrawCache.end();){
                if(!tiles||!tiles->contains(it->first))
                    it=f.tileDrawCache.erase(it);
                else ++it;
            }
            if(refreshVrs){
                struct VrsParameters {
                    uint32_t width,height,tileSize,image;
                    XMFLOAT4 tanFov[2];
                } parameters{renderWidth,renderHeight,vrsTileSize,Physical(vrsImageUAV),{}};
                static_assert(sizeof(VrsParameters)==48u);
                for(uint32_t eye=0;eye<2u;++eye){
                    const auto& fov=xr.fov[eye];
                    parameters.tanFov[eye]={std::tan(fov.angleLeft),std::tan(fov.angleUp),
                        std::tan(fov.angleRight),std::tan(fov.angleDown)};
                }
                const UINT64 offset=Reserve(f,sizeof(parameters));
                std::memcpy(f.uploadGeneration->mapped+offset,&parameters,sizeof(parameters));
                const auto address=f.uploadGeneration->resource->GetGPUVirtualAddress()+offset;
                const uint32_t tileWidth=(renderWidth+vrsTileSize-1u)/vrsTileSize;
                const uint32_t tileHeight=(renderHeight+vrsTileSize-1u)/vrsTileSize;
                const uint32_t groupsX=(tileWidth+7u)/8u;
                const uint32_t groupsY=(tileHeight+7u)/8u;
                const auto vrsNode=frameGraph.AddPass("VRS.Build",GraphQueue::Direct,59,
                    [this,address,groupsX,groupsY](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                        ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};
                        l->SetDescriptorHeaps(2,heaps);
                        l->SetComputeRootSignature(root.Get());
                        l->SetComputeRootConstantBufferView(0,address);
                        l->SetPipelineState(vrsPSO.Get());
                        l->Dispatch(groupsX,groupsY,1);
                    });
                useTexture(vrsNode,vrsImage,GraphMode::Write,
                    D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            }
            std::vector<ID3D12CommandList*> residentOpaqueLists;
            residentOpaqueLists.reserve(tileBatchDispatch.size());
            const bool reflectedMode=passPlan.reflections;
            const uint32_t modeIndex=reflectedMode?1u:0u;

            uint64_t pipelineSignature=1469598103934665603ull;
            for(uint32_t mask=0;mask<32;++mask){
                const auto* pso=(reflectedMode?sceneVariantPSO[mask]:
                    sceneDirectVariantPSO[mask]).Get();
                const auto address=reinterpret_cast<uintptr_t>(pso);
                pipelineSignature=HashMemory(&address,sizeof(address),
                    pipelineSignature);
            }
            for(const auto& tileBatch:tileBatchDispatch){
                const auto found=residentTileGpu.find(tileBatch.key);
                if(found==residentTileGpu.end())
                    Fail("Resident material batch lost its GPU tile");
                const auto& tile=found->second;
                if(!tileBatch.count||tile.runs.empty())continue;
                auto& cached=f.tileDrawCache[tileBatch.key][modeIndex];
                const uintptr_t pointers[]{
                    reinterpret_cast<uintptr_t>(f.cpuVisibility.batchArguments.Get()),
                    reinterpret_cast<uintptr_t>(f.cpuVisibility.batchCount.Get()),
                    reinterpret_cast<uintptr_t>(f.uploadGeneration->resource.Get()),
                    reinterpret_cast<uintptr_t>(hdr.resource.Get()),
                    reinterpret_cast<uintptr_t>(reflections.resource.Get()),
                    reinterpret_cast<uintptr_t>(depth.resource.Get()),
                    reinterpret_cast<uintptr_t>(root.Get()),
                    reinterpret_cast<uintptr_t>(descriptors.Get()),
                    reinterpret_cast<uintptr_t>(samplers.Get()),
                    reinterpret_cast<uintptr_t>(indirect.Get()),
                    reinterpret_cast<uintptr_t>(shadingRateImage)
                };
                const uint64_t commandState[]{
                    tile.runRevision,tile.topologyGeneration,
                    tileBatch.first,tileBatch.runFirst,tileBatch.count,
                    f.constantsOffset,renderWidth,renderHeight,MsaaSamples,
                    modeIndex,HashMemory(pointers,sizeof(pointers)),
                    hdrRTV,reflectionsRTV,depthDSV,pipelineSignature
                };
                const uint64_t signature=HashMemory(
                    commandState,sizeof(commandState));
                if(!cached.initialized||cached.signature!=signature){
                    if(!cached.initialized)
                        CommandsInit(cached.commands,D3D12_COMMAND_LIST_TYPE_DIRECT);
                    cached.commands.Reset();
                    auto* l=cached.commands.list.Get();
                    ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};
                    l->SetDescriptorHeaps(2,heaps);
                    l->SetGraphicsRootSignature(root.Get());
                    l->SetGraphicsRootConstantBufferView(0,
                        f.uploadGeneration->resource->GetGPUVirtualAddress()+
                        f.constantsOffset);
                    const uint32_t drawParams[4]{};
                    l->SetGraphicsRoot32BitConstants(1,4,drawParams,0);
                    l->SetGraphicsRoot32BitConstant(1,3,4);
                    Viewport(l,renderWidth,renderHeight);
                    auto dsv=Dsv(depthDSV);
                    D3D12_CPU_DESCRIPTOR_HANDLE targets[]{Rtv(hdrRTV),
                        Rtv(reflectionsRTV)};
                    l->OMSetRenderTargets(reflectedMode?2u:1u,targets,FALSE,&dsv);
                    bindVrs(l);
                    for(uint32_t r=0;r<tile.runs.size();++r){
                        const auto& run=tile.runs[r];
                        auto* pso=(reflectedMode?sceneVariantPSO[run.mask]:
                            sceneDirectVariantPSO[run.mask]).Get();
                        if(!pso)
                            Fail("Resident opaque material specialization missing");
                        l->SetPipelineState(pso);
                        l->ExecuteIndirect(indirect.Get(),run.count,
                            cpuOpaqueArguments,
                            uint64_t(tileBatch.first+run.first)*16,
                            cpuOpaqueCount,
                            uint64_t(tileBatch.runFirst+r)*4);
                    }
                    cached.commands.Close();
                    cached.signature=signature;
                    cached.initialized=true;
                }
                residentOpaqueLists.push_back(cached.commands.list.Get());
            }


            auto opaqueNode=frameGraph.AddPass("Opaque",GraphQueue::Direct,60,
                [&,cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount]
                (const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,true);
                    l->SetGraphicsRoot32BitConstants(1,4,params,0);
                    l->SetGraphicsRoot32BitConstant(1,3,4);
                    Viewport(l,renderWidth,renderHeight);
                    auto depthHandle=Dsv(depthDSV);
                    D3D12_CPU_DESCRIPTOR_HANDLE targets[]={Rtv(hdrRTV),Rtv(reflectionsRTV)};
                    const float clear[]={.008f,.013f,.021f,1.f},black[]={0,0,0,0};
                    l->ClearRenderTargetView(targets[0],blendSky?black:clear,0,nullptr);
                    if(passPlan.reflections)l->ClearRenderTargetView(targets[1],black,0,nullptr);
                    l->OMSetRenderTargets(passPlan.reflections?2:1,targets,FALSE,&depthHandle);
                    bindVrs(l);
                    for(uint32_t i=0;i<opaqueRuns.size();++i){
                        const auto& run=opaqueRuns[i];
                        auto* pso=(passPlan.reflections?
                            sceneVariantPSO[run.mask]:sceneDirectVariantPSO[run.mask]).Get();
                        if(!pso)Fail("Opaque material specialization unavailable");
                        l->SetPipelineState(pso);
                        l->ExecuteIndirect(indirect.Get(),run.count,cpuOpaqueArguments,
                            uint64_t(run.first)*16,cpuOpaqueCount,uint64_t(i)*4);
                    }
                    if(shadingRateImage)l->RSSetShadingRateImage(nullptr);
                });
            useVrs(opaqueNode);
            auto opaqueTileNode=frameGraph.AddPass(
                "Opaque.Resident",GraphQueue::Direct,61);
            frameGraph.Node(opaqueTileNode).reusableDirectLists=
                residentOpaqueLists;
            if(!residentOpaqueLists.empty())useVrs(opaqueTileNode);
            frameGraph.Dependency(opaqueNode,opaqueTileNode);
            useCutMesh(opaqueNode);
            useCutMesh(opaqueTileNode);
            auto opaqueTailNode=frameGraph.AddPass("Opaque.Tail",GraphQueue::Direct,62,
                [&,publishedOpaqueArguments,publishedOpaqueCount]
                (const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,true);
                    l->SetGraphicsRoot32BitConstants(1,4,params,0);
                    l->SetGraphicsRoot32BitConstant(1,0,4);
                    Viewport(l,renderWidth,renderHeight);
                    auto depthHandle=Dsv(depthDSV);
                    D3D12_CPU_DESCRIPTOR_HANDLE targets[]{
                        Rtv(hdrRTV),Rtv(reflectionsRTV)};
                    l->OMSetRenderTargets(passPlan.reflections?2u:1u,targets,
                        FALSE,&depthHandle);
                    bindVrs(l);
                    if(f.instanceCount>f.cpuCount){
                        l->SetPipelineState((passPlan.reflections?scenePSO:sceneDirectPSO).Get());
                        l->ExecuteIndirect(indirect.Get(),f.instanceCount-f.cpuCount,
                            publishedOpaqueArguments,0,publishedOpaqueCount,0);
                    }
                    if(drawCutBlocks){
                        l->SetPipelineState((passPlan.reflections?cutScenePSO:cutSceneDirectPSO).Get());
                        l->SetGraphicsRoot32BitConstant(1,2,4);
                        l->ExecuteIndirect(cutBlockIndirect.Get(),cutRange.count,
                            cutCacheDrawArguments.Get(),cutDrawOffset,cutDrawCount.Get(),0);
                        l->SetGraphicsRoot32BitConstant(1,0,4);
                    }
                    if(shadingRateImage)l->RSSetShadingRateImage(nullptr);
                    if(f.transparentCount){
                        l->SetPipelineState((passPlan.reflections?
                                            transparentPSO:transparentDirectPSO).Get());
                        for(uint32_t index=0;index<f.transparentCount;index++){
                            const auto& object=f.transparent[index];
                            l->SetGraphicsRoot32BitConstant(1,object.index,0);
                            l->DispatchMesh(meshes.ranges[object.mesh].meshletCount,1,1);
                        }
                    }
                });
            frameGraph.Dependency(opaqueTileNode,opaqueTailNode);
            useVrs(opaqueTailNode);
            useCutMesh(opaqueTailNode);
            if(f.rayInstanceCount)useBuffer(opaqueNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueNode;
            useTileSources(opaqueNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            if(f.cpuCount)useBuffer(opaqueNode,f.worldInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(opaqueNode,f.publishedInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(opaqueNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount})useBuffer(opaqueNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)for(auto* lightIndexResource:{
                f.clusters.Get(),f.clusterIndices.Get(),
                f.clusterDirectionalIndices.Get(),f.clusterDirectionalCount.Get()})
                useBuffer(opaqueNode,lightIndexResource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries&&f.rayInstanceCount)useBuffer(opaqueNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);


            if(f.rayInstanceCount)useBuffer(opaqueTileNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueTileNode;
            useTileSources(opaqueTileNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            if(f.cpuCount)useBuffer(opaqueTileNode,f.worldInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(opaqueTileNode,f.publishedInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(opaqueTileNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount})useBuffer(opaqueTileNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueTileNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)for(auto* lightIndexResource:{
                f.clusters.Get(),f.clusterIndices.Get(),
                f.clusterDirectionalIndices.Get(),f.clusterDirectionalCount.Get()})
                useBuffer(opaqueTileNode,lightIndexResource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueTileNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries&&f.rayInstanceCount)useBuffer(opaqueTileNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueTileNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueTileNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueTileNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueTileNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

            if(f.rayInstanceCount)useBuffer(opaqueTailNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueTailNode;
            useTileSources(opaqueTailNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            if(f.cpuCount)useBuffer(opaqueTailNode,f.worldInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(opaqueTailNode,f.publishedInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(opaqueTailNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount})useBuffer(opaqueTailNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueTailNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)for(auto* lightIndexResource:{
                f.clusters.Get(),f.clusterIndices.Get(),
                f.clusterDirectionalIndices.Get(),f.clusterDirectionalCount.Get()})
                useBuffer(opaqueTailNode,lightIndexResource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueTailNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries&&f.rayInstanceCount)useBuffer(opaqueTailNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueTailNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueTailNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueTailNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueTailNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

            if(MsaaSamples>1){frameGraph.TransitionTexture(opaqueTailNode,hdr,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(opaqueTailNode,reflections,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);}else{frameGraph.TransitionTexture(opaqueTailNode,hdr,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(opaqueTailNode,reflections,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}frameGraph.TransitionTexture(opaqueTailNode,depth,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            if(MsaaSamples>1){
                auto resolveNode=frameGraph.AddPass("ResolveColor",GraphQueue::Direct,70,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){for(uint32_t view=0;view<viewCount;view++){l->ResolveSubresource(hdrResolved.resource.Get(),view,hdr.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);if(passPlan.reflections)l->ResolveSubresource(reflectionsResolved.resource.Get(),view,reflections.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);}});
                coreColorNode=resolveNode;
                useTexture(resolveNode,hdr,GraphMode::Read,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);useTexture(resolveNode,hdrResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);if(passPlan.reflections){useTexture(resolveNode,reflections,GraphMode::Read,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);useTexture(resolveNode,reflectionsResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);}
                frameGraph.TransitionTexture(resolveNode,hdrResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(resolveNode,reflectionsResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }

            if(passPlan.reflections){
                auto postNode=frameGraph.AddPass("Post",GraphQueue::Direct,90,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);l->SetPipelineState(denoisePSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
                useTexture(postNode,resolvedReflection,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(postNode,hierarchy,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(reflectionHistoryReady){useTexture(postNode,history[previous],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(postNode,historyDepth[previous],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}useTexture(postNode,history[output],GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);useTexture(postNode,historyDepth[output],GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                frameGraph.TransitionTexture(postNode,history[output],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);frameGraph.TransitionTexture(postNode,historyDepth[output],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                auto mergeNode=frameGraph.AddPass("Reflection.Composite",GraphQueue::Direct,91,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);uint32_t mergeParams[]{0,0,Physical(hdrResolvedUAV),0};l->SetComputeRoot32BitConstants(1,4,mergeParams,0);l->SetPipelineState(mergeReflectionPSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
                useTexture(mergeNode,history[output],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(mergeNode,resolvedColor,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);coreColorNode=mergeNode;
            }

        }else if(!sphereProxy){
            auto clearNode=frameGraph.AddPass("Core.ClearVisualTarget",GraphQueue::Direct,60,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){
                Viewport(l,renderWidth,renderHeight);
                const float color[]={.008f,.013f,.021f,1.f},black[]={0,0,0,0};
                auto rtv=Rtv(hdrRTV);auto dsv=Dsv(depthDSV);
                l->ClearRenderTargetView(rtv,blendSky?black:color,0,nullptr);
                l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0.0f,0,0,nullptr);
                EmitBarriers(l,compiled.barriersDuring[0]);
                EmitBarriers(l,compiled.barriersDuring[1]);
                if(MsaaSamples>1){
                    EmitBarriers(l,compiled.barriersDuring[2]);
                    for(uint32_t view=0;view<viewCount;++view)l->ResolveSubresource(hdrResolved.resource.Get(),view,hdr.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);
                }
            });
            useTexture(clearNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);
            useTexture(clearNode,depth,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ|D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
            if(MsaaSamples>1)useTexture(clearNode,hdrResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);
            frameGraph.TransitionTexture(clearNode,hdr,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET,true);
            frameGraph.TransitionTexture(clearNode,depth,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ|D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,true);
            if(MsaaSamples>1){
                frameGraph.TransitionTexture(clearNode,hdr,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE,true);
                frameGraph.TransitionTexture(clearNode,hdrResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }
            coreColorNode=clearNode;
        }


        uint32_t sphereCaptureNode=Invalid,sphereCaptureSlot=Invalid,sphereMeanNode=Invalid;
        SphereLodCache::View sphereCapture{};
        const bool needLiveView=sphereLod.enabled&&sphereLod.revision>1u&&!selection.fresh&&
            (buildingRevision!=0u||!runCut)&&!xr.laserCut&&!xr.laserHeld;
        if(sphereBake||(needLiveView&&coreSceneWork&&xr.input.sceneUnitsPerMeter==1.f&&
                        xr.input.time>=std::max(sphereLod.refreshAfter,sphereLod.liveCaptureAfter))){
            sphereCapture.vp=c.vp[0];
            const auto vp=XMLoadFloat4x4(&sphereCapture.vp);
            sphereCapture.rect=sphereBake?XMFLOAT4{0.f,0.f,1.f,1.f}:
                SphereScreenBounds(vp,atmosphere.center,atmosphere.radius);
            const auto& r=sphereCapture.rect;
            const XMFLOAT3 center=atmosphere.center;
            const float z=XMVectorGetW(XMVector3Transform(XMLoadFloat3(&center),vp));
            if(r.x>=0.f&&r.y>=0.f&&r.z<=1.f&&r.w<=1.f&&r.z>r.x&&r.w>r.y&&
               z>atmosphere.radius&&z+atmosphere.radius<xr.input.SceneFarZ()){
                sphereCaptureSlot=xr.sphereBakeView;
                if(!sphereBake){
                    const uint32_t a=SphereBankAngles,b=a+1u;
                    sphereCaptureSlot=sphereLod.views[a].fence<=sphereLod.views[b].fence?a:b;
                }
                sphereCapture.depth={z,0.f,0.f,0.f};
                sphereCapture.direction=Unit(Sub(xr.eyes[0],center));
                sphereCapture.revision=sphereLod.revision;
                c.sphereRect=r;
                c.sphereSourceDepth[0]=sphereCapture.depth;
                const bool bakeNormals=solarSystem&&sphereBake;
                sphereCaptureNode=frameGraph.AddPass("Sphere.Capture",GraphQueue::Direct,92,
                    [&,bakeNormals](const CompiledNode&,ID3D12GraphicsCommandList7* list){
                        Bind(list,f,false);
                        const uint32_t p[]{Physical(atlas.reliefUAV),SphereViewLayer(renderSphere,sphereCaptureSlot),
                                           bakeNormals?Physical(atlas.reliefNormalsUAV):Invalid};
                        list->SetComputeRoot32BitConstants(1,3,p,0);
                        list->SetPipelineState(sphereCapturePSO.Get());
                        list->Dispatch(SphereCacheSize/8u,SphereCacheSize/8u,1);
                    });
                frameGraph.Dependency(coreColorNode,sphereCaptureNode);
                useTexture(sphereCaptureNode,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(sphereCaptureNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                if(drawDebris)useBuffer(sphereCaptureNode,debris.floor.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(sphereCaptureNode,atlas.relief,GraphMode::Write,
                    D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                if(bakeNormals)useTexture(sphereCaptureNode,atlas.reliefNormals,GraphMode::Write,
                    D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                if(!dense&&(!sphereBake||sphereCaptureSlot+1u==SphereBankAngles)){
                    const uint32_t first=sphereBake?0u:sphereCaptureSlot;
                    const uint32_t count=sphereBake?SphereBankAngles:1u;
                    sphereMeanNode=frameGraph.AddPass("Sphere.Mean",GraphQueue::Direct,93,
                        [&,first,count](const CompiledNode&,ID3D12GraphicsCommandList7* list){
                            Bind(list,f,false);
                            const uint32_t p[]{Physical(sphereLod.reliefSRV),Physical(sphereLod.meanUAV),first,count};
                            list->SetComputeRoot32BitConstants(1,4,p,0);
                            list->SetPipelineState(sphereMeanPSO.Get());
                            list->Dispatch(1,1,1);
                        });
                    frameGraph.Dependency(sphereCaptureNode,sphereMeanNode);
                    useTexture(sphereMeanNode,sphereLod.relief,GraphMode::Read,
                        D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    useBuffer(sphereMeanNode,sphereLod.mean.Get(),GraphMode::Write,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                }
            }
        }

        if(aaMode&&!useDlss&&!sphereProxy&&!sphereBake){
            if(!aaSourceLease){GpuTextureDesc d{};d.width=renderWidth;d.height=renderHeight;d.depthOrArraySize=viewCount;d.format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.uav=false;d.name=L"FXAA.SpatialSource";aaSourceLease=CreateManagedTexture2D(d);if(!aaSourceLease)Fail("FXAA source allocation");aaSource.resource=aaSourceLease->resource;aaSourceArraySrv=TextureSrv(aaSource.resource.Get(),d.format,{},true);aaSource.layout=D3D12_BARRIER_LAYOUT_COMMON;}
            auto copy=frameGraph.AddPass("FXAA.Snapshot",GraphQueue::Direct,92,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){l->CopyResource(aaSource.resource.Get(),resolvedColor.resource.Get());});
            useTexture(copy,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);useTexture(copy,aaSource,GraphMode::Write,D3D12_BARRIER_LAYOUT_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            auto filter=frameGraph.AddPass("FXAA.Spatial",GraphQueue::Direct,93,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);uint32_t p[]{DescriptorIndex(aaSourceArraySrv),Physical(hdrResolvedUAV),aaMode,0};l->SetComputeRoot32BitConstants(1,4,p,0);l->SetPipelineState(fxaaPSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
            useTexture(filter,aaSource,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(filter,resolvedColor,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        }
        for(auto& domain:externalDomains){domainBuilder.SetDomain(domain->Identity());domain->BuildPasses(domainBuilder,domainContext);}domainBuilder.SetDomain(GpuDomainId::Invalid);

        uint32_t dlssOverlayNode=UINT32_MAX;
        if(runOverlay){
            // The overlay is shared across XR frame slots; the direct queue and
            // the graph hazards serialize mask/list recycling without a CPU wait.
            const uint32_t current=cutDispatch.overlayCurrent,previous=cutDispatch.overlayPrevious;
            const bool reset=cutDispatch.overlayReset;
            auto clearMarks=frameGraph.AddPass("Projectile.ClearPreviousTiles",GraphQueue::Direct,94,[&,current,previous,reset](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                const UINT zeros[4]{};
                ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};
                l->SetDescriptorHeaps(2,heaps);
                if(reset){
                    l->ClearUnorderedAccessViewUint(Gpu(Physical(cutOverlayUAV)),Cpu(Physical(cutOverlayUAV)),cutOverlayMask.resource.Get(),zeros,0,nullptr);
                    l->ClearUnorderedAccessViewUint(Gpu(Physical(cutOverlayTileLastSeenUAV)),Cpu(Physical(cutOverlayTileLastSeenUAV)),cutOverlayTileLastSeen.Get(),zeros,0,nullptr);
                    for(uint32_t i=0;i<2;++i)
                        l->ClearUnorderedAccessViewUint(Gpu(Physical(cutOverlayActiveCountUAVs[i])),Cpu(Physical(cutOverlayActiveCountUAVs[i])),cutOverlayActiveCounts[i].Get(),zeros,0,nullptr);
                }else{
                    BindCutPipeline(l,CutKernel::ClearTiles,cutDispatch.overlay);
                    l->ExecuteIndirect(dispatchIndirect.Get(),1,cutOverlayTileDispatchArgs.Get(),
                                       UINT64(previous)*sizeof(D3D12_DISPATCH_ARGUMENTS),nullptr,0);
                    // The opposite list is now vacant for this frame.
                    l->ClearUnorderedAccessViewUint(Gpu(Physical(cutOverlayActiveCountUAVs[current])),Cpu(Physical(cutOverlayActiveCountUAVs[current])),cutOverlayActiveCounts[current].Get(),zeros,0,nullptr);
                }
            });
            useTexture(clearMarks,cutOverlayMask,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            useBuffer(clearMarks,cutOverlayActiveCounts[current].Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            if(reset){
                useBuffer(clearMarks,cutOverlayTileLastSeen.Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useBuffer(clearMarks,cutOverlayActiveCounts[previous].Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            }else{
                useBuffer(clearMarks,cutOverlayActiveTiles[previous].Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useBuffer(clearMarks,cutOverlayTileDispatchArgs.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            }
            auto markBuffers=[&](uint32_t node){
                for(auto* buffer:{cutOverlayTileLastSeen.Get(),cutOverlayActiveTiles[current].Get(),
                                  cutOverlayActiveCounts[current].Get()})
                    useBuffer(node,buffer,GraphMode::ReadWrite,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            };
            uint32_t marksProducer=clearMarks;
            if(stepProjectiles){
                auto stamp=frameGraph.AddPass("Projectile.Stamp",GraphQueue::Direct,95,
                    [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                        BindCutPipeline(l,CutKernel::Stamp,cutDispatch.overlay);
                        l->Dispatch(CutProjectileCapacity,xr.viewCount,1);
                    });
                frameGraph.Dependency(clearMarks,stamp);
                useBuffer(stamp,cutProjectiles[cutDispatch.destination].Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(stamp,cutOverlayMask,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                markBuffers(stamp);
                marksProducer=stamp;
            }
            if(aimVisible){
                auto laser=frameGraph.AddPass("Projectile.Laser",GraphQueue::Direct,95,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    BindCutPipeline(l,CutKernel::Laser,cutDispatch.overlay);
                    l->Dispatch((width+height+63u)/64u,cutDispatch.laserLines,xr.viewCount);
                });
                frameGraph.Dependency(marksProducer,laser);
                useTexture(laser,cutOverlayMask,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                if(!sphereProxy&&(xr.basaltRay||xr.laserAfterimageCount))
                    useTexture(laser,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                markBuffers(laser);
                marksProducer=laser;
            }
            auto args=frameGraph.AddPass("Projectile.BuildTileArgs",GraphQueue::Direct,95,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                BindCutPipeline(l,CutKernel::BuildTileArgs,cutDispatch.overlay);
                l->Dispatch(1,1,1);
            });
            frameGraph.Dependency(marksProducer,args);
            useBuffer(args,cutOverlayActiveCounts[current].Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            useBuffer(args,cutOverlayTileDispatchArgs.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            if(paired||useDlss||sphereProxy)dlssOverlayNode=args;
            else{
                auto composite=frameGraph.AddPass("Projectile.Composite",GraphQueue::Direct,96,[&,current](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    BindCutPipeline(l,CutKernel::Composite,cutDispatch.overlay);
                    l->ExecuteIndirect(dispatchIndirect.Get(),1,cutOverlayTileDispatchArgs.Get(),
                                       UINT64(current)*sizeof(D3D12_DISPATCH_ARGUMENTS),nullptr,0);
                });
                frameGraph.Dependency(coreColorNode,composite);
                frameGraph.Dependency(args,composite);
                useBuffer(composite,cutOverlayTileDispatchArgs.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                useBuffer(composite,cutOverlayActiveTiles[current].Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useTexture(composite,cutOverlayMask,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(composite,resolvedColor,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                coreColorNode=composite;
            }
        }
        // 4e: raster/collision read the previously committed block ranges.
        // The current frame clips only BVH-selected blocks after all readers,
        // then publishes their new cache generation atomically for next frame.
        uint32_t cutStageNode=UINT32_MAX;
        const uint32_t cutToolMask=drawCut?
            uint32_t(cutDispatch.projectileCut!=0)+2u*uint32_t(cutDispatch.laserCut!=0):0u;
        const uint32_t cutTraceMask=cutDiagnosticEnabled&&!f.cutDiagnosticFence?cutToolMask:0u;
        const uint32_t cutCacheMask=!f.cutCacheReadbackFence?cutToolMask:0u;
        if(cutCacheMask&&!f.cutCacheReadback)
            f.cutCacheReadback=Host(2u*sizeof(uint32_t),D3D12_HEAP_TYPE_READBACK);
        if(cutTraceMask&&!f.cutDiagnosticReadback)
            f.cutDiagnosticReadback=Host(2u*CutDiagnosticStride,D3D12_HEAP_TYPE_READBACK);
        const bool samplePendingImpacts=cutDispatch.projectileCut&&!stepProjectiles&&
            !f.cutImpactReadbackFence;
        if(samplePendingImpacts&&!f.cutImpactReadback)
            f.cutImpactReadback=Host(sizeof(uint32_t),D3D12_HEAP_TYPE_READBACK);
        if(drawCut&&(cutDispatch.projectileCut||cutDispatch.laserCut)){
            auto stage=frameGraph.AddPass("Cut.LocalBlocks",GraphQueue::Direct,97,
                [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    auto cacheBarrier=[&](ID3D12Resource* r){
                        BBarrier(l,r,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    };
                    // Both operations may finish on the same frame if SCORIA
                    // was already in flight. The second reads the first commit.
                    for(const auto parameters:{cutDispatch.projectileCut,cutDispatch.laserCut}){
                        if(!parameters)continue;
                        const bool laser=parameters==cutDispatch.laserCut;
                        if(laser&&cutDispatch.projectileCut){
                            for(auto* arguments:{cutDispatchArguments.Get(),cutCacheDispatch.Get()})
                                BBarrier(l,arguments,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            for(auto* resource:{cutState.Get(),cutCandidateSelection.Get(),
                                               cutScratch.Get(),cutCacheWork.Get()})
                                BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE|
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                            cacheBarrier(cutControl.Get());
                            cacheBarrier(cutBrushes.Get());
                            cacheBarrier(cutBrushCells.Get());
                            if(drawDebris)cacheBarrier(debris.events.Get());
                        }
                        BindCutPipeline(l,CutKernel::Begin,parameters);
                        l->Dispatch(1,1,1);
                        if(drawDebris)cacheBarrier(debris.events.Get());
                        BBarrier(l,cutControl.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        BBarrier(l,cutDispatchArguments.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                        // BeginCut also advances the unique attempt stamp in
                        // cutState.w. Publish it alongside BVH candidate marks.
                        BBarrier(l,cutState.Get(),
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                        BBarrier(l,cutCandidateSelection.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                        BindCutPipeline(l,CutKernel::CacheClear,parameters);
                        l->ExecuteIndirect(dispatchIndirect.Get(),1,cutDispatchArguments.Get(),0,nullptr,0);
                        cacheBarrier(cutCacheBlocks[1].Get());
                        cacheBarrier(cutCacheResults.Get());
                        BindCutPipeline(l,CutKernel::Clip,parameters);
                        l->ExecuteIndirect(dispatchIndirect.Get(),1,cutDispatchArguments.Get(),0,nullptr,0);
                        if(laser){
                            cacheBarrier(cutScratch.Get());
                            cacheBarrier(cutWallCounts.Get());
                            cacheBarrier(cutControl.Get());
                            cacheBarrier(cutBrushCandidates.Get());
                            BindCutPipeline(l,CutKernel::GatherLaserSections,parameters);
                            l->Dispatch(LaserSweep::MaxPlanes,1,1);
                            cacheBarrier(cutWallCounts.Get());
                            cacheBarrier(cutWallSegments.Get());
                            cacheBarrier(cutBrushCandidates.Get());
                            BindCutPipeline(l,CutKernel::BuildLaserWallArgs,parameters);
                            l->Dispatch(1,1,1);
                            cacheBarrier(cutBrushCandidates.Get());
                            BBarrier(l,cutWallDispatch.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                            BindCutPipeline(l,CutKernel::EmitLaserWalls,parameters);
                            l->ExecuteIndirect(dispatchIndirect.Get(),1,cutWallDispatch.Get(),0,nullptr,0);
                        }
                        BBarrier(l,cutControl.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        // Only selected blocks populate temporary triangle scratch.
                        // CacheCount and CacheScatter read that local scratch.
                        BBarrier(l,cutScratch.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                        // The persistent block cache is the authoritative source
                        // for collision and raster after the first successful cut.
                        // Scratch is never published as a complete world mesh.
                        BindCutPipeline(l,CutKernel::CacheBegin,parameters);
                        l->Dispatch(1,1,1);
                        cacheBarrier(cutCacheResults.Get());
                        cacheBarrier(cutCacheWork.Get());
                        BBarrier(l,cutCacheDispatch.Get(),
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                        auto dispatchCache=[&](CutKernel kernel,UINT64 offset){
                            BindCutPipeline(l,kernel,parameters);
                            l->ExecuteIndirect(dispatchIndirect.Get(),1,cutCacheDispatch.Get(),
                                               offset,nullptr,0);
                        };
                        cacheBarrier(cutCacheResults.Get());
                        BindCutPipeline(l,CutKernel::CacheCount,parameters);
                        l->ExecuteIndirect(dispatchIndirect.Get(),1,
                            cutCacheDispatch.Get(),0,nullptr,0);
                        cacheBarrier(cutCacheResults.Get());
                        cacheBarrier(cutCacheWork.Get());
                        // All prior readers of these ranges ran before the late Direct pass.
                        BBarrier(l,cutCachePool.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        dispatchCache(CutKernel::CacheReserve,12u);
                        for(auto* r:{cutCachePool.Get(),cutCacheBlocks[1].Get(),
                                     cutCacheAllocator.Get(),cutCacheWork.Get()})cacheBarrier(r);
                        dispatchCache(CutKernel::CacheScatter,0u);
                        for(auto* r:{cutCachePool.Get(),cutCacheResults.Get(),
                                     cutCacheWork.Get()})cacheBarrier(r);
                        dispatchCache(CutKernel::CacheValidate,12u);
                        cacheBarrier(cutCacheWork.Get());
                        BBarrier(l,cutCacheBlocks[0].Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        dispatchCache(CutKernel::CachePublish,12u);
                        for(auto* r:{cutCacheBlocks[0].Get(),cutCachePool.Get(),
                                     cutCacheAllocator.Get(),cutCacheDrawArguments.Get()})cacheBarrier(r);
                        BindCutPipeline(l,CutKernel::CacheCommit,parameters);
                        l->Dispatch(1,1,1);
                        cacheBarrier(cutCacheState.Get());
                        // Advance the cut revision ONLY after its local ranges
                        // have been validated. A failed impact leaves both
                        // generations and the previous visible mesh untouched.
                        // The local clipping and counting passes read cutState
                        // as SRV between selection and final publication.
                        BBarrier(l,cutState.Get(),
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE|
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        BBarrier(l,cutShadowPredicates.Get(),
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        if(drawDebris)cacheBarrier(debris.events.Get());
                        BindCutPipeline(l,CutKernel::Commit,parameters);
                        l->Dispatch(1,1,1);
                        BBarrier(l,cutState.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                        BBarrier(l,cutDrawCount.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        if(!laser){
                            cacheBarrier(cutControl.Get());
                            BindCutPipeline(l,CutKernel::FinishImpact,parameters);
                            l->Dispatch(1,1,1);
                            if(samplePendingImpacts){
                                BBarrier(l,cutControl.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                    D3D12_BARRIER_ACCESS_COPY_SOURCE);
                                l->CopyBufferRegion(f.cutImpactReadback.Get(),0,cutControl.Get(),
                                                    24u,sizeof(uint32_t));
                                BBarrier(l,cutControl.Get(),D3D12_BARRIER_SYNC_COPY,
                                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            }
                        }
                        if(cutTraceMask){
                            const UINT64 offset=laser?CutDiagnosticStride:0u;
                            const auto snapshot=[&](ID3D12Resource* resource,UINT64 destination,
                                                    UINT64 bytes,D3D12_BARRIER_ACCESS access){
                                BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_SYNC_COPY,access,D3D12_BARRIER_ACCESS_COPY_SOURCE);
                                l->CopyBufferRegion(f.cutDiagnosticReadback.Get(),offset+destination,
                                                    resource,0,bytes);
                                BBarrier(l,resource,D3D12_BARRIER_SYNC_COPY,
                                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_COPY_SOURCE,access);
                            };
                            snapshot(cutControl.Get(),0,20,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            snapshot(cutCacheWork.Get(),32,32,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            snapshot(cutState.Get(),64,16,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                        }
                        if(cutCacheMask){
                            auto* work=cutCacheWork.Get();
                            BBarrier(l,work,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                D3D12_BARRIER_ACCESS_COPY_SOURCE);
                            l->CopyBufferRegion(f.cutCacheReadback.Get(),laser?sizeof(uint32_t):0u,
                                                work,sizeof(uint32_t),sizeof(uint32_t));
                            BBarrier(l,work,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_COPY_SOURCE,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        }
                    }
                    for(auto* r:{cutCacheDrawArguments.Get(),cutDrawCount.Get()})
                        BBarrier(l,r,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                });
            cutStageNode=stage;
            frameGraph.Dependency(coreColorNode,stage);
            if(projectileStepNode!=UINT32_MAX)frameGraph.Dependency(projectileStepNode,stage);
            if(drawDebris)
                useBuffer(stage,debris.events.Get(),GraphMode::ReadWrite,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            if(cutDispatch.projectileCut)
                useBuffer(stage,cutPendingImpacts.Get(),GraphMode::ReadWrite,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            if(cutDispatch.laserCut){
                for(auto* buffer:{cutWallCounts.Get(),cutWallSegments.Get(),cutBrushCandidates.Get()})
                    useBuffer(stage,buffer,GraphMode::ReadWrite,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useBuffer(stage,cutWallDispatch.Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            }
            if(cutBrushes)for(auto* resource:{cutBrushes.Get(),cutBrushCells.Get()})
                useBuffer(stage,resource,GraphMode::ReadWrite,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            for(auto* resource:{cutOriginal.Get(),cutSpatialNodes.Get(),cutSpatialBlocks.Get()})
                useBuffer(stage,resource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(stage,cutCandidateSelection.Get(),GraphMode::ReadWrite,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cutState.Get(),cutScratch.Get(),cutControl.Get()})
                useBuffer(stage,resource,GraphMode::ReadWrite,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(cutBrushes){
                cutBrushAttempts=uint32_t(cutDispatch.projectileCut!=0)+uint32_t(cutDispatch.laserCut!=0);
                // Reconcile the upper bound with committed cuts near the end
                // of the allocation. Read only once the submission fence is ready.
                if(!cutBrushSampleFence&&cutBrushBound+cutBrushAttempts>=cutBrushCapacity-128u){
                    if(!cutBrushSample)cutBrushSample=Host(sizeof(uint32_t),D3D12_HEAP_TYPE_READBACK);
                    // Register this read after the write that publishes the sampled generation.
                    cutBrushReadNode=frameGraph.AddPass("Cut.ReadGeneration",GraphQueue::Direct,98,
                        [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                            l->CopyBufferRegion(cutBrushSample.Get(),0,cutState.Get(),8u,sizeof(uint32_t));
                        });
                    frameGraph.Dependency(stage,cutBrushReadNode);
                    useBuffer(cutBrushReadNode,cutState.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COPY,
                        D3D12_BARRIER_ACCESS_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);
                }
            }
            useBuffer(stage,cutDispatchArguments.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useBuffer(stage,cutDrawCount.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            for(auto* r:{cutCachePool.Get(),cutCacheState.Get(),
                         cutCacheBlocks[0].Get(),cutCacheBlocks[1].Get(),
                         cutCacheAllocator.Get(),
                         cutCacheResults.Get(),cutCacheWork.Get()})
                useBuffer(stage,r,GraphMode::ReadWrite,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(stage,cutCacheDispatch.Get(),GraphMode::ReadWrite,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING|
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                    D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useBuffer(stage,cutShadowPredicates.Get(),GraphMode::ReadWrite,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            useBuffer(stage,cutCacheDrawArguments.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
        }
        uint32_t dlssEvaluationNode=UINT32_MAX;
        if(useDlss){
            const auto inputNode=frameGraph.AddPass("DLSS.Inputs",GraphQueue::Direct,98,
                [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    RecordDlssInputs(l,f);
                });
            frameGraph.Dependency(coreColorNode,inputNode);
            for(auto* source:{&resolvedColor,&depth})
                useTexture(inputNode,*source,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(!f.dlssEvaluation.list)CommandsInit(f.dlssEvaluation,D3D12_COMMAND_LIST_TYPE_DIRECT);
            dlssEvaluationNode=frameGraph.AddPass("DLSS.Evaluate",GraphQueue::Direct,99);
            frameGraph.Node(dlssEvaluationNode).standaloneDirectList=f.dlssEvaluation.list.Get();
            frameGraph.Dependency(inputNode,dlssEvaluationNode);
        }

        GpuBatchTimer timer{};
        // Append pass timestamps to the Direct batch queries without splitting batches.
        auto timeSpherePass=[&](ID3D12GraphicsCommandList7* list,uint32_t offset){
            if(!timer.extraDirectQueries)return;
            const auto queue=QI(GraphQueue::Direct);
            const auto query=timer.slot*GpuBatchTimer::QueriesPerSlot+2u*timer.counts[queue]+offset;
            list->EndQuery(timer.queries[queue],D3D12_QUERY_TYPE_TIMESTAMP,query);
        };
        if(drawMembrane){
            const auto node=frameGraph.AddPass("Membrane.Field",GraphQueue::Direct,97,
                [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);
                    timeSpherePass(l,2u);
                    const uint32_t bindings[]{Physical(membrane.eventSRV),Physical(membrane.fieldUAV)};
                    l->SetComputeRoot32BitConstants(1,2,bindings,0);
                    l->SetPipelineState(membranePSO.Get());
                    l->Dispatch((MembraneFieldSize+7u)/8u,(MembraneFieldSize+7u)/8u,1);
                    timeSpherePass(l,3u);
                });
            useBuffer(node,membrane.events.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useTexture(node,membrane.field,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        }

        if(drawDebris){
            const auto node=frameGraph.AddPass("Debris.Update",GraphQueue::Direct,98,
                [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);
                    const uint32_t bindings[]{Physical(debris.eventUAV),Physical(debris.floorUAV),
                                              Physical(debris.argsUAV)};
                    l->SetComputeRoot32BitConstants(1,3,bindings,0);
                    l->SetPipelineState(debrisBuildPSO.Get());
                    l->Dispatch(1,1,1);
                });
            for(auto* buffer:{debris.events.Get(),debris.floor.Get()})
                useBuffer(node,buffer,GraphMode::ReadWrite,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            useBuffer(node,debris.drawArgs.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            for(auto* source:{materials.Get(),cutOriginal.Get(),cutSpatialNodes.Get(),
                             cutCacheState.Get(),cutCacheBlocks[0].Get(),cutCacheBlocks[1].Get(),
                             cutCachePool.Get()})
                if(source)useBuffer(node,source,GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }

        const auto viewModels=PrepareVrViewModels(f,xr);
        bool dlssPresented=false;
        auto presentNode=frameGraph.AddPass("Present",GraphQueue::Direct,100,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
            timeSpherePass(l,0u);
            if(sphereBake){
                timeSpherePass(l,1u);
                return;
            }
            uint32_t dlssStatus=dlssUsable?0u:Invalid;
            if(useDlss)dlssStatus=dlssPresented?uint32_t(std::lround(1000.f*float(renderWidth)/float(width))):Invalid;
            if(dlssPresented)TransitionDlssOutputs(l,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            auto* output=xrColorTargets[xr.color].Get();
            D3D12_TEXTURE_BARRIER displayBarrier{};
            D3D12_BARRIER_GROUP displayGroup{};displayGroup.Type=D3D12_BARRIER_TYPE_TEXTURE;displayGroup.NumBarriers=1;displayGroup.pTextureBarriers=&displayBarrier;
            if(outputSurface.desktop){
                const bool simultaneous=(output->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)!=0;
                displayBarrier.SyncBefore=D3D12_BARRIER_SYNC_NONE;displayBarrier.SyncAfter=D3D12_BARRIER_SYNC_RENDER_TARGET;
                displayBarrier.AccessBefore=D3D12_BARRIER_ACCESS_NO_ACCESS;displayBarrier.AccessAfter=D3D12_BARRIER_ACCESS_RENDER_TARGET;
                displayBarrier.LayoutBefore=D3D12_BARRIER_LAYOUT_PRESENT;displayBarrier.LayoutAfter=simultaneous?D3D12_BARRIER_LAYOUT_COMMON:D3D12_BARRIER_LAYOUT_RENDER_TARGET;
                displayBarrier.pResource=output;l->Barrier(1,&displayGroup);
            }
            DrawPresentComposite(l,f,xr,params,{dlssStatus,passPlan.reflections,useDlss,
                                                proxyOnly,dense,runOverlay,visibleSphere});
            if(dlssPresented)TransitionDlssOutputs(l,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if(drawDebris){
                l->SetPipelineState(debrisDrawPSO.Get());
                l->ExecuteIndirect(debrisDrawSignature.Get(),1,debris.drawArgs.Get(),0,nullptr,0);
            }
            if(viewModels)DrawVrViewModels(l,viewModels);
            timeSpherePass(l,1u);
            RecordCapture(l,output);
            if(outputSurface.desktop){
                displayBarrier.SyncBefore=D3D12_BARRIER_SYNC_ALL;displayBarrier.SyncAfter=D3D12_BARRIER_SYNC_NONE;
                displayBarrier.AccessBefore=D3D12_BARRIER_ACCESS_RENDER_TARGET;displayBarrier.AccessAfter=D3D12_BARRIER_ACCESS_NO_ACCESS;
                displayBarrier.LayoutBefore=displayBarrier.LayoutAfter;displayBarrier.LayoutAfter=D3D12_BARRIER_LAYOUT_PRESENT;
                l->Barrier(1,&displayGroup);
            }
        });
        if(dense&&!sphereBake){
            if(std::any_of(xr.visibleSpheres.begin(),xr.visibleSpheres.end(),[](uint32_t count){return count>0u;})){
                useTexture(presentNode,atlas.relief,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                if(solarBodies)useTexture(presentNode,atlas.reliefNormals,GraphMode::Read,
                    D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }
        }else if(paired&&!sphereBake){
            for(uint32_t owner=0;owner<sphereCount;++owner){
                const auto level=selections[owner].level;
                auto& cache=SphereCache(owner);
                if(SphereUsesRelief(level))
                    useTexture(presentNode,cache.relief,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                        D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                else if(level==SphereRepresentation::Mean)
                    useBuffer(presentNode,cache.mean.Get(),GraphMode::Read,
                        D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                        D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }
        }else if(sphereProjected){
            useTexture(presentNode,sphereLod.relief,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }else if(sphereMean){
            useBuffer(presentNode,sphereLod.mean.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }
        if(!proxyOnly&&!sphereBake){
            useTexture(presentNode,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.reflections)useTexture(presentNode,history[output],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useTexture(presentNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }
        if(!sphereBake&&(pairShadows||((!sphereProxy||sphereRelief)&&atmosphere.radius>0.f&&
           atmosphere.density>0.f&&c.atmosphereParams.x>=0.f)))
            useTexture(presentNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        if(drawMembrane)
            useTexture(presentNode,membrane.field,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

        if(drawDebris){
            useBuffer(presentNode,debris.events.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_VERTEX_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_VERTEX_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(presentNode,debris.floor.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(presentNode,debris.drawArgs.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
        }
        if(viewModels){
            for(auto* buffer:{phosphorOrbit.vertices.Get(),phosphorOrbit.indices.Get()}){
                useBuffer(presentNode,buffer,GraphMode::Read,
                    D3D12_BARRIER_SYNC_VERTEX_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_VERTEX_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }
        }
        if(coreColorNode!=Invalid)frameGraph.Dependency(coreColorNode,presentNode);
        if(sphereCaptureNode!=Invalid)frameGraph.Dependency(sphereCaptureNode,presentNode);
        if(sphereMeanNode!=Invalid)frameGraph.Dependency(sphereMeanNode,presentNode);
        if(useDlss)frameGraph.Dependency(dlssEvaluationNode,presentNode);
        if(dlssOverlayNode!=Invalid){
            frameGraph.Dependency(dlssOverlayNode,presentNode);
            useTexture(presentNode,cutOverlayMask,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }
        if(cutStageNode!=UINT32_MAX)
            frameGraph.Dependency(cutStageNode,presentNode);
        if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Budget().headroom)){
            if(previousScratch){
                ReleaseDescriptor(cutScratchSRV);
                ReleaseDescriptor(cutScratchUAV);
                cutScratch=std::move(previousScratch);
                cutScratchSRV=previousScratchSRV;
                cutScratchUAV=previousScratchUAV;
                cutCapacity=previousScratchCapacity;
            }
            if(previousPool){
                ReleaseDescriptor(cutCachePoolSRV);
                ReleaseDescriptor(cutCachePoolUAV);
                cutCachePool=std::move(previousPool);
                cutCachePoolSRV=previousPoolSRV;
                cutCachePoolUAV=previousPoolUAV;
                cutCachePoolCapacity=previousPoolCapacity;
            }
            if(previousBrushes){
                ReleaseDescriptor(cutBrushesUAV);
                ReleaseDescriptor(cutBrushCandidatesUAV);
                cutBrushes=std::move(previousBrushes);
                cutBrushCandidates=std::move(previousCandidates);
                cutBrushesUAV=previousBrushUAV;
                cutBrushCandidatesUAV=previousCandidatesUAV;
                cutBrushCapacity=previousBrushCapacity;
            }
            releasePublications(true);publications.clear();managedMemory.EndFrame(SubmittedFences());
            endDomains();active=nullptr;historyReady=false;return;
        }
        residency.Wait(copy.queue.Get());residency.Wait(compute.queue.Get());residency.Wait(direct.queue.Get());
        for(const auto& bound:publications)frameGraph.PublicationComplete(bound.publication,presentNode);
        frameGraph.CompileExecutionBatches();
        float dlssCpuMs=-1.f;
        if(useDlss){
            f.dlssEvaluation.Reset();
            const int64_t evaluationStart=dlssDiagnosticEnabled?QpcCounter():0;
            dlssPresented=EvaluateDlss(f.dlssEvaluation.list.Get(),c,resetDlssHistory);
            if(evaluationStart)dlssCpuMs=float(QpcSeconds(QpcCounter()-evaluationStart)*1000.0);
            f.dlssEvaluation.Close();
        }
        auto batchCommands=[&](GraphQueue queue,uint32_t index)->Commands&{
            auto& slots=f.batchCommands[QI(queue)];while(slots.size()<=index){auto commands=std::make_unique<Commands>();CommandsInit(*commands,queue==GraphQueue::Copy?D3D12_COMMAND_LIST_TYPE_COPY:queue==GraphQueue::Compute?D3D12_COMMAND_LIST_TYPE_COMPUTE:D3D12_COMMAND_LIST_TYPE_DIRECT);slots.push_back(std::move(commands));}return *slots[index];
        };
        const auto progress=coordinator.progress?
            coordinator.progress:std::make_shared<GpuSubmissionProgress>();


        for(const auto& bound:publications)if(bound.deferred){
            if(bound.publication.submission)bound.publication.submission->Observe(progress);
            for(const auto& group:bound.publication.groups)
                if(group.submission)group.submission->Observe(progress);
        }
        auto canSubmit=[&](const GraphPass& pass)->bool{
            for(const auto& wait:pass.externalWaits)
                for(const auto& bound:publications)if(bound.deferred){
                    const auto& pub=bound.publication;
                    if(wait.fence==pub.producerFence.Get()){
                        if(wait.value>pub.producerValue)Fail("GPU publication wait exceeds reserved signal");
                        if(pub.submission){
                            if(!pub.submission->Enqueued(wait.value)){
                                if(coordinator.fallbackSubmitted&&coordinator.fallbackSubmitted())
                                    Fail("GPU producer completed without enqueueing publication signal");
                                return false;
                            }
                        }else if(!coordinator.fallbackSubmitted||!coordinator.fallbackSubmitted())return false;
                    }
                    for(const auto& group:pub.groups)
                        if(wait.fence==group.producerFence.Get()){
                            if(wait.value>group.producerValue)Fail("GPU group wait exceeds reserved signal");
                            if(group.submission){
                                if(!group.submission->Enqueued(wait.value)){
                                    if(coordinator.fallbackSubmitted&&coordinator.fallbackSubmitted())
                                        Fail("GPU producer completed without enqueueing group signal");
                                    return false;
                                }
                            }else if(!coordinator.fallbackSubmitted||!coordinator.fallbackSubmitted())return false;
                        }
                }
            return true;
        };
        bool recordTiming=false;
        uint32_t dlssBatch=Invalid;
        if(measureRender){
            std::array<uint32_t,GraphQueueCount> counts{};
            for(const auto& batch:frameGraph.executionBatches){
                auto& count=counts[QI(batch.queue)];
                if(dlssDiagnosticEnabled&&useDlss&&batch.queue==GraphQueue::Direct&&
                   std::find(batch.nodes.begin(),batch.nodes.end(),dlssEvaluationNode)!=batch.nodes.end())
                    dlssBatch=count;
                ++count;
            }
            const uint32_t slot=uint32_t(frameNumber%GpuBatchTimer::Slots);
            bool supported=!timerSamples[slot].pending;
            for(uint32_t q=0;q<GraphQueueCount;++q)if(counts[q]){
                if(counts[q]>GpuBatchTimer::MaxBatches||!timerQueues[q].queries||!timerQueues[q].readback)supported=false;
            }
            if(supported){
                recordTiming=true;timer.slot=slot;timer.counts=counts;
                if(sphereBaselineTiming)timer.extraDirectQueries=drawMembrane?4u:2u;
                for(uint32_t q=0;q<GraphQueueCount;++q)if(counts[q]){
                    timer.queries[q]=timerQueues[q].queries.Get();
                    timer.readbacks[q]=timerQueues[q].readback.Get();
                }
            }
        }
        frameGraph.Execute(*jobs,batchCommands,canSubmit,progress,recordTiming?&timer:nullptr);
        if(materials)dirtySurfaces.clear();
        if(previousPool){
            passGraph.Forget(previousPool.Get());
            ReleaseDescriptor(previousPoolSRV);
            ReleaseDescriptor(previousPoolUAV);
            f.retiredCutResources.push_back(std::move(previousPool));
            cutCacheNeedsGrowth=false;
        }
        if(previousScratch){
            passGraph.Forget(previousScratch.Get());
            ReleaseDescriptor(previousScratchSRV);
            ReleaseDescriptor(previousScratchUAV);
            f.retiredCutResources.push_back(std::move(previousScratch));
            cutScratchNeedsGrowth=false;
        }
        if(samplePendingImpacts&&cutStageNode!=UINT32_MAX){
            f.cutImpactReadbackFence=frameGraph.Signal(cutStageNode);
            f.cutImpactReadbackEpoch=cutFlightEpoch[renderSphere];
            f.cutImpactReadbackOwner=renderSphere;
        }
        if(stepProjectiles&&cutDispatch.buildingReadback){
            auto& cf=cutFrames[frameSlot];
            cf.buildingFence=frameGraph.Signal(projectileStepNode);
            cf.buildingFrame=frameNumber;
        }
        if(cutCacheMask){
            f.cutCacheReadbackMask=cutCacheMask;
            f.cutCacheReadbackCapacity=cutCachePoolCapacity;
            f.cutScratchReadbackCapacity=cutCapacity;
            f.cutCacheReadbackFence=frameGraph.Signal(cutStageNode);
        }
        if(cutTraceMask){
            f.cutDiagnosticPoolCapacity=cutCachePoolCapacity;
            f.cutDiagnosticScratchCapacity=cutCapacity;
            f.cutDiagnosticMask=cutTraceMask;
            f.cutDiagnosticFrame=xr.input.frameId;
            f.cutDiagnosticFence=frameGraph.Signal(cutStageNode);
        }
        cutBrushBound+=cutBrushAttempts;
        if(cutBrushReadNode!=UINT32_MAX){
            cutBrushSampleBound=cutBrushBound;
            cutBrushSampleFence=frameGraph.Signal(cutBrushReadNode);
        }
        if(previousBrushes){
            passGraph.Forget(previousBrushes.Get());
            passGraph.Forget(previousCandidates.Get());
            ReleaseDescriptor(previousBrushUAV);
            ReleaseDescriptor(previousCandidatesUAV);
            f.retiredCutResources.push_back(std::move(previousBrushes));
            f.retiredCutResources.push_back(std::move(previousCandidates));
        }
        if(resetDebris)debris.needsReset=false;
        if(drawMembrane){
            membrane.resetHistory=false;
            membrane.lastTime=f.simulationTime;
            membrane.patternTime=patternTime;
            membrane.waveTime=waveTime;
        }
if(coreSceneWork)for(uint32_t dynamicIndex:f.deformationWork.Active())f.dynamicGeometryRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];if(passPlan.rayQueries)for(uint32_t dynamicIndex:f.blasWork.Active())f.dynamicBlasRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];f.queueFences=frameGraph.LastFences();f.fenceValue=frameGraph.Signal(presentNode);if(!f.fenceValue)Fail("Frame graph present pass missing fence");


        if(coreSceneWork&&bvhSortNeeded){
            f.lightBvhSorted=true;
            f.lightBvhPendingSort=false;
            f.lightBvhSortedCount=f.lightCount;
            f.lightBvhSortSnapshot=f.lightData;
            f.lightBvhLastSortFrame=frameNumber;
        }

        if(copyStatic)sceneCache.Commit(frameSlot);

        for(const auto& transfer:f.tileRunTransfers){
            auto it=residentTileGpu.find(transfer.key);
            if(it!=residentTileGpu.end()&&
               it->second.runRevision==transfer.revision&&
               it->second.runBuffer.Get()==transfer.buffer.Get())
                it->second.runsDirty=false;
        }
        if(coreSceneWork&&(!rayDirty||passPlan.rayWork))rayScene.Commit(frameSlot);
        if(coreSceneWork&&tileCatalog){
            for(const auto& transfer:f.tileTransfers)
                tileCatalog->CommitTileChanges(transfer.key,
                    transfer.topologyGeneration,transfer.updates);
            if(passPlan.rayWork)
                for(const auto& ack:rayTileAcks)
                    tileCatalog->CommitRayTileChanges(frameSlot,ack.key,
                        ack.topologyGeneration,ack.updates);
        }
        if(passPlan.rayWork){
            f.lastTlasCount=f.rayInstanceCount;
            f.tlasBuilt=true;
            if(f.rayMapBuild)f.committedRayMapSegments=f.rayMapSegments;
        }
        if(recordTiming){
            auto& sample=timerSamples[timer.slot];
            sample.slot=timer.slot;sample.counts=timer.counts;sample.fences=f.queueFences;
            sample.cpuMs=cpuPrepareMs+float(QpcSeconds(QpcCounter()-cpuRenderStart)*1000.0);
            sample.diagnosticFrame=(dlssDiagnosticEnabled||sphereBaselineTiming)?xr.input.frameId:0;
            sample.dlssBatch=dlssBatch;
            sample.extraDirectQueries=timer.extraDirectQueries;
            sample.dlssScale=useDlss?float(renderWidth)/float(width):0.f;
            sample.dlssCpuMs=dlssCpuMs;
            if(dlssDiagnosticEnabled){
                std::array<uint32_t,GraphQueueCount> ordinal{};
                for(const auto& batch:frameGraph.executionBatches){
                    const auto queue=QI(batch.queue);
                    auto& name=sample.batchNames[queue][ordinal[queue]++];
                    const auto& first=frameGraph.Node(batch.nodes.front()).name;
                    const auto& last=frameGraph.Node(batch.nodes.back()).name;
                    if(batch.nodes.size()==1u)std::snprintf(name.data(),name.size(),"%s",first.c_str());
                    else std::snprintf(name.data(),name.size(),"%s..%s",first.c_str(),last.c_str());
                }
            }
            sample.pending=true;
        }
        if(stepProjectiles){
            ++cutStepSerial;
            flightRemaining=std::max(0.f,flightRemaining-cutDt);
            if(cutDispatch.projectileCut){
                cutDrainPending[renderSphere]=true;
                ++cutFlightEpoch[renderSphere];
            }
        }
        if(refreshVrs){
            vrsFov=xr.fov;
            vrsWidth=renderWidth;
            vrsHeight=renderHeight;
            vrsImageReady=true;
        }
        dlss.historyReady=dlssPresented;
        if(useDlss){
            dlss.lastUseFence=f.fenceValue;
            if(dlssPresented){
                dlss.lastRenderSize={renderWidth,renderHeight};
                for(uint32_t eye=0;eye<dlss.eyes.size();++eye)dlss.previousVP[eye]=xr.vp[eye];
            }else{
                dlss.requested=0.f;
                dlss.available=false;
            }
        }
        if(sphereCaptureNode!=Invalid){
            sphereCapture.fence=f.fenceValue;
            sphereLod.views[sphereCaptureSlot]=sphereCapture;
            if(!sphereBake)sphereLod.liveCaptureAfter=xr.input.time+1.0/sphereLod.settings.liteRefreshHz;
        }
        if(sphereMeanNode!=Invalid)sphereLod.meanReady=true;
        if(sphereLod.enabled&&cutStageNode!=Invalid){
            ++sphereLod.revision;
            sphereLod.refreshAfter=xr.input.time+(paired?0.:.30);
            if(paired&&!dense){
                auto& adjacent=SphereCache(1u-renderSphere);
                ++adjacent.revision;adjacent.refreshAfter=sphereLod.refreshAfter;
            }
        }
        cutShadowsPending=cutStageNode!=UINT32_MAX;
        cutWorkSubmitted|=cutShadowsPending;
        lastCutShadowSlots=cutShadowSlots;
        residency.Submitted(SubmittedFences());releasePublications(false);managedMemory.EndFrame(SubmittedFences());
        if(coreSceneWork)for(uint32_t slot=0;slot<ShadowSlots;slot++)if(f.shadowDirty&(1u<<slot))shadowCache[slot]={f.shadowIDs[slot],f.shadowKeys[slot],true};frameNumber++;historyReady=coreSceneWork&&passPlan.reflections;endDomains();active=nullptr;outputFrame.submitted=true;
        outputFrame.sphereRepresentation=xr.sphereRepresentation;
        outputFrame.visibleSpheres=xr.visibleSpheres;
        const int64_t hudNow=QpcCounter();
        if(!hudStart)hudStart=hudNow;
        else{
            ++hudFrames;
            if(hudNow-hudStart>=QpcFrequency()){
                hudFps=float(double(hudFrames)*double(QpcFrequency())/double(hudNow-hudStart));
                hudFrames=0;hudStart=hudNow;
            }
        }
    }


}
