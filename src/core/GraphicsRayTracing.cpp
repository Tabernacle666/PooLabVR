#include "GraphicsInternal.h"
#include <set>
#include <nvapi.h>
#include <cstddef>

namespace OASIS {
    namespace {
        using PtlasWrite=NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_OP_ARG_WRITE_INSTANCE;
        static_assert(sizeof(PtlasWrite)==104);
        static_assert(offsetof(PtlasWrite,accelerationStructure)==96);
        using PtlasUpdate=NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_OP_ARG_UPDATE_INSTANCE;
        static_assert(sizeof(PtlasUpdate)==16);
        static_assert(offsetof(PtlasUpdate,accelerationStructure)==8);
        NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_INPUTS PtlasInputs(uint32_t capacity){
            NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_INPUTS input{};
            input.flags=NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_FLAG_FAST_TRACE;
            input.instanceCount=capacity;
            input.partitionCount=(capacity+255u)/256u;
            input.maxInstancePerPartitionCount=std::min(256u,capacity);
            input.maxInstanceInGlobalPartitionCount=0;
            return input;
        }
    }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Graphics::Impl::PtlasPrebuild(uint32_t capacity){
        auto input=PtlasInputs(capacity);
        NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PREBUILD_INFO result{};
        NVAPI_GET_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PREBUILD_INFO_PARAMS params{
            NVAPI_GET_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PREBUILD_INFO_PARAMS_VER,&input,&result};
        if(NvAPI_D3D12_GetRaytracingPartitionedTlasIndirectPrebuildInfo(device.Get(),&params)!=NVAPI_OK)
            Fail("NVAPI PTLAS prebuild failed");
        return {result.resultDataMaxSizeInBytes,result.scratchDataSizeInBytes,result.scratchDataSizeInBytes};
    }
    Graphics::Impl::PtlasDispatch Graphics::Impl::PreparePtlasDispatch(Frame& f,uint32_t writes,uint32_t updates){
        if(!writes&&!updates)return {};
        if(uint64_t(writes)+updates>f.ptlasCapacity)Fail("PTLAS operation capacity");
        struct Upload {uint32_t count{},pad{};NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_OP ops[2]{};};
        Upload upload{};
        const auto args=f.ptlasWriteArgs->GetGPUVirtualAddress();
        if(writes){
            auto& op=upload.ops[upload.count++];
            op.type=NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_OP_WRITE_INSTANCE;
            op.count=writes;op.data={args,sizeof(PtlasWrite)};
        }
        if(updates){
            auto& op=upload.ops[upload.count++];
            op.type=NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_OP_UPDATE_INSTANCE;
            const uint64_t offset=(uint64_t(writes)*sizeof(PtlasWrite)+15u)&~uint64_t(15u);
            op.count=updates;op.data={args+offset,sizeof(PtlasUpdate)};
        }
        const auto offset=Reserve(f,sizeof(Upload),alignof(Upload));
        std::memcpy(f.uploadGeneration->mapped+offset,&upload,sizeof(upload));
        const auto base=f.uploadGeneration->resource->GetGPUVirtualAddress()+offset;
        return {base+offsetof(Upload,count),base+offsetof(Upload,ops)};
    }
    void Graphics::Impl::BuildPtlas(Frame& f,ID3D12GraphicsCommandList7* list,bool update,const PtlasDispatch& dispatch){
        if(!dispatch.count)return;
        NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_DESC desc{};
        desc.inputs=PtlasInputs(f.ptlasCapacity);
        desc.destAccelerationStructureData=f.tlas->GetGPUVirtualAddress();
        desc.srcAccelerationStructureData=update?desc.destAccelerationStructureData:0;
        desc.scratchAccelerationStructureData=f.scratch->GetGPUVirtualAddress();
        desc.indirectOpCount=dispatch.count;desc.indirectOps=dispatch.ops;
        NVAPI_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PARAMS params{
            NVAPI_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PARAMS_VER,&desc};
        if(NvAPI_D3D12_BuildRaytracingPartitionedTlasIndirect(list,&params)!=NVAPI_OK)
            Fail("NVAPI PTLAS build failed");
    }

    uint32_t Graphics::Impl::ReserveRaySlots(const RaySlotOwner& owner,uint32_t count){
        constexpr uint32_t limit=1u<<24;
        auto& lease=raySlotLeases[owner];
        if(count<=lease.capacity)return lease.first;
        if(count>limit)Fail("DXR InstanceID is 24 bits: ray slot capacity exceeded");
        // Whole partition leases keep unrelated tiles and moving world objects
        // out of the same hardware partition. Preserve the native TLAS allocator.
        const uint32_t granularity=partitionedTlas?256u:1u;
        const auto round=[&](uint32_t n){return std::min(limit,(n+granularity-1u)&~(granularity-1u));};
        const uint32_t wanted=round(std::min(limit,count+count/4u));
        std::vector<RaySlotLease> live;
        for(const auto& [key,span]:raySlotLeases)
            if(key!=owner&&span.capacity)live.push_back(span);
        std::sort(live.begin(),live.end(),[](auto a,auto b){return a.first<b.first;});
        auto find=[&](uint32_t capacity){
            uint32_t first=0;
            for(auto used:live){
                if(used.first>=first&&used.first-first>=capacity)return first;
                first=round(used.first+used.capacity);
            }
            return limit-first>=capacity?first:Invalid;
        };
        // Prefer expanding the current lease in place over relocating every
        // instance ID and invalidating its old and new partitions.
        auto fits=[&](uint32_t first,uint32_t capacity){
            if(first%granularity||capacity>limit-first)return false;
            return std::ranges::none_of(live,[&](auto used){
                return first<used.first+used.capacity&&used.first<first+capacity;
            });
        };
        auto capacity=wanted;
        auto first=lease.capacity&&fits(lease.first,capacity)?lease.first:find(capacity);
        if(first==Invalid){
            capacity=round(count);
            first=lease.capacity&&fits(lease.first,capacity)?lease.first:find(capacity);
        }
        if(first==Invalid)Fail("DXR InstanceID is 24 bits: ray slot capacity exceeded");
        lease={first,capacity};
        raySlotCount=std::max(raySlotCount,first+capacity);
        return first;
    }

    Graphics::Impl::RayFrameData Graphics::Impl::PrepareRayScene(
        Frame& f,const GpuSceneCache* tileCatalog,uint32_t frameSlot,bool campusVisible) {
        RayFrameData result;
        const auto* tiles=tileCatalog?&tileCatalog->ResidentTiles():nullptr;
        auto& rayTileAcks=result.acks;
        if(tiles)for(const auto& [key,tile]:*tiles){
            if(!campusVisible&&key.layer>=64)continue;
            const auto& indices=tile.rayModifiedIndices[frameSlot];
            if(indices.empty())continue;
            RayTileAck ack{};
            ack.key=key;
            ack.topologyGeneration=tile.topologyGeneration;
            ack.updates.reserve(indices.size());
            for(uint32_t index:indices){
                if(index>=tile.objects.size())
                    Fail("Resident tile ray update index out of range");
                ack.updates.emplace_back(index,tile.objects[index].revision);
            }
            rayTileAcks.push_back(std::move(ack));
        }


        auto ownerOf=[](const auto& bound){
            const auto* physical=PhysicalRayMetadata(bound.publication);
            return RaySlotOwner{2u,bound.publication.sourceOwner,
                physical?physical->sceneIdentity:0u,uint64_t(bound.publication.domain)};
        };
        std::set<RaySlotOwner> owners;
        const uint32_t staticCount=sceneCache.StaticCount();
        const uint32_t worldCount=staticCount+f.snapshotCount;
        if(partitionedTlas){
            if(staticCount)owners.insert({0,0,0,0});
            if(f.snapshotCount)owners.insert({3,0,0,0});
        }else if(worldCount)owners.insert({0,0,0,0});
        if(tiles)for(const auto& [key,tile]:*tiles)
            owners.insert({1,uint64_t(key.x),uint64_t(key.z),uint64_t(uint32_t(key.layer))});
        for(const auto& bound:meshDomain->Bindings())
            if(PhysicalRayMetadata(bound.publication))owners.insert(ownerOf(bound));
        std::erase_if(raySlotLeases,[&](const auto& it){return !owners.contains(it.first);});

        f.rayMapSegments.clear();
        auto addSegment=[&](uint32_t first,uint32_t count,uint32_t render){
            if(count)f.rayMapSegments.push_back({first,count,render});
        };
        const bool forestChanged=f.forestRaster!=f.forestTlas||f.forestRayReset;
        // Compare effective BLAS addresses, not generation or variant ordinal.
        // A different ordinal in another generation can resolve the same BLAS.
        const auto forestDirty=[&](const Mg2InstanceRoute& key,uint32_t flags,bool& addressOnly){
            addressOnly=false;
            if(flags&(NoRayTracing|DeformedObject|AlphaBlendObject))return false;
            ++forestStatus.traceForestRayCompared;
            const auto after=f.forestRaster?f.forestRaster->RayAddress(key):0;
            // Different owners can now share the exact same BLAS.
            // Compare actual addresses even across geometry generations.
            const auto before=f.forestTlas?f.forestTlas->RayAddress(key):0;
            addressOnly=partitionedTlas&&f.tlasBuilt&&before!=0&&after!=0;
            return before!=after;
        };
        const auto dynamicDirty=[&](uint32_t mesh,uint32_t flags){
            if(!partitionedTlas||!(flags&DeformedObject)||(flags&NoRayTracing)||mesh>=meshDynamicIndex.size())return false;
            const auto index=meshDynamicIndex[mesh];
            return std::ranges::find(f.blasWork.Active(),index)!=f.blasWork.Active().end();
        };
        const auto addForestDirty=[&](uint32_t slot,bool addressOnly=false){
            ++forestStatus.traceForestRayDirty;
            result.forestAddressesChanged=true;
            auto& ranges=addressOnly?result.blasDirty:result.cpuDirty;
            if(!ranges.empty()&&ranges.back().first+ranges.back().count==slot)
                ++ranges.back().count;
            else ranges.push_back({slot,1u});
        };
        if(worldCount){
            uint32_t firstStatic=0,firstMoving=0;
            if(partitionedTlas){
                if(staticCount){
                    firstStatic=ReserveRaySlots({0,0,0,0},staticCount);
                    addSegment(firstStatic,staticCount,0u);
                }
                if(f.snapshotCount){
                    firstMoving=ReserveRaySlots({3,0,0,0},f.snapshotCount);
                    addSegment(firstMoving,f.snapshotCount,staticCount);
                }
            }else{
                firstStatic=ReserveRaySlots({0,0,0,0},worldCount);
                firstMoving=firstStatic+staticCount;
                addSegment(firstStatic,worldCount,0u);
            }
            const auto slotFor=[&](uint32_t i){return i<staticCount?firstStatic+i:firstMoving+i-staticCount;};
            const bool dynamicWork=partitionedTlas&&!f.blasWork.Empty();
            const bool staticWork=dynamicWork&&std::ranges::any_of(staticDeformedRtMeshes,[&](uint32_t index){
                return std::ranges::find(f.blasWork.Active(),index)!=f.blasWork.Active().end();
            });
            if(forestChanged||dynamicWork){
                const auto statics=sceneCache.StaticInstances();
                // Do not scan immutable world geometry for an animated hand/prop.
                const uint32_t start=(forestChanged||staticWork)?0u:staticCount;
                for(uint32_t i=start;i<worldCount;++i){
                    const auto& object=i<statics.size()?statics[i]:f.objects[i-statics.size()];
                    bool addressOnly=false;
                    if(forestChanged&&(f.forestRayReset||
                       (std::ranges::binary_search(forestParts,object.mesh,{},&Mg2ForestPart::mesh)&&
                        forestDirty({i,object.id,object.mesh,0u},object.flags,addressOnly))))
                        addForestDirty(slotFor(i),addressOnly);
                    if(dynamicDirty(object.mesh,object.flags))result.cpuDirty.push_back({slotFor(i),1u});
                }
            }
        }
        uint32_t tileFirst=worldCount;
        if(tiles)for(const auto& [key,tile]:*tiles){
            const uint32_t count=uint32_t(tile.objects.size());
            if(count&&(campusVisible||key.layer<64)){
                const uint32_t first=ReserveRaySlots({1,uint64_t(key.x),uint64_t(key.z),uint64_t(uint32_t(key.layer))},count);
                addSegment(first,count,tileFirst);
                if(forestChanged)for(uint32_t i=0;i<count;++i){
                    const auto& object=tile.objects[i];
                    if(f.forestRayReset){addForestDirty(first+i);}
                    if(!object.enabled||!object.identity||
                       !std::ranges::binary_search(forestParts,object.mesh,{},&Mg2ForestPart::mesh))continue;
                    const Mg2InstanceRoute route{uint32_t(object.identity),uint32_t(object.identity>>32u),
                        object.mesh|Mg2ResidentKeyBit,0u};
                    bool addressOnly=false;
                    if(!f.forestRayReset&&forestDirty(route,object.flags,addressOnly))
                        addForestDirty(first+i,addressOnly);
                    if(mg2Diagnostic&&result.forestProofSlot==Invalid&&f.forestRaster&&
                       f.forestRaster->backend!=Mg2RayBackend::Standard&&
                       !(object.flags&(NoRayTracing|DeformedObject|AlphaBlendObject))&&
                       f.forestRaster->FindRoute(route)!=Invalid)
                        result.forestProofSlot=first+i;
                }
                const bool tileBlasWork=partitionedTlas&&!f.blasWork.Empty()&&
                    std::ranges::any_of(tile.deformableMeshes,[&](const auto& use){
                        const auto mesh=use.first;
                        return use.second.ray&&mesh<meshDynamicIndex.size()&&
                            std::ranges::find(f.blasWork.Active(),meshDynamicIndex[mesh])!=f.blasWork.Active().end();
                    });
                // Existing tile usage metadata identifies relevant meshes without
                // a per-instance walk of the entire immutable forest.
                if(tileBlasWork)for(uint32_t i=0;i<count;++i){
                    const auto& object=tile.objects[i];
                    if(object.enabled&&dynamicDirty(object.mesh,object.flags))result.cpuDirty.push_back({first+i,1u});
                }
            }
            tileFirst+=count;
        }
        rayPublicationScratch.clear();rayPublicationRanges.clear();
        rayPublicationScratch.reserve(meshDomain->Bindings().size());
        rayPublicationRanges.reserve(meshDomain->Bindings().size());
        for(const auto& bound:meshDomain->Bindings()){
            const auto* physical=PhysicalRayMetadata(bound.publication);
            uint32_t physicalFirst=0,physicalCount=0;
            if(bound.publication.FindGroup(GpuMeshPublicationGroupId::PhysicsVisual)){
                if(!physical)Fail("Physical ray publication missing physical range metadata");
                const auto* source=bound.publication.Find(GpuMeshPublicationAbi::PhysicsInstances);
                if(physical->physicalCount&&(!source||
                    physical->sourceFirst>source->bytes/sizeof(GpuInstance)||
                    physical->physicalCount>source->bytes/sizeof(GpuInstance)-physical->sourceFirst))
                    Fail("Physical ray publication source range exceeds its storage");
                physicalFirst=physical->sourceFirst;physicalCount=physical->physicalCount;
            }
            if(physicalFirst>bound.capacity||physicalCount>bound.capacity-physicalFirst)
                Fail("Physical ray publication exceeds raster instance range");
            const uint32_t compactFirst=physicalCount?
                ReserveRaySlots(ownerOf(bound),physicalCount):0u;
            addSegment(compactFirst,physicalCount,bound.base+physicalFirst);
            if(partitionedTlas&&physicalCount&&!f.blasWork.Empty())
                result.cpuDirty.push_back({compactFirst,physicalCount});
            rayPublicationRanges.push_back({
                compactFirst,physicalCount,physicalCount?bound.base+physicalFirst:0u});


            uint64_t rayRevision=0;
            if(physicalCount){
                if(!physical->rayRevision)Fail("Physical ray publication missing revision");
                rayRevision=physical->rayRevision;
            }
            const uint64_t sourceOwner=physicalCount?bound.publication.sourceOwner:0;
            const uint64_t sceneIdentity=physicalCount&&physical?physical->sceneIdentity:0;
            const uint32_t sourceBase=physicalCount?bound.base+physicalFirst:0;
            rayPublicationScratch.push_back({rayRevision,sourceOwner,sceneIdentity,
                compactFirst,physicalCount,sourceBase});
        }
        f.rayInstanceCount=raySlotCount;
        if(f.rayInstanceCount){
            EnsureFrameInstanceCapacity(f,f.rayInstanceCount);
            EnsureRayInstanceMap(f,f.rayInstanceCount);
        }

        auto& map=f.rayMapSegments;
        std::sort(map.begin(),map.end(),[](auto a,auto b){return a.compactFirst<b.compactFirst;});
        std::vector<Frame::RayMapSegment> complete;
        uint32_t end=0;
        for(const auto& segment:map){
            if(segment.compactFirst<end)Fail("Overlapping DXR ray ranges");
            if(segment.compactFirst>end)
                complete.push_back({end,segment.compactFirst-end,Invalid});
            complete.push_back(segment);
            end=segment.compactFirst+segment.count;
        }
        if(end<f.rayInstanceCount)
            complete.push_back({end,f.rayInstanceCount-end,Invalid});
        map=std::move(complete);

        // Only update slots whose raster route changed for this frame resource.
        f.dirtyRayMapSegments.clear();
        const auto& committed=f.committedRayMapSegments;
        size_t oldIndex=0;
        for(const auto& segment:map){
            const uint32_t finish=segment.compactFirst+segment.count;
            uint32_t first=segment.compactFirst;
            while(first<finish){
                while(oldIndex<committed.size()&&
                      committed[oldIndex].compactFirst+committed[oldIndex].count<=first)
                    ++oldIndex;
                const auto* prior=oldIndex<committed.size()?&committed[oldIndex]:nullptr;
                uint32_t next=finish;
                const uint32_t render=segment.renderFirst==Invalid?Invalid:
                    segment.renderFirst+(first-segment.compactFirst);
                bool same=false;
                if(prior){
                    next=std::min(next,prior->compactFirst+prior->count);
                    const uint32_t oldRender=prior->renderFirst==Invalid?Invalid:
                        prior->renderFirst+(first-prior->compactFirst);
                    same=oldRender==render;
                }
                if(!same)f.dirtyRayMapSegments.push_back({first,next-first,render});
                first=next;
            }
        }
        f.rayMapBuild=!f.dirtyRayMapSegments.empty();

        rayScene.StageTiles(frameSlot,f.snapshotCount,tileCatalog,campusVisible);
        rayScene.StagePublications(frameSlot,rayPublicationScratch);
        if(!f.blasWork.Empty())rayScene.MarkGeometryDirty(frameSlot);
        for(const auto& segment:f.dirtyRayMapSegments)
            result.cpuDirty.push_back({segment.compactFirst,segment.count});
        const auto pending=rayScene.PendingCpu(frameSlot);
        for(const auto& segment:map){
            if(segment.renderFirst>=f.cpuCount)continue;
            auto it=std::lower_bound(pending.begin(),pending.end(),segment.renderFirst,
                [](auto range,uint32_t first){return range.first+range.count<=first;});
            for(;it!=pending.end()&&it->first<segment.renderFirst+segment.count;++it){
                const uint32_t first=std::max(it->first,segment.renderFirst);
                const uint32_t end=std::min(it->first+it->count,segment.renderFirst+segment.count);
                result.cpuDirty.push_back({segment.compactFirst+(first-segment.renderFirst),end-first});
            }
        }
        for(uint32_t index:rayScene.PendingPublications(frameSlot)){
            const auto& range=rayPublicationRanges[index];
            if(range.count)result.cpuDirty.push_back({range.compactFirst,range.count});
        }
        // Build each descriptor at most once, even if content and mapping both changed.
        auto merge=[](auto& dirty){
            std::sort(dirty.begin(),dirty.end(),[](auto a,auto b){return a.first<b.first;});
            size_t written=0;
            for(const auto range:dirty){
                if(written&&dirty[written-1].first+dirty[written-1].count>=range.first){
                    auto& previous=dirty[written-1];
                    previous.count=std::max(previous.count,range.first+range.count-previous.first);
                }else dirty[written++]=range;
            }
            dirty.resize(written);
        };
        auto& dirty=result.cpuDirty;
        merge(dirty);
        if(partitionedTlas&&!f.tlasBuilt&&f.rayInstanceCount){
            dirty={{0u,f.rayInstanceCount}};result.blasDirty.clear();
        }else if(!result.blasDirty.empty()){
            // WRITE_INSTANCE takes precedence over UPDATE_INSTANCE. A slot
            // must never occur in both operations in the same PTLAS build.
            merge(result.blasDirty);
            std::vector<GpuRayDirtyRange> only;
            size_t i=0;
            for(auto range:result.blasDirty){
                uint32_t first=range.first;
                const uint32_t end=first+range.count;
                while(i<dirty.size()&&dirty[i].first+dirty[i].count<=first)++i;
                for(size_t j=i;j<dirty.size()&&dirty[j].first<end;++j){
                    if(dirty[j].first>first)only.push_back({first,dirty[j].first-first});
                    first=std::max(first,dirty[j].first+dirty[j].count);
                    if(first>=end)break;
                }
                if(first<end)only.push_back({first,end-first});
            }
            result.blasDirty=std::move(only);
        }
        result.dirty=f.rayMapBuild||rayScene.NeedsTlas(frameSlot)||result.forestAddressesChanged;
        return result;
    }
}
