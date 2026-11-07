#include "Reverb.h"
#include "AudioSignal.h"
#include "../cartridge_api/LaboVRAudioService.h"
namespace lab {
namespace {
void Check(IPLerror code,const char* text){if(code!=IPL_STATUS_SUCCESS)Fail(text);}
IPLVector3 AudioVector(XMFLOAT3 p){return {p.x,p.y,-p.z};}
IPLCoordinateSpace3 Coordinates(const Pose& p) {
    IPLCoordinateSpace3 out{};out.origin=AudioVector(p.p);
    out.right=AudioVector(Sub(Transform({1,0,0},p),p.p));out.up=AudioVector(Sub(Transform({0,1,0},p),p.p));out.ahead=AudioVector(Sub(Transform({0,0,1},p),p.p));return out;
}
}
struct Reverb::Impl {
    static constexpr uint32_t Samples=256,Channels=4,IRSize=48000,Sources=5,HistoryBlocks=(IRSize+Samples-1)/Samples,RecentBlocks=32;
    enum State:uint32_t {Free,Writing,Ready,Active};
    struct Key {uint64_t id{},owner{},generation{};bool operator==(const Key&)const=default;};
    struct Location {Key key{};XMFLOAT3 position{};bool valid{};};
    struct Record {Pose listener{};std::array<Location,Sources> sources{};std::array<std::array<float,Samples>,Sources> samples{};uint64_t serial{};};
    struct Bank {
        IPLSimulator simulator{};std::array<IPLSource,Sources> sources{};
        std::array<IPLReflectionEffect,Sources> effects{};std::array<IPLReflectionEffectParams,Sources> parameters{};
        std::array<Location,Sources> locations{};uint64_t processed{};std::atomic<uint32_t> state{Free};std::atomic<uint64_t> version{};
    };
    std::array<Bank,3> banks;
    IPLScene scene{};IPLStaticMesh geometry{};IPLAmbisonicsDecodeEffect decoder{};IPLHRTF hrtf{};
    SpscQueue<Record,256> records;std::array<Record,HistoryBlocks> workerHistory{};Record workerFrame{};
    std::array<Record,RecentBlocks> recent{};uint64_t serial{};int warming{-1};
    std::array<uint32_t,Sources> hold{};
    std::thread thread;HANDLE stopEvent{},wakeEvent{};
    std::array<Location,Sources> locations{};
    std::array<std::array<float,Samples>,Sources> inputs{};std::array<float,Samples> silence{};
    std::array<std::array<float,Samples>,Channels> temporary{},mix{};
    std::array<float,Samples> left{},right{};
    int current{-1},previous{-1};float blend{};
    void Initialize(IPLContext context,IPLHRTF h,const SceneData& assets,uint32_t size) {
        if(size!=Samples)Fail("reverb block size");hrtf=h;IPLAudioSettings audio{48000,int(Samples)};
        IPLSceneSettings ss{};ss.type=IPL_SCENETYPE_DEFAULT;Check(iplSceneCreate(context,&ss,&scene),"acoustic scene");
        std::vector<XMFLOAT3> positions;std::vector<std::array<uint32_t,3>> indices;AppendAcousticGeometry(assets,positions,indices);
        std::vector<IPLVector3> vertices;vertices.reserve(positions.size());for(auto p:positions)vertices.push_back(AudioVector(p));
        std::vector<IPLTriangle> triangles(indices.size());std::vector<IPLint32> materialIndices(indices.size(),0);
        for(size_t i=0;i<indices.size();i++){triangles[i].indices[0]=int(indices[i][0]);triangles[i].indices[1]=int(indices[i][2]);triangles[i].indices[2]=int(indices[i][1]);}
        IPLMaterial material{{.18f,.26f,.38f},.35f,{.02f,.01f,.005f}};IPLStaticMeshSettings ms{};ms.numVertices=int(vertices.size());ms.numTriangles=int(triangles.size());ms.numMaterials=1;ms.vertices=vertices.data();ms.triangles=triangles.data();ms.materialIndices=materialIndices.data();ms.materials=&material;
        Check(iplStaticMeshCreate(scene,&ms,&geometry),"acoustic geometry");iplStaticMeshAdd(geometry,scene);iplSceneCommit(scene);
        IPLSimulationSettings simulation{};simulation.flags=IPL_SIMULATIONFLAGS_REFLECTIONS;simulation.sceneType=IPL_SCENETYPE_DEFAULT;simulation.reflectionType=IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;simulation.maxNumRays=512;simulation.numDiffuseSamples=16;simulation.maxDuration=1;simulation.maxOrder=1;simulation.maxNumSources=Sources;simulation.numThreads=1;simulation.samplingRate=48000;simulation.frameSize=Samples;
        IPLSourceSettings source{};source.flags=IPL_SIMULATIONFLAGS_REFLECTIONS;
        IPLReflectionEffectSettings effect{};effect.type=IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;effect.irSize=IRSize;effect.numChannels=Channels;
        for(auto& b:banks){
            Check(iplSimulatorCreate(context,&simulation,&b.simulator),"reflection simulator");iplSimulatorSetScene(b.simulator,scene);
            for(uint32_t i=0;i<Sources;i++){
                Check(iplSourceCreate(b.simulator,&source,&b.sources[i]),"reverb source");iplSourceAdd(b.sources[i],b.simulator);
                Check(iplReflectionEffectCreate(context,&audio,&effect,&b.effects[i]),"convolution effect");
            }
            iplSimulatorCommit(b.simulator);
        }
        IPLAmbisonicsDecodeEffectSettings decode{};decode.maxOrder=1;decode.hrtf=h;decode.speakerLayout.type=IPL_SPEAKERLAYOUTTYPE_STEREO;Check(iplAmbisonicsDecodeEffectCreate(context,&audio,&decode,&decoder),"reverb HRTF decoder");
        wakeEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!stopEvent||!wakeEvent)Fail("acoustic stop event");thread=std::thread([this]{Run();});
    }
    void Run() {
        wchar_t threadName[96];swprintf_s(threadName,L"%ls acoustics",GetCoreIdentity().diagnosticLabelW);SetThreadDescription(GetCurrentThread(),threadName);SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);uint64_t version=0;
        while(WaitForSingleObject(stopEvent,0)!=WAIT_OBJECT_0) {
            Record record;while(records.pop(record)){workerFrame=record;workerHistory[record.serial%HistoryBlocks]=record;}
            const auto& data=workerFrame;Bank* selected=nullptr;
            for(auto& b:banks){uint32_t state=Free;if(b.state.compare_exchange_strong(state,Writing,std::memory_order_acq_rel)){selected=&b;break;}}
            if(selected){auto& b=*selected;b.locations=data.sources;
                for(uint32_t i=0;i<Sources;i++){
                    IPLSimulationInputs input{};input.flags=IPL_SIMULATIONFLAGS_REFLECTIONS;
                    // Source and listener are separate world-space coordinate systems.
                    input.source=Coordinates(Pose{data.sources[i].position,{0,0,0,1}});
                    input.distanceAttenuationModel.type=IPL_DISTANCEATTENUATIONTYPE_DEFAULT;input.airAbsorptionModel.type=IPL_AIRABSORPTIONTYPE_DEFAULT;
                    input.reverbScale[0]=input.reverbScale[1]=input.reverbScale[2]=data.sources[i].valid?1.f:0.f;
                    iplSourceSetInputs(b.sources[i],IPL_SIMULATIONFLAGS_REFLECTIONS,&input);
                }
                IPLSimulationSharedInputs shared{};shared.listener=Coordinates(data.listener);shared.numRays=512;shared.numBounces=8;shared.duration=1;shared.order=1;shared.irradianceMinDistance=.5f;
                iplSimulatorSetSharedInputs(b.simulator,IPL_SIMULATIONFLAGS_REFLECTIONS,&shared);iplSimulatorRunReflections(b.simulator);
                for(uint32_t i=0;i<Sources;i++){
                    IPLSimulationOutputs output{};iplSourceGetOutputs(b.sources[i],IPL_SIMULATIONFLAGS_REFLECTIONS,&output);b.parameters[i]=output.reflections;
                    b.parameters[i].type=IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;b.parameters[i].numChannels=Channels;b.parameters[i].irSize=IRSize;
                }
                // Prime new impulse banks on the worker with actual source history.
                // A short impact remains in its world-space lane after its direct voice ends.
                for(auto effect:b.effects)iplReflectionEffectReset(effect);
                std::array<std::array<float,Samples>,Channels> scratch{};float* tmp[]{scratch[0].data(),scratch[1].data(),scratch[2].data(),scratch[3].data()};IPLAudioBuffer out{int(Channels),int(Samples),tmp};
                uint64_t first=data.serial>=HistoryBlocks?data.serial-HistoryBlocks+1:1;
                for(uint64_t n=first;n<=data.serial;n++)ApplyRecord(b,workerHistory[n%HistoryBlocks],n,out);
                b.processed=data.serial;b.version.store(++version,std::memory_order_relaxed);b.state.store(Ready,std::memory_order_release);
            }
            HANDLE events[]{stopEvent,wakeEvent};if(WaitForMultipleObjects(2,events,FALSE,150)==WAIT_OBJECT_0)break;
        }
    }
    void ApplyRecord(Bank& b,const Record& record,uint64_t expected,IPLAudioBuffer& out){
        for(uint32_t slot=0;slot<Sources;slot++)if(b.locations[slot].valid){
            bool matched=record.serial==expected&&record.sources[slot].valid&&record.sources[slot].key==b.locations[slot].key;
            float* in[]{matched?const_cast<float*>(record.samples[slot].data()):silence.data()};IPLAudioBuffer input{1,int(Samples),in};
            iplReflectionEffectApply(b.effects[slot],&b.parameters[slot],&input,&out,nullptr);
        }
    }
    void SelectSources(const Pose& pose,std::span<const ReverbSend> sends){
        struct Candidate {Location location{};std::array<float,Samples> mono{};float weight{},level{};bool diffuse{},used{};};
        std::array<Candidate,AudioVoiceCount> candidates{};uint32_t count=0;
        for(const auto& send:sends){
            if(!send.mono||send.level<=.000001f)continue;bool diffuse=(send.flags&LABOVR_AUDIO_EMITTER_DIFFUSE_V1)!=0;
            uint32_t match=count;Key key{send.id,send.owner,send.generation};
            for(uint32_t i=0;i<count;i++){
                const auto& c=candidates[i];bool sameOwner=c.location.key.owner==key.owner&&c.location.key.generation==key.generation;
                bool group=diffuse&&c.diffuse&&sameOwner;
                bool loops=(key.id>>63)==0&&(c.location.key.id>>63)==0;
                if(c.location.key==key||group||(!diffuse&&!c.diffuse&&loops&&sameOwner&&Length(Sub(c.location.position,send.position))<.08f)){match=i;break;}
            }
            if(match==count){if(count==candidates.size())continue;auto& c=candidates[count++];c.location={key,send.position,true};c.diffuse=diffuse;}
            auto& c=candidates[match];if(diffuse){c.location.key.id=uint64_t(1)<<62;c.location.position=Scale(Add(Scale(c.location.position,c.level),Scale(send.position,send.level)),1/(c.level+send.level));}
            else c.location.key.id=std::min(c.location.key.id,key.id);
            c.level+=send.level;for(uint32_t i=0;i<Samples;i++)c.mono[i]+=send.mono[i];
        }
        for(uint32_t i=0;i<count;i++){float d=Length(Sub(candidates[i].location.position,pose.p));candidates[i].weight=candidates[i].level/(1+d*d);}
        std::array<int,Sources> chosen;chosen.fill(-1);
        auto compatible=[&](uint32_t slot,const Candidate& c){
            bool event=(c.location.key.id>>63)!=0;
            return slot==Sources-1?c.diffuse:slot==Sources-2?(!c.diffuse&&event):(!c.diffuse&&!event);
        };
        // Keep selected physical sources in their lanes while they persist.
        for(uint32_t slot=0;slot<Sources;slot++)for(uint32_t i=0;i<count;i++)if(!candidates[i].used&&compatible(slot,candidates[i])&&locations[slot].valid&&locations[slot].key==candidates[i].location.key){chosen[slot]=int(i);candidates[i].used=true;break;}
        for(uint32_t slot=0;slot<Sources;slot++)if(chosen[slot]<0){
            int best=-1;float weight=-1;for(uint32_t i=0;i<count;i++)if(!candidates[i].used&&compatible(slot,candidates[i])&&candidates[i].weight>weight){best=int(i);weight=candidates[i].weight;}
            if(best>=0){chosen[slot]=best;candidates[best].used=true;}
        }
        // Replace a persistent representative only for a substantially stronger
        // candidate. The reserved event lane cannot be occupied by cartridge loops.
        for(uint32_t slot=0;slot<Sources;slot++)if(chosen[slot]>=0){
            int best=chosen[slot];for(uint32_t i=0;i<count;i++)if(!candidates[i].used&&compatible(slot,candidates[i])&&candidates[i].weight>candidates[best].weight*1.5f)best=int(i);
            if(best!=chosen[slot]){candidates[chosen[slot]].used=false;candidates[best].used=true;chosen[slot]=best;}
        }
        for(uint32_t slot=0;slot<Sources;slot++){
            inputs[slot].fill(0);if(chosen[slot]<0){
                hold[slot]=hold[slot]>Samples?hold[slot]-Samples:0;if(!hold[slot])locations[slot].valid=false;continue;
            }hold[slot]=IRSize;
            const auto& c=candidates[chosen[slot]];auto next=c.location;
            if(c.diffuse&&locations[slot].valid&&locations[slot].key==next.key)next.position=Add(locations[slot].position,Scale(Sub(next.position,locations[slot].position),1-std::exp(-float(Samples)/(48000.f*.08f))));
            locations[slot]=next;inputs[slot]=c.mono;
        }
    }
    void Process(const Pose& pose,std::span<const ReverbSend> sends,float* output) {
        auto before=locations;SelectSources(pose,sends);Record& record=recent[++serial%RecentBlocks];record={pose,locations,inputs,serial};records.push(record);
        bool changed=false;for(uint32_t i=0;i<Sources;i++)if(locations[i].valid&&(!before[i].valid||!(before[i].key==locations[i].key)))changed=true;
        if(changed)SetEvent(wakeEvent);
        if(previous<0&&warming<0&&(current<0||blend>=1)){int newest=-1;uint64_t version=0;for(int i=0;i<3;i++)if(banks[i].state.load(std::memory_order_acquire)==Ready&&banks[i].version.load(std::memory_order_relaxed)>version){newest=i;version=banks[i].version.load(std::memory_order_relaxed);}
            if(newest>=0){warming=newest;banks[warming].state.store(Active,std::memory_order_release);
                for(int i=0;i<3;i++)if(i!=current&&i!=warming&&banks[i].state.load(std::memory_order_acquire)==Ready)banks[i].state.store(Free,std::memory_order_release);}}
        if(warming>=0){
            auto& b=banks[warming];float* tmp[]{temporary[0].data(),temporary[1].data(),temporary[2].data(),temporary[3].data()};IPLAudioBuffer out{int(Channels),int(Samples),tmp};
            // Catch up at most four blocks per callback. Simulation and full-history
            // convolution warmup stay on the worker, even under irregular scheduling.
            if(serial-b.processed>=RecentBlocks){b.state.store(Free,std::memory_order_release);warming=-1;}
            else {
                uint32_t advanced=0;while(b.processed+1<serial&&advanced++<4){uint64_t n=++b.processed;ApplyRecord(b,recent[n%RecentBlocks],n,out);}
                if(b.processed+1==serial){previous=current;current=warming;warming=-1;blend=0;}
            }
        }
        if(current<0)return;for(auto& channel:mix)channel.fill(0);
        float* tmp[]{temporary[0].data(),temporary[1].data(),temporary[2].data(),temporary[3].data()};IPLAudioBuffer temp{int(Channels),int(Samples),tmp};
        float next=std::min(1.f,blend+float(Samples)/(48000.f*.12f));
        for(int bank:{previous,current})if(bank>=0)for(uint32_t slot=0;slot<Sources;slot++){
            auto& b=banks[bank];if(!b.locations[slot].valid)continue;
            bool matched=locations[slot].valid&&locations[slot].key==b.locations[slot].key;
            float* inputChannels[]{matched?inputs[slot].data():silence.data()};IPLAudioBuffer input{1,int(Samples),inputChannels};
            iplReflectionEffectApply(b.effects[slot],&b.parameters[slot],&input,&temp,nullptr);
            for(uint32_t c=0;c<Channels;c++)for(uint32_t i=0;i<Samples;i++){float gain=blend+(next-blend)*float(i+1)/Samples;if(bank==previous)gain=1-gain;
                // The swarm lane is diffuse: retain only the omnidirectional component.
                if(slot!=Sources-1||c==0)mix[c][i]+=temporary[c][i]*gain*(slot==Sources-1?.35f:1.f);
            }
        }
        for(int bank:{previous,current})if(bank>=0)banks[bank].processed=serial;
        blend=next;if(previous>=0&&blend>=1){banks[previous].state.store(Free,std::memory_order_release);previous=-1;}
        float* mixed[]{mix[0].data(),mix[1].data(),mix[2].data(),mix[3].data()};float* stereo[]{left.data(),right.data()};IPLAudioBuffer ambi{int(Channels),int(Samples),mixed},ears{2,int(Samples),stereo};IPLAmbisonicsDecodeEffectParams params{};params.order=1;params.hrtf=hrtf;params.orientation=Coordinates(pose);params.binaural=IPL_TRUE;iplAmbisonicsDecodeEffectApply(decoder,&params,&ambi,&ears);
        for(uint32_t i=0;i<Samples;i++){if(std::isfinite(left[i]))output[i*2]+=.12f*left[i];if(std::isfinite(right[i]))output[i*2+1]+=.12f*right[i];}
    }
    ~Impl(){if(stopEvent)SetEvent(stopEvent);if(thread.joinable())thread.join();for(auto& b:banks){for(auto& effect:b.effects)if(effect)iplReflectionEffectRelease(&effect);for(auto& source:b.sources)if(source)iplSourceRelease(&source);if(b.simulator)iplSimulatorRelease(&b.simulator);}if(decoder)iplAmbisonicsDecodeEffectRelease(&decoder);if(geometry)iplStaticMeshRelease(&geometry);if(scene)iplSceneRelease(&scene);if(stopEvent)CloseHandle(stopEvent);if(wakeEvent)CloseHandle(wakeEvent);}
};
Reverb::Reverb():impl_(std::make_unique<Impl>()){}Reverb::~Reverb()=default;
void Reverb::Initialize(IPLContext c,IPLHRTF h,const SceneData& s,uint32_t n){impl_->Initialize(c,h,s,n);}
void Reverb::Process(const Pose& p,std::span<const ReverbSend> sends,float* stereo){impl_->Process(p,sends,stereo);}
}
