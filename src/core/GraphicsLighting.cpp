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

    void Graphics::Impl::PrepareLights(Frame& f,const RenderScene& snapshot) {
        if(snapshot.lightCount!=snapshot.lights.size())Fail("Render scene light count mismatch");
        f.lightCount=snapshot.lightCount;f.lightData.resize(f.lightCount);f.shadowDirty=0;f.hasShadowedLights=false;f.hasShadowAtlasLights=false;f.shadowIDs.fill(Invalid);f.shadowKeys.fill(0);


        const GpuSceneCache* tiles=contributions.residentTiles?
            contributions.residentTiles(contributions.context):nullptr;
        const auto shadowCasterNear=[&](const SceneLight& light,const GpuInstance& o){
            if(o.flags&(DynamicObject|DeformedObject|NoShadow))return false;
            if(o.mesh>=meshes.ranges.size())
                Fail("Shadow caster references missing mesh");
            const auto bounds=meshes.ranges[o.mesh].bounds;
            const double maximumScale=std::max({
                std::abs(double(o.scale.x)),std::abs(double(o.scale.y)),
                std::abs(double(o.scale.z))});


            const double localReach=std::sqrt(double(bounds.x)*bounds.x+
                double(bounds.y)*bounds.y+double(bounds.z)*bounds.z)+
                double(bounds.w);
            const double radius=double(light.radius)+
                localReach*maximumScale*2.5+2.;
            const double dx=double(o.positionRadius.x)-double(light.position.x);
            const double dz=double(o.positionRadius.z)-double(light.position.z);
            return dx*dx+dz*dz<=radius*radius;
        };
        std::array<bool,ShadowSlots> assigned{};
        for(uint32_t i=0;i<f.lightCount;i++) {
            auto light=snapshot.lights[i];if(light.animationNode!=Invalid){auto world=animation.NodeMatrix(light.animationNode)*XMMatrixScaling(1,1,-1);XMStoreFloat3(&light.position,XMVector3TransformCoord(XMVectorZero(),world));XMStoreFloat3(&light.direction,XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0,0,-1,0),world)));}if(contributions.lightContext)contributions.lightContext(contributions.context,light,f.simulationTime);int slot=-1;
            if((light.flags&1)&&!(light.flags&(2|4))) {
                for(uint32_t j=0;j<ShadowSlots;j++)if(!assigned[j]&&shadowCache[j].id==light.id){slot=int(j);break;}
                if(slot<0)for(uint32_t j=0;j<ShadowSlots;j++)if(!assigned[j]){bool reserved=false;for(uint32_t k=i+1;k<f.lightCount;k++)if(snapshot.lights[k].id==shadowCache[j].id)reserved=true;if(!reserved){slot=int(j);break;}}
            }
            if(slot>=0) {
                uint32_t s=uint32_t(slot);assigned[s]=true;f.shadowIDs[s]=light.id;f.shadowLights[s]=light;
                uint64_t key=1469598103934665603ull;
                key=HashMemory(&light.position,sizeof(light.position),key);
                key=HashMemory(&light.radius,sizeof(light.radius),key);
                key=HashMemory(&light.revision,sizeof(light.revision),key);

                for(const auto& object:sceneCache.StaticInstances())
                    if(shadowCasterNear(light,object))
                        key=HashMemory(&object,sizeof(object),key);
                for(uint32_t j=0;j<f.snapshotCount;++j)
                    if(shadowCasterNear(light,f.objects[j]))
                        key=HashMemory(&f.objects[j],sizeof(GpuInstance),key);


                if(tiles)for(const auto& [tileKey,tile]:tiles->ResidentTiles())
                    for(uint32_t cell=0;cell<GpuResidentTile::Cells;++cell){
                        if(!tile.shadowCasterCounts[cell])continue;
                        const uint64_t identity=tile.cellObjects[cell].front();
                        const auto obj=tile.byIdentity.find(identity);
                        if(obj==tile.byIdentity.end())
                            Fail("Resident shadow cell identity lookup failed");
                        const auto& sample=tile.objects[obj->second];
                        const double radius=double(light.radius)+
                            double(sample.cellRadius);
                        const double dx=double(sample.cellX)-double(light.position.x);
                        const double dz=double(sample.cellZ)-double(light.position.z);
                        if(dx*dx+dz*dz>radius*radius)continue;
                        key=HashMemory(&tileKey.x,sizeof(tileKey.x),key);
                        key=HashMemory(&tileKey.z,sizeof(tileKey.z),key);
                        key=HashMemory(&cell,sizeof(cell),key);
                        key=HashMemory(&tile.shadowCellSignatures[cell],
                                       sizeof(tile.shadowCellSignatures[cell]),key);
                    }
                f.shadowKeys[s]=key;
                if(!shadowCache[s].valid||shadowCache[s].id!=light.id||shadowCache[s].key!=key)f.shadowDirty|=1u<<s;
                auto at=XMLoadFloat3(&light.position);const XMVECTOR directions[]={XMVectorSet(1,0,0,0),XMVectorSet(-1,0,0,0),XMVectorSet(0,1,0,0),XMVectorSet(0,-1,0,0),XMVectorSet(0,0,1,0),XMVectorSet(0,0,-1,0)};
                for(uint32_t face=0;face<6;face++){auto up=face==2?XMVectorSet(0,0,-1,0):face==3?XMVectorSet(0,0,1,0):XMVectorSet(0,1,0,0);XMStoreFloat4x4(&shadowMatrices[s*6+face],XMMatrixLookToLH(at,directions[face],up)*XMMatrixPerspectiveFovLH(XM_PIDIV2,1,std::max(.1f,light.radius),.02f));}
            }
            f.hasShadowedLights|=(light.flags&1)!=0;f.hasShadowAtlasLights|=slot>=0;
            f.lightData[i]={{light.position.x,light.position.y,light.position.z,std::max(.1f,light.radius)},{light.color.x,light.color.y,light.color.z,light.power},{light.direction.x,light.direction.y,light.direction.z,light.outerCos},{light.innerCos,float(slot),float(light.flags),0}};
        }
        f.lightsOffset=Reserve(f,std::max<uint64_t>(1,UINT64(f.lightCount))*sizeof(Light));
        if(f.lightCount)memcpy(f.uploadGeneration->mapped+f.lightsOffset,
            f.lightData.data(),UINT64(f.lightCount)*sizeof(Light));
    }

}
