#include "GraphicsInternal.h"

namespace lab {
    Graphics::Impl::ResidentBatchData Graphics::Impl::PrepareResidentBatches(
        Frame& f,const GpuSceneCache* tileCatalog,bool coreSceneWork) {
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
            auto& gpu=residentTileGpu[key];
            const uint32_t count=uint32_t(tile.objects.size());
            const bool rebuild=!gpu.buffer||gpu.objectCount!=count||
                gpu.topologyGeneration!=tile.topologyGeneration;
            if(rebuild){
                if(gpu.srv.Valid())ReleaseDescriptor(gpu.srv);
                if(gpu.buffer)passGraph.Forget(gpu.buffer.Get());
                gpu.buffer=Temporary(Buffer(UINT64(std::max(1u,count))*
                    sizeof(GpuResidentTileGpuInstance)),D3D12_RESOURCE_STATE_COMMON);
                gpu.srv=Srv(gpu.buffer.Get(),std::max(1u,count),
                            sizeof(GpuResidentTileGpuInstance));
                gpu.objectCount=count;
                gpu.topologyGeneration=tile.topologyGeneration;
            }


            auto maskAt=[&](uint32_t index){
                const uint32_t material=tile.objects[index].material;
                if(material>=surfaceData.size())
                    Fail("Resident tile object material out of range");
                return MaterialTextureMask(surfaceData[material]);
            };
            bool runsChanged=rebuild||gpu.materialMasks.size()!=count;
            if(rebuild||gpu.materialMasks.size()!=count){
                gpu.materialMasks.resize(count);
                gpu.runs.clear();
                for(uint32_t index=0;index<count;++index){
                    const uint32_t mask=maskAt(index);
                    gpu.materialMasks[index]=mask;
                    if(gpu.runs.empty()||gpu.runs.back().mask!=mask)
                        gpu.runs.push_back({index,1u,mask,0});
                    else ++gpu.runs.back().count;
                }
            }else{
                auto changeMask=[&](uint32_t index){
                    if(index>=count)Fail("Resident material edit outside tile");
                    const uint32_t mask=maskAt(index);
                    if(mask==gpu.materialMasks[index])return;
                    gpu.materialMasks[index]=mask;
                    auto it=std::upper_bound(gpu.runs.begin(),gpu.runs.end(),index,
                        [](uint32_t value,const ResidentMaterialRun& run){
                            return value<run.first;
                        });
                    if(it==gpu.runs.begin())Fail("Resident material run missing");
                    --it;
                    if(index>=it->first+it->count)
                        Fail("Resident material run corrupt");
                    const ResidentMaterialRun old=*it;
                    const size_t offset=size_t(it-gpu.runs.begin());
                    gpu.runs.erase(it);
                    std::vector<ResidentMaterialRun> pieces;
                    if(index>old.first)
                        pieces.push_back({old.first,index-old.first,old.mask,0});
                    pieces.push_back({index,1u,mask,0});
                    if(index+1u<old.first+old.count)
                        pieces.push_back({index+1u,old.first+old.count-index-1u,old.mask,0});
                    gpu.runs.insert(gpu.runs.begin()+offset,pieces.begin(),pieces.end());
                    for(size_t j=offset?offset-1u:0u;j+1u<gpu.runs.size();){
                        if(gpu.runs[j].mask==gpu.runs[j+1u].mask){
                            gpu.runs[j].count+=gpu.runs[j+1u].count;
                            gpu.runs.erase(gpu.runs.begin()+j+1u);
                        }else ++j;
                        if(j>offset+3u)break;
                    }
                    runsChanged=true;
                };
                if(surfacesDirty)
                    for(uint32_t index=0;index<count;++index)changeMask(index);
                else
                    for(uint32_t index:tile.modifiedIndices)changeMask(index);
            }
            if(runsChanged){
                uint64_t covered=0;
                for(const auto& run:gpu.runs){
                    if(run.first!=covered||!run.count||run.mask>=32u)
                        Fail("Resident material partition invalid");
                    covered+=run.count;
                }
                if(covered!=count)
                    Fail("Resident material partition size mismatch");
            }
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
                transfer.upload=Host(uint64_t(required)*
                    sizeof(ResidentMaterialRun),D3D12_HEAP_TYPE_UPLOAD);
                D3D12_RANGE range{0,0};
                void* ptr{};
                Hr(transfer.upload->Map(0,&range,&ptr),"tile run staging map");
                auto* dst=static_cast<ResidentMaterialRun*>(ptr);
                dst[0]={uint32_t(gpu.runs.size()),0,0,0};
                std::copy(gpu.runs.begin(),gpu.runs.end(),dst+1);
                transfer.upload->Unmap(0,nullptr);
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


            transfer.upload=Host(UINT64(indices.size())*
                                 sizeof(GpuResidentTileGpuInstance),
                                 D3D12_HEAP_TYPE_UPLOAD);
            D3D12_RANGE empty{0,0};void* mapped{};
            Hr(transfer.upload->Map(0,&empty,&mapped),"resident tile staging map");
            auto* output=static_cast<GpuResidentTileGpuInstance*>(mapped);
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
            transfer.upload->Unmap(0,nullptr);
            f.tileTransfers.push_back(std::move(transfer));
        }
        if(first!=f.cpuCount)Fail("GPU tile source size changed after preparation");
        f.instanceRangeData[route]={f.cpuCount,f.instanceCount-f.cpuCount,
                                    (f.instanceCount>f.cpuCount)?Physical(f.publishedInstanceSRV):0u,2};
        if(coreSceneWork)EnsureVisibilityBuffers(f,f.cpuCount,f.instanceCount-f.cpuCount);


        auto& opaqueRuns=result.opaqueRuns;
        auto appendMaterial=[&](uint32_t material){
            if(material>=surfaceData.size())Fail("Opaque instance references missing material");
            uint32_t mask=MaterialTextureMask(surfaceData[material]);
            if(opaqueRuns.empty()||opaqueRuns.back().mask!=mask)
                opaqueRuns.push_back({uint32_t(opaqueRuns.empty()?0u:
                    opaqueRuns.back().first+opaqueRuns.back().count),1u,mask});
            else ++opaqueRuns.back().count;
        };
        for(uint32_t i=0;i<worldCount;++i)appendMaterial(CpuInstance(f,i).material);
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
