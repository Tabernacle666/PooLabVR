#include "GraphicsInternal.h"
#include <limits>
#include <numeric>
#include <iterator>
#include <tuple>

namespace OASIS {
    void Graphics::Impl::CollectResidentSpans(){
        const auto completed=CompletedFences();
        std::erase_if(retiredResidentSpans,[&](const RetiredResidentSpan& span){
            if(!GpuFencesComplete(span.fences,completed))return false;
            auto& free=residentInstancePages.at(span.page).free;
            auto next=free.lower_bound(span.first);
            uint32_t first=span.first,count=span.count;
            if(next!=free.begin()){
                auto previous=std::prev(next);
                if(previous->first+previous->second==first){
                    first=previous->first;count+=previous->second;free.erase(previous);
                }
            }
            if(next!=free.end()&&first+count==next->first){count+=next->second;free.erase(next);}
            free.emplace(first,count);
            return true;
        });
    }
    void Graphics::Impl::RetireResidentSpan(ResidentTileGpu& gpu){
        if(gpu.page==Invalid)return;
        retiredResidentSpans.push_back({gpu.page,gpu.firstElement,gpu.objectCapacity,SubmittedFences()});
        gpu.page=Invalid;gpu.firstElement=0;gpu.objectCapacity=0;
    }
    void Graphics::Impl::AllocateResidentSpan(ResidentTileGpu& gpu,uint32_t count,int32_t layer){
        // Building floors retain distinct territorial keys but share pages.
        layer=layer==64||layer==68||layer==69?64:layer==65||layer==70?65:layer==TerrainTileLayer||layer==67?TerrainTileLayer:0;
        const uint32_t wanted=GrowCapacity(gpu.objectCapacity,std::max(1u,count));
        RetireResidentSpan(gpu);
        // Pages are grouped by producer layer, not by tile or material.
        // Keep immutable vegetation separate from changing building records.
        auto take=[&](uint32_t index){
            auto& page=residentInstancePages[index];
            if(page.layer!=layer)return false;
            for(auto it=page.free.begin();it!=page.free.end();++it){
                if(it->second<wanted)continue;
                const auto first=it->first,remaining=it->second-wanted;
                page.free.erase(it);
                if(remaining)page.free.emplace(first+wanted,remaining);
                gpu.page=index;gpu.firstElement=first;gpu.objectCapacity=wanted;
                return true;
            }
            return false;
        };
        for(uint32_t i=0;i<residentInstancePages.size();++i)if(take(i))return;
        ResidentInstancePage page{};
        page.layer=layer;
        page.capacity=std::max(4096u,wanted);
        if(page.capacity>=0x80000000u)Fail("Resident instance page offset overflow");
        page.buffer=Temporary(Buffer(UINT64(page.capacity)*sizeof(GpuResidentTileGpuInstance)),
            D3D12_RESOURCE_STATE_COMMON);
        page.srv=Srv(page.buffer.Get(),page.capacity,sizeof(GpuResidentTileGpuInstance));
        page.free.emplace(0u,page.capacity);
        residentInstancePages.push_back(std::move(page));
        if(!take(uint32_t(residentInstancePages.size()-1)))Fail("Resident instance span allocation");
    }
    XMFLOAT4 Graphics::Impl::InstanceCameraSphere(const GpuInstance& instance,uint32_t mesh)const{
        const auto& bounds=meshes.ranges[mesh].bounds;
        const auto& s=instance.scale;
        const auto& q=instance.rotation;
        const auto local=XMVectorSet(bounds.x*s.x,bounds.y*s.y,bounds.z*s.z,0.f);
        const auto center=XMVector3Rotate(local,XMVectorSet(q.x,q.y,q.z,q.w));
        const auto& p=instance.positionRadius;
        return {p.x+XMVectorGetX(center),p.y+XMVectorGetY(center),p.z+XMVectorGetZ(center),
            bounds.w*std::max({std::abs(s.x),std::abs(s.y),std::abs(s.z)})};
    }

    Graphics::Impl::ResidentBatchData Graphics::Impl::PrepareResidentBatches(
        Frame& f,const GpuSceneCache* tileCatalog,const XRFrame& xr,bool coreSceneWork,bool campusVisible) {
        ResidentBatchData result;
        const auto* tiles=tileCatalog?&tileCatalog->ResidentTiles():nullptr;
        const uint32_t tileCount=tiles?uint32_t(tiles->size()):0;
        if(tileCount>=0x80000000u-2u)Fail("Too many GPU resident tile ranges");
        EnsureInstanceRoutes(f,tileCount+2);
        if(!f.instanceRangeData)Fail("Frame instance source table unavailable");
        f.tileTransfers.clear();f.heldTileSources.clear();
        CollectResidentSpans();
        std::vector<uint8_t> pageSeen(residentInstancePages.size(),0u);


        for(auto it=residentTileGpu.begin();it!=residentTileGpu.end();){
            if(!tiles||!tiles->contains(it->first)){
                RetireResidentSpan(it->second);
                it=residentTileGpu.erase(it);
            }else ++it;
        }
        const uint32_t worldCount=sceneCache.StaticCount()+f.snapshotCount;
        auto includeCameraRange=[&](uint32_t first,uint32_t count){
            if(!count)return;
            auto& ranges=result.cameraRanges;
            if(!ranges.empty()&&ranges.back().first+ranges.back().count==first)
                ranges.back().count+=count;
            else ranges.push_back({first,count});
        };
        if(coreSceneWork)includeCameraRange(0,worldCount);
        std::array<std::array<XMVECTOR,6>,MaxViews> frusta{};
        for(uint32_t eye=0;eye<xr.viewCount;++eye){
            const auto m=XMMatrixTranspose(XMLoadFloat4x4(&xr.vp[eye]));
            frusta[eye]={m.r[3]+m.r[0],m.r[3]-m.r[0],m.r[3]+m.r[1],m.r[3]-m.r[1],
                m.r[2],m.r[3]-m.r[2]};
        }
        const auto inCamera=[&](const ResidentTileGpu& gpu){
            if(gpu.cameraBoundsUnbounded||(visibilityDiagnosticMask&1u)!=0u)return true;
            if(!gpu.cameraBoundsValid)return false;
            const auto lower=XMLoadFloat3(&gpu.cameraBoundsMin);
            const auto upper=XMLoadFloat3(&gpu.cameraBoundsMax);
            for(uint32_t eye=0;eye<xr.viewCount;++eye){
                bool outside=false;
                for(const auto& plane:frusta[eye]){
                    const auto corner=XMVectorSelect(lower,upper,
                        XMVectorGreaterOrEqual(plane,XMVectorZero()));
                    if(XMVectorGetX(XMVector4Dot(XMVectorSetW(corner,1.f),plane))<0.f){
                        outside=true;
                        break;
                    }
                }
                if(!outside)return true;
            }
            return false;
        };
        auto batchKey=[&](uint32_t material,uint32_t mesh,uint32_t flags){
            if(material>=surfaceData.size())Fail("Opaque instance references missing material");
            const auto& surface=surfaceData[material];
            const auto variant=(flags&ShuttleCrt)?ShuttleCrtVariant:MaterialRunKey(surface);
            return std::tuple{variant,mesh,material,flags};
        };
        // NVIDIA Advanced API Performance: PSOs. Only reorder ordinary opaque
        // spans; alpha coverage, CRT, deformation and other explicit ordering
        // boundaries stay in the original relative order.
        auto reorderable=[&](const auto& value){
            auto [variant,mesh,material,flags]=value;
            return variant!=ShuttleCrtVariant&&surfaceData[material].extra.y==0u&&
                (flags&(DynamicObject|DeformedObject|AlphaBlendObject|ShuttleSpeedDust|
                        SolarWorldVisual))==0u;
        };
        // Compaction stability is not a global rendering dependency.
        // Alpha-tested, dynamic and deformed opaque instances keep the
        // stable scan within their draw group, but need not flush every
        // other compatible group. Keep explicit special-visual boundaries.
        enum ResidentOrder : uint32_t { FreeOrder, StableOrder, StrictOrder, NotOpaque };
        auto residentOrder=[&](const GpuResidentTileObject& object,const auto& value){
            const auto& surface=surfaceData[object.material];
            // Same early rejection as camera visibility in compute.hlsl,
            // including the alpha-blend flag added by ConvertObject and
            // the inactive flag added by LoadInstanceImpl.
            if(!object.enabled||(object.flags&(16u|64u|128u))||surface.extra.y==2u)
                return NotOpaque;
            if(object.flags&(ShuttleCrt|ShuttleSpeedDust|SolarWorldVisual))
                return StrictOrder;
            return reorderable(value)?FreeOrder:StableOrder;
        };
        auto residentCategory=[](uint32_t flags){
            return OASIS_PRISM_CATEGORY(flags);
        };
        auto sortOpaque=[&](std::span<uint32_t> order,const auto& entry) {
            auto canReorder=[&](uint32_t index){return reorderable(entry(index));};
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
        auto& buckets=result.residentBuckets;
        // Spans refer to cached tile orders; no per-instance sort or new upload.
        // The existing frame opaque-order upload becomes a packed resident
        // candidate list, independent of the immutable instance routes.
        struct Span {const uint32_t* order;uint32_t sourceFirst,count;};
        // Key by complete material/VRS state and the compaction/address mode.
        // An ordered scan is local to its group, not a global flush boundary.
        std::array<std::vector<Span>,256> pending;
        std::vector<uint32_t> activeMasks;
        uint32_t residentFirst=worldCount;
        auto appendSpan=[&](const Span& span){
            if(span.count>f.cpuCount-residentFirst)
                Fail("Resident opaque candidate partition overflow");
            for(uint32_t i=0;i<span.count;++i)
                orderedInstances[residentFirst+i]=span.sourceFirst+span.order[i];
            residentFirst+=span.count;
        };
        auto addBucket=[&](uint32_t mask,uint32_t start,uint32_t ordered){
            const uint32_t count=residentFirst-start;
            if(!count)return;
            // Coalesce consecutive ordered spans with identical pipeline
            // state without changing the candidate sequence.
            if(!buckets.empty()){
                auto& last=buckets.back();
                if(last.mask==mask&&last.ordered==ordered&&last.first+last.count==start){
                    last.count+=count;
                    return;
                }
            }
            buckets.push_back({start,count,mask,ordered});
        };
        auto flush=[&]{
            for(uint32_t key:activeMasks){
                const uint32_t start=residentFirst;
                for(const auto& span:pending[key])appendSpan(span);
                addBucket(key/2u,start,key%2u);
                pending[key].clear();
            }
            activeMasks.clear();
        };
        if(tiles)for(const auto& [key,tile]:*tiles){
            if(tile.objects.size()>UINT32_MAX-first)
                Fail("Resident tile range exceeds 32-bit instance address space");
            const uint32_t count=uint32_t(tile.objects.size());
            if(!coreSceneWork||(!campusVisible&&key.layer>=64)){
                // Keep ordinals while OASIS is represented by its atlas or
                // another body is native. Dirty pages remain pending.
                f.instanceRangeData[route++]={first,count,Invalid,1};
                first+=count;
                continue;
            }
            auto& gpu=residentTileGpu[key];
            const bool topologyChanged=gpu.topologyGeneration!=tile.topologyGeneration;
            const bool rebuild=gpu.page==Invalid||count>gpu.objectCapacity||
                gpu.topologyGeneration==0;
            if(rebuild){
                AllocateResidentSpan(gpu,count,key.layer);
            }
            gpu.objectCount=count;
            gpu.topologyGeneration=tile.topologyGeneration;

            if(gpu.opaqueOrderRevision!=tile.revision||gpu.opaqueOrder.size()!=count||topologyChanged||rebuild){
                auto entry=[&](uint32_t index){
                    const auto& object=tile.objects[index];
                    return batchKey(object.material,object.mesh,object.flags);
                };
                gpu.opaqueOrder.resize(count);
                std::iota(gpu.opaqueOrder.begin(),gpu.opaqueOrder.end(),0u);
                sortOpaque(gpu.opaqueOrder,entry);
                std::vector<ResidentMaterialRun> runs;
                for(uint32_t i=0;i<count;++i){
                    const auto index=gpu.opaqueOrder[i];
                    const auto value=entry(index);
                    const auto mask=std::get<0>(value);
                    const auto& object=tile.objects[index];
                    const uint32_t ordered=residentOrder(object,value);
                    const uint32_t category=residentCategory(object.flags);
                    if(!runs.empty()&&runs.back().mask==mask&&
                       runs.back().reserved==ordered&&runs.back().category==category)
                        ++runs.back().count;
                    else runs.push_back({i,1u,mask,ordered,category});
                }
                gpu.runs=std::move(runs);
                gpu.opaqueOrderRevision=tile.revision;
                gpu.cameraBoundsValid=gpu.cameraBoundsUnbounded=false;
                auto lower=XMVectorReplicate(std::numeric_limits<float>::infinity());
                auto upper=XMVectorNegate(lower);
                for(const auto& object:tile.objects){
                    if(!object.enabled)continue;
                    if(object.flags&(DeformedObject|ShuttleSpeedDust)){
                        gpu.cameraBoundsUnbounded=true;
                        break;
                    }
                    const auto instance=ConvertResidentBase(object);
                    const auto addMesh=[&](uint32_t mesh){
                        const auto sphere=InstanceCameraSphere(instance,mesh);
                        const auto center=XMLoadFloat4(&sphere);
                        const auto extent=XMVectorReplicate(sphere.w+.01f);
                        lower=XMVectorMin(lower,center-extent);
                        upper=XMVectorMax(upper,center+extent);
                    };
                    addMesh(object.mesh);
                    gpu.cameraBoundsValid=true;
                }
                XMStoreFloat3(&gpu.cameraBoundsMin,lower);
                XMStoreFloat3(&gpu.cameraBoundsMax,upper);
            }
            const bool cameraVisible=count&&inCamera(gpu);
            const auto& page=residentInstancePages.at(gpu.page);
            if(pageSeen.size()<residentInstancePages.size())pageSeen.resize(residentInstancePages.size());
            if(!pageSeen[gpu.page]){
                f.heldTileSources.push_back(page.buffer);pageSeen[gpu.page]=1u;
            }
            f.instanceRangeData[route++]={first,count,Physical(page.srv),0x80000000u|gpu.firstElement};
            if(cameraVisible){
                includeCameraRange(first,count);
                if(gpu.runs.size()>UINT32_MAX)Fail("Resident run count overflow");
                tileBatchDispatch.push_back({key,first,count,uint32_t(gpu.runs.size())});
                if(!orderedInstances)Fail("Visible resident opaque order missing");
                for(const auto& run:gpu.runs){
                    // Do not alter the instance routes, tile visibility ranges,
                    // shadow/ray work or the separately sorted transparent pass.
                    if(run.reserved==NotOpaque||
                       !OASIS_PRISM_VISIBLE(prismSceneMask,run.category)){
                        result.residentCpuRejected+=run.count;
                        continue;
                    }
                    const Span span{gpu.opaqueOrder.data()+run.first,first,run.count};
                    if(run.reserved==StrictOrder){
                        ++result.residentOrderBoundaries;
                        flush();
                        const uint32_t start=residentFirst;
                        appendSpan(span);
                        addBucket(run.mask,start,1u);
                    }else{
                        const uint32_t key=run.mask*2u+(run.reserved==StableOrder?1u:0u);
                        if(key>=pending.size())Fail("Resident material bucket key out of range");
                        auto& spans=pending[key];
                        if(spans.empty())activeMasks.push_back(key);
                        spans.push_back(span);
                    }
                }
            }
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
            transfer.buffer=page.buffer;
            transfer.firstElement=gpu.firstElement;
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
                record.padding0=uint32_t(o.identity);
                record.padding1=uint32_t(o.identity>>32u);
                record.enabled=o.enabled;
                record.padding2=0;
            }
            f.tileTransfers.push_back(std::move(transfer));
        }
        flush();
        // Scan workspace is needed only for the remaining ordered buckets.
        for(auto& bucket:buckets)if(bucket.ordered){
            bucket.blockFirst=tileBatchBlocks;
            bucket.blockCount=uint32_t((uint64_t(bucket.count)+255u)/256u);
            bucket.superFirst=tileBatchSuperBlocks;
            bucket.superCount=(bucket.blockCount+255u)/256u;
            if(tileBatchBlocks>UINT32_MAX-bucket.blockCount||
               tileBatchSuperBlocks>UINT32_MAX-bucket.superCount)
                Fail("Ordered resident scan capacity exhausted");
            tileBatchBlocks+=bucket.blockCount;
            tileBatchSuperBlocks+=bucket.superCount;
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
                MaterialRunKey(surfaceData[material]);
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
        const uint64_t totalRuns=uint64_t(opaqueRuns.size())+buckets.size();
        if(totalRuns>UINT32_MAX-7u)
            Fail("Resident opaque run address space exhausted");
        result.forestCounter=uint32_t(totalRuns);
        if(coreSceneWork)EnsureBatchRunCounters(f,result.forestCounter+7u);
        if(tileBatchBlocks>UINT32_MAX-tileBatchSuperBlocks)
            Fail("Resident tile scan hierarchy capacity exhausted");
        if(coreSceneWork)EnsureTileBatchBuffers(f,f.cpuCount,tileBatchBlocks+tileBatchSuperBlocks);
        return result;
    }
}
