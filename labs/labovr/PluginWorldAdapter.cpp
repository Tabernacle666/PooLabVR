#include "PluginWorldAdapter.h"
#include "PluginHost.h"
#include "LaboVRVisualService.h"
#include "LaboVRServices.h"
#include "LaboVRGpuService.h"
#include "Graphics.h"
#include <d3d12.h>
#include <cstddef>
#include <string>
namespace lab {
namespace {
bool HasWorldApiPrefix(const LabWorldApiV1* api,size_t end)noexcept{return api&&api->version==LABOVR_WORLD_API_VERSION_V1&&api->structSize>=end;}
std::string Utf8(const std::wstring& value){if(value.empty())return {};int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);if(bytes<=0)Fail("Cannot encode plugin scene path as UTF-8");std::string out(size_t(bytes),'\0');if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),out.data(),bytes,nullptr,nullptr)!=bytes)Fail("Cannot encode plugin scene path as UTF-8");return out;}
Pose ToPose(const LabPoseV1& p){return {{p.position.x,p.position.y,p.position.z},{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w}};}
}
PluginWorldAdapter::PluginWorldAdapter(const LabPluginApiV1& plugin,const LabHostApiV1& host,std::wstring packageRoot,std::wstring sceneOverride,std::vector<std::string> requiredInputActions):plugin_(&plugin),host_(&host),packageRoot_(std::move(packageRoot)),sceneOverride_(std::move(sceneOverride)),requiredInputActions_(std::move(requiredInputActions)){}
const LabWorldApiV1* PluginWorldAdapter::WorldApi()const noexcept{return plugin_&&plugin_->structSize>=offsetof(LabPluginApiV1,worldApi)+sizeof(plugin_->worldApi)?plugin_->worldApi:nullptr;}
void PluginWorldAdapter::Load(WorldServices& services){
    if(instance_)Fail("Plugin world already loaded");
    if(!plugin_||plugin_->abiVersion!=LABOVR_PLUGIN_ABI_V1||plugin_->structSize<offsetof(LabPluginApiV1,worldApi)+sizeof(plugin_->worldApi)||!plugin_->CreateWorld||!plugin_->DestroyWorld||!plugin_->worldApi)Fail("Invalid plugin API V1");
    if(!host_||host_->abiVersion!=LABOVR_PLUGIN_ABI_V1||host_->structSize<sizeof(LabHostApiV1)||!host_->QueryService)Fail("Invalid plugin host API V1");
    hostImpl_=&services.Require<PluginHost>();graphics_=services.Find<Graphics>();inputSystem_=&services.Require<InputSystem>();
    constexpr size_t createExEnd=offsetof(LabPluginApiV1,CreateWorldEx)+sizeof(plugin_->CreateWorldEx);
    if(plugin_->structSize>=createExEnd&&plugin_->CreateWorldEx){std::string sceneUtf8=Utf8(sceneOverride_),packageRootUtf8=Utf8(packageRoot_);LabWorldCreateInfoV1 create{1u,sizeof(LabWorldCreateInfoV1),sceneUtf8.empty()?nullptr:sceneUtf8.c_str(),packageRootUtf8.empty()?nullptr:packageRootUtf8.c_str()};instance_=plugin_->CreateWorldEx(host_,&create);}
    else instance_=plugin_->CreateWorld(host_);
    if(!instance_)Fail("Plugin world creation failed");
    auto* api=WorldApi();const LabVisualPackageV1* visual=nullptr;
    if(!HasWorldApiPrefix(api,offsetof(LabWorldApiV1,DeclareVisual)+sizeof(api->DeclareVisual))||!api->DeclareVisual||!api->DeclareVisual(instance_,&visual)||!visual)Fail("Plugin visual declaration failed");
    scene_=hostImpl_->ConfigureVisualPackage(*visual);lightModifiers_.reserve(ConsoleSpec::MaxLights);
}
void PluginWorldAdapter::Activate(uint32_t meshBase,uint32_t materialBase,uint32_t lightBase){if(!instance_||active_)Fail("Plugin world activation state");if(!inputSystem_)Fail("Plugin input registry unavailable");for(const auto& name:requiredInputActions_)if(!inputSystem_->FindAction(name)){std::string error="Plugin world requires missing input action: "+name;Fail(error.c_str());}auto* api=WorldApi();if(!HasWorldApiPrefix(api,offsetof(LabWorldApiV1,Activate)+sizeof(api->Activate))||!api->Activate)Fail("Plugin world activation API missing");LabWorldActivationV1 activation{LABOVR_WORLD_API_VERSION_V1,sizeof(LabWorldActivationV1),meshBase,materialBase,lightBase,0};if(!api->Activate(instance_,&activation))Fail("Plugin world activation failed");meshBase_=meshBase;materialBase_=materialBase;streamMaterials_.clear();if(auto* provider=hostImpl_->GpuProvider()){constexpr size_t materialEnd=offsetof(LabGpuPublicationProviderV1,streamMaterialCount)+sizeof(provider->streamMaterialCount);if(provider->structSize>=materialEnd&&provider->streamMaterials)for(uint32_t i=0;i<provider->streamMaterialCount;++i){uint32_t material=provider->streamMaterials[i];if(provider->reserved&LABOVR_GPU_PROVIDER_REBASED_MATERIALS_V1){if(material<materialBase||uint64_t(material)>=uint64_t(materialBase)+scene_.materials.size())Fail("GPU publication rebased material ID");streamMaterials_.push_back(material);}else{if(material>=scene_.materials.size())Fail("GPU publication material ID");streamMaterials_.push_back(materialBase+material);}}}if(graphics_&&hostImpl_->GpuProvider()){graphics_->AttachStream(*this);gpuAttached_=true;}active_=true;}
void PluginWorldAdapter::Deactivate()noexcept{if(!instance_||!active_)return;if(graphics_)graphics_->WaitIdle();if(gpuAttached_&&graphics_){graphics_->DetachStream(*this);gpuAttached_=false;}auto* api=WorldApi();if(HasWorldApiPrefix(api,offsetof(LabWorldApiV1,Deactivate)+sizeof(api->Deactivate))&&api->Deactivate)api->Deactivate(instance_);if(hostImpl_){hostImpl_->ClearVisualPackage();}active_=false;streamMaterials_.clear();lightModifiers_.clear();attractiveLights_.clear();frameBatch_={};batchFrame_=UINT64_MAX;materialBase_=meshBase_=0;}
void PluginWorldAdapter::Frame(uint64_t frameId,double timeSeconds)noexcept{
    if(!instance_||!active_)return;
    auto* api=WorldApi();
    if(HasWorldApiPrefix(api,offsetof(LabWorldApiV1,Frame)+sizeof(api->Frame))&&api->Frame){LabWorldFrameV1 frame{LABOVR_WORLD_API_VERSION_V1,sizeof(LabWorldFrameV1),frameId,timeSeconds};api->Frame(instance_,&frame);}
    UpdateBatch(frameId,timeSeconds);
}
void PluginWorldAdapter::UpdateBatch(uint64_t frameId,double timeSeconds){
    // The public batch remains borrowed until the next cartridge Frame(). Build
    // once, so both Home phototaxis and rendering see the same current modifiers.
    frameBatch_={};batchFrame_=frameId;
    if(!hostImpl_->BuildVisualBatch(frameId,timeSeconds,frameBatch_)){frameBatch_={};frameBatch_.version=LABOVR_VISUAL_BATCH_VERSION_V1;frameBatch_.structSize=sizeof(frameBatch_);}
    lightModifiers_.clear();
    if(frameBatch_.lightModifierCount)lightModifiers_.assign(frameBatch_.lightModifiers,frameBatch_.lightModifiers+frameBatch_.lightModifierCount);
    attractiveLights_=scene_.lights;
    for(auto& light:attractiveLights_)ModifyLight(light,timeSeconds);
}
void PluginWorldAdapter::Unload(WorldServices&){if(!instance_)return;Deactivate();if(graphics_)graphics_->WaitIdle();if(hostImpl_){hostImpl_->ClearVisualPackage();}auto instance=instance_;instance_=nullptr;scene_={};hostImpl_=nullptr;graphics_=nullptr;inputSystem_=nullptr;plugin_->DestroyWorld(instance);}
const SceneData& PluginWorldAdapter::Scene()const{if(!instance_)Fail("Plugin world is not loaded");return scene_;}
void PluginWorldAdapter::AppendThunk(void* context,RenderScene& scene,const FrameInput& input,double time){static_cast<PluginWorldAdapter*>(context)->Append(scene,input,time);}
void PluginWorldAdapter::LightThunk(void* context,SceneLight& light,double time){static_cast<PluginWorldAdapter*>(context)->ModifyLight(light,time);}
void PluginWorldAdapter::Append(RenderScene& scene,const FrameInput& input,double time){
    if(!active_||!hostImpl_)return;
    if(batchFrame_!=input.frameId)UpdateBatch(input.frameId,time);
    hostImpl_->BorrowVisualBatch(frameBatch_,input.frameId,input.head);hostImpl_->AppendVisualStandard(scene,meshBase_,materialBase_);
}
void PluginWorldAdapter::ModifyLight(SceneLight& light,double)const{for(const auto& modifier:lightModifiers_)if(modifier.structSize>=sizeof(LabVisualLightModifierV1)&&(light.flags&modifier.matchFlags)==modifier.matchFlags)light.power*=modifier.powerScale;}
RenderContributions PluginWorldAdapter::Contributions()const{if(!active_)return {};RenderContributions out;out.streamMaterials=streamMaterials_;out.context=const_cast<PluginWorldAdapter*>(this);out.appendContext=AppendThunk;out.lightContext=LightThunk;return out;}
GpuWorldDomain PluginWorldAdapter::Domain()const noexcept{auto* provider=hostImpl_?hostImpl_->GpuProvider():nullptr;return provider?provider->domainId:GpuDomainId::Invalid;}
GpuPublication PluginWorldAdapter::Acquire(){return hostImpl_?hostImpl_->AcquireGpuPublication():GpuPublication{};}
void PluginWorldAdapter::Release(const GpuPublication& publication){if(hostImpl_)hostImpl_->ReleaseGpuPublication(publication);}

}
