#include "PluginHost.h"
#include "ClientAssets.h"
#include "Graphics.h"
#include "Audio.h"
#include "PhysicsRuntime.h"
#include "GpuFrameResources.h"
#include <filesystem>
#include <string>
#include <array>
#include <span>
#include <limits>
#include <algorithm>
#include <cstring>
namespace lab {
namespace {
std::wstring Wide(const char* value){if(!value||!*value)return {};int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,nullptr,0);if(!count)Fail("Invalid UTF-8 plugin path");std::wstring result(size_t(count),L'\0');if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,result.data(),count))Fail("Invalid UTF-8 plugin path");result.pop_back();return result;}
XMFLOAT3 V(const LabVec3V1& v){return {v.x,v.y,v.z};}
Pose P(const LabPoseV1& p){return {V(p.position),{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w}};}

}
PluginHost::PluginHost()noexcept{
    api_.abiVersion=LABOVR_PLUGIN_ABI_V1;api_.structSize=sizeof(api_);api_.hostContext=this;api_.QueryService=QueryServiceThunk;
    visualService_={LABOVR_VISUAL_SERVICE_VERSION_V1,sizeof(LabVisualServiceV1),this};
    visualService_.SetContribution=[](void* c,const LabVisualContributionV1* p)->LabU32{return c?static_cast<PluginHost*>(c)->SetVisualContribution(p):0;};
    visualService_.PollCapture=[](void* c,LabVisualCaptureHandleV1 id,LabVisualCaptureResultV1* out)->LabU32 {auto* self=static_cast<PluginHost*>(c);return self&&self->visualDomain_?self->visualDomain_->PollCapture(id,out):0;};
    visualService_.ReleaseCapture=[](void* c,LabVisualCaptureHandleV1 id){auto* self=static_cast<PluginHost*>(c);if(self&&self->visualDomain_)self->visualDomain_->ReleaseCapture(id);};
    RegisterService(LABOVR_SERVICE_VISUAL_V1,LABOVR_VISUAL_SERVICE_VERSION_V1,"visual_v1",&visualService_);
    inputService_={LABOVR_INPUT_SERVICE_VERSION_V1,sizeof(LabInputServiceV1),this,GetInputSnapshotThunk,EmitHapticThunk,ResolveActionThunk,GetActionInfoThunk};
    audioService_={LABOVR_AUDIO_SERVICE_VERSION_V1,sizeof(LabAudioServiceV1),this,PublishAudioEventsThunk,PublishAudioEmittersThunk};
    gpuService_={LABOVR_GPU_SERVICE_VERSION_V1,sizeof(LabGpuServiceV1),this,SetGpuProviderThunk};
    RegisterService(LABOVR_SERVICE_INPUT_V1,LABOVR_INPUT_SERVICE_VERSION_V1,"input_v1",&inputService_);
    audioService_.SetAcousticBoxes=[](void* c,const LabAcousticBoxV1* boxes,LabU32 n)->LabU32{if(!c)return 0;auto* self=static_cast<PluginHost*>(c);return self->audio_?self->audio_->SetAcousticBoxes(reinterpret_cast<uintptr_t>(self),boxes,n):0;};
    RegisterService(LABOVR_SERVICE_AUDIO_V1,LABOVR_AUDIO_SERVICE_VERSION_V1,"audio_v1",&audioService_);
    inputService_.GetPhysicalSources=[](void* c,LabInputPhysicalSourcesV1* out)->LabU32{
        if(!c||!out||out->structSize<sizeof(*out))return 0;auto* self=static_cast<PluginHost*>(c);auto* system=self->inputSystem_;if(!system)return 0;auto action=system->ActionFromHandle(out->action);if(!action)return 0;
        out->count=system->PhysicalCount();if(out->capacity<out->count||!out->sources)return 0;const auto& raw=system->LastPhysical();
        for(uint32_t i=0;i<out->count;i++){auto& result=out->sources[i];result={};result.id=i+1;const auto& info=system->Physical({i});snprintf(result.name,sizeof(result.name),"%s",info.name.c_str());result.type=uint32_t(info.type)+1;
            for(uint32_t h=0;h<2;h++){bool available=system->PhysicalAvailable({i},h);result.availableMask|=available?1u<<h:0;if(i<raw.controlCount&&raw.focused&&available&&raw.controls[i].active[h]){result.activeMask|=1u<<h;result.downMask|=raw.controls[i].down[h]?1u<<h:0;}}
            for(const auto& b:system->Mapping().Bindings())if(b.action==action&&b.source.value==i)result.selectedMask|=1u<<b.hand;
        }return 1;
    };
    inputService_.SetBooleanSources=[](void* c,const LabInputBooleanSourcesV1* r)->LabU32{
        if(!c||!r||r->structSize<sizeof(*r)||(r->count&&!r->sourceIds)||r->count>MaxPhysicalControls)return 0;auto* self=static_cast<PluginHost*>(c);if(!self->inputSystem_)return 0;std::vector<PhysicalControlId> sources;for(uint32_t i=0;i<r->count;i++){if(!r->sourceIds[i])return 0;sources.push_back({r->sourceIds[i]-1});}return self->inputSystem_->SetBooleanSources(self->inputSystem_->ActionFromHandle(r->action),r->handMask,sources)?1u:0u;
    };
    gpuService_.RequestCapture=[](void* c,const LabCaptureRequestV1* r,LabU64* id)->LabU32 {if(!c||!r||!id)return 0;auto* self=static_cast<PluginHost*>(c);return self->graphics_?self->graphics_->RequestCapture(reinterpret_cast<uintptr_t>(self),*r,*id):0;};
    gpuService_.SetMultisampling=[](void* c,const LabMultisamplingV1* s)->LabU32{if(!c||!s)return 0;auto* self=static_cast<PluginHost*>(c);return self->graphics_?self->graphics_->SetMultisampling(reinterpret_cast<uintptr_t>(self),*s):0;};
    gpuService_.SetAntialiasing=[](void* c,const LabAntialiasingV1* s)->LabU32 {if(!c||!s)return 0;auto* h=static_cast<PluginHost*>(c);return h->graphics_?h->graphics_->SetAntialiasing(reinterpret_cast<uintptr_t>(h),*s):0;};
    gpuService_.SetFogTransport=[](void* c,const LabFogTransportV1* s)->LabU32 {if(!c||!s)return 0;auto* h=static_cast<PluginHost*>(c);return h->graphics_?h->graphics_->SetFogTransport(reinterpret_cast<uintptr_t>(h),*s):0;};
    gpuService_.SetSurfaceSettings=[](void* c,const LabSurfaceSettingsV1* s)->LabU32 {if(!c||!s)return 0;auto* h=static_cast<PluginHost*>(c);return h->graphics_?h->graphics_->SetSurfaceSettings(reinterpret_cast<uintptr_t>(h),*s):0;};
    gpuService_.GetCaptureStatus=[](void* c,LabCaptureStatusV1* s)->LabU32 {if(!c||!s)return 0;auto* self=static_cast<PluginHost*>(c);return self->graphics_?self->graphics_->CaptureStatus(reinterpret_cast<uintptr_t>(self),*s):0;};
    gpuService_.PublishTerminalV2=[](void* c,const LabTerminalFrameV2* f)->LabU32 {if(!c||!f)return 0;auto* self=static_cast<PluginHost*>(c);return self->graphics_?self->graphics_->PublishTerminal(reinterpret_cast<uintptr_t>(self),*f):0;};
    gpuService_.HitTestTerminalV2=[](void* c,const LabTerminalHitQueryV2* q,LabTerminalHitV2* h)->LabU32 {if(!c||!q||!h)return 0;auto* self=static_cast<PluginHost*>(c);return self->graphics_?self->graphics_->HitTestTerminal(reinterpret_cast<uintptr_t>(self),*q,*h):0;};
    gpuService_.PublishTerminal=[](void* c,const LabTerminalFrameV1* f)->LabU32 {if(!c||!f)return 0;auto* self=static_cast<PluginHost*>(c);return self->graphics_?self->graphics_->PublishTerminal(reinterpret_cast<uintptr_t>(self),*f):0;};
    gpuService_.SetFogSettings=[](void* c,const LabFogSettingsV1* f)->LabU32{if(!c||!f)return 0;auto* self=static_cast<PluginHost*>(c);return self->graphics_?self->graphics_->SetFogSettings(reinterpret_cast<uintptr_t>(self),*f):0;};
    gpuService_.SetFogInput=[](void* c,const LabFogInputV1* f)->LabU32{if(!c||!f)return 0;auto* self=static_cast<PluginHost*>(c);return self->graphics_?self->graphics_->SetFogInput(reinterpret_cast<uintptr_t>(self),*f):0;};
    storageService_=profileStorage_.Service();RegisterService(LABOVR_SERVICE_STORAGE_V1,1,"storage_v1",&storageService_);
    gpuService_.SetFogWind=[](void* c,const LabFogWindV1* f)->LabU32{if(!c||!f)return 0;auto* self=static_cast<PluginHost*>(c);return self->graphics_?self->graphics_->SetFogWind(reinterpret_cast<uintptr_t>(self),*f):0;};
    gpuService_.SetPopulationOpticalSettings=[](void* c,const LabPopulationOpticalSettingsV1* s)->LabU32{if(!c||!s)return 0;auto* self=static_cast<PluginHost*>(c);return self->graphics_?self->graphics_->SetPopulationOpticalSettings(reinterpret_cast<uintptr_t>(self),*s):0;};
    RegisterService(LABOVR_SERVICE_GPU_V1,LABOVR_GPU_SERVICE_VERSION_V1,"gpu_v1",&gpuService_);
    gpuInteropService_={1,sizeof(LabGpuInteropServiceV1),this,{}};
    gpuInteropService_.CreateSharedBuffer=[](void* c,LabU64 bytes,LabGpuInteropResourceHandleV1* out)->LabU32{return c?static_cast<PluginHost*>(c)->CreateSharedBuffer(bytes,out):0;};
    gpuInteropService_.CreateSharedFence=[](void* c,LabU64 value,LabGpuInteropFenceHandleV1* out)->LabU32{return c?static_cast<PluginHost*>(c)->CreateSharedFence(value,out):0;};
    gpuInteropService_.GetCudaMemoryHandle=[](void* c,LabGpuInteropResourceHandleV1 h,LabGpuInteropMemoryExportV1* out)->LabU32{return c?static_cast<PluginHost*>(c)->GetCudaMemoryHandle(h,out):0;};
    gpuInteropService_.GetCudaFenceHandle=[](void* c,LabGpuInteropFenceHandleV1 h,LabU32 duplicate,LabGpuInteropFenceExportV1* out)->LabU32{return c?static_cast<PluginHost*>(c)->GetCudaFenceHandle(h,duplicate,out):0;};
    gpuInteropService_.ReleaseResource=[](void* c,LabGpuInteropResourceHandleV1 h)->LabU32{return c?static_cast<PluginHost*>(c)->ReleaseInteropResource(h):0;};
    gpuInteropService_.ReleaseFence=[](void* c,LabGpuInteropFenceHandleV1 h)->LabU32{return c?static_cast<PluginHost*>(c)->ReleaseInteropFence(h):0;};
    RegisterService(LABOVR_SERVICE_GPU_INTEROP_V1,1,"gpu_interop_v1",&gpuInteropService_);
}
const void* LABOVR_PLUGIN_CALL PluginHost::QueryServiceThunk(void* context,LabU32 serviceId,LabU32 requestedVersion){return context?static_cast<PluginHost*>(context)->QueryService(serviceId,requestedVersion):nullptr;}
const void* PluginHost::QueryService(LabU32 serviceId,LabU32 requestedVersion)const noexcept{if(std::find(allowedServices_.begin(),allowedServices_.end(),serviceId)==allowedServices_.end())return nullptr;for(const auto& entry:services_)if(entry.id==serviceId&&entry.version==requestedVersion)return entry.table;return nullptr;}
bool PluginHost::RegisterService(LabU32 serviceId,LabU32 version,const char* manifestName,const void* table){if(!serviceId||!version||!manifestName||!*manifestName||!table)return false;for(const auto& entry:services_)if(entry.id==serviceId&&entry.version==version)return false;services_.push_back({serviceId,version,manifestName,table});return true;}

void PluginHost::ConfigureWorldServices(std::string worldId,std::filesystem::path packageRoot,std::span<const std::string> requiredServices){
    ClearWorldServices();if(++worldGeneration_==0)++worldGeneration_;activeWorldId_=std::move(worldId);activePackageRoot_=std::move(packageRoot);profileStorage_.SetOwner(activeWorldId_);
    for(const auto& name:requiredServices)for(const auto& entry:services_)if(name==entry.manifestName){if(std::find(allowedServices_.begin(),allowedServices_.end(),entry.id)==allowedServices_.end())allowedServices_.push_back(entry.id);break;}
}
void PluginHost::ClearWorldServices() noexcept{
    if(graphics_&&!activeWorldId_.empty())graphics_->WaitIdle();
    if(graphics_){graphics_->ClearTerminal(reinterpret_cast<uintptr_t>(this));graphics_->ClearCapture(reinterpret_cast<uintptr_t>(this));graphics_->ClearRendering(reinterpret_cast<uintptr_t>(this));graphics_->ClearAtmosphere(reinterpret_cast<uintptr_t>(this));}
    if(audio_)audio_->SetAcousticBoxes(reinterpret_cast<uintptr_t>(this),nullptr,0);
    if(++audioGeneration_==0)++audioGeneration_;ClearVisualPackage();gpuProvider_={};if(physics_&&physicsOwner_)physics_->ResetOwner(physicsOwner_);ClearInteropGeneration();audioEmitterCount_=0;
    profileStorage_.SetOwner({});activeWorldId_.clear();activePackageRoot_.clear();allowedServices_.clear();
}

void PluginHost::BindGraphics(Graphics& graphics)noexcept{graphics_=&graphics;visualDomain_=std::make_shared<VisualDomain>(graphics);graphics.InitializeDomain(*visualDomain_);auto luid=graphics.Device()->GetAdapterLuid();std::memcpy(gpuInteropService_.adapterLuid,&luid,sizeof(luid));}
void PluginHost::BindPhysics(PhysicsRuntime& physics){if(physics_)Fail("Physics already bound to this host");physics_=&physics;physicsOwner_=physics.CreateOwner(gpuInteropService_);physicsService_=physics.Service(physicsOwner_);if(!RegisterService(LABOVR_SERVICE_PHYSICS_V1,1,"physics_v1",&physicsService_))Fail("Cannot register physics_v1");}
LabU32 PluginHost::CreateSharedBuffer(LabU64 bytes,LabGpuInteropResourceHandleV1* out){
    if(out)*out=0;if(!graphics_||!out||!bytes||bytes%4||activeWorldId_.empty())return 0;std::lock_guard lock(interopMutex_);
    auto e=std::make_unique<InteropResource>();e->worldGeneration=worldGeneration_;e->bytes=bytes;auto* device=graphics_->Device();
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    e->allocationBytes=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;if(!e->allocationBytes||e->allocationBytes==UINT64_MAX)return 0;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&e->resource)))||FAILED(device->CreateSharedHandle(e->resource.Get(),nullptr,GENERIC_ALL,nullptr,&e->shared)))return 0;
    auto h=nextInteropHandle_++;interopResources_.emplace(h,std::move(e));*out=h;return 1;
}
LabU32 PluginHost::CreateSharedFence(LabU64 value,LabGpuInteropFenceHandleV1* out){
    if(out)*out=0;if(!graphics_||!out||activeWorldId_.empty())return 0;std::lock_guard lock(interopMutex_);auto e=std::make_unique<InteropFence>();e->worldGeneration=worldGeneration_;auto* device=graphics_->Device();
    if(FAILED(device->CreateFence(value,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&e->fence)))||FAILED(device->CreateSharedHandle(e->fence.Get(),nullptr,GENERIC_ALL,nullptr,&e->shared)))return 0;auto h=nextInteropHandle_++;interopFences_.emplace(h,std::move(e));*out=h;return 1;
}
LabU32 PluginHost::GetCudaMemoryHandle(LabGpuInteropResourceHandleV1 h,LabGpuInteropMemoryExportV1* out){
    if(!out||out->structSize<sizeof(*out))return 0;std::lock_guard lock(interopMutex_);auto i=interopResources_.find(h);if(i==interopResources_.end()||i->second->worldGeneration!=worldGeneration_)return 0;
    HANDLE duplicate{};if(!DuplicateHandle(GetCurrentProcess(),i->second->shared,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))return 0;*out={sizeof(*out),0,duplicate,i->second->allocationBytes,i->second->bytes};return 1;
}
LabU32 PluginHost::GetCudaFenceHandle(LabGpuInteropFenceHandleV1 h,LabU32 duplicateHandle,LabGpuInteropFenceExportV1* out){
    if(!out||out->structSize<sizeof(*out))return 0;std::lock_guard lock(interopMutex_);auto i=interopFences_.find(h);if(i==interopFences_.end()||i->second->worldGeneration!=worldGeneration_)return 0;
    HANDLE duplicate{};if(duplicateHandle&&!DuplicateHandle(GetCurrentProcess(),i->second->shared,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))return 0;*out={sizeof(*out),0,duplicate,i->second->fence->GetCompletedValue()};return 1;
}
LabU32 PluginHost::ReleaseInteropResource(LabGpuInteropResourceHandleV1 h){std::lock_guard lock(interopMutex_);auto i=interopResources_.find(h);if(i==interopResources_.end()||i->second->worldGeneration!=worldGeneration_)return 0;interopResources_.erase(i);return 1;}
LabU32 PluginHost::ReleaseInteropFence(LabGpuInteropFenceHandleV1 h){std::lock_guard lock(interopMutex_);auto i=interopFences_.find(h);if(i==interopFences_.end()||i->second->worldGeneration!=worldGeneration_)return 0;interopFences_.erase(i);return 1;}
Microsoft::WRL::ComPtr<ID3D12Resource> PluginHost::ResolveInteropResource(LabGpuInteropResourceHandleV1 h)const{std::lock_guard lock(interopMutex_);auto i=interopResources_.find(h);return i!=interopResources_.end()&&i->second->worldGeneration==worldGeneration_?i->second->resource:Microsoft::WRL::ComPtr<ID3D12Resource>{};}
Microsoft::WRL::ComPtr<ID3D12Fence> PluginHost::ResolveInteropFence(LabGpuInteropFenceHandleV1 h)const{std::lock_guard lock(interopMutex_);auto i=interopFences_.find(h);return i!=interopFences_.end()&&i->second->worldGeneration==worldGeneration_?i->second->fence:Microsoft::WRL::ComPtr<ID3D12Fence>{};}
void PluginHost::ClearInteropGeneration()noexcept{std::lock_guard lock(interopMutex_);std::erase_if(interopResources_,[&](const auto& e){return e.second->worldGeneration==worldGeneration_;});std::erase_if(interopFences_,[&](const auto& e){return e.second->worldGeneration==worldGeneration_;});}
LabU32 LABOVR_PLUGIN_CALL PluginHost::SetGpuProviderThunk(void* context,const LabGpuPublicationProviderV1* provider){if(!context)return 0;auto& self=*static_cast<PluginHost*>(context);if(!provider){self.gpuProvider_={};return 1;}constexpr size_t required=offsetof(LabGpuPublicationProviderV1,domainId)+sizeof(provider->domainId);if(provider->gpuAbiVersion!=LABOVR_GPU_ABI_V1||provider->structSize<required||!provider->Acquire||!provider->Release||!provider->domainId)return 0;self.gpuProvider_={};auto bytes=(std::min)(size_t(provider->structSize),sizeof(LabGpuPublicationProviderV1));memcpy(&self.gpuProvider_,provider,bytes);self.gpuProvider_.structSize=LabU32(bytes);if(self.gpuProvider_.streamMaterialCount>ConsoleSpec::MaxMaterials||(self.gpuProvider_.streamMaterialCount&&!self.gpuProvider_.streamMaterials)){self.gpuProvider_={};return 0;}return 1;}

LabU32 LABOVR_PLUGIN_CALL PluginHost::PublishAudioEventsThunk(void* context,const LabAudioEventBatchV1* batch){return context?static_cast<PluginHost*>(context)->PublishAudioEvents(batch):0;}
LabU32 LABOVR_PLUGIN_CALL PluginHost::PublishAudioEmittersThunk(void* context,const LabAudioEmitterBatchV1* batch){return context?static_cast<PluginHost*>(context)->PublishAudioEmitters(batch):0;}
LabU32 PluginHost::PublishAudioEvents(const LabAudioEventBatchV1* batch){if(!channels_||!batch||batch->version!=LABOVR_AUDIO_SERVICE_VERSION_V1)return 0;bool ok=true;if(batch->structSize>=offsetof(LabAudioEventBatchV1,detailedEventCount)+sizeof(batch->detailedEventCount)&&batch->detailedEvents)for(LabU32 i=0;i<batch->detailedEventCount;i++){const auto& e=batch->detailedEvents[i];if(e.structSize<sizeof(LabAudioSoundEventV1)){ok=false;continue;}AudioEvent out;out.kind=e.kind;out.position={e.position[0],e.position[1],e.position[2]};out.gain=e.gain;out.inputFrameId=e.frameId;out.frequency=e.frequency;out.decay=e.decay;out.wave=e.wave;out.flags=1;ok&=channels_->audioEvents.push(out);}else if(batch->events)for(LabU32 i=0;i<batch->eventCount;i++){const auto& e=batch->events[i];AudioEvent out;out.kind=e.kind;out.position={e.position[0],e.position[1],e.position[2]};out.gain=e.gain;out.inputFrameId=e.frameId;ok&=channels_->audioEvents.push(out);}return ok?1u:0u;}
LabU32 PluginHost::PublishAudioEmitters(const LabAudioEmitterBatchV1* batch){if(!batch||batch->version!=LABOVR_AUDIO_SERVICE_VERSION_V1||batch->structSize<sizeof(LabAudioEmitterBatchV1)||(batch->emitterCount&&!batch->emitters))return 0;
    const LabU32 copied=(std::min)(batch->emitterCount,LabU32(audioEmitters_.size()));
    for(LabU32 i=0;i<copied;i++){
        if(batch->emitters[i].structSize<sizeof(LabAudioEmitterV1))return 0;
        for(LabU32 j=0;j<i;j++)if(batch->emitters[i].id==batch->emitters[j].id)return 0;
    }audioEmitterCount_=copied;for(LabU32 i=0;i<audioEmitterCount_;i++)audioEmitters_[i]=batch->emitters[i];return 1;}
void PluginHost::AppendAudio(AudioFrame& frame)const noexcept{for(LabU32 i=0;i<audioEmitterCount_&&frame.count<MaxAudioEmitters;i++){const auto& in=audioEmitters_[i];if(in.structSize<sizeof(LabAudioEmitterV1))continue;auto& out=frame.emitters[frame.count++];out.id=in.id;out.flags=in.flags;out.owner=reinterpret_cast<uintptr_t>(this);out.generation=audioGeneration_;out.wave=in.wave;out.position={in.position[0],in.position[1],in.position[2]};out.velocity={in.velocity[0],in.velocity[1],in.velocity[2]};out.gain=in.gain;out.frequency=in.frequency;out.occlusion=in.occlusion;}}

LabU32 LABOVR_PLUGIN_CALL PluginHost::GetInputSnapshotThunk(void* context,LabInputSnapshotV1* snapshot){return context?static_cast<PluginHost*>(context)->GetInputSnapshot(snapshot):0;}
LabU32 LABOVR_PLUGIN_CALL PluginHost::EmitHapticThunk(void* context,const LabInputHapticV1* haptic){return context?static_cast<PluginHost*>(context)->EmitHaptic(haptic):0;}
LabU32 LABOVR_PLUGIN_CALL PluginHost::ResolveActionThunk(void* context,const char* name,LabInputActionHandleV1* out){return context?static_cast<PluginHost*>(context)->ResolveAction(name,out):0;}
LabU32 LABOVR_PLUGIN_CALL PluginHost::GetActionInfoThunk(void* context,LabInputActionHandleV1 handle,LabInputActionInfoV1* out){return context?static_cast<PluginHost*>(context)->GetActionInfo(handle,out):0;}
namespace {
LabU32 PluginActionType(InputActionType type) noexcept {switch(type){case InputActionType::Boolean:return LABOVR_INPUT_ACTION_TYPE_BOOLEAN_V1;case InputActionType::Float:return LABOVR_INPUT_ACTION_TYPE_FLOAT_V1;case InputActionType::Vector2:return LABOVR_INPUT_ACTION_TYPE_VECTOR2_V1;case InputActionType::Pose:return LABOVR_INPUT_ACTION_TYPE_POSE_V1;case InputActionType::Haptic:return LABOVR_INPUT_ACTION_TYPE_HAPTIC_V1;}return 0;}
}
LabU32 PluginHost::ResolveAction(const char* name,LabInputActionHandleV1* out)const{
    if(!out)return 0;
    *out=0;
    if(!inputSystem_||!name||!*name)return 0;
    const auto action=inputSystem_->FindAction(name);
    if(!action)return 0;
    const LabInputActionHandleV1 handle=inputSystem_->ActionHandle(action);
    if(!handle)return 0;
    *out=handle;
    return 1;
}
LabU32 PluginHost::GetActionInfo(LabInputActionHandleV1 handle,LabInputActionInfoV1* out)const{
    if(!inputSystem_||!handle||!out||out->structSize<sizeof(LabInputActionInfoV1))return 0;
    const auto action=inputSystem_->ActionFromHandle(handle);
    if(!action)return 0;
    const auto& info=inputSystem_->Action(action);
    const auto& set=inputSystem_->ActionSet(info.actionSet);
    LabInputActionInfoV1 result{};
    result.structSize=sizeof(result);
    result.handle=handle;
    result.type=PluginActionType(info.type);
    result.nameUtf8=info.name.c_str();
    result.displayNameUtf8=info.displayName.c_str();
    result.actionSetNameUtf8=set.name.c_str();
    *out=result;
    return 1;
}
LabU32 PluginHost::GetInputSnapshot(LabInputSnapshotV1* out){
    if(!channels_||!inputSystem_||!out)return 0;const auto& frame=channels_->input.read();LabU32 sample=0,pose=0;
    for(const auto& info:inputSystem_->Actions()){
        if(!info.id||info.id.value>=frame.actionCount)continue;const auto& state=frame.actions[info.id.value];
        if(info.type==InputActionType::Pose){for(LabU32 source=0;source<InputSourceCount&&pose<inputPoses_.size();source++){auto& dst=inputPoses_[pose++];dst={};dst.structSize=sizeof(dst);dst.controlId=info.handle;dst.sourceId=source;const auto& p=state.pose[source];if(state.active[source])dst.flags|=LABOVR_INPUT_SAMPLE_ACTIVE_V1;if(p.tracked)dst.flags|=LABOVR_INPUT_SAMPLE_TRACKED_V1;dst.position[0]=p.pose.p.x;dst.position[1]=p.pose.p.y;dst.position[2]=p.pose.p.z;dst.orientation[0]=p.pose.q.x;dst.orientation[1]=p.pose.q.y;dst.orientation[2]=p.pose.q.z;dst.orientation[3]=p.pose.q.w;dst.velocity[0]=p.velocity.x;dst.velocity[1]=p.velocity.y;dst.velocity[2]=p.velocity.z;dst.angularVelocity[0]=p.angularVelocity.x;dst.angularVelocity[1]=p.angularVelocity.y;dst.angularVelocity[2]=p.angularVelocity.z;}continue;}
        if(info.type==InputActionType::Haptic)continue;
        for(LabU32 source=0;source<InputSourceCount&&sample<inputSamples_.size();source++){auto& dst=inputSamples_[sample++];dst={};dst.controlId=info.handle;dst.sourceId=source;if(state.active[source])dst.flags|=LABOVR_INPUT_SAMPLE_ACTIVE_V1;if(state.down[source])dst.flags|=LABOVR_INPUT_SAMPLE_DOWN_V1;if(state.pressed[source])dst.flags|=LABOVR_INPUT_SAMPLE_PRESSED_V1;if(info.type==InputActionType::Vector2){dst.valueCount=2;dst.value[0]=state.vector[source].x;dst.value[1]=state.vector[source].y;}else{dst.valueCount=1;dst.value[0]=state.scalar[source];}}
    }
    if(inputCaptured_)for(LabU32 i=0;i<sample;++i){auto& dst=inputSamples_[i];dst.flags&=~(LABOVR_INPUT_SAMPLE_DOWN_V1|LABOVR_INPUT_SAMPLE_PRESSED_V1);std::fill(std::begin(dst.value),std::end(dst.value),0.f);}
    *out={};out->version=LABOVR_INPUT_SERVICE_VERSION_V1;out->structSize=sizeof(*out);out->frameId=frame.frameId;out->timeSeconds=frame.time;out->samples=inputSamples_.data();out->sampleCount=sample;out->trackingRevision=frame.trackingRevision;out->focused=frame.focused&&!inputCaptured_?1u:0u;out->head={};out->head.structSize=sizeof(out->head);out->head.flags=(frame.focused?LABOVR_INPUT_SAMPLE_ACTIVE_V1:0u)|LABOVR_INPUT_SAMPLE_TRACKED_V1;out->head.position[0]=frame.head.p.x;out->head.position[1]=frame.head.p.y;out->head.position[2]=frame.head.p.z;out->head.orientation[0]=frame.head.q.x;out->head.orientation[1]=frame.head.q.y;out->head.orientation[2]=frame.head.q.z;out->head.orientation[3]=frame.head.q.w;out->poses=inputPoses_.data();out->poseCount=pose;return 1;
}
LabU32 PluginHost::EmitHaptic(const LabInputHapticV1* haptic){if(!channels_||!inputSystem_||!haptic||haptic->structSize<sizeof(LabInputHapticV1)||!haptic->actionId)return 0;auto action=inputSystem_->ActionFromHandle(haptic->actionId);if(!action||inputSystem_->Action(action).type!=InputActionType::Haptic)return 0;return channels_->haptics.push({haptic->sourceId,haptic->amplitude,haptic->durationSeconds,haptic->frameId,action})?1u:0u;}

}

namespace lab {
LabU32 PluginHost::SetVisualContribution(const LabVisualContributionV1* contribution) {
    if(!contribution){visualContribution_={};return 1;}
    if(activeWorldId_.empty()||contribution->version!=LABOVR_VISUAL_SERVICE_VERSION_V1||
       contribution->structSize<sizeof(*contribution)||!contribution->BuildBatch)return 0;
    visualContribution_=*contribution;return 1;
}
SceneData PluginHost::ConfigureVisualPackage(const LabVisualPackageV1& package) {
    if(package.version!=LABOVR_VISUAL_PACKAGE_VERSION_V1||package.structSize<sizeof(package))Fail("Invalid visual package ABI");
    if(package.textureCount>ConsoleSpec::MaxTextures||package.meshCount>ConsoleSpec::MaxMeshes||
       package.materialCount>ConsoleSpec::MaxMaterials||package.objectCount>ConsoleSpec::MaxObjects||
       package.lightCount>ConsoleSpec::MaxLights||package.streamCount>ConsoleSpec::MaxVisualStreams||
       package.effectCount>ConsoleSpec::MaxVisualEffects||package.captureCount>ConsoleSpec::MaxVisualCaptures)
        Fail("Visual package exceeds console capacity");
    if((package.textureCount&&!package.textures)||(package.meshCount&&!package.meshes)||
       (package.materialCount&&!package.materials)||(package.objectCount&&!package.objects)||
       (package.lightCount&&!package.lights)||(package.streamCount&&!package.streams)||
       (package.effectCount&&!package.effects)||(package.captureCount&&!package.captures))Fail("Missing visual package arrays");
    if(!graphics_||!visualDomain_)Fail("Visual renderer not initialized");
    visualPackage_=&package;
    auto scene=visualDomain_->Configure(activePackageRoot_,worldGeneration_,package);
    if(!visualAttached_){graphics_->RegisterSharedDomain(visualDomain_);visualAttached_=true;}
    return scene;
}
bool PluginHost::BuildVisualBatch(uint64_t frameId,double timeSeconds,LabVisualBatchV1& out) const {
    out={};out.version=LABOVR_VISUAL_BATCH_VERSION_V1;out.structSize=sizeof(out);
    if(!visualContribution_.BuildBatch)return false;
    LabWorldFrameV1 frame{LABOVR_WORLD_API_VERSION_V1,sizeof(frame),frameId,timeSeconds};
    if(!visualContribution_.BuildBatch(visualContribution_.callbackContext,&frame,&out))return false;
    return out.version==LABOVR_VISUAL_BATCH_VERSION_V1&&out.structSize>=sizeof(out)&&
        out.objectStateCount<=ConsoleSpec::MaxObjects&&out.streamCount<=ConsoleSpec::MaxVisualStreams&&
        out.effectCount<=ConsoleSpec::MaxVisualEffects&&out.captureCount<=ConsoleSpec::MaxVisualCaptures&&
        out.lightModifierCount<=ConsoleSpec::MaxLights&&
        (!out.objectStateCount||out.objectStates)&&(!out.streamCount||out.streams)&&
        (!out.effectCount||out.effects)&&(!out.captureCount||out.captures)&&(!out.lightModifierCount||out.lightModifiers);
}
void PluginHost::ClearVisualPackage() noexcept {
    visualContribution_={};visualPackage_=nullptr;
    if(visualAttached_){if(!graphics_||!graphics_->UnregisterDomain(VisualDomainId))Fail("Visual domain detach failed");visualAttached_=false;}
    if(visualDomain_)visualDomain_->Clear();
}
void PluginHost::BorrowVisualBatch(const LabVisualBatchV1& batch,uint64_t frameId,const Pose& camera){if(visualDomain_)visualDomain_->BorrowBatch(batch,frameId,camera);}
void PluginHost::AppendVisualStandard(RenderScene& scene,uint32_t meshBase,uint32_t materialBase)const{if(visualDomain_)visualDomain_->AppendStandard(scene,meshBase,materialBase);}
}

namespace lab {
GpuPublication PluginHost::AcquireGpuPublication(){GpuPublication out;auto* provider=GpuProvider();if(!provider)return out;LabGpuPublicationV1 publication{};publication.gpuAbiVersion=LABOVR_GPU_ABI_V1;publication.structSize=sizeof(publication);if(!provider->Acquire(provider->callbackContext,&publication)||publication.gpuAbiVersion!=LABOVR_GPU_ABI_V1||publication.structSize<sizeof(LabGpuPublicationV1))return out;out.sourceOwner=reinterpret_cast<uintptr_t>(this);out.domain=publication.domainId;out.generation=publication.sequence;out.producerFence=ResolveInteropFence(publication.producerFence);out.producerValue=publication.producerValue;out.consumerFence=ResolveInteropFence(publication.consumerFence);out.consumerValue=publication.consumerValue;out.token=publication.token;out.tokenGeneration=publication.tokenGeneration;out.resources.reserve(publication.resourceCount);for(LabU32 i=0;i<publication.resourceCount;i++){const auto& in=publication.resources[i];auto resource=ResolveInteropResource(in.resourceHandle);if(!resource)continue;GpuPublishedResource r;r.binding=in.binding;r.flags=in.flags;r.type=in.kind==LABOVR_GPU_RESOURCE_TEXTURE2D_V1?GpuPublishedResourceType::Texture2D:(in.kind==LABOVR_GPU_RESOURCE_TEXTURE3D_V1?GpuPublishedResourceType::Texture3D:GpuPublishedResourceType::Buffer);r.resource=std::move(resource);r.offset=in.offset;r.bytes=in.bytes;r.initialLayout=static_cast<D3D12_BARRIER_LAYOUT>(in.initialLayout);r.generation=in.generation;out.resources.push_back(std::move(r));}return out;}
void PluginHost::ReleaseGpuPublication(const GpuPublication& publication){auto* provider=GpuProvider();if(provider&&provider->Release&&publication.token)provider->Release(provider->callbackContext,publication.token,publication.tokenGeneration);}
}
