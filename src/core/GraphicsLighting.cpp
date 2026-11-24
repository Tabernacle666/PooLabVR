#include "GraphicsInternal.h"

namespace OASIS {
    bool Graphics::Impl::AddLightBvhPass(Frame& f,bool coreSceneWork) {
        bool bvhGpuLightsMayMove=false;
        for(const auto& light:f.lightData)
            bvhGpuLightsMayMove|=(uint32_t(light.parameters.z)&4u)!=0u;
        for(const auto& bound:meshDomain->Bindings())
            for(const auto& resource:bound.publication.resources)
                if(resource.binding>=GpuMeshPublicationAbi::LightBase&&
                   resource.binding<GpuMeshPublicationAbi::LightBase+MaxLights)
                    bvhGpuLightsMayMove=true;
        const bool bvhSourceChanged=f.lightBvhSortSnapshot.size()!=f.lightData.size()||
            (!f.lightData.empty()&&std::memcmp(f.lightBvhSortSnapshot.data(),
                f.lightData.data(),f.lightData.size()*sizeof(Light))!=0);
        const uint64_t bvhAge=frameNumber>=f.lightBvhLastSortFrame?
            frameNumber-f.lightBvhLastSortFrame:UINT64_MAX;
        const bool bvhSortNeeded=f.lightBvhPendingSort||!f.lightBvhSorted||
            f.lightBvhSortedCount!=f.lightCount||
            ((bvhSourceChanged||bvhGpuLightsMayMove)&&bvhAge>=256u);
        f.lightBvhPendingSort=bvhSortNeeded;
        if(coreSceneWork){
            const uint32_t leaves=f.lightBvhLeaves;
            const uint32_t boundGroups=uint32_t(
                (uint64_t(std::max(1u,f.lightCount))+255u)/256u);
            const auto bvhNode=frameGraph.AddPass(
                "Light.BVH",GraphQueue::Compute,29,
                [this,frame=&f,leaves,boundGroups,bvhSortNeeded]
                (const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    auto& f=*frame;
                    Bind(l,f,false);
                    uint32_t args[5]{leaves,0u,boundGroups,0u,0u};
                    l->SetComputeRoot32BitConstants(1,5,args,0);
                    auto dispatch256=[&](uint32_t count){
                        const uint64_t groups=(uint64_t(count)+255u)/256u;
                        if(groups>uint64_t(65535u)*65535u)
                            Fail("Light BVH dispatch dimensions exhausted");
                        l->Dispatch(uint32_t(std::min<uint64_t>(groups,65535u)),
                            uint32_t((groups+65534u)/65535u),1u);
                    };
                    auto uavBarrier=[&](ID3D12Resource* buffer){
                        BBarrier(l,buffer,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    };
                    if(bvhSortNeeded){
                        l->SetPipelineState(lightBvhPartialPSO.Get());
                        dispatch256(std::max(1u,f.lightCount));
                        uavBarrier(f.lightBvhBounds.Get());
                        l->SetComputeRoot32BitConstant(1,boundGroups,2);
                        l->SetPipelineState(lightBvhBoundsPSO.Get());
                        l->Dispatch(1,1,1);
                        uavBarrier(f.lightBvhBounds.Get());
                        l->SetPipelineState(lightBvhMortonPSO.Get());
                        dispatch256(leaves);
                        uavBarrier(f.lightBvhPairs.Get());
                        for(uint64_t size=2u;size<=leaves;size*=2u)
                            for(uint32_t stride=uint32_t(size/2u);
                                stride>0u;stride>>=1u){
                                l->SetComputeRoot32BitConstant(1,uint32_t(size),1);
                                l->SetComputeRoot32BitConstant(1,stride,2);
                                l->SetPipelineState(lightBvhSortPSO.Get());
                                dispatch256(leaves);
                                uavBarrier(f.lightBvhPairs.Get());
                            }
                    }


                    l->SetPipelineState(lightBvhLeavesPSO.Get());
                    dispatch256(leaves);
                    uavBarrier(f.lightBvhNodes.Get());
                    for(uint32_t level=leaves/2u;level>0u;level>>=1u){
                        l->SetComputeRoot32BitConstant(1,level,1);
                        l->SetComputeRoot32BitConstant(1,level,2);
                        l->SetPipelineState(lightBvhParentsPSO.Get());
                        dispatch256(level);
                        uavBarrier(f.lightBvhNodes.Get());
                    }
                });
            frameGraph.Buffer(bvhNode,f.lights.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.lightBvhNodes.Get(),f.lightBvhPairs.Get(),
                                f.lightBvhBounds.Get()})
                frameGraph.Buffer(bvhNode,resource,GraphMode::ReadWrite,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }

        return bvhSortNeeded;
    }

    Graphics::Impl::RoomProbeUpdate Graphics::Impl::PrepareRoomProbes(Frame& f,XMFLOAT3 viewer,double now) {
        RoomProbeUpdate result{};
        if(!f.lightCount)return result;
        constexpr uint64_t Seed=1469598103934665603ull;
        constexpr int64_t MaxSpan=24;
        auto rangeFor=[](double x,double z,double r){
            constexpr double CellSize=2.;
            return std::array<int64_t,4>{int64_t(std::floor((x-r)/CellSize)),int64_t(std::floor((x+r)/CellSize)),
                int64_t(std::floor((z-r)/CellSize)),int64_t(std::floor((z+r)/CellSize))};
        };
        auto wide=[&](const auto& r){return r[1]-r[0]>MaxSpan||r[3]-r[2]>MaxSpan;};
        auto add=[this](uint64_t& key,const auto& value){key=HashMemory(&value,sizeof(value),key);};
        // Resident caster influence index: fixed 12 Hz CPU refresh, not
        // once per eye or once per display image. Conservative XZ coverage.
        if(roomProbeNextCellScan<0.||now>=roomProbeNextCellScan){
            roomProbeNextCellScan=now+1./12.;
            roomProbeCells.clear();
            roomProbeWideRevision=roomProbeAllCellsRevision=roomProbeNonresidentRevision=Seed;
            for(const auto& object:sceneCache.StaticInstances())
                if(object.material<roomProbeSurfaceRevision.size())
                    add(roomProbeNonresidentRevision,roomProbeSurfaceRevision[object.material]);
            const auto* tiles=contributions.residentTiles?contributions.residentTiles(contributions.context):nullptr;
            if(tiles)for(const auto& [tileKey,tile]:tiles->ResidentTiles())
                for(uint32_t cell=0;cell<GpuResidentTile::Cells;++cell){
                    if(!tile.shadowCasterCounts[cell])continue;
                    const auto& volume=tile.shadowVolumes[cell];
                    uint64_t key=Seed;
                    add(key,tileKey.x);add(key,tileKey.z);add(key,tileKey.layer);add(key,cell);
                    add(key,tile.topologyGeneration);add(key,tile.shadowCellSignatures[cell]);
                    for(auto material:volume.materials)
                        if(material<roomProbeSurfaceRevision.size())add(key,roomProbeSurfaceRevision[material]);
                    add(roomProbeAllCellsRevision,key);
                    const auto r=rangeFor(volume.x,volume.z,volume.radius);
                    if(wide(r)){add(roomProbeWideRevision,key);continue;}
                    for(auto x=r[0];x<=r[1];++x)for(auto z=r[2];z<=r[3];++z)
                        add(roomProbeCells.try_emplace(std::pair{x,z},Seed).first->second,key);
                }
        }
        if(f.lightCount>roomProbeCapacity){
            constexpr UINT stride=sizeof(XMFLOAT4)*8u+sizeof(XMUINT4);
            const uint32_t capacity=GrowCapacity(roomProbeCapacity,f.lightCount);
            passGraph.Forget(roomProbes.Get());
            roomProbes=Temporary(Buffer(uint64_t(capacity)*stride,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_COMMON);
            roomProbesSRV=Srv(roomProbes.Get(),capacity,stride,roomProbesSRV);
            roomProbesUAV=UavBuffer(roomProbes.Get(),capacity,stride,roomProbesUAV);
            roomProbeCapacity=capacity;roomProbeSlots.clear();roomProbeCursor=0;
        }
        roomProbeSlots.resize(f.lightCount);
        uint64_t base=Seed;
        add(base,sceneCache.ContentGeneration());add(base,geometryGenerationSerial);
        add(base,renderSphere);add(base,roomProbeNonresidentRevision);
        std::vector<uint32_t> missing;
        missing.reserve(f.lightCount);
        uint32_t roomCount=0;
        for(uint32_t i=0;i<f.lightCount;++i){
            auto& slot=roomProbeSlots[i];const auto& light=f.lightData[i];
            if((uint32_t(light.parameters.z)&BuildingLightFlag)==0u){slot={};continue;}
            ++roomCount;
            auto source=light;source.parameters.y=source.parameters.w=0.f;
            uint64_t key=base;add(key,source);add(key,f.lightIds[i]);add(key,roomProbeWideRevision);
            const auto r=rangeFor(light.positionRadius.x,light.positionRadius.z,
                double(light.positionRadius.w)+.42*std::abs(double(light.directionOuter.y)));
            if(wide(r))add(key,roomProbeAllCellsRevision);
            else for(auto x=r[0];x<=r[1];++x)for(auto z=r[2];z<=r[3];++z){
                const auto it=roomProbeCells.find(std::pair{x,z});
                add(key,it==roomProbeCells.end()?0ull:it->second);
            }
            if(slot.key!=key)slot={key,false};
            if(!slot.ready)missing.push_back(i);
        }
        if(!roomCount)return result;
        // Keep the reference 72 Hz budgets, but use real-time credits so
        // faster headsets do not multiply the same illumination work.
        const bool warming=!missing.empty();
        const double rate=72.*(warming?48.:12.);
        const double dt=roomProbeLastSchedule<0.?1./72.:std::clamp(now-roomProbeLastSchedule,0.,.25);
        roomProbeLastSchedule=now;
        roomProbeCredits=std::min(warming?48.:12.,roomProbeCredits+dt*rate);
        const uint32_t budget=uint32_t(roomProbeCredits);
        std::vector<uint32_t> selected;
        selected.reserve(budget);
        if(warming&&budget){
            auto distance=[&](uint32_t i){
                const auto& p=f.lightData[i].positionRadius;
                const auto d=Sub(XMFLOAT3{p.x,p.y,p.z},viewer);return Dot(d,d);
            };
            const auto n=std::min(size_t(budget),missing.size());
            std::partial_sort(missing.begin(),missing.begin()+n,missing.end(),
                [&](uint32_t a,uint32_t b){return distance(a)<distance(b);});
            selected.insert(selected.end(),missing.begin(),missing.begin()+n);
        }else if(budget){
            for(uint32_t visited=0;visited<f.lightCount&&selected.size()<budget;++visited){
                const uint32_t i=roomProbeCursor++%f.lightCount;
                if((uint32_t(f.lightData[i].parameters.z)&BuildingLightFlag)!=0u)selected.push_back(i);
            }
        }
        roomProbeCredits-=double(selected.size());
        std::sort(selected.begin(),selected.end());
        for(auto i:selected){
            roomProbeSlots[i].ready=true;
            if(!result.runs.empty()&&result.runs.back().first+result.runs.back().second==i)++result.runs.back().second;
            else result.runs.emplace_back(i,1u);
            ++result.dispatchCount;
        }
        // Readiness follows the light slot. Selected probes are written
        // before shading; an invalid slot cannot expose an old occupant.
        for(uint32_t i=0;i<f.lightCount;++i){
            f.lightData[i].parameters.w=roomProbeSlots[i].ready?1.f:0.f;
            result.ready+=roomProbeSlots[i].ready?1u:0u;
        }
        memcpy(f.uploadGeneration->mapped+f.lightsOffset,f.lightData.data(),UINT64(f.lightCount)*sizeof(Light));
        return result;
    }

    void Graphics::Impl::PrepareLights(Frame& f,const RenderScene& snapshot,const std::optional<SceneLight>& sun) {
        if(snapshot.lightCount!=snapshot.lights.size())Fail("Render scene light count mismatch");
        f.lightCount=snapshot.lightCount;f.lightData.resize(f.lightCount);f.lightIds.resize(f.lightCount);f.shadowDirty=0;f.hasShadowedLights=false;f.hasShadowAtlasLights=false;f.shadowIDs.fill(Invalid);f.shadowKeys.fill(0);


        auto prepareLight=[&](SceneLight light){
            if(light.animationNode!=Invalid){
                auto world=animation.NodeMatrix(light.animationNode)*XMMatrixScaling(1,1,-1);
                XMStoreFloat3(&light.position,XMVector3TransformCoord(XMVectorZero(),world));
                XMStoreFloat3(&light.direction,XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0,0,-1,0),world)));
            }
            if(contributions.lightContext)contributions.lightContext(contributions.context,light,f.simulationTime);
            return light;
        };
        auto packLight=[](const SceneLight& light,int slot){
            return Light{{light.position.x,light.position.y,light.position.z,std::max(.1f,light.radius)},
                {light.color.x,light.color.y,light.color.z,light.power},
                {light.direction.x,light.direction.y,light.direction.z,light.outerCos},
                {light.innerCos,float(slot),float(light.flags),0}};
        };
        f.hasGlobalSun=sun.has_value();
        f.globalSun={};

        const GpuSceneCache* tiles=contributions.residentTiles?
            contributions.residentTiles(contributions.context):nullptr;
        auto affectedSlots=[&](double x,double z,double reach){
            uint32_t mask=0;
            for(uint32_t slot=0;slot<ShadowSlots;++slot){
                if(f.shadowIDs[slot]==Invalid)continue;
                const auto& light=f.shadowLights[slot];
                const double dx=x-light.position.x,dz=z-light.position.z;
                const double radius=light.radius+reach;
                if(dx*dx+dz*dz<=radius*radius)mask|=1u<<slot;
            }
            return mask;
        };
        auto includeMaterial=[&](uint64_t key,uint32_t material){
            if(material>=shadowSurfaceRevision.size())
                Fail("Shadow caster references missing material");
            const auto revision=shadowSurfaceRevision[material];
            return HashMemory(&revision,sizeof(revision),key);
        };
        std::array<uint32_t,ShadowSlots> lastAppearance{};
        for(uint32_t i=0;i<f.lightCount;++i)
            for(uint32_t j=0;j<ShadowSlots;++j)
                if(snapshot.lights[i].id==shadowCache[j].id)lastAppearance[j]=i;
        std::array<bool,ShadowSlots> assigned{};
        for(uint32_t i=0;i<f.lightCount;i++) {
            auto light=prepareLight(snapshot.lights[i]);int slot=-1;
            if((light.flags&1)&&!(light.flags&(2|4))) {
                for(uint32_t j=0;j<ShadowSlots;j++)if(!assigned[j]&&shadowCache[j].id==light.id){slot=int(j);break;}
                if(slot<0)for(uint32_t j=0;j<ShadowSlots;j++)if(!assigned[j]){if(lastAppearance[j]<=i){slot=int(j);break;}}
            }
            if(slot>=0) {
                uint32_t s=uint32_t(slot);assigned[s]=true;f.shadowIDs[s]=light.id;f.shadowLights[s]=light;
                uint64_t key=1469598103934665603ull;
                key=HashMemory(&light.position,sizeof(light.position),key);
                key=HashMemory(&light.radius,sizeof(light.radius),key);
                key=HashMemory(&light.revision,sizeof(light.revision),key);

                f.shadowKeys[s]=key;
                auto at=XMLoadFloat3(&light.position);const XMVECTOR directions[]={XMVectorSet(1,0,0,0),XMVectorSet(-1,0,0,0),XMVectorSet(0,1,0,0),XMVectorSet(0,-1,0,0),XMVectorSet(0,0,1,0),XMVectorSet(0,0,-1,0)};
                for(uint32_t face=0;face<6;face++){auto up=face==2?XMVectorSet(0,0,-1,0):face==3?XMVectorSet(0,0,1,0):XMVectorSet(0,1,0,0);XMStoreFloat4x4(&shadowMatrices[s*6+face],XMMatrixLookToLH(at,directions[face],up)*XMMatrixPerspectiveFovLH(XM_PIDIV2,1,std::max(.1f,light.radius),.02f));}
            }
            f.hasShadowedLights|=(light.flags&1)!=0;f.hasShadowAtlasLights|=slot>=0;
            f.lightData[i]=packLight(light,slot);f.lightIds[i]=light.id;
        }
        if(sun){
            const auto light=prepareLight(*sun);
            f.globalSun=packLight(light,-1);
            f.hasShadowedLights|=(light.flags&1u)!=0u;
        }
        if(f.hasShadowAtlasLights){
            auto includeCaster=[&](const GpuInstance& object){
                if(object.flags&(DynamicObject|DeformedObject|NoShadow))return;
                if(object.mesh>=meshes.ranges.size())Fail("Shadow caster references missing mesh");
                const auto bounds=meshes.ranges[object.mesh].bounds;
                const double scale=std::max({std::abs(double(object.scale.x)),
                    std::abs(double(object.scale.y)),std::abs(double(object.scale.z))});
                const double localReach=std::sqrt(double(bounds.x)*bounds.x+
                    double(bounds.y)*bounds.y+double(bounds.z)*bounds.z)+bounds.w;
                const auto slots=affectedSlots(object.positionRadius.x,object.positionRadius.z,
                    localReach*scale*2.5+2.);
                for(uint32_t slot=0;slot<ShadowSlots;++slot)if(slots&(1u<<slot))
                    f.shadowKeys[slot]=includeMaterial(
                        HashMemory(&object,sizeof(object),f.shadowKeys[slot]),object.material);
            };
            for(const auto& object:sceneCache.StaticInstances())includeCaster(object);
            for(uint32_t i=0;i<f.snapshotCount;++i)includeCaster(f.objects[i]);
            if(tiles)for(const auto& [tileKey,tile]:tiles->ResidentTiles())
                for(uint32_t cell=0;cell<GpuResidentTile::Cells;++cell){
                    if(!tile.shadowCasterCounts[cell])continue;
                    const auto& volume=tile.shadowVolumes[cell];
                    const auto slots=affectedSlots(volume.x,volume.z,volume.radius);
                    for(uint32_t slot=0;slot<ShadowSlots;++slot)if(slots&(1u<<slot)){
                        auto& key=f.shadowKeys[slot];
                        key=HashMemory(&tileKey.x,sizeof(tileKey.x),key);
                        key=HashMemory(&tileKey.z,sizeof(tileKey.z),key);
                        key=HashMemory(&cell,sizeof(cell),key);
                        key=HashMemory(&tile.shadowCellSignatures[cell],
                            sizeof(tile.shadowCellSignatures[cell]),key);
                        for(uint32_t material:volume.materials)key=includeMaterial(key,material);
                    }
                }
            for(uint32_t slot=0;slot<ShadowSlots;++slot)
                if(assigned[slot]&&(!shadowCache[slot].valid||
                   shadowCache[slot].id!=f.shadowIDs[slot]||shadowCache[slot].key!=f.shadowKeys[slot]))
                    f.shadowDirty|=1u<<slot;
        }
        f.lightsOffset=Reserve(f,std::max<uint64_t>(1,UINT64(f.lightCount))*sizeof(Light));
        if(f.lightCount)memcpy(f.uploadGeneration->mapped+f.lightsOffset,
            f.lightData.data(),UINT64(f.lightCount)*sizeof(Light));
    }

}
