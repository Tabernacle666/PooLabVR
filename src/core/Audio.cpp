#include "Audio.h"
#include "Reverb.h"
#include "SessionLog.h"
#include "RealtimeMemory.h"
#include <xaudio2.h>
#include <phonon.h>
#include <avrt.h>
#include <map>
#include <cstring>
namespace lab {
namespace {
constexpr uint32_t Samples=256,VoiceCount=AudioVoiceCount,BufferCount=3,LoopVoices=MaxAudioEmitters*2,EventVoices=VoiceCount-LoopVoices;
RealtimeMemory AudioMemory;
void Ipl(IPLerror result,const char* text){if(result!=IPL_STATUS_SUCCESS)Fail(text);}
bool HasAcousticGeometry(const SceneData& scene){
    for(const auto& object:scene.objects){
        if(object.flags&NoCollision)continue;
        if(object.mesh>=scene.meshes.ranges.size())continue;
        const auto& range=scene.meshes.ranges[object.mesh];
        if(range.vertexCount&&range.indexCount>=3)return true;
    }
    return false;
}
}
struct Audio::Impl:IXAudio2VoiceCallback {
    SceneData acousticBase;std::map<uint64_t,std::vector<LabAcousticBoxV1>> acousticBoxes;
    AudioEventDecoder eventDecoder{};AudioSignalProvider signals;
    struct AcousticUpdate {Reverb* value{};};
    std::atomic<AcousticUpdate*> pendingAcoustics{};SpscQueue<Reverb*,64> retiredAcoustics;
    Reverb* reverb{};Reverb* oldReverb{};Reverb* waitingRetire{};float acousticBlend{1};Channels* io{};IXAudio2* engine{};IXAudio2MasteringVoice* master{};IXAudio2SourceVoice* source{};
    IPLContext context{};IPLHRTF hrtf{};HANDLE ready{};std::thread thread;std::atomic<bool> stop{};
    std::atomic<uint32_t> free{(1u<<BufferCount)-1};std::atomic<HRESULT> error{S_OK};
    std::array<std::array<float,Samples*2>,BufferCount> buffers{};
    std::array<float,Samples*2> reflected{},oldReflected{};
    struct Voice {
        std::array<IPLBinauralEffect,2> effects{};
        std::array<float,Samples> mono{},send{};
        std::array<std::array<float,Samples>,2> left{},right{};
        std::array<XMFLOAT3,2> directions{{{0,0,1},{0,0,1}}};uint32_t ear{};
        AudioEmitter target{};XMFLOAT3 position{},velocity{};double targetTime{};
        float gain{},level{},filter{},frequency{},occlusion{},doppler{1},envelope{},eventDecay{.9980f};
        SoundWave wave{};uint64_t eventIdentity{};bool loop{},active{},retiring{},draining{},pending{};AudioEvent nextEvent{};
    };
    std::array<Voice,VoiceCount> voices;uint64_t eventSerial{},lastLoggedInput{};AudioFrame frame{},previousFrame{};
    bool haveFrame{};Pose listener{};double blockTime{};
    static float Finite(float x,float fallback=0){return std::isfinite(x)?x:fallback;}
    static XMFLOAT3 SafeVector(XMFLOAT3 v){return {Finite(v.x),Finite(v.y),Finite(v.z)};}
    static XMFLOAT3 LimitVelocity(XMFLOAT3 v){v=SafeVector(v);float speed=Length(v);return speed>100?Scale(v,100/speed):v;}
    static bool Same(const Voice& v,const AudioEmitter& e){return v.active&&v.loop&&v.target.id==e.id&&v.target.owner==e.owner&&v.target.generation==e.generation;}
    void Begin(Voice& v,uint32_t slot,const AudioEmitter& e,bool loop,float decay=.998f){
        v.target=e;v.targetTime=frame.time;v.position=e.position;v.velocity=e.velocity;
        v.wave=e.wave;v.gain=v.level=0;v.frequency=e.frequency;v.occlusion=e.occlusion;v.doppler=1;v.filter=0;v.envelope=0;
        v.loop=loop;v.active=true;v.retiring=v.draining=false;v.eventDecay=decay;v.ear=0;
        auto relative=Sub(e.position,listener.p);XMFLOAT3 local;
        XMStoreFloat3(&local,XMVector3TransformNormal(XMLoadFloat3(&relative),XMMatrixTranspose(XMMatrixRotationQuaternion(XMLoadFloat4(&listener.q)))));
        local=Length(local)>.0001f?Unit(local):XMFLOAT3{0,0,1};v.directions={local,local};
        for(auto effect:v.effects)iplBinauralEffectReset(effect);
        if(signals.reset)signals.reset(signals.context,slot,e.id,loop);
    }
    void BeginEvent(Voice& v,uint32_t slot,const AudioEvent& event){
        AudioEmitter e{};e.position=SafeVector(event.position);e.gain=std::clamp(Finite(event.gain),0.f,4.f);
        e.frequency=std::clamp(Finite(event.frequency,220),10.f,18000.f);e.wave=event.wave;
        Begin(v,slot,e,false,std::clamp(Finite(event.decay,.998f),0.f,.99999f));v.eventIdentity=(uint64_t(1)<<63)|++eventSerial;v.pending=false;
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32)override{}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd()override{}
    void STDMETHODCALLTYPE OnStreamEnd()override{}
    void STDMETHODCALLTYPE OnBufferStart(void*)override{}
    void STDMETHODCALLTYPE OnLoopEnd(void*)override{}
    void STDMETHODCALLTYPE OnVoiceError(void*,HRESULT result)override{error.store(result,std::memory_order_relaxed);stop=true;SetEvent(ready);}
    void STDMETHODCALLTYPE OnBufferEnd(void* context)override{auto i=uint32_t(reinterpret_cast<uintptr_t>(context)-1);free.fetch_or(1u<<i,std::memory_order_release);SetEvent(ready);}
    void InitializeService(Channels& channels,AudioEventDecoder decode,AudioSignalProvider provider){if(engine||context)Fail("Audio service already initialized");signals=provider;eventDecoder=decode;
        io=&channels;ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!ready)Fail("audio event");
        Hr(XAudio2Create(&engine,0,XAUDIO2_DEFAULT_PROCESSOR),"XAudio2");Hr(engine->CreateMasteringVoice(&master,2,48000),"stereo headphones");
        WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;format.nChannels=2;format.nSamplesPerSec=48000;format.wBitsPerSample=32;format.nBlockAlign=8;format.nAvgBytesPerSec=384000;Hr(engine->CreateSourceVoice(&source,&format,0,2.f,this),"binaural voice");
        AudioMemory.Initialize(28);IPLContextSettings settings{};settings.version=STEAMAUDIO_VERSION;
        settings.allocateCallback=[](IPLsize size,IPLsize alignment)->void*{return AudioMemory.Allocate(size,alignment);};settings.freeCallback=[](void* pointer){RealtimeMemory::Release(pointer);};
        Ipl(iplContextCreate(&settings,&context),"Steam Audio context");IPLAudioSettings audio{48000,int(Samples)};IPLHRTFSettings h{};h.type=IPL_HRTFTYPE_DEFAULT;h.volume=1;Ipl(iplHRTFCreate(context,&audio,&h,&hrtf),"Steam Audio HRTF");
        IPLBinauralEffectSettings effect{};effect.hrtf=hrtf;for(auto& voice:voices)for(auto& ear:voice.effects)Ipl(iplBinauralEffectCreate(context,&audio,&effect,&ear),"binaural effect");
    }
    void RebuildAcoustics(){
        if(!context||!hrtf)return;SceneData assets=acousticBase;
        if(!acousticBoxes.empty()){
            MeshRange cube{};cube.firstVertex=uint32_t(assets.meshes.vertices.size());cube.firstIndex=uint32_t(assets.meshes.indices.size());cube.vertexCount=8;cube.indexCount=36;
            const XMFLOAT3 points[]{{-1,-1,-1},{1,-1,-1},{-1,1,-1},{1,1,-1},{-1,-1,1},{1,-1,1},{-1,1,1},{1,1,1}};
            for(auto p:points){Vertex v{};v.position=p;assets.meshes.vertices.push_back(v);}
            const uint32_t triangles[]{0,2,1,1,2,3,4,5,6,5,7,6,0,1,4,1,5,4,2,6,3,3,6,7,0,4,2,2,4,6,1,3,5,3,7,5};
            for(auto i:triangles)assets.meshes.indices.push_back(cube.firstVertex+i);
            uint32_t mesh=uint32_t(assets.meshes.ranges.size());assets.meshes.ranges.push_back(cube);
            for(const auto& [owner,boxes]:acousticBoxes)for(const auto& box:boxes){RenderObject object;object.mesh=mesh;object.current={{box.pose.position.x,box.pose.position.y,box.pose.position.z},{box.pose.orientation.x,box.pose.orientation.y,box.pose.orientation.z,box.pose.orientation.w}};object.previous=object.current;object.scale={box.halfExtent.x,box.halfExtent.y,box.halfExtent.z};assets.objects.push_back(object);}
        }
        std::unique_ptr<Reverb> next;if(HasAcousticGeometry(assets)){next=std::make_unique<Reverb>();next->Initialize(context,hrtf,assets,Samples);}PublishAcoustics(next.release());
    }
    void PublishAcoustics(Reverb* value){
        Reverb* retired;while(retiredAcoustics.pop(retired))delete retired;AcousticUpdate* consumed;while(consumedUpdates.pop(consumed))delete consumed;
        auto* update=new AcousticUpdate{value};auto* superseded=pendingAcoustics.exchange(update,std::memory_order_acq_rel);
        if(superseded){delete superseded->value;delete superseded;}
    }
    void LoadScene(const SceneData& assets){if(!context||!hrtf)Fail("Audio service is not initialized");acousticBase=assets;RebuildAcoustics();}
    uint32_t SetAcousticBoxes(uint64_t owner,const LabAcousticBoxV1* boxes,uint32_t n){
        if(!owner||n>128||(n&&!boxes))return 0;
        for(uint32_t i=0;i<n;i++){const auto& b=boxes[i];const float v[]{b.pose.position.x,b.pose.position.y,b.pose.position.z,b.pose.orientation.x,b.pose.orientation.y,b.pose.orientation.z,b.pose.orientation.w,b.halfExtent.x,b.halfExtent.y,b.halfExtent.z};for(float x:v)if(!std::isfinite(x))return 0;
            float q=b.pose.orientation.x*b.pose.orientation.x+b.pose.orientation.y*b.pose.orientation.y+b.pose.orientation.z*b.pose.orientation.z+b.pose.orientation.w*b.pose.orientation.w;if(std::abs(q-1)>.01f||b.halfExtent.x<=0||b.halfExtent.y<=0||b.halfExtent.z<=0||b.halfExtent.x>64||b.halfExtent.y>64||b.halfExtent.z>64)return 0;
        }
        auto old=acousticBoxes.find(owner);if(!n&&old==acousticBoxes.end())return 1;
        if(n&&old!=acousticBoxes.end()&&old->second.size()==n&&!memcmp(old->second.data(),boxes,n*sizeof(*boxes)))return 1;
        if(n)acousticBoxes[owner].assign(boxes,boxes+n);else acousticBoxes.erase(owner);RebuildAcoustics();return 1;
    }
    void UnloadScene(){acousticBase={};PublishAcoustics(nullptr);}
    void Initialize(Channels& channels,const SceneData& assets,AudioEventDecoder decode,AudioSignalProvider provider){InitializeService(channels,decode,provider);LoadScene(assets);}
    void UpdateVoices(){
        AudioFrame next=io->audio.read();
        bool fresh=!haveFrame||next.inputFrameId!=frame.inputFrameId||next.time!=frame.time;
        if(fresh){
            previousFrame=frame;frame=next;frame.count=std::min(frame.count,MaxAudioEmitters);
            frame.time=std::isfinite(frame.time)&&frame.time>0?frame.time:QpcNowSeconds();frame.velocity=LimitVelocity(frame.velocity);
            frame.listener.p=SafeVector(frame.listener.p);auto q=XMLoadFloat4(&frame.listener.q);
            float length=XMVectorGetX(XMVector4Length(q));if(!std::isfinite(length)||length<.001f)frame.listener.q={0,0,0,1};else XMStoreFloat4(&frame.listener.q,XMQuaternionNormalize(q));
            if(!haveFrame){previousFrame=frame;listener=frame.listener;haveFrame=true;}
            if(frame.inputFrameId!=lastLoggedInput){LogEvent(frame.inputFrameId,"audio.consume","emitters=%u hrtf=1 sample_rate=48000 block=256",frame.count);lastLoggedInput=frame.inputFrameId;}
            for(uint32_t i=0;i<frame.count;i++){auto& e=frame.emitters[i];e.position=SafeVector(e.position);e.velocity=LimitVelocity(e.velocity);e.gain=std::clamp(Finite(e.gain),0.f,4.f);e.frequency=std::clamp(Finite(e.frequency,220),10.f,18000.f);e.occlusion=std::clamp(Finite(e.occlusion),0.f,1.f);}
        }
        std::array<bool,LoopVoices> assigned{};
        // Match all survivors before admitting newcomers, including voices fading out.
        for(uint32_t i=0;i<frame.count;i++)for(uint32_t v=0;v<LoopVoices;v++)if(!assigned[v]&&Same(voices[v],frame.emitters[i])){
            assigned[v]=true;voices[v].target=frame.emitters[i];voices[v].targetTime=frame.time;voices[v].retiring=false;break;
        }
        for(uint32_t v=0;v<LoopVoices;v++)if(voices[v].active&&!assigned[v])voices[v].retiring=true;
        for(uint32_t i=0;i<frame.count;i++){
            const auto& e=frame.emitters[i];bool matched=false;for(uint32_t v=0;v<LoopVoices;v++)if(assigned[v]&&Same(voices[v],e))matched=true;
            if(matched)continue;
            for(uint32_t v=0;v<LoopVoices;v++)if(!voices[v].active){Begin(voices[v],v,e,true);assigned[v]=true;break;}
        }
        AudioEvent event;while(io->audioEvents.pop(event)){
            AudioEventSound sound;SoundWave wave=0;
            if(event.flags&1u){sound={event.gain,event.frequency,event.decay};wave=event.wave;}
            else if(!eventDecoder||!eventDecoder(event,sound))continue;
            event.gain=sound.gain;event.frequency=sound.frequency;event.decay=sound.decay;event.wave=wave;
            uint32_t selected=Invalid;float quietest=std::numeric_limits<float>::max();
            for(uint32_t v=LoopVoices;v<VoiceCount;v++){
                if(!voices[v].active&&!voices[v].pending){selected=v;break;}
                float level=voices[v].gain*voices[v].envelope+(voices[v].pending?8.f:0.f);
                if(level<quietest){quietest=level;selected=v;}
            }
            if(selected==Invalid)continue;auto& v=voices[selected];
            if(!v.active)BeginEvent(v,selected,event);else{v.nextEvent=event;v.pending=true;v.retiring=true;}
        }
    }
    Pose ListenerAt(double time)const{
        Pose p=frame.listener;float horizon=float(std::clamp(time-frame.time,0.,.020));p.p=Add(p.p,Scale(frame.velocity,horizon));
        double dt=frame.time-previousFrame.time;
        if(dt>.001&&dt<.1&&horizon>0){
            auto a=XMLoadFloat4(&previousFrame.listener.q),b=XMLoadFloat4(&frame.listener.q);
            float angle=2*std::acos(std::clamp(std::abs(XMVectorGetX(XMVector4Dot(a,b))),0.f,1.f));
            if(angle<float(dt)*12.f)XMStoreFloat4(&p.q,XMQuaternionNormalize(XMQuaternionSlerp(a,b,1+std::min(horizon/float(dt),1.f))));
        }
        return p;
    }
    void ProcessReflections(const Pose& pose,std::span<const ReverbSend> sends,float* output){
        if(waitingRetire&&retiredAcoustics.push(waitingRetire))waitingRetire=nullptr;
        if(!oldReverb&&!waitingRetire&&acousticBlend>=1){
            if(auto* update=pendingAcoustics.exchange(nullptr,std::memory_order_acq_rel)){
                oldReverb=reverb;reverb=update->value;acousticBlend=0;
                // Retire the tiny handover record on the producer too.
                consumedUpdates.push(update);
            }
        }
        reflected.fill(0);oldReflected.fill(0);
        if(reverb)reverb->Process(pose,sends,reflected.data());
        if(oldReverb)oldReverb->Process(pose,sends,oldReflected.data());
        float next=std::min(1.f,acousticBlend+float(Samples)/(48000.f*.12f));
        for(uint32_t i=0;i<Samples;i++){float t=acousticBlend+(next-acousticBlend)*float(i+1)/Samples;for(uint32_t c=0;c<2;c++)output[i*2+c]+=reflected[i*2+c]*t+oldReflected[i*2+c]*(1-t);}
        acousticBlend=next;if(oldReverb&&next>=1){waitingRetire=oldReverb;oldReverb=nullptr;}
    }
    SpscQueue<AcousticUpdate*,64> consumedUpdates;
    void Generate(uint32_t index){
        UpdateVoices();auto& output=buffers[index];output.fill(0);
        XAUDIO2_VOICE_STATE state{};source->GetState(&state,XAUDIO2_VOICE_NOSAMPLESPLAYED);
        double now=QpcNowSeconds();double playAt=now+double(state.BuffersQueued)*Samples/48000.;
        blockTime=std::max(blockTime,playAt);Pose nextListener=ListenerAt(blockTime+double(Samples)/48000.);
        float poseStep=std::min(1.f,float(Samples)/(48000.f*.006f));nextListener=Blend(listener,nextListener,poseStep);
        auto inverse=XMMatrixTranspose(XMMatrixRotationQuaternion(XMLoadFloat4(&nextListener.q)));
        std::array<ReverbSend,VoiceCount> sends{};uint32_t sendCount=0;
        for(uint32_t slot=0;slot<VoiceCount;slot++){
            auto& v=voices[slot];if(!v.active){if(v.pending)BeginEvent(v,slot,v.nextEvent);else continue;}
            auto targetPosition=Add(v.target.position,Scale(v.target.velocity,float(std::clamp(blockTime-v.targetTime,0.,.020))));
            float motion=1-std::exp(-float(Samples)/(48000.f*.008f));
            v.position=Add(v.position,Scale(Sub(targetPosition,v.position),motion));v.velocity=Add(v.velocity,Scale(Sub(v.target.velocity,v.velocity),motion));
            auto relative=Sub(v.position,nextListener.p);float distance=Length(relative);auto direction=distance>.0001f?Scale(relative,1/distance):XMFLOAT3{0,0,1};
            float doppler=std::clamp((343.f+std::clamp(Dot(frame.velocity,direction),-100.f,100.f))/(343.f+std::clamp(Dot(v.velocity,direction),-100.f,100.f)),.5f,2.f);
            float endDoppler=v.doppler+(doppler-v.doppler)*motion,endFrequency=v.frequency+(v.target.frequency-v.frequency)*motion;
            float endGain=v.gain+(v.target.gain-v.gain)*motion,endOcclusion=v.occlusion+(v.target.occlusion-v.occlusion)*motion;
            float attenuation=1.f/(1.f+distance*distance*1.4f);
            // Gain includes distance: interpolate the final scalar rather than stepping attenuation.
            float startLevel=v.level;
            float endLevel=endGain*attenuation;
            AudioSignalRequest request{slot,v.wave,v.frequency,v.doppler,v.eventDecay,v.loop,endFrequency,endDoppler};
            bool continuing=false;if(!v.draining&&signals.generate)continuing=signals.generate(signals.context,request,v.mono);else v.mono.fill(0);
            bool fading=v.retiring||v.wave!=v.target.wave;
            float envelopeEnd=std::clamp(v.envelope+(fading?-1.f:1.f)*float(Samples)/(48000.f*(fading?.008f:.006f)),0.f,1.f);
            for(uint32_t i=0;i<Samples;i++){
                float t=float(i+1)/Samples;float value=Finite(v.mono[i]);float occ=v.occlusion+(endOcclusion-v.occlusion)*t;
                v.filter+=(value-v.filter)*(.72f-.675f*occ);
                v.send[i]=v.filter*(v.gain+(endGain-v.gain)*t)*(v.envelope+(envelopeEnd-v.envelope)*t);
                v.mono[i]=v.filter*(startLevel+(endLevel-startLevel)*t)*(1-occ*.65f);
            }
            XMFLOAT3 local;XMStoreFloat3(&local,XMVector3TransformNormal(XMLoadFloat3(&direction),inverse));
            uint32_t from=v.ear,to=from^1u;v.directions[to]=local;
            for(uint32_t ear=0;ear<2;ear++){
                IPLBinauralEffectParams parameters{};parameters.direction={v.directions[ear].x,v.directions[ear].y,-v.directions[ear].z};
                parameters.interpolation=IPL_HRTFINTERPOLATION_BILINEAR;parameters.spatialBlend=1;parameters.hrtf=hrtf;
                float* in[]{v.mono.data()};float* out[]{v.left[ear].data(),v.right[ear].data()};IPLAudioBuffer input{1,int(Samples),in},ears{2,int(Samples),out};
                if(v.draining)iplBinauralEffectGetTail(v.effects[ear],&ears);else iplBinauralEffectApply(v.effects[ear],&parameters,&input,&ears);
            }
            // Two continuously fed convolution histories. Retarget only the inaudible
            // branch, then crossfade its HRTF output across every sample of this block.
            for(uint32_t i=0;i<Samples;i++){float t=float(i+1)/Samples;float envelope=v.envelope+(envelopeEnd-v.envelope)*t;
                output[i*2]+=(v.left[from][i]*(1-t)+v.left[to][i]*t)*envelope;
                output[i*2+1]+=(v.right[from][i]*(1-t)+v.right[to][i]*t)*envelope;
            }
            v.ear=to;v.envelope=envelopeEnd;v.gain=endGain;v.level=endLevel;v.frequency=endFrequency;v.occlusion=endOcclusion;v.doppler=endDoppler;
            sends[sendCount++]={v.loop?v.target.id:v.eventIdentity,v.target.owner,v.target.generation,v.position,v.target.flags,v.send.data(),v.gain*v.envelope};
            if(!v.loop&&!continuing)v.draining=true;
            if(!v.retiring&&v.envelope<=0)v.wave=v.target.wave;
            if((v.retiring&&v.envelope<=0)||(v.draining&&iplBinauralEffectGetTailSize(v.effects[0])==0&&iplBinauralEffectGetTailSize(v.effects[1])==0))v.active=false;
        }
        ProcessReflections(nextListener,{sends.data(),sendCount},output.data());listener=nextListener;blockTime+=double(Samples)/48000.;
        for(auto& sample:output)sample=std::isfinite(sample)?sample/(1+std::abs(sample)):0;
        XAUDIO2_BUFFER buffer{};buffer.AudioBytes=UINT32(sizeof(output));buffer.pAudioData=reinterpret_cast<const BYTE*>(output.data());buffer.pContext=reinterpret_cast<void*>(uintptr_t(index+1));HRESULT result=source->SubmitSourceBuffer(&buffer);if(FAILED(result)){LogEvent(frame.inputFrameId,"audio.error","HRESULT=0x%08lX",static_cast<unsigned long>(result));error=result;stop=true;}
    }
    void Run(){
        wchar_t threadName[96];swprintf_s(threadName,L"%ls audio",GetCoreIdentity().diagnosticLabelW);SetThreadDescription(GetCurrentThread(),threadName);Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"audio COM");DWORD task=0;HANDLE priority=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task);if(priority)AvSetMmThreadPriority(priority,AVRT_PRIORITY_HIGH);
        uint32_t initial=free.exchange(0,std::memory_order_acquire);for(uint32_t i=0;i<BufferCount;i++)if(initial&(1u<<i))Generate(i);Hr(source->Start(),"audio start");
        while(!stop.load(std::memory_order_relaxed)&&!io->stop.load(std::memory_order_relaxed)){uint32_t mask=free.exchange(0,std::memory_order_acquire);for(uint32_t i=0;i<BufferCount;i++)if(mask&(1u<<i))Generate(i);WaitForSingleObject(ready,20);}
        source->Stop();if(priority)AvRevertMmThreadCharacteristics(priority);CoUninitialize();if(FAILED(error.load()))io->stop=true;
    }
    ~Impl(){stop=true;if(ready)SetEvent(ready);if(thread.joinable())thread.join();if(source){source->Stop();source->DestroyVoice();}if(master)master->DestroyVoice();if(engine)engine->Release();delete reverb;delete oldReverb;delete waitingRetire;Reverb* retired;while(retiredAcoustics.pop(retired))delete retired;AcousticUpdate* update;while(consumedUpdates.pop(update))delete update;if((update=pendingAcoustics.exchange(nullptr))){delete update->value;delete update;}for(auto& voice:voices)for(auto& effect:voice.effects)if(effect)iplBinauralEffectRelease(&effect);if(hrtf)iplHRTFRelease(&hrtf);if(context)iplContextRelease(&context);if(ready)CloseHandle(ready);}
};
Audio::Audio():impl_(std::make_unique<Impl>()){}Audio::~Audio()=default;
void Audio::InitializeService(Channels& channels,AudioEventDecoder decode,AudioSignalProvider provider){impl_->InitializeService(channels,decode,provider);}
uint32_t Audio::SetAcousticBoxes(uint64_t owner,const LabAcousticBoxV1* boxes,uint32_t n){return impl_->SetAcousticBoxes(owner,boxes,n);}
void Audio::LoadScene(const SceneData& assets){impl_->LoadScene(assets);}
void Audio::UnloadScene(){impl_->UnloadScene();}
void Audio::Initialize(Channels& channels,const SceneData& assets,AudioEventDecoder decode,AudioSignalProvider provider){impl_->Initialize(channels,assets,decode,provider);}
void Audio::Start(){impl_->stop=false;impl_->thread=std::thread([this]{impl_->Run();});}
void Audio::Stop(){impl_->stop=true;if(impl_->ready)SetEvent(impl_->ready);if(impl_->thread.joinable())impl_->thread.join();}
}
