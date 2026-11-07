#include "PerformanceCapture.h"
#include "LabAudio.h"
#include "BuildConfig.h"
#include "BuildVersion.h"
#include "ConsoleSpec.h"
#include "SessionLog.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <map>
#include <numeric>
#include <sstream>
#include <string_view>
#include <fstream>
#include <limits>

namespace lab {
namespace {
struct NumericStats {
    uint64_t samples{};
    double average{},p50{},p95{},p99{},maximum{};
};
NumericStats Stats(std::vector<double> values){
    if(values.empty())return {};
    std::sort(values.begin(),values.end());
    auto percentile=[&](double p){return values[size_t(std::ceil(p*double(values.size())))-1];};
    return {uint64_t(values.size()),std::accumulate(values.begin(),values.end(),0.0)/double(values.size()),
        percentile(.50),percentile(.95),percentile(.99),values.back()};
}
struct SteadyStateStats {
    uint64_t baselineFrames{},telemetryFrames{};
    double baselineP50Ms{},telemetryP50Ms{},extraMs{},costPercent{};
    uint64_t telemetryMissedPeriods{},telemetryStallFrames{};
};
struct StallFrame {
    uint64_t frameId{};
    double displayIntervalMs{};
    uint32_t missedPeriods{};
    double activeFrameMs{},gpuSpanMs{};
    std::string dominantCpuStage;
    double dominantCpuStageMs{};
    std::string dominantGpuPass;
    double dominantGpuPassMs{};
};
constexpr const char* CpuStageNames[]{"world.frame","home.update","physics.advance","audio_events","graphics.prepare","xr.late_acquire","graphics.render_submit","xr.end_frame"};
constexpr const char* XrStageNames[]{"xr.wait_frame","xr.begin_frame","xr.sync_actions","xr.locate_views"};
uint32_t DisplayPeriods(double intervalMs,double periodMs){
    if(intervalMs<=0||periodMs<=0)return 1;
    return uint32_t(std::clamp<long long>(std::llround(intervalMs/periodMs),1,std::numeric_limits<uint32_t>::max()));
}
double Median(std::vector<double> values){
    if(values.empty())return 0;std::sort(values.begin(),values.end());size_t middle=values.size()/2;
    return values.size()%2?values[middle]:(values[middle-1]+values[middle])*.5;
}
PerformanceFrameClass Classify(const CpuFrameSample& sample,double deadline){
    bool missed=sample.missedPeriods>0,cpu=sample.activeFrameMs>deadline,gpu=sample.gpuSpanMs>deadline;
    uint32_t causes=uint32_t(missed)+uint32_t(cpu)+uint32_t(gpu);
    if(causes>1)return PerformanceFrameClass::MultiCause;
    if(missed)return PerformanceFrameClass::MissedPeriod;
    if(cpu)return PerformanceFrameClass::CpuStall;
    if(gpu)return PerformanceFrameClass::GpuDeadline;
    return PerformanceFrameClass::Normal;
}
const char* ClassName(PerformanceFrameClass value){
    switch(value){
    case PerformanceFrameClass::MissedPeriod:return "missed_period";
    case PerformanceFrameClass::CpuStall:return "cpu_stall";
    case PerformanceFrameClass::GpuDeadline:return "gpu_deadline";
    case PerformanceFrameClass::MultiCause:return "multi_cause";
    default:return "normal";
    }
}
std::array<double,4> XrValues(const CpuFrameSample& sample){return {sample.xrWaitFrameMs,sample.xrBeginFrameMs,sample.xrSyncActionsMs,sample.xrLocateViewsMs};}
std::string Utf8(const std::wstring& value){
    if(value.empty())return {};
    int size=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(size>0?size_t(size):0,'\0');
    if(size>0)WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),result.data(),size,nullptr,nullptr);
    return result;
}
const char* QueueName(uint32_t queue){static constexpr const char* names[]{"copy","compute","direct"};return names[queue];}
class Json {
    std::ostringstream stream_;
    std::vector<bool> first_;
    bool afterKey_{};
    void Prefix(){
        if(afterKey_){afterKey_=false;return;}
        if(!first_.empty()){if(!first_.back())stream_<<',';first_.back()=false;}
    }
    void String(std::string_view value){
        static constexpr char hex[]="0123456789abcdef";
        stream_<<'"';
        for(unsigned char c:value){
            if(c=='"')stream_<<"\\\"";
            else if(c=='\\')stream_<<"\\\\";
            else if(c<0x20)stream_<<"\\u00"<<hex[c>>4]<<hex[c&15];
            else stream_<<char(c);
        }
        stream_<<'"';
    }
public:
    Json(){stream_.imbue(std::locale::classic());stream_<<std::setprecision(17);}
    void Object(){Prefix();stream_<<'{';first_.push_back(true);}
    void EndObject(){stream_<<'}';first_.pop_back();}
    void Array(){Prefix();stream_<<'[';first_.push_back(true);}
    void EndArray(){stream_<<']';first_.pop_back();}
    void Key(const char* name){Prefix();String(name);stream_<<':';afterKey_=true;}
    void Value(std::string_view value){Prefix();String(value);}
    void Value(const std::string& value){Value(std::string_view(value));}
    void Value(const char* value){Value(std::string_view(value));}
    void Value(uint64_t value){Prefix();stream_<<value;}
    void Value(int64_t value){Prefix();stream_<<value;}
    void Value(uint32_t value){Prefix();stream_<<value;}
    void Value(double value){Prefix();stream_<<(std::isfinite(value)?value:0.0);}
    void Value(bool value){Prefix();stream_<<(value?"true":"false");}
    template<class T>void Field(const char* name,const T& value){Key(name);Value(value);}
    std::string Finish(){stream_<<'\n';return stream_.str();}
};
void WriteStatsFields(Json& json,const NumericStats& s){
    json.Field("samples",s.samples);json.Field("average_ms",s.average);json.Field("p50_ms",s.p50);
    json.Field("p95_ms",s.p95);json.Field("p99_ms",s.p99);json.Field("max_ms",s.maximum);
}
void WriteStats(Json& json,const NumericStats& s){json.Object();WriteStatsFields(json,s);json.EndObject();}
void WritePosition(Json& json,const char* key,const Pose& pose){
    json.Key(key);json.Array();json.Value(double(pose.p.x));json.Value(double(pose.p.y));json.Value(double(pose.p.z));json.EndArray();
}
void WriteQuaternion(Json& json,const char* key,const Pose& pose){
    json.Key(key);json.Array();json.Value(double(pose.q.x));json.Value(double(pose.q.y));json.Value(double(pose.q.z));json.Value(double(pose.q.w));json.EndArray();
}
struct Span {
    bool valid{};
    double begin{},end{};
    void Add(const GpuPerformancePassSample& sample){
        if(!valid){begin=sample.startQpc;end=sample.endQpc;valid=true;}
        else {begin=std::min(begin,sample.startQpc);end=std::max(end,sample.endQpc);}
    }
    double Milliseconds(int64_t frequency)const{return valid?(end-begin)*1000.0/double(frequency):0.0;}
};
struct GpuFrame {
    Span frame;
    std::array<Span,3> queues;
    std::vector<const GpuPerformancePassSample*> passes;
};
void EmitCaptureStartTone(const XRFrame& frame,Channels& io){
    AudioEvent event{};event.position=frame.input.head.p;event.gain=.12f;event.inputFrameId=frame.input.frameId;
    event.frequency=880.0f;event.decay=.9980f;event.wave=LabWaves::Sine;event.flags=1u;io.audioEvents.push(event);
}
void EmitCaptureCompleteTone(const Pose& pose,uint64_t inputFrameId,Channels& io){
    AudioEvent event{};event.position=pose.p;event.gain=.12f;event.inputFrameId=inputFrameId;
    event.frequency=1320.0f;event.decay=.9970f;event.wave=LabWaves::Sine;event.flags=1u;io.audioEvents.push(event);
}
void EmitCaptureErrorTone(const Pose& pose,uint64_t inputFrameId,Channels& io){
    AudioEvent event{};event.position=pose.p;event.gain=.12f;event.inputFrameId=inputFrameId;
    event.frequency=220.0f;event.decay=.9970f;event.wave=LabWaves::Sine;event.flags=1u;io.audioEvents.push(event);
}
void WriteXrTiming(Json& json,const CpuFrameSample& sample){
    json.Object();json.Field("wait_frame_ms",sample.xrWaitFrameMs);json.Field("begin_frame_ms",sample.xrBeginFrameMs);
    json.Field("sync_actions_ms",sample.xrSyncActionsMs);json.Field("locate_views_ms",sample.xrLocateViewsMs);json.Field("valid",sample.xrTimingValid);json.EndObject();
}
void WriteGap(Json& json,const GpuGapSample& gap){
    json.Object();json.Field("frame_id",gap.frameId);json.Field("queue",QueueName(uint32_t(gap.queue)));
    json.Field("previous_pass",gap.previousPass);json.Field("next_pass",gap.nextPass);json.Field("gap_ms",gap.gapMs);
    json.Field("boundary",gap.boundary==GpuGapSample::Boundary::IntraBatch?"intra_batch":gap.boundary==GpuGapSample::Boundary::CrossQueue?"cross_queue":"inter_batch");json.EndObject();
}
void WritePercentiles(Json& json,const NumericStats& stats){
    json.Object();json.Field("p50_ms",stats.p50);json.Field("p95_ms",stats.p95);json.Field("p99_ms",stats.p99);json.EndObject();
}
}

PerformanceCapture::CpuScope::CpuScope(PerformanceCapture* capture,CpuStage stage):stage_(stage){
    if(capture&&capture->state_==State::Telemetry){capture_=capture;LARGE_INTEGER now{};QueryPerformanceCounter(&now);started_=now.QuadPart;}
}
PerformanceCapture::CpuScope::~CpuScope(){
    if(capture_&&capture_->state_==State::Telemetry){
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        capture_->currentCpu_.milliseconds[uint32_t(stage_)]+=double(now.QuadPart-started_)*1000.0/double(capture_->qpcFrequency_);
    }
}
void PerformanceCapture::Segment::Reset(){
    firstFrame=lastFrame=0;firstDisplayTime=lastDisplayTime=previousDisplayTime=0;firstDisplayPeriod=0;
    frames=renderRequested=submitted=tracked=skippedDisplayPeriods=0;intervalStartDisplayTime=0;displayIntervalsMs.clear();normalIntervalsMs.clear();
}
double PerformanceCapture::Segment::Add(const XRFrame& frame){
    double interval=0;
    if(!frames){firstFrame=frame.input.frameId;firstDisplayTime=frame.displayTime;firstDisplayPeriod=frame.displayPeriod;
        intervalStartDisplayTime=previousDisplayTime?previousDisplayTime:frame.displayTime-frame.displayPeriod;}
    if(previousDisplayTime){
        auto delta=frame.displayTime-previousDisplayTime;
        if(delta>0){
            interval=double(delta)/1'000'000.0;displayIntervalsMs.push_back(interval);
            if(frame.displayPeriod>0){
                uint32_t periods=DisplayPeriods(interval,double(frame.displayPeriod)/1'000'000.0);
                skippedDisplayPeriods+=periods-1;
                if(periods==1)normalIntervalsMs.push_back(interval);
            }
        }
    }
    ++frames;renderRequested+=frame.render?1u:0u;submitted+=frame.submitted?1u:0u;tracked+=frame.tracked?1u:0u;
    lastFrame=frame.input.frameId;lastDisplayTime=previousDisplayTime=frame.displayTime;
    return interval;
}
void PerformanceCapture::PrepareStorage(){
    LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)Fail("Performance QPC frequency");
    qpcFrequency_=frequency.QuadPart;
    // Reserve before the beep; the measured window needs no frame-vector growth at VR refresh rates.
    for(auto& segment:segments_){segment.displayIntervalsMs.reserve(1024);segment.normalIntervalsMs.reserve(1024);}
    frames_.reserve(1024);
}
void PerformanceCapture::StartSequence(const XRFrame& frame,const std::string& id,const std::string& name,const std::wstring& scene){
    for(auto& segment:segments_)segment.Reset();frames_.clear();
    worldId_=id;worldName_=name;scene_=scene;++captureSerial_;GetSystemTime(&captureUtc_);
    startHead_=frame.input.head;startFrame_=frame.input.frameId;displayPeriod_=frame.displayPeriod;
    segmentStarted_=frame.displayTime;previousDisplayTime_=0;telemetryPosePending_=false;
}
void PerformanceCapture::BeginFrame(const XRFrame& frame,const std::string& worldId,const std::string& worldName,
                                    const std::wstring& scene,Graphics& graphics,Channels& io){
    lastHeadPose_=frame.input.head;lastInputFrameId_=frame.input.frameId;
    switch(state_){
    case State::Idle:
        if(frame.performanceGestureDown){state_=State::Holding;holdStarted_=frame.input.time;}
        break;
    case State::Holding:
        if(!frame.performanceGestureDown)state_=State::Idle;
        else if(frame.input.time-holdStarted_>=HoldSeconds){
            graphics.PreparePerformanceCapture();PrepareStorage();EmitCaptureStartTone(frame,io);state_=State::AwaitRelease;
        }
        break;
    case State::AwaitRelease:
        if(!frame.performanceGestureDown){StartSequence(frame,worldId,worldName,scene);state_=State::BaselineBefore;}
        break;
    default:break;
    }
    if(state_==State::Telemetry){
        if(telemetryPosePending_){telemetryStartHead_=frame.input.head;displayPeriod_=frame.displayPeriod;telemetryPosePending_=false;}
        currentCpu_={};currentCpu_.frameId=frame.input.frameId;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);activeStarted_=now.QuadPart;
    }
}
void PerformanceCapture::EndFrame(const XRFrame& frame,Graphics& graphics,XR& xr,Channels& io){
    uint32_t segmentIndex;
    switch(state_){
    case State::BaselineBefore:segmentIndex=0;break;
    case State::Telemetry:segmentIndex=1;break;
    case State::BaselineAfter:segmentIndex=2;break;
    default:return;
    }
    if(state_==State::Telemetry){
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        currentCpu_.activeFrameMs=double(now.QuadPart-activeStarted_)*1000.0/double(qpcFrequency_);
        currentCpu_.xrWaitFrameMs=frame.performanceTiming.waitFrameMs;currentCpu_.xrBeginFrameMs=frame.performanceTiming.beginFrameMs;
        currentCpu_.xrSyncActionsMs=frame.performanceTiming.syncActionsMs;currentCpu_.xrLocateViewsMs=frame.performanceTiming.locateViewsMs;
        currentCpu_.xrTimingValid=frame.performanceTiming.valid;
        currentCpu_.displayIntervalMs=previousDisplayTime_&&frame.displayTime>previousDisplayTime_?double(frame.displayTime-previousDisplayTime_)/1'000'000.0:0.0;
        currentCpu_.displayPeriods=DisplayPeriods(currentCpu_.displayIntervalMs,double(frame.displayPeriod)/1'000'000.0);
        currentCpu_.missedPeriods=currentCpu_.displayPeriods-1;
    }
    double interval=segments_[segmentIndex].Add(frame);previousDisplayTime_=frame.displayTime;
    if(state_==State::Telemetry)frames_.push_back({currentCpu_,frame.displayTime,frame.displayPeriod,interval,frame.render,frame.submitted,frame.tracked});
    const double elapsed=double(frame.displayTime-segmentStarted_)/1'000'000'000.0;
    if(state_==State::BaselineBefore&&elapsed>=BaselineSeconds){
        xr.SetPerformanceCaptureActive(true);graphics.BeginPerformanceCapture();
        state_=State::Telemetry;segmentStarted_=frame.displayTime;telemetryPosePending_=true;
        segments_[1].previousDisplayTime=frame.displayTime;
    }
    else if(state_==State::Telemetry&&elapsed>=TelemetrySeconds){
        telemetryEndHead_=frame.input.head;graphics.EndPerformanceCapture();xr.SetPerformanceCaptureActive(false);
        state_=State::BaselineAfter;segmentStarted_=frame.displayTime;segments_[2].previousDisplayTime=frame.displayTime;
    }
    else if(state_==State::BaselineAfter&&elapsed>=BaselineSeconds){
        endHead_=frame.input.head;
        std::vector<GpuPerformancePassSample> samples;std::vector<GpuQueueFrameAnalysis> queues;std::vector<GpuGapSample> gaps;
        graphics.CollectPerformanceCapture(samples,queues,gaps);
        std::filesystem::path path;
        if(WriteCapture(graphics.PerformanceInfo(),samples,queues,gaps,path))EmitCaptureCompleteTone(lastHeadPose_,lastInputFrameId_,io);
        else EmitCaptureErrorTone(lastHeadPose_,lastInputFrameId_,io);
        state_=State::Idle;
    }
}

bool PerformanceCapture::WriteCapture(const GraphicsPerformanceInfo& hardware,const std::vector<GpuPerformancePassSample>& gpuSamples,
    const std::vector<GpuQueueFrameAnalysis>& queueAnalysis,const std::vector<GpuGapSample>& gaps,std::filesystem::path& writtenPath){
    // All sorting, grouping, JSON construction and disk I/O happens after the final witness segment.
    std::array<NumericStats,3> intervals;
    for(uint32_t i=0;i<3;++i)intervals[i]=Stats(segments_[i].displayIntervalsMs);
    std::array<std::vector<double>,uint32_t(CpuStage::Count)> stageValues;
    std::vector<double> activeValues,gpuValues;
    std::array<std::vector<double>,3> queueValues;
    std::array<std::vector<double>,4> xrValues;
    std::array<std::array<std::vector<double>,4>,3> queueMetrics;
    std::array<std::vector<double>,3> batchValues;
    std::array<std::vector<double>,3> launchValues;
    std::map<std::string,std::vector<double>> chartPassValues;
    std::vector<double> telemetryNormalIntervals;
    std::vector<StallFrame> stalls;
    std::array<uint64_t,5> classificationCounts{};
    uint64_t missedPeriodFrames{},totalMissedPeriods{};
    double worstCpu{},worstGpu{};
    std::map<uint64_t,GpuFrame> gpuFrames;
    std::map<std::pair<uint32_t,std::string>,std::vector<double>> passValues;
    for(const auto& sample:gpuSamples){
        auto queue=uint32_t(sample.queue);auto& frame=gpuFrames[sample.frameId];
        frame.frame.Add(sample);frame.queues[queue].Add(sample);frame.passes.push_back(&sample);
        passValues[{queue,sample.name}].push_back(sample.milliseconds);chartPassValues[sample.name].push_back(sample.milliseconds);
    }
    for(const auto& analysis:queueAnalysis){
        auto& values=queueMetrics[uint32_t(analysis.queue)];
        values[0].push_back(analysis.spanMs);values[1].push_back(analysis.workMs);
        values[2].push_back(analysis.gapMs);values[3].push_back(analysis.occupancyPercent);
    }
    for(const auto& [id,frame]:gpuFrames)if(!frame.passes.empty())
        for(uint32_t queue=0;queue<3;++queue){
            batchValues[queue].push_back(frame.passes.front()->batchesPerQueue[queue]);
            if(frame.frame.valid&&frame.queues[queue].valid)
                launchValues[queue].push_back((frame.queues[queue].begin-frame.frame.begin)*1000.0/double(qpcFrequency_));
        }
    for(auto& frame:frames_){
        auto& sample=frame.cpu;activeValues.push_back(sample.activeFrameMs);
        for(uint32_t stage=0;stage<uint32_t(CpuStage::Count);++stage)stageValues[stage].push_back(sample.milliseconds[stage]);
        auto timing=XrValues(sample);for(uint32_t i=0;i<4;++i)if(i<3||sample.xrTimingValid)xrValues[i].push_back(timing[i]);
        auto found=gpuFrames.find(sample.frameId);const GpuFrame* gpu=found==gpuFrames.end()?nullptr:&found->second;
        if(gpu){
            sample.gpuSpanMs=gpu->frame.Milliseconds(qpcFrequency_);gpuValues.push_back(sample.gpuSpanMs);
            for(uint32_t queue=0;queue<3;++queue)if(gpu->queues[queue].valid)queueValues[queue].push_back(gpu->queues[queue].Milliseconds(qpcFrequency_));
        }
        const double periodMs=double(frame.displayPeriod)/1'000'000.0;
        sample.classification=Classify(sample,periodMs);++classificationCounts[uint32_t(sample.classification)];
        totalMissedPeriods+=sample.missedPeriods;missedPeriodFrames+=sample.missedPeriods>0?1u:0u;
        if(sample.displayIntervalMs>0&&sample.displayPeriods==1&&sample.activeFrameMs<periodMs&&sample.gpuSpanMs<periodMs)
            telemetryNormalIntervals.push_back(sample.displayIntervalMs);
        if(sample.classification!=PerformanceFrameClass::Normal){
            StallFrame stall{};stall.frameId=sample.frameId;stall.displayIntervalMs=sample.displayIntervalMs;
            stall.missedPeriods=sample.missedPeriods;stall.activeFrameMs=sample.activeFrameMs;stall.gpuSpanMs=sample.gpuSpanMs;
            stall.dominantCpuStage=CpuStageNames[0];
            for(uint32_t stage=0;stage<uint32_t(CpuStage::Count);++stage)if(sample.milliseconds[stage]>stall.dominantCpuStageMs){
                stall.dominantCpuStage=CpuStageNames[stage];stall.dominantCpuStageMs=sample.milliseconds[stage];
            }
            for(uint32_t i=0;i<4;++i)if(timing[i]>stall.dominantCpuStageMs){stall.dominantCpuStage=XrStageNames[i];stall.dominantCpuStageMs=timing[i];}
            if(gpu)for(const auto* pass:gpu->passes)if(pass->milliseconds>stall.dominantGpuPassMs){stall.dominantGpuPass=pass->name;stall.dominantGpuPassMs=pass->milliseconds;}
            worstCpu=std::max(worstCpu,sample.activeFrameMs);worstGpu=std::max(worstGpu,sample.gpuSpanMs);stalls.push_back(std::move(stall));
        }
    }
    std::stable_sort(stalls.begin(),stalls.end(),[](const auto& a,const auto& b){return a.displayIntervalMs>b.displayIntervalMs;});
    const uint64_t stallCount=uint64_t(stalls.size());const double worstInterval=stalls.empty()?0.0:stalls.front().displayIntervalMs;
    if(stalls.size()>20)stalls.resize(20);
    std::vector<double> baselineNormalIntervals=segments_[0].normalIntervalsMs;
    baselineNormalIntervals.insert(baselineNormalIntervals.end(),segments_[2].normalIntervalsMs.begin(),segments_[2].normalIntervalsMs.end());
    SteadyStateStats steady{};steady.baselineFrames=uint64_t(baselineNormalIntervals.size());steady.telemetryFrames=uint64_t(telemetryNormalIntervals.size());
    steady.baselineP50Ms=Median(std::move(baselineNormalIntervals));steady.telemetryP50Ms=Median(std::move(telemetryNormalIntervals));
    const bool steadyValid=steady.baselineFrames>0&&steady.telemetryFrames>0;
    steady.extraMs=steadyValid?steady.telemetryP50Ms-steady.baselineP50Ms:0.0;
    steady.costPercent=steady.baselineP50Ms>0?steady.extraMs/steady.baselineP50Ms*100.0:0.0;
    steady.telemetryMissedPeriods=totalMissedPeriods;steady.telemetryStallFrames=stallCount;
    std::array<NumericStats,4> xrStats;for(uint32_t i=0;i<4;++i)xrStats[i]=Stats(std::move(xrValues[i]));
    std::array<std::array<NumericStats,4>,3> queueStats;
    for(uint32_t queue=0;queue<3;++queue)for(uint32_t metric=0;metric<4;++metric)queueStats[queue][metric]=Stats(std::move(queueMetrics[queue][metric]));
    std::vector<const GpuGapSample*> largestGaps;largestGaps.reserve(gaps.size());for(const auto& gap:gaps)largestGaps.push_back(&gap);
    std::stable_sort(largestGaps.begin(),largestGaps.end(),[](const auto* a,const auto* b){return a->gapMs>b->gapMs;});
    if(largestGaps.size()>32)largestGaps.resize(32);
    const auto gpuStats=Stats(std::move(gpuValues)),activeStats=Stats(std::move(activeValues));
    const double deadline=double(displayPeriod_)/1'000'000.0;
    const double referenceMs=(intervals[0].average+intervals[2].average)*.5;
    const double extraMs=intervals[1].average-referenceMs;
    char utc[64]{};
    sprintf_s(utc,"%04u%02u%02uT%02u%02u%02u.%03uZ",unsigned(captureUtc_.wYear),unsigned(captureUtc_.wMonth),unsigned(captureUtc_.wDay),
        unsigned(captureUtc_.wHour),unsigned(captureUtc_.wMinute),unsigned(captureUtc_.wSecond),unsigned(captureUtc_.wMilliseconds));
    Json json;json.Object();
    json.Field("schema","labovr-performance-capture");json.Field("schema_version",2u);
    json.Field("engine_version",LAB_ENGINE_VERSION);json.Field("pipeline_abi",LAB_PIPELINE_ABI);
    json.Field("capture_utc",utc);json.Field("capture_serial",captureSerial_);json.Field("qpc_frequency",uint64_t(qpcFrequency_));
    json.Key("world");json.Object();json.Field("id",worldId_);json.Field("name",worldName_);json.Field("scene",Utf8(scene_));json.EndObject();
    json.Key("sequence");json.Object();json.Field("hold_seconds",HoldSeconds);json.Field("baseline_before_seconds",BaselineSeconds);
    json.Field("telemetry_seconds",TelemetrySeconds);json.Field("baseline_after_seconds",BaselineSeconds);json.EndObject();
    json.Key("hardware");json.Object();json.Field("gpu",hardware.adapter);json.Field("dedicated_vram_bytes",hardware.dedicatedVramBytes);
    json.Field("physical_cpu_cores",ConsoleSpec::PhysicalCoreCount());json.Field("cuda_architecture",uint32_t(LAB_CUDA_ARCHITECTURE));
    json.Field("render_width",hardware.renderWidth);json.Field("render_height",hardware.renderHeight);json.Field("view_count",hardware.viewCount);
    json.Field("copy_queue_timestamps",hardware.copyQueueTimestamps);json.EndObject();
    json.Key("capture_quality");json.Object();json.Field("dropped_gpu_passes",hardware.droppedGpuPasses);
    json.Field("gpu_samples_complete",hardware.droppedGpuPasses==0);json.EndObject();
    json.Key("location");json.Object();json.Field("start_frame_id",startFrame_);
    WritePosition(json,"start_head_position",startHead_);WritePosition(json,"telemetry_start_head_position",telemetryStartHead_);
    WritePosition(json,"telemetry_end_head_position",telemetryEndHead_);WritePosition(json,"end_head_position",endHead_);
    WriteQuaternion(json,"start_head_quaternion",startHead_);WriteQuaternion(json,"telemetry_start_head_quaternion",telemetryStartHead_);
    WriteQuaternion(json,"telemetry_end_head_quaternion",telemetryEndHead_);WriteQuaternion(json,"end_head_quaternion",endHead_);
    double dx=double(telemetryEndHead_.p.x)-telemetryStartHead_.p.x,dy=double(telemetryEndHead_.p.y)-telemetryStartHead_.p.y,dz=double(telemetryEndHead_.p.z)-telemetryStartHead_.p.z;
    json.Field("telemetry_head_translation_m",std::sqrt(dx*dx+dy*dy+dz*dz));json.EndObject();
    const char* segmentNames[]{"baseline_before","telemetry","baseline_after"};
    for(uint32_t i=0;i<3;++i){
        const auto& segment=segments_[i];const auto& stats=intervals[i];
        const double duration=double(segment.lastDisplayTime-segment.intervalStartDisplayTime)/1'000'000'000.0;
        const double fps=stats.average>0?1000.0/stats.average:0.0;
        json.Key(segmentNames[i]);json.Object();json.Field("frames",segment.frames);json.Field("submitted_frames",segment.submitted);
        json.Field("render_requested_frames",segment.renderRequested);json.Field("tracked_frames",segment.tracked);
        json.Field("first_frame_id",segment.firstFrame);json.Field("last_frame_id",segment.lastFrame);
        json.Field("first_display_time",int64_t(segment.firstDisplayTime));json.Field("last_display_time",int64_t(segment.lastDisplayTime));
        json.Field("duration_seconds",duration);json.Field("fps",fps);json.Field("submitted_fps",segment.frames?fps*double(segment.submitted)/double(segment.frames):0.0);
        json.Field("average_interval_ms",stats.average);json.Field("p50_interval_ms",stats.p50);json.Field("p95_interval_ms",stats.p95);
        json.Field("p99_interval_ms",stats.p99);json.Field("max_interval_ms",stats.maximum);json.Field("skipped_display_periods",segment.skippedDisplayPeriods);json.EndObject();
    }
    for(const char* key:{"telemetry_cost","telemetry_cost_window_average"}){
    json.Key(key);json.Object();json.Field("baseline_reference_fps",referenceMs>0?1000.0/referenceMs:0.0);
    json.Field("telemetry_fps",intervals[1].average>0?1000.0/intervals[1].average:0.0);
    json.Field("baseline_reference_frame_ms",referenceMs);json.Field("telemetry_frame_ms",intervals[1].average);
    json.Field("extra_frame_ms",extraMs);json.Field("frame_cost_percent",referenceMs>0?extraMs/referenceMs*100.0:0.0);
    json.Field("baseline_skipped_periods",segments_[0].skippedDisplayPeriods+segments_[2].skippedDisplayPeriods);
    json.Field("telemetry_skipped_periods",segments_[1].skippedDisplayPeriods);json.EndObject();
    }
    const auto& stageNames=CpuStageNames;
    json.Key("cpu_stages");json.Array();
    for(uint32_t stage=0;stage<uint32_t(CpuStage::Count);++stage){json.Object();json.Field("name",stageNames[stage]);WriteStatsFields(json,Stats(std::move(stageValues[stage])));json.EndObject();}
    json.Object();json.Field("name","active_frame");WriteStatsFields(json,activeStats);json.EndObject();json.EndArray();
    json.Key("gpu_summary");json.Object();json.Key("frame_span");WriteStats(json,gpuStats);
    for(uint32_t queue=0;queue<3;++queue){std::string key=std::string(QueueName(queue))+"_span";json.Key(key.c_str());WriteStats(json,Stats(std::move(queueValues[queue])));}
    json.Field("deadline_ms",deadline);json.Field("margin_p50_ms",deadline-gpuStats.p50);json.Field("margin_p95_ms",deadline-gpuStats.p95);json.Field("margin_p99_ms",deadline-gpuStats.p99);json.EndObject();
    json.Key("refresh_budget");json.Object();json.Field("refresh_hz",deadline>0?1000.0/deadline:0.0);json.Field("deadline_ms",deadline);
    json.Field("gpu_p50_margin_ms",deadline-gpuStats.p50);json.Field("gpu_p95_margin_ms",deadline-gpuStats.p95);json.Field("gpu_p99_margin_ms",deadline-gpuStats.p99);
    json.Field("cpu_active_p50_margin_ms",deadline-activeStats.p50);json.Field("cpu_active_p95_margin_ms",deadline-activeStats.p95);json.Field("cpu_active_p99_margin_ms",deadline-activeStats.p99);
    for(uint32_t hz:{72u,90u,120u}){std::string key="margin_to_"+std::to_string(hz)+"hz";const double budget=1000.0/double(hz);
        json.Key(key.c_str());json.Object();json.Field("p50_ms",budget-gpuStats.p50);json.Field("p95_ms",budget-gpuStats.p95);json.Field("p99_ms",budget-gpuStats.p99);json.EndObject();}
    json.EndObject();
    json.Key("pacing");json.Object();json.Field("display_period_ms",deadline);json.Field("telemetry_frames",uint64_t(frames_.size()));
    json.Field("normal_frames",classificationCounts[uint32_t(PerformanceFrameClass::Normal)]);json.Field("missed_period_frames",missedPeriodFrames);
    json.Field("total_missed_periods",totalMissedPeriods);
    json.Field("cpu_stall_frames",classificationCounts[uint32_t(PerformanceFrameClass::CpuStall)]);
    json.Field("gpu_deadline_frames",classificationCounts[uint32_t(PerformanceFrameClass::GpuDeadline)]);
    json.Field("multi_cause_frames",classificationCounts[uint32_t(PerformanceFrameClass::MultiCause)]);
    const char* xrKeys[]{"xr_wait_frame","xr_begin_frame","xr_sync_actions","xr_locate_views"};
    for(uint32_t i=0;i<4;++i){json.Key(xrKeys[i]);WriteStats(json,xrStats[i]);}json.EndObject();
    json.Key("steady_state");json.Object();json.Field("valid",steadyValid);
    json.Field("baseline_normal_frames",steady.baselineFrames);json.Field("telemetry_normal_frames",steady.telemetryFrames);
    json.Field("baseline_p50_interval_ms",steady.baselineP50Ms);json.Field("telemetry_p50_interval_ms",steady.telemetryP50Ms);
    json.Field("instrumentation_extra_ms",steady.extraMs);json.Field("instrumentation_cost_percent",steady.costPercent);
    json.Field("telemetry_stall_frames",steady.telemetryStallFrames);json.Field("telemetry_missed_periods",steady.telemetryMissedPeriods);json.EndObject();
    json.Key("stall_summary");json.Object();json.Field("stall_frames",stallCount);json.Field("worst_display_interval_ms",worstInterval);
    json.Field("worst_cpu_active_ms",worstCpu);json.Field("worst_gpu_span_ms",worstGpu);json.Key("frames");json.Array();
    for(const auto& stall:stalls){json.Object();json.Field("frame_id",stall.frameId);json.Field("display_interval_ms",stall.displayIntervalMs);
        json.Field("missed_periods",stall.missedPeriods);json.Field("active_frame_ms",stall.activeFrameMs);json.Field("gpu_span_ms",stall.gpuSpanMs);
        json.Field("dominant_cpu_stage",stall.dominantCpuStage);json.Field("dominant_cpu_stage_ms",stall.dominantCpuStageMs);
        json.Field("dominant_gpu_pass",stall.dominantGpuPass);json.Field("dominant_gpu_pass_ms",stall.dominantGpuPassMs);json.EndObject();}
    json.EndArray();json.EndObject();
    json.Key("gpu_queue_analysis");json.Object();
    for(uint32_t queue=0;queue<3;++queue){const auto& stats=queueStats[queue];json.Key(QueueName(queue));json.Object();
        json.Field("samples",stats[0].samples);json.Field("span_p50_ms",stats[0].p50);json.Field("work_p50_ms",stats[1].p50);
        json.Field("gap_p50_ms",stats[2].p50);json.Field("gap_p95_ms",stats[2].p95);json.Field("occupancy_p50_percent",stats[3].p50);json.EndObject();}json.EndObject();
    json.Key("largest_gaps");json.Array();for(const auto* gap:largestGaps)WriteGap(json,*gap);json.EndArray();
    json.Key("gpu_batch_analysis");json.Object();
    for(uint32_t queue=0;queue<3;++queue){std::string key=std::string(QueueName(queue))+"_batches_per_frame_p50";json.Field(key.c_str(),Stats(std::move(batchValues[queue])).p50);}json.EndObject();
    json.Key("gpu_launch");json.Object();
    const auto directLaunch=Stats(std::move(launchValues[uint32_t(PerformanceQueue::Direct)]));
    json.Field("first_gpu_to_direct_p50_ms",directLaunch.p50);json.Field("first_gpu_to_direct_p95_ms",directLaunch.p95);
    json.Field("first_gpu_to_compute_p50_ms",Stats(std::move(launchValues[uint32_t(PerformanceQueue::Compute)])).p50);
    json.Field("first_gpu_to_copy_p50_ms",Stats(std::move(launchValues[uint32_t(PerformanceQueue::Copy)])).p50);json.EndObject();
    json.Key("gpu_gaps");json.Array();for(const auto& gap:gaps)WriteGap(json,gap);json.EndArray();
    json.Key("chart_data");json.Object();json.Key("frame_budget_ms");json.Object();
    json.Field("72hz",1000.0/72.0);json.Field("90hz",1000.0/90.0);json.Field("120hz",1000.0/120.0);json.EndObject();
    json.Key("gpu_frame");WritePercentiles(json,gpuStats);json.Key("cpu_active");WritePercentiles(json,activeStats);json.Key("xr_wait");WritePercentiles(json,xrStats[0]);
    json.Key("gpu_pass_p50_ms");json.Object();for(const auto& [name,values]:chartPassValues)json.Field(name.c_str(),Stats(values).p50);json.EndObject();
    json.Key("gpu_queue_work_p50_ms");json.Object();for(uint32_t queue=0;queue<3;++queue)json.Field(QueueName(queue),queueStats[queue][1].p50);json.EndObject();
    json.Key("gpu_queue_gap_p50_ms");json.Object();for(uint32_t queue=0;queue<3;++queue)json.Field(QueueName(queue),queueStats[queue][2].p50);json.EndObject();json.EndObject();
    json.Key("telemetry_timeline");json.Array();
    for(const auto& frame:frames_){const auto& sample=frame.cpu;json.Object();json.Field("frame_id",sample.frameId);
        json.Field("time_ms",double(frame.displayTime-frames_.front().displayTime)/1'000'000.0);
        json.Field("display_interval_ms",sample.displayIntervalMs);json.Field("display_period_ms",double(frame.displayPeriod)/1'000'000.0);
        json.Field("display_periods",sample.displayPeriods);json.Field("missed_periods",sample.missedPeriods);json.Field("classification",ClassName(sample.classification));
        json.Field("cpu_active_ms",sample.activeFrameMs);json.Field("gpu_span_ms",sample.gpuSpanMs);json.Key("xr");WriteXrTiming(json,sample);json.EndObject();}json.EndArray();
    json.Key("gpu_passes");json.Array();
    for(const auto& [key,values]:passValues){json.Object();json.Field("queue",QueueName(key.first));json.Field("name",key.second);WriteStatsFields(json,Stats(values));json.EndObject();}json.EndArray();
    json.Key("frames");json.Array();
    for(const auto& frame:frames_){
        auto found=gpuFrames.find(frame.cpu.frameId);const GpuFrame* gpu=found==gpuFrames.end()?nullptr:&found->second;
        json.Object();json.Field("frame_id",frame.cpu.frameId);json.Field("display_time",int64_t(frame.displayTime));json.Field("display_period_ns",int64_t(frame.displayPeriod));
        json.Field("display_interval_ms",frame.displayIntervalMs);json.Field("submitted",frame.submitted);json.Field("render_requested",frame.renderRequested);json.Field("tracked",frame.tracked);
        json.Field("cpu_active_ms",frame.cpu.activeFrameMs);json.Field("gpu_span_ms",gpu?gpu->frame.Milliseconds(qpcFrequency_):0.0);
        json.Field("gpu_samples_available",gpu!=nullptr);
        json.Field("display_periods",frame.cpu.displayPeriods);json.Field("missed_periods",frame.cpu.missedPeriods);
        json.Field("classification",ClassName(frame.cpu.classification));json.Key("xr");WriteXrTiming(json,frame.cpu);
        json.Key("gpu_queue_analysis");json.Array();for(const auto& analysis:queueAnalysis)if(analysis.frameId==frame.cpu.frameId){
            json.Object();json.Field("queue",QueueName(uint32_t(analysis.queue)));json.Field("span_ms",analysis.spanMs);json.Field("work_ms",analysis.workMs);
            json.Field("gap_ms",analysis.gapMs);json.Field("occupancy_percent",analysis.occupancyPercent);json.EndObject();}json.EndArray();
        json.Key("cpu_stages_ms");json.Object();for(uint32_t stage=0;stage<uint32_t(CpuStage::Count);++stage)json.Field(stageNames[stage],frame.cpu.milliseconds[stage]);json.EndObject();
        json.Key("gpu_queue_span_ms");json.Object();for(uint32_t queue=0;queue<3;++queue)json.Field(QueueName(queue),gpu?gpu->queues[queue].Milliseconds(qpcFrequency_):0.0);json.EndObject();
        json.Key("gpu");json.Array();if(gpu)for(const auto* sample:gpu->passes){json.Object();json.Field("queue",QueueName(uint32_t(sample->queue)));json.Field("pass",sample->name);
            json.Field("batch_index",sample->batchIndex);json.Field("node_index",sample->nodeIndex);
            json.Field("start_qpc",sample->startQpc);json.Field("end_qpc",sample->endQpc);json.Field("ms",sample->milliseconds);json.EndObject();}
        json.EndArray();json.EndObject();
    }
    json.EndArray();
    json.Key("capture_completion");json.Object();json.Field("gpu_samples_complete",hardware.droppedGpuPasses==0);
    json.Field("gpu_samples_dropped",hardware.droppedGpuPasses);json.Field("json_schema_version",2u);json.EndObject();
    json.EndObject();const std::string data=json.Finish();
    std::string safeWorld=worldId_;
    for(char& c:safeWorld)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'))c='_';
    if(safeWorld.empty())safeWorld="world";
    // Windows filenames have a component limit; catalog IDs need not be path-sized.
    if(safeWorld.size()>96)safeWorld.resize(96);
    std::ostringstream filename;filename.imbue(std::locale::classic());
    filename<<"perf_v2_"<<LAB_ENGINE_VERSION<<'_'<<utc<<'_'<<safeWorld<<'_'<<std::setw(3)<<std::setfill('0')<<captureSerial_<<".json";
    const std::filesystem::path directory=AssetPath(L"perf");std::error_code error;
    std::filesystem::create_directories(directory,error);
    if(error){OutputDebugStringW(L"LaboVR performance capture: cannot create perf directory\n");return false;}
    auto path=directory/filename.str();
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    if(!file){OutputDebugStringW(L"LaboVR performance capture: cannot create JSON file\n");return false;}
    file.write(data.data(),std::streamsize(data.size()));file.flush();
    const bool valid=file.good();file.close();
    if(!valid||file.fail()){
        std::error_code ignored;std::filesystem::remove(path,ignored);
        OutputDebugStringW(L"LaboVR performance capture: JSON write or close failed\n");return false;
    }
    writtenPath=path;
    LogEvent(startFrame_,"performance.capture","file=%s frames=%llu gpu_passes=%llu",filename.str().c_str(),static_cast<unsigned long long>(frames_.size()),static_cast<unsigned long long>(gpuSamples.size()));
    OutputDebugStringW((L"LaboVR performance capture: "+path.wstring()+L"\n").c_str());
    return true;
}
}
