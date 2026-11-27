#include "XR.h"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>
namespace OASIS {
void DiagnosticLine(const char*) noexcept;
namespace {
void Xr(XrResult r,const char* s){if(XR_FAILED(r)){char message[768];snprintf(message,sizeof(message),"%s (XrResult %d)",s,int(r));Fail(message);}}
Pose Convert(const XrPosef& p){return {{p.position.x,p.position.y,-p.position.z},{-p.orientation.x,-p.orientation.y,p.orientation.z,p.orientation.w}};}
XMMATRIX Projection(const XrFovf& f,float nearZ,float farZ){float l=std::tan(f.angleLeft),r=std::tan(f.angleRight),b=std::tan(f.angleDown),t=std::tan(f.angleUp);return XMMatrixSet(2/(r-l),0,0,0,0,2/(t-b),0,0,-(r+l)/(r-l),-(t+b)/(t-b),nearZ/(nearZ-farZ),1,0,0,nearZ*farZ/(farZ-nearZ),0);}
// User-measured visible limits (-35/+35 horizontal, -45/+30 vertical)
 // with three degrees of guard on each edge. The capture reference is the
 // previous per-eye OpenXR projection, used only to retain approximate
 // pixel density when allocating the smaller swapchain.
constexpr float CropHorizontal=38.f*XM_PI/180.f;
constexpr float CropDown=48.f*XM_PI/180.f;
constexpr float CropUp=33.f*XM_PI/180.f;
XrFovf CroppedFov(XrFovf f) noexcept {
    f.angleLeft=std::max(f.angleLeft,-CropHorizontal);
    f.angleRight=std::min(f.angleRight,CropHorizontal);
    f.angleDown=std::max(f.angleDown,-CropDown);
    f.angleUp=std::min(f.angleUp,CropUp);
    return f;
}
uint32_t CroppedExtent(uint32_t full,float fraction) noexcept {
    const auto extent=uint32_t(std::ceil(float(full)*fraction/16.f))*16u;
    return std::min(full,std::max(16u,extent));
}
bool Valid(XrSpaceLocationFlags flags){return (flags&(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))==(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);}
}
XrPath XR::Path(const char* p){XrPath r;Xr(xrStringToPath(instance_,p,&r),p);return r;}
std::string XR::PathString(XrPath path)const{if(path==XR_NULL_PATH)return {};uint32_t size=0;if(XR_FAILED(xrPathToString(instance_,path,0,&size,nullptr))||!size)return {};std::string out(size,'\0');if(XR_FAILED(xrPathToString(instance_,path,size,&size,out.data())))return {};if(!out.empty()&&out.back()=='\0')out.pop_back();return out;}
LARGE_INTEGER XR::ToQpc(XrTime time)const{LARGE_INTEGER qpc{};Xr(timeToQpc_(instance_,time,&qpc),"xrConvertTimeToWin32PerformanceCounterKHR");return qpc;}
void XR::InitializeInstance(XRIdentity identity,InputSystem& inputSystem,TrackingMapping mapping){identity_=identity;inputSystem_=&inputSystem;mapping_=mapping;
    uint32_t n=0;Xr(xrEnumerateInstanceExtensionProperties(nullptr,0,&n,nullptr),"OpenXR extensions");std::vector<XrExtensionProperties> props(n,{XR_TYPE_EXTENSION_PROPERTIES});Xr(xrEnumerateInstanceExtensionProperties(nullptr,n,&n,props.data()),"OpenXR extensions");auto has=[&](const char* s){return std::any_of(props.begin(),props.end(),[&](auto& p){return !std::strcmp(s,p.extensionName);});};
    const char* required[]={XR_KHR_D3D12_ENABLE_EXTENSION_NAME,XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME,XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};for(auto* e:required)if(!has(e))Fail("OpenXR D3D12 + composition depth + Win32 time conversion required");std::vector<const char*> extensions(std::begin(required),std::end(required));if(has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
#ifdef XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME
    touchPlusExtension_=has(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);if(touchPlusExtension_)extensions.push_back(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
#endif
    XrInstanceCreateInfo c{XR_TYPE_INSTANCE_CREATE_INFO};strcpy_s(c.applicationInfo.applicationName,identity_.application);strcpy_s(c.applicationInfo.engineName,identity_.engine);c.applicationInfo.applicationVersion=identity_.version;c.applicationInfo.engineVersion=identity_.version;c.applicationInfo.apiVersion=XR_MAKE_VERSION(1,1,0);c.enabledExtensionCount=uint32_t(extensions.size());c.enabledExtensionNames=extensions.data();Xr(xrCreateInstance(&c,&instance_),"xrCreateInstance 1.1");
    Xr(xrGetInstanceProcAddr(instance_,"xrConvertTimeToWin32PerformanceCounterKHR",reinterpret_cast<PFN_xrVoidFunction*>(&timeToQpc_)),"OpenXR time to QPC");if(!timeToQpc_)Fail("OpenXR Win32 time conversion function required");XrSystemGetInfo s{XR_TYPE_SYSTEM_GET_INFO};s.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;Xr(xrGetSystem(instance_,&s,&system_),"xrGetSystem");
    uint32_t configurationCount=0;Xr(xrEnumerateViewConfigurations(instance_,system_,0,&configurationCount,nullptr),"view configurations");std::vector<XrViewConfigurationType> configurations(configurationCount);Xr(xrEnumerateViewConfigurations(instance_,system_,configurationCount,&configurationCount,configurations.data()),"view configurations");if(std::find(configurations.begin(),configurations.end(),XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)!=configurations.end())viewConfigurationType_=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;else if(std::find(configurations.begin(),configurations.end(),XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET)!=configurations.end())viewConfigurationType_=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET;else if(std::find(configurations.begin(),configurations.end(),XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO)!=configurations.end())viewConfigurationType_=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;else Fail("No supported primary OpenXR view configuration");
    PFN_xrGetD3D12GraphicsRequirementsKHR requirements;Xr(xrGetInstanceProcAddr(instance_,"xrGetD3D12GraphicsRequirementsKHR",reinterpret_cast<PFN_xrVoidFunction*>(&requirements)),"D3D12 requirements");Xr(requirements(instance_,system_,&requirements_),"D3D12 requirements");if(has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)){
        xrGetInstanceProcAddr(instance_,"xrRequestDisplayRefreshRateFB",reinterpret_cast<PFN_xrVoidFunction*>(&refresh_));
        xrGetInstanceProcAddr(instance_,"xrGetDisplayRefreshRateFB",reinterpret_cast<PFN_xrVoidFunction*>(&getRefresh_));
    }XMStoreFloat4x4(&trackingToWorld_,XMMatrixIdentity());
}
void XR::InitializeSession(ID3D12Device* d,ID3D12CommandQueue* q){if(!inputSystem_)Fail("InputSystem is required before OpenXR session initialization");if(!inputSystem_->FreezePhysicalControls())Fail("Unable to freeze physical input registry before OpenXR actions");XrGraphicsBindingD3D12KHR b{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};b.device=d;b.queue=q;XrSessionCreateInfo c{XR_TYPE_SESSION_CREATE_INFO};c.next=&b;c.systemId=system_;Xr(xrCreateSession(instance_,&c,&session_),"xrCreateSession");uint32_t n=0;Xr(xrEnumerateReferenceSpaces(session_,0,&n,nullptr),"reference spaces");std::vector<XrReferenceSpaceType> types(n);Xr(xrEnumerateReferenceSpaces(session_,n,&n,types.data()),"reference spaces");stage_=std::find(types.begin(),types.end(),XR_REFERENCE_SPACE_TYPE_STAGE)!=types.end();XrReferenceSpaceCreateInfo s{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};s.referenceSpaceType=stage_?XR_REFERENCE_SPACE_TYPE_STAGE:XR_REFERENCE_SPACE_TYPE_LOCAL;s.poseInReferenceSpace.orientation.w=1;Xr(xrCreateReferenceSpace(session_,&s,&space_),"tracking space");s.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;Xr(xrCreateReferenceSpace(session_,&s,&viewSpace_),"view space");hands_={Path("/user/hand/left"),Path("/user/hand/right")};Actions();Swapchains();}
XrAction XR::Action(const char* name,XrActionType type){XrAction a{};XrActionCreateInfo c{XR_TYPE_ACTION_CREATE_INFO};strcpy_s(c.actionName,name);strcpy_s(c.localizedActionName,name);c.actionType=type;c.countSubactionPaths=HandCount;c.subactionPaths=hands_.data();Xr(xrCreateAction(physicalActionsSet_,&c,&a),name);return a;}
void XR::SuggestProfile(const char* profile,uint32_t kind){if(!inputSystem_)return;std::vector<XrActionSuggestedBinding> bindings;std::array<std::vector<PhysicalControlId>,HandCount> available;auto add=[&](const char* control,uint32_t hand,const char* suffix){auto id=inputSystem_->FindPhysicalControl(control);if(!id)return;std::string path=hand?"/user/hand/right":"/user/hand/left";path+=suffix;bindings.push_back({physicalActions_[id.value],Path(path.c_str())});available[hand].push_back(id);};auto both=[&](const char* control,const char* suffix){add(control,0,suffix);add(control,1,suffix);};
    if(kind==0){both("thumbstick","/input/thumbstick");both("squeeze","/input/squeeze/value");both("trigger_value","/input/trigger/value");both("trigger_force","/input/trigger/force");both("trigger_touch","/input/trigger/touch");both("trigger_curl","/input/trigger_curl/value");both("trigger_slide","/input/trigger_slide/value");both("trigger_proximity","/input/trigger/proximity");both("thumbstick_click","/input/thumbstick/click");both("thumbstick_touch","/input/thumbstick/touch");both("thumbrest_touch","/input/thumbrest/touch");both("thumb_proximity","/input/thumb_resting_surfaces/proximity");}
    else if(kind==1){both("thumbstick","/input/thumbstick");both("squeeze","/input/squeeze/value");both("trigger_value","/input/trigger/value");both("trigger_force","/input/trigger/force");both("trigger_touch","/input/trigger/touch");both("trigger_curl","/input/trigger/curl_meta");both("trigger_slide","/input/trigger/slide_meta");both("trigger_proximity","/input/trigger/proximity_meta");both("thumbstick_click","/input/thumbstick/click");both("thumbstick_touch","/input/thumbstick/touch");both("thumbrest_touch","/input/thumbrest/touch");both("thumb_proximity","/input/thumb_meta/proximity_meta");}
    else {both("thumbstick","/input/thumbstick");both("squeeze","/input/squeeze/value");both("trigger_value","/input/trigger/value");both("trigger_touch","/input/trigger/touch");both("trigger_proximity","/input/trigger/proximity");both("thumbstick_click","/input/thumbstick/click");both("thumbstick_touch","/input/thumbstick/touch");both("thumbrest_touch","/input/thumbrest/touch");both("thumb_proximity","/input/thumb_resting_surfaces/proximity");}
    add("primary_click",0,"/input/x/click");add("primary_click",1,"/input/a/click");add("primary_touch",0,"/input/x/touch");add("primary_touch",1,"/input/a/touch");add("secondary_click",0,"/input/y/click");add("secondary_click",1,"/input/b/click");add("secondary_touch",0,"/input/y/touch");add("secondary_touch",1,"/input/b/touch");add("menu_click",0,"/input/menu/click");both("grip_pose","/input/grip/pose");both("aim_pose","/input/aim/pose");both("haptic","/output/haptic");XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};suggested.interactionProfile=Path(profile);suggested.countSuggestedBindings=uint32_t(bindings.size());suggested.suggestedBindings=bindings.data();XrResult result=xrSuggestInteractionProfileBindings(instance_,&suggested);if(result==XR_ERROR_PATH_UNSUPPORTED){return;}Xr(result,"xrSuggestInteractionProfileBindings");suggestedProfiles_.push_back({profile,std::move(available)});}
void XR::Actions(){if(!inputSystem_)Fail("InputSystem is required before OpenXR actions");if(!inputSystem_->PhysicalControlsFrozen())Fail("Physical input registry must be frozen before OpenXR action creation");const uint32_t expectedPhysicalControls=inputSystem_->PhysicalCount();if(expectedPhysicalControls>physicalActions_.size())Fail("Physical input registry exceeds OpenXR action capacity");XrActionSetCreateInfo c{XR_TYPE_ACTION_SET_CREATE_INFO};strcpy_s(c.actionSetName,"physical");strcpy_s(c.localizedActionSetName,"Physical");Xr(xrCreateActionSet(instance_,&c,&physicalActionsSet_),"physical action set");auto xrType=[](InputActionType type){switch(type){case InputActionType::Boolean:return XR_ACTION_TYPE_BOOLEAN_INPUT;case InputActionType::Float:return XR_ACTION_TYPE_FLOAT_INPUT;case InputActionType::Vector2:return XR_ACTION_TYPE_VECTOR2F_INPUT;case InputActionType::Pose:return XR_ACTION_TYPE_POSE_INPUT;case InputActionType::Haptic:return XR_ACTION_TYPE_VIBRATION_OUTPUT;}Fail("Invalid physical input type");};uint32_t createdPhysicalControls=0;for(const auto& info:inputSystem_->PhysicalControls()){if(info.id.value!=createdPhysicalControls)Fail("Physical input registry must be contiguous before OpenXR action creation");physicalActions_[info.id.value]=Action(info.name.c_str(),xrType(info.type));++createdPhysicalControls;}suggestedProfiles_.clear();SuggestProfile("/interaction_profiles/meta/touch_plus_controller",0);if(touchPlusExtension_)SuggestProfile("/interaction_profiles/meta/touch_controller_plus",1);SuggestProfile("/interaction_profiles/oculus/touch_controller",2);if(createdPhysicalControls!=expectedPhysicalControls||inputSystem_->PhysicalCount()!=expectedPhysicalControls)Fail("Physical input registry changed while building OpenXR action catalog");XrSessionActionSetsAttachInfo attached{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};attached.countActionSets=1;attached.actionSets=&physicalActionsSet_;Xr(xrAttachSessionActionSets(session_,&attached),"attach physical action set");for(const auto& info:inputSystem_->PhysicalControls())if(info.type==InputActionType::Pose)for(uint32_t hand=0;hand<HandCount;hand++){XrActionSpaceCreateInfo pose{XR_TYPE_ACTION_SPACE_CREATE_INFO};pose.subactionPath=hands_[hand];pose.poseInActionSpace.orientation.w=1;pose.action=physicalActions_[info.id.value];Xr(xrCreateActionSpace(session_,&pose,&poseSpaces_[info.id.value][hand]),"physical pose action space");}}
void XR::UpdateInteractionProfiles(){if(!inputSystem_||!session_)return;for(uint32_t hand=0;hand<HandCount;hand++){XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};XrResult result=xrGetCurrentInteractionProfile(session_,hands_[hand],&state);if(XR_FAILED(result)){if(result==XR_ERROR_SESSION_NOT_RUNNING)continue;Xr(result,"xrGetCurrentInteractionProfile");}std::string profile=PathString(state.interactionProfile);std::vector<PhysicalControlId> available;for(const auto& p:suggestedProfiles_)if(p.path==profile){available=p.available[hand];break;}
    inputSystem_->SetInteractionProfile(hand,profile,available);}}
void XR::Swapchains(){uint32_t n=0;Xr(xrEnumerateViewConfigurationViews(instance_,system_,viewConfigurationType_,0,&n,nullptr),"view topology");if(!n||n>MaxViews)Fail("OpenXR view topology exceeds MaxViews");viewCount_=n;std::vector<XrViewConfigurationView> v(viewCount_);for(auto& view:v)view={XR_TYPE_VIEW_CONFIGURATION_VIEW};Xr(xrEnumerateViewConfigurationViews(instance_,system_,viewConfigurationType_,viewCount_,&n,v.data()),"view topology");if(n!=viewCount_)Fail("OpenXR view topology changed during initialization");uint32_t recommendedWidth=0,recommendedHeight=0,maxCommonWidth=UINT32_MAX,maxCommonHeight=UINT32_MAX;for(const auto& view:v){recommendedWidth=std::max(recommendedWidth,view.recommendedImageRectWidth);recommendedHeight=std::max(recommendedHeight,view.recommendedImageRectHeight);maxCommonWidth=std::min(maxCommonWidth,view.maxImageRectWidth);maxCommonHeight=std::min(maxCommonHeight,view.maxImageRectHeight);}width_=std::min(recommendedWidth,maxCommonWidth);height_=std::min(recommendedHeight,maxCommonHeight);if(!width_||!height_)Fail("OpenXR view topology has no common render extent");
    // Allocate the cropped image directly, before any renderer resources.
    submitWidth_=width_;submitHeight_=height_;
    width_=CroppedExtent(width_,2.f*std::tan(CropHorizontal)/
        (std::tan(.91241771f)+std::tan(.79412581f)));
    height_=CroppedExtent(height_,(std::tan(CropDown)+std::tan(CropUp))/
        (std::tan(.87909295f)+std::tan(.84459636f)));
    Xr(xrEnumerateSwapchainFormats(session_,0,&n,nullptr),"formats");std::vector<int64_t> f(n);Xr(xrEnumerateSwapchainFormats(session_,n,&n,f.data()),"formats");auto has=[&](DXGI_FORMAT a){return std::find(f.begin(),f.end(),int64_t(a))!=f.end();};format_=has(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;if(!has(format_)||!has(DXGI_FORMAT_D32_FLOAT))Fail("sRGB + D32_FLOAT swapchains required");auto create=[&](XrSwapchain& sc,std::vector<XrSwapchainImageD3D12KHR>& out,DXGI_FORMAT format,XrSwapchainUsageFlags usage){XrSwapchainCreateInfo c{XR_TYPE_SWAPCHAIN_CREATE_INFO};c.usageFlags=usage;c.format=format;c.sampleCount=1;c.width=submitWidth_;c.height=submitHeight_;c.faceCount=1;c.arraySize=viewCount_;c.mipCount=1;Xr(xrCreateSwapchain(session_,&c,&sc),"swapchain array");uint32_t count=0;Xr(xrEnumerateSwapchainImages(sc,0,&count,nullptr),"swapchain images");out.resize(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});Xr(xrEnumerateSwapchainImages(sc,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(out.data())),"swapchain images");};create(color_,colors_,format_,XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT);create(depth_,depths_,DXGI_FORMAT_D32_FLOAT,XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);}
void XR::PollEvents(){XrEventDataBuffer e{XR_TYPE_EVENT_DATA_BUFFER};while(xrPollEvent(instance_,&e)==XR_SUCCESS){if(e.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){state_=reinterpret_cast<XrEventDataSessionStateChanged*>(&e)->state;if(state_==XR_SESSION_STATE_READY){lastPresented_.valid=false;missedStreak_=0;XrSessionBeginInfo b{XR_TYPE_SESSION_BEGIN_INFO};b.primaryViewConfigurationType=viewConfigurationType_;Xr(xrBeginSession(session_,&b),"begin session");running_=true;UpdateInteractionProfiles();if(refresh_)refresh_(session_,RequestedHeadsetHz);
        headsetTargetHz_=RequestedHeadsetHz;
        float activeHz{};
        if(getRefresh_&&XR_SUCCEEDED(getRefresh_(session_,&activeHz))&&std::isfinite(activeHz)&&activeHz>0.f)
            headsetTargetHz_=activeHz;
    }else if(state_==XR_SESSION_STATE_STOPPING){ReleaseHeldSwapchains();lastPresented_.valid=false;Xr(xrEndSession(session_),"end session");running_=false;for(uint32_t h=0;h<HandCount;h++)inputSystem_->SetInteractionProfile(h,{},{});if(mapping_.event)mapping_.event(mapping_.context,TrackingEvent::SessionStopped);}else if(state_==XR_SESSION_STATE_EXITING||state_==XR_SESSION_STATE_LOSS_PENDING)exit_=true;}else if(e.type==XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB){
        const auto activeHz=reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&e)->toDisplayRefreshRate;
        if(std::isfinite(activeHz)&&activeHz>0.f)headsetTargetHz_=activeHz;
    }else if(e.type==XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)UpdateInteractionProfiles();else if(e.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)exit_=true;else if(e.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING){lastPresented_.valid=false;if(mapping_.event)mapping_.event(mapping_.context,TrackingEvent::ReferenceSpaceChanged);}e={XR_TYPE_EVENT_DATA_BUFFER};}}
void XR::RequestExit(){if(running_)Xr(xrRequestExitSession(session_),"request session exit");}
void XR::LocateInput(XRFrame& f,bool pollControllers){
    PhysicalFrame physical{};physical.frameId=f.input.frameId;physical.targetDisplayTime=f.input.targetDisplayTime;physical.targetDisplayQpc=f.input.targetDisplayQpc;physical.time=f.input.time;physical.focused=state_==XR_SESSION_STATE_FOCUSED;physical.controlCount=inputSystem_->PhysicalCount();
    if(pollControllers){
        for(const auto& info:inputSystem_->PhysicalControls()){
            if(info.type==InputActionType::Pose||info.type==InputActionType::Haptic)continue;
            for(uint32_t hand=0;hand<HandCount;hand++){
                XrActionStateGetInfo g{XR_TYPE_ACTION_STATE_GET_INFO};g.action=physicalActions_[info.id.value];g.subactionPath=hands_[hand];auto& out=physical.State(info.id);
                if(info.type==InputActionType::Boolean){XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};Xr(xrGetActionStateBoolean(session_,&g,&state),"physical boolean action");out.active[hand]=physical.focused&&state.isActive;out.down[hand]=out.active[hand]&&state.currentState;}
                else if(info.type==InputActionType::Float){XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};Xr(xrGetActionStateFloat(session_,&g,&state),"physical float action");out.active[hand]=physical.focused&&state.isActive;out.scalar[hand]=out.active[hand]?state.currentState:0.f;}
                else if(info.type==InputActionType::Vector2){XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};Xr(xrGetActionStateVector2f(session_,&g,&state),"physical vector action");out.active[hand]=physical.focused&&state.isActive;out.vector[hand]=out.active[hand]?XMFLOAT2{state.currentState.x,state.currentState.y}:XMFLOAT2{};}
                inputSystem_->ObservePhysicalActivity(info.id,hand,out.active[hand]);
            }
        }
        inputSystem_->ResolveFrame(physical,f.input);
    }else{
        f.input.focused=physical.focused;
    }
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};Xr(xrLocateSpace(viewSpace_,space_,f.displayTime,&head),"head pose");bool headValid=Valid(head.locationFlags);f.headValid=headValid;Pose raw;if(headValid)raw=Convert(head.pose);
    if(pollControllers){
        for(const auto& info:inputSystem_->PhysicalControls())if(info.type==InputActionType::Pose)for(uint32_t hand=0;hand<HandCount;hand++){
            XrActionStateGetInfo g{XR_TYPE_ACTION_STATE_GET_INFO};g.action=physicalActions_[info.id.value];g.subactionPath=hands_[hand];XrActionStatePose active{XR_TYPE_ACTION_STATE_POSE};Xr(xrGetActionStatePose(session_,&g,&active),"physical pose action");
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};location.next=&velocity;Xr(xrLocateSpace(poseSpaces_[info.id.value][hand],space_,f.displayTime,&location),"physical action pose");
            auto& out=physical.State(info.id);auto& pose=out.pose[hand];pose.tracked=physical.focused&&active.isActive&&Valid(location.locationFlags);out.active[hand]=pose.tracked;inputSystem_->ObservePhysicalActivity(info.id,hand,active.isActive);if(!pose.tracked)continue;
            pose.pose=Convert(location.pose);
            pose.velocity=(velocity.velocityFlags&XR_SPACE_VELOCITY_LINEAR_VALID_BIT)?XMFLOAT3{velocity.linearVelocity.x,velocity.linearVelocity.y,-velocity.linearVelocity.z}:XMFLOAT3{};
            pose.angularVelocity=(velocity.velocityFlags&XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)?XMFLOAT3{-velocity.angularVelocity.x,-velocity.angularVelocity.y,velocity.angularVelocity.z}:XMFLOAT3{};
        }
        inputSystem_->ResolvePoses(physical,f.input);
    }
    if(mapping_.frame&&mapping_.frame(mapping_.context,f.input,headValid?&raw:nullptr,stage_,trackingToWorld_))xrRequestExitSession(session_);
    else if(!mapping_.frame&&headValid)f.input.head=FromMatrix(Matrix(raw)*XMLoadFloat4x4(&trackingToWorld_));
    if(!pollControllers)return;
    auto tracking=XMLoadFloat4x4(&trackingToWorld_);
    for(const auto& info:inputSystem_->PhysicalControls())if(info.type==InputActionType::Pose)for(auto& pose:physical.State(info.id).pose)if(pose.tracked){
        pose.pose=FromMatrix(Matrix(pose.pose)*tracking);
        XMStoreFloat3(&pose.velocity,XMVector3TransformNormal(XMLoadFloat3(&pose.velocity),tracking));
        const auto angular=XMVector3TransformNormal(XMLoadFloat3(&pose.angularVelocity),tracking);
        XMStoreFloat3(&pose.angularVelocity,XMVectorScale(angular,1.f/f.input.sceneUnitsPerMeter));
    }
    inputSystem_->ResolvePoses(physical,f.input);
}
bool XR::BeginFrame(XRFrame& f,double* waitMilliseconds,bool pollControllers){f={};if(!running_)return false;XrFrameWaitInfo w{XR_TYPE_FRAME_WAIT_INFO};XrFrameState s{XR_TYPE_FRAME_STATE};{
        const int64_t start=waitMilliseconds?QpcCounter():0;
        XrResult result=xrWaitFrame(session_,&w,&s);
        if(waitMilliseconds)*waitMilliseconds=1000.0*QpcSeconds(QpcCounter()-start);
        Xr(result,"xrWaitFrame");
    }XrFrameBeginInfo b{XR_TYPE_FRAME_BEGIN_INFO};{
        XrResult result=xrBeginFrame(session_,&b);
        Xr(result,"xrBeginFrame");
    }f.begun=true;f.viewCount=viewCount_;f.input.actionCount=inputSystem_->ActionCount();f.input.frameId=++frameSerial_;f.displayTime=s.predictedDisplayTime;f.headsetTargetHz=headsetTargetHz_;f.displayPeriod=s.predictedDisplayPeriod>0?s.predictedDisplayPeriod:13888889;f.render=s.shouldRender;LARGE_INTEGER targetQpc=ToQpc(f.displayTime);f.input.targetDisplayTime=static_cast<int64_t>(f.displayTime);f.input.targetDisplayQpc=targetQpc.QuadPart;f.input.time=QpcSeconds(targetQpc.QuadPart);
    if(pollControllers){
        XrActiveActionSet a{physicalActionsSet_,XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};sync.countActiveActionSets=1;sync.activeActionSets=&a;
        const XrResult synced=xrSyncActions(session_,&sync);
        if(XR_FAILED(synced)&&synced!=XR_SESSION_NOT_FOCUSED)Xr(synced,"xrSyncActions");
    }
    LocateInput(f,pollControllers);
    return true;
}
bool XR::AcquireSwapchains(XRFrame& f){
    if(!f.begun||!f.render||f.ready)return f.ready;
    // A rejected render never submitted writes to these images. Reuse the
    // already-waited pair instead of acquiring a second pair.
    if(!swapchainsHeld_){
        XrSwapchainImageAcquireInfo ainfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=XR_INFINITE_DURATION;
        Xr(xrAcquireSwapchainImage(color_,&ainfo,&heldColor_),"acquire color");
        Xr(xrWaitSwapchainImage(color_,&wi),"wait color");
        Xr(xrAcquireSwapchainImage(depth_,&ainfo,&heldDepth_),"acquire depth");
        Xr(xrWaitSwapchainImage(depth_,&wi),"wait depth");
        swapchainsHeld_=true;
    }
    f.color=heldColor_;f.depth=heldDepth_;f.ready=true;
    return true;
}
void XR::ReleaseHeldSwapchains(){
    if(!swapchainsHeld_)return;
    XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    Xr(xrReleaseSwapchainImage(color_,&r),"release color");
    Xr(xrReleaseSwapchainImage(depth_,&r),"release depth");
    swapchainsHeld_=false;
}
void XR::LocateLate(XRFrame& f,const XRCaptureView* capture){XrViewLocateInfo l{XR_TYPE_VIEW_LOCATE_INFO};l.viewConfigurationType=viewConfigurationType_;l.displayTime=f.displayTime;l.space=f.headLockedDiagnostic?viewSpace_:space_;XrViewState s{XR_TYPE_VIEW_STATE};uint32_t n=0;for(auto& v:views_)v={XR_TYPE_VIEW};
    XrResult located=xrLocateViews(session_,&l,&s,viewCount_,&n,views_.data());
    Xr(located,"late xrLocateViews");f.viewCount=viewCount_;f.tracked=n==viewCount_&&(s.viewStateFlags&XR_VIEW_STATE_POSITION_VALID_BIT)&&(s.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT);if(!f.tracked)return;
    // Use the runtime's eye offsets in VIEW space on the scripted diagnostic rig.
    auto tracking=f.headLockedDiagnostic?Matrix(f.input.head):XMLoadFloat4x4(&trackingToWorld_);
    if(f.headLockedDiagnostic&&f.input.sceneUnitsPerMeter!=1.f){
        const float scale=f.input.sceneUnitsPerMeter;
        tracking=XMMatrixScaling(scale,scale,scale)*tracking;
    }
    if(capture){
        // Anchor the capture to eye zero while preserving the runtime's interocular transforms.
        tracking=XMMatrixInverse(nullptr,Matrix(Convert(views_[0].pose)))*Matrix(capture->eye);
        f.captureOnly=true;
    }
    for(uint32_t i=0;i<viewCount_;i++){
        const auto m=capture&&i==0?Matrix(capture->eye):Matrix(Convert(views_[i].pose))*tracking;
        const auto fullFov=capture&&i==0?capture->fov:views_[i].fov;
        f.fov[i]=CroppedFov(fullFov);
        // Map the cropped frustum to the original compositor's tangent plane.
        const float left=std::tan(fullFov.angleLeft),right=std::tan(fullFov.angleRight);
        const float down=std::tan(fullFov.angleDown),up=std::tan(fullFov.angleUp);
        auto xPixel=[&](float angle){
            return std::clamp(int32_t(std::lround((std::tan(angle)-left)/(right-left)*submitWidth_)),0,int32_t(submitWidth_));
        };
        auto yPixel=[&](float angle){
            return std::clamp(int32_t(std::lround((up-std::tan(angle))/(up-down)*submitHeight_)),0,int32_t(submitHeight_));
        };
        const int32_t x0=xPixel(f.fov[i].angleLeft),x1=xPixel(f.fov[i].angleRight);
        const int32_t y0=yPixel(f.fov[i].angleUp),y1=yPixel(f.fov[i].angleDown);
        f.presentRects[i]={{x0,y0},{x1-x0,y1-y0}};
        const auto pose=FromMatrix(m);
        // Tracking translations include the portal scale. The view basis is
        // rigid and the clip distances use the same scene units as the geometry.
        const auto camera=!capture&&f.input.sceneUnitsPerMeter!=1.f?Matrix(pose):m;
        const auto vp=XMMatrixInverse(nullptr,camera)*
            Projection(f.fov[i],f.input.SceneNearZ(),f.input.SceneFarZ());
        XMStoreFloat4x4(&f.vp[i],vp);
        XMStoreFloat4x4(&f.inverseVP[i],XMMatrixInverse(nullptr,vp));
        f.eyes[i]=pose.p;
    }
}
void XR::EndFrame(const XRFrame& f){
    if(!f.begun)return;
    const bool fresh=f.render&&f.ready&&f.tracked&&f.submitted&&!f.captureOnly;
    const bool live=f.render&&!f.captureOnly&&!f.headLockedDiagnostic&&f.headValid&&
        f.presentationFailure!=XRFrameFailure::Tracking;
    // Reuse only a complete, previously released stereo pair with its original
    // poses/FOV/depth range. Never reuse across a tracking/scale discontinuity.
    bool reuse=!fresh&&live&&lastPresented_.valid&&
        lastPresented_.trackingRevision==f.input.trackingRevision&&
        lastPresented_.sceneUnitsPerMeter==f.input.sceneUnitsPerMeter;
    if(swapchainsHeld_){
        // Do not ask the compositor to sample an image currently owned by the
        // app. On admission failure retain only a distinct, unwritten pair;
        // the next successful render will release it normally.
        const bool distinct=heldColor_!=lastPresented_.color&&heldDepth_!=lastPresented_.depth;
        if(f.submitted||!reuse||!distinct){
            ReleaseHeldSwapchains();
            lastPresented_.valid=false;
            reuse=false;
        }
    }
    if(!live&&!fresh)lastPresented_.valid=false;
    if(fresh){
        if(f.viewCount!=viewCount_)Fail("OpenXR frame view topology mismatch");
        lastPresented_.views=views_;
        lastPresented_.frame=f.input.frameId;
        lastPresented_.trackingRevision=f.input.trackingRevision;
        lastPresented_.farClipMeters=f.input.farClipMeters;
        lastPresented_.sceneUnitsPerMeter=f.input.sceneUnitsPerMeter;
        lastPresented_.color=f.color;lastPresented_.depth=f.depth;
        // Scripted VIEW-space and screenshot frames never seed live recovery.
        lastPresented_.valid=!f.headLockedDiagnostic&&f.headValid;
    }
    std::array<XrCompositionLayerProjectionView,MaxViews> p{};
    std::array<XrCompositionLayerDepthInfoKHR,MaxViews> d{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    if(fresh||reuse){
        const auto& source=fresh?views_:lastPresented_.views;
        const float farClip=fresh?f.input.farClipMeters:lastPresented_.farClipMeters;
        for(uint32_t i=0;i<viewCount_;++i){
            d[i]={XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
            d[i].subImage={depth_,{{0,0},{int32_t(submitWidth_),int32_t(submitHeight_)}},i};
            d[i].minDepth=0;d[i].maxDepth=1;d[i].nearZ=farClip;d[i].farZ=NearZ;
            p[i]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};p[i].next=&d[i];
            p[i].pose=source[i].pose;p[i].fov=source[i].fov;
            p[i].subImage={color_,{{0,0},{int32_t(submitWidth_),int32_t(submitHeight_)}},i};
        }
        layer.space=fresh&&f.headLockedDiagnostic?viewSpace_:space_;
        layer.viewCount=viewCount_;layer.views=p.data();
    }
    XrFrameEndInfo e{XR_TYPE_FRAME_END_INFO};
    e.displayTime=f.displayTime;e.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    e.layerCount=fresh||reuse?1u:0u;e.layers=e.layerCount?layers:nullptr;
    const auto beforeEnd=QpcCounter();
    Xr(xrEndFrame(session_,&e),"xrEndFrame");
    if(fresh){
        ++freshTotal_;
        if(missedStreak_){
            char line[320];
            std::snprintf(line,sizeof(line),"mg2_a20_recovered xr=%llu streak=%llu fresh_total=%llu reused_total=%llu empty_total=%llu\r\n",
                (unsigned long long)f.input.frameId,(unsigned long long)missedStreak_,
                (unsigned long long)freshTotal_,(unsigned long long)reusedTotal_,(unsigned long long)emptyTotal_);
            DiagnosticLine(line);
        }
        missedStreak_=0;lastFailure_=XRFrameFailure::None;lastReused_=false;
    }else if(f.render&&!f.captureOnly){
        ++missedStreak_;
        if(reuse)++reusedTotal_;else ++emptyTotal_;
        const auto failure=f.presentationFailure==XRFrameFailure::None?XRFrameFailure::Render:f.presentationFailure;
        if(missedStreak_==1||(missedStreak_&(missedStreak_-1))==0||failure!=lastFailure_||reuse!=lastReused_){
            const char* reason="render";
            switch(failure){
            case XRFrameFailure::Prepare:reason="prepare";break;
            case XRFrameFailure::Tracking:reason="tracking";break;
            case XRFrameFailure::SceneResidency:reason="scene_residency";break;
            case XRFrameFailure::GraphResidency:reason="graph_residency";break;
            default:break;
            }
            char line[512];
            std::snprintf(line,sizeof(line),"mg2_a20_miss xr=%llu reason=%s layer=%s held=%u source_xr=%llu streak=%llu fresh_total=%llu reused_total=%llu empty_total=%llu target_minus_end_call_ms=%.3f\r\n",
                (unsigned long long)f.input.frameId,reason,reuse?"reused":"empty",unsigned(swapchainsHeld_),
                (unsigned long long)(reuse?lastPresented_.frame:0), (unsigned long long)missedStreak_,
                (unsigned long long)freshTotal_,(unsigned long long)reusedTotal_,(unsigned long long)emptyTotal_,
                1000.*QpcSeconds(f.input.targetDisplayQpc-beforeEnd));
            DiagnosticLine(line);
        }
        lastFailure_=failure;lastReused_=reuse;
    }
}
XrResult XR::Haptic(const HapticEvent& event){
    if(state_!=XR_SESSION_STATE_FOCUSED)return XR_SESSION_NOT_FOCUSED;
    if(!inputSystem_)return XR_ERROR_VALIDATION_FAILURE;
    const auto physical=inputSystem_->ResolveHaptic(event.action,event.hand);
    if(!physical)return XR_ERROR_PATH_UNSUPPORTED;
    XrHapticActionInfo action{XR_TYPE_HAPTIC_ACTION_INFO};
    action.action=physicalActions_[physical.value];
    action.subactionPath=hands_[event.hand&1u];
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude=std::clamp(event.amplitude,0.f,1.f);
    vibration.frequency=XR_FREQUENCY_UNSPECIFIED;
    vibration.duration=XrDuration(event.duration*1e9);
    return xrApplyHapticFeedback(session_,&action,reinterpret_cast<XrHapticBaseHeader*>(&vibration));
}
XR::~XR(){if(color_)xrDestroySwapchain(color_);if(depth_)xrDestroySwapchain(depth_);for(auto& action:poseSpaces_)for(auto space:action)if(space)xrDestroySpace(space);if(viewSpace_)xrDestroySpace(viewSpace_);if(space_)xrDestroySpace(space_);if(session_)xrDestroySession(session_);if(physicalActionsSet_)xrDestroyActionSet(physicalActionsSet_);if(instance_)xrDestroyInstance(instance_);}
}
