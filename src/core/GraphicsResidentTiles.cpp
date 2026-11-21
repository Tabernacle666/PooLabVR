#include "GraphicsInternal.h"
#include <numeric>
#include <tuple>

namespace OASIS {
    Graphics::Impl::ResidentBatchData Graphics::Impl::PrepareResidentBatches(
        Frame& f,const GpuSceneCache* tileCatalog,bool coreSceneWork,bool campusVisible) {
        ResidentBatchData result;
        const auto* tiles=tileCatalog?&tileCatalog->ResidentTiles():nullptr;
        const uint32_t tileCount=tiles?uint32_t(tiles->size()):0;
        if(tileCount>UINT32_MAX-2)Fail("Too many GPU resident tile ranges");
        EnsureInstanceRoutes(f,tileCount+2);
        if(!f.instanceRangeData)Fail("Frame instance source table unavailable");
        f.tileTransfers.clear();f.tileRunTransfers.clear();f.heldTileSources.clear();


        for(auto it=residentTileGpu.begin();it!=residentTileGpu.end();){
            if(!tiles||!tiles->contains(it->first)){
                ReleaseDescriptor(it->second.srv);
                ReleaseDescriptor(it->second.runSrv);
                passGraph.Forget(it->second.buffer.Get());
                passGraph.Forget(it->second.runBuffer.Get());
                it=residentTileGpu.erase(it);
            }else ++it;
        }
        const uint32_t worldCount=sceneCache.StaticCount()+f.snapshotCount;
        auto batchKey=[&](uint32_t material,uint32_t mesh,uint32_t flags){
            if(material>=surfaceData.size())Fail("Opaque instance references missing material");
            const auto& surface=surfaceData[material];
            const auto variant=(flags&ShuttleCrt)?ShuttleCrtVariant:MaterialTextureMask(surface);
            return std::tuple{variant,mesh,material,flags};
        };
        auto sortOpaque=[&](std::span<uint32_t> order,const auto& entry) {
            // Keep explicit visual ordering around alpha, CRT and other specialized objects.
            auto canReorder=[&](uint32_t index){
                auto [variant,mesh,material,flags]=entry(index);
                return variant!=ShuttleCrtVariant&&surfaceData[material].extra.y==0u&&
                    (flags&(DynamicObject|DeformedObject|AlphaBlendObject|ShuttleSpeedDust|
                            SolarWorldVisual|LunarActorVisual))==0u;
            };
            for(size_t i=0;i<order.size();){
                if(!canReorder(order[i])){++i;continue;}
                size_t end=i+1;
                while(end<order.size()&&canReorder(order[end]))++end;
                if(end-i>1)std::stable_sort(order.begin()+i,order.begin()+end,
                    [&](uint32_t left,uint32_t right){return entry(left)<entry(right);});
                i=end;
            }
        };
        Frame::UploadSlice orderUpload;
        uint32_t* orderedInstances=nullptr;
        if(coreSceneWork&&f.cpuCount){
            orderUpload=StageFrameTransfer(f,UINT64(f.cpuCount)*sizeof(uint32_t));
            orderedInstances=reinterpret_cast<uint32_t*>(orderUpload.data);
            f.opaqueOrderSRV=Srv(orderUpload.resource,f.cpuCount,sizeof(uint32_t),
                f.opaqueOrderSRV,orderUpload.offset/sizeof(uint32_t));
            const uint32_t staticCount=sceneCache.StaticCount();
            auto entry=[&](uint32_t index){
                const auto& instance=CpuInstance(f,index);
                return batchKey(instance.material,instance.mesh,instance.flags);
            };
            if(staticOpaqueOrder.size()!=staticCount){
                staticOpaqueOrder.resize(staticCount);
                std::iota(staticOpaqueOrder.begin(),staticOpaqueOrder.end(),0u);
                sortOpaque(staticOpaqueOrder,entry);
            }
            std::copy(staticOpaqueOrder.begin(),staticOpaqueOrder.end(),orderedInstances);
            std::span dynamicOrder(orderedInstances+staticCount,f.snapshotCount);
            std::iota(dynamicOrder.begin(),dynamicOrder.end(),staticCount);
            sortOpaque(dynamicOrder,entry);
        }
        uint32_t first=worldCount;
        f.instanceRangeData[0]={0,worldCount,worldCount?Physical(f.worldInstanceSRV):0u,
                                 tileCount+2};
        uint32_t route=1;
        auto& tileBatchDispatch=result.tileBatchDispatch;
        auto& tileBatchBlocks=result.tileBatchBlocks;
        uint32_t tileBatchSuperBlocks=0;
        if(tiles)for(const auto& [key,tile]:*tiles){
            if(tile.objects.size()>UINT32_MAX-first)
                Fail("Resident tile range exceeds 32-bit instance address space");
            const uint32_t count=uint32_t(tile.objects.size());
            if(!campusVisible&&key.layer>=64){
                // Keep ordinals while OASIS is represented by its atlas or
                // another body is native. Dirty pages remain pending.
                f.instanceRangeData[route++]={first,count,Invalid,1};
                first+=count;
                continue;
            }
            auto& gpu=residentTileGpu[key];
            const bool rebuild=!gpu.buffer||count>gpu.objectCapacity||
                gpu.topologyGeneration==0;
            if(rebuild){
                if(gpu.srv.Valid())ReleaseDescriptor(gpu.srv);
                if(gpu.buffer)passGraph.Forget(gpu.buffer.Get());
                gpu.objectCapacity=GrowCapacity(gpu.objectCapacity,std::max(1u,count));
                gpu.buffer=Temporary(Buffer(UINT64(gpu.objectCapacity)*
                    sizeof(GpuResidentTileGpuInstance)),D3D12_RESOURCE_STATE_COMMON);
                gpu.srv=Srv(gpu.buffer.Get(),gpu.objectCapacity,
                            sizeof(GpuResidentTileGpuInstance));
            }
            gpu.objectCount=count;
            gpu.topologyGeneration=tile.topologyGeneration;

            bool runsChanged=false;
            if(gpu.opaqueOrderRevision!=tile.revision||gpu.opaqueOrder.size()!=count||rebuild){
                auto entry=[&](uint32_t index){
                    const auto& object=tile.objects[index];
                    return batchKey(object.material,object.mesh,object.flags);
                };
                gpu.opaqueOrder.resize(count);
                std::iota(gpu.opaqueOrder.begin(),gpu.opaqueOrder.end(),0u);
                sortOpaque(gpu.opaqueOrder,entry);
                std::vector<ResidentMaterialRun> runs;
                for(uint32_t i=0;i<count;++i){
                    const auto mask=std::get<0>(entry(gpu.opaqueOrder[i]));
                    if(!runs.empty()&&runs.back().mask==mask)++runs.back().count;
                    else runs.push_back({i,1u,mask,0u});
                }
                runsChanged=runs!=gpu.runs;
                if(runsChanged)gpu.runs=std::move(runs);
                gpu.opaqueOrderRevision=tile.revision;
            }
            if(orderedInstances)for(uint32_t i=0;i<count;++i)
                orderedInstances[first+i]=first+gpu.opaqueOrder[i];
            if(runsChanged||(!gpu.runBuffer&&!gpu.runsDirty)){
                if(gpu.runRevision==UINT64_MAX)
                    Fail("Resident material run revision exhausted");
                ++gpu.runRevision;
                gpu.runsDirty=true;
            }
            if(gpu.runsDirty){
                if(gpu.runs.size()>=UINT32_MAX)
                    Fail("Resident run table index space exhausted");
                const uint32_t required=uint32_t(gpu.runs.size())+1u;
                if(!gpu.runBuffer||required>gpu.runCapacity){
                    const uint32_t capacity=GrowCapacity(gpu.runCapacity,required);
                    if(capacity<required)Fail("Resident run buffer capacity exhausted");
                    if(gpu.runSrv.Valid())ReleaseDescriptor(gpu.runSrv);
                    if(gpu.runBuffer)passGraph.Forget(gpu.runBuffer.Get());
                    gpu.runBuffer=Temporary(Buffer(uint64_t(capacity)*
                        sizeof(ResidentMaterialRun)),D3D12_RESOURCE_STATE_COMMON);
                    gpu.runSrv=Srv(gpu.runBuffer.Get(),capacity,
                        sizeof(ResidentMaterialRun));
                    gpu.runCapacity=capacity;
                }
                Frame::TileRunTransfer transfer{};
                transfer.key=key;
                transfer.revision=gpu.runRevision;
                transfer.buffer=gpu.runBuffer;
                transfer.staging=StageFrameTransfer(f,uint64_t(required)*sizeof(ResidentMaterialRun));
                auto* dst=reinterpret_cast<ResidentMaterialRun*>(transfer.staging.data);
                dst[0]={uint32_t(gpu.runs.size()),0,0,0};
                std::copy(gpu.runs.begin(),gpu.runs.end(),dst+1);
                f.tileRunTransfers.push_back(std::move(transfer));
            }
            f.heldTileSources.push_back(gpu.buffer);
            f.heldTileSources.push_back(gpu.runBuffer);
            f.instanceRangeData[route++]={first,count,Physical(gpu.srv),1};
            if(gpu.runs.size()>UINT32_MAX)
                Fail("Resident material run ordinal overflow");
            const uint32_t blockCount=(count+255u)/256u;
            const uint32_t superCount=(blockCount+255u)/256u;
            tileBatchDispatch.push_back({key,first,count,tileBatchBlocks,
                blockCount,tileBatchSuperBlocks,superCount,0,
                uint32_t(gpu.runs.size()),Physical(gpu.runSrv)});
            if(tileBatchBlocks>UINT32_MAX-blockCount||
               tileBatchSuperBlocks>UINT32_MAX-superCount)
                Fail("Resident tile batch prefix capacity exhausted");
            tileBatchBlocks+=blockCount;
            tileBatchSuperBlocks+=superCount;
            first+=count;


            std::vector<uint32_t> indices;
            if(rebuild){
                indices.resize(count);
                for(uint32_t i=0;i<count;++i)indices[i]=i;
            }else indices=tile.modifiedIndices;
            if(indices.empty())continue;
            std::sort(indices.begin(),indices.end());
            indices.erase(std::unique(indices.begin(),indices.end()),indices.end());
            Frame::TileTransfer transfer{};
            transfer.key=key;
            transfer.topologyGeneration=tile.topologyGeneration;
            transfer.buffer=gpu.buffer;
            uint32_t runStart=indices.front(),runCount=0;
            for(uint32_t index:indices){
                if(index>=count)Fail("Resident tile dirty index out of range");
                if(runCount&&uint64_t(runStart)+runCount!=index){
                    transfer.ranges.push_back({runStart,runCount});
                    runStart=index;runCount=0;
                }
                ++runCount;
                transfer.updates.emplace_back(index,tile.objects[index].revision);
            }
            if(runCount)transfer.ranges.push_back({runStart,runCount});


            transfer.staging=StageFrameTransfer(f,UINT64(indices.size())*sizeof(GpuResidentTileGpuInstance));
            auto* output=reinterpret_cast<GpuResidentTileGpuInstance*>(transfer.staging.data);
            for(size_t n=0;n<transfer.updates.size();++n){
                const auto& o=tile.objects[transfer.updates[n].first];
                auto& record=output[n];
                record.instance=ConvertResidentBase(o);
                record.sector={o.cellX,o.cellZ,o.cellRadius,0};
                record.farMesh=o.farMesh;
                record.hideFar=o.hideFar;
                record.enabled=o.enabled;
                record.reserved=0;
            }
            f.tileTransfers.push_back(std::move(transfer));
        }
        if(first!=f.cpuCount)Fail("GPU tile source size changed after preparation");
        f.instanceRangeData[route]={f.cpuCount,f.instanceCount-f.cpuCount,
                                    (f.instanceCount>f.cpuCount)?Physical(f.publishedInstanceSRV):0u,2};
        if(coreSceneWork)EnsureVisibilityBuffers(f,f.cpuCount,f.instanceCount-f.cpuCount);


        auto& opaqueRuns=result.opaqueRuns;
        auto appendMaterial=[&](const GpuInstance& instance){
            const auto material=instance.material;
            if(material>=surfaceData.size())Fail("Opaque instance references missing material");
            const uint32_t mask=(instance.flags&ShuttleCrt)?ShuttleCrtVariant:
                MaterialTextureMask(surfaceData[material]);
            if(opaqueRuns.empty()||opaqueRuns.back().mask!=mask)
                opaqueRuns.push_back({uint32_t(opaqueRuns.empty()?0u:
                    opaqueRuns.back().first+opaqueRuns.back().count),1u,mask});
            else ++opaqueRuns.back().count;
        };
        for(uint32_t i=0;i<worldCount;++i)appendMaterial(CpuInstance(f,orderedInstances?orderedInstances[i]:i));
        if(worldCount){
            if(opaqueRuns.empty()||
               opaqueRuns.back().first+opaqueRuns.back().count!=worldCount)
                Fail("Material run partition does not cover the nonresident scene");
        }
        uint64_t totalRuns=opaqueRuns.size();
        for(auto& tileBatch:tileBatchDispatch){
            tileBatch.runFirst=uint32_t(totalRuns);
            totalRuns+=tileBatch.runCount;
        }
        if(totalRuns>UINT32_MAX)
            Fail("Resident opaque run address space exhausted");
        if(coreSceneWork)EnsureBatchRunCounters(f,uint32_t(totalRuns));
        if(tileBatchBlocks>UINT32_MAX-tileBatchSuperBlocks)
            Fail("Resident tile scan hierarchy capacity exhausted");
        if(coreSceneWork)EnsureTileBatchBuffers(f,f.cpuCount,tileBatchBlocks+tileBatchSuperBlocks);
        return result;
    }
}
