#include "SessionLog.h"
#if LABOVR_DIAGNOSTIC_TRACE
#include <cstdarg>
namespace lab {
namespace {
constexpr uint64_t Capacity=4096;
struct Record {DWORD size{};char text[1024]{};};
struct Slot {std::atomic<uint64_t> sequence{};Record record;};
alignas(64) std::array<Slot,Capacity> slots;
std::atomic<uint64_t> enqueue{},dropped{},writeErrors{};
uint64_t dequeue{};
std::atomic<bool> enabled{},stopping{},compactMode{};
HANDLE file=INVALID_HANDLE_VALUE,worker{};
void Write(const Record& record) noexcept {
    DWORD written=0;
    if(file!=INVALID_HANDLE_VALUE&&(!WriteFile(file,record.text,record.size,&written,nullptr)||written!=record.size))writeErrors.fetch_add(1,std::memory_order_relaxed);
}
Record Format(uint64_t frame,const char* stage,const char* detail) noexcept {
    Record r;
    int n=snprintf(r.text,sizeof(r.text),"%.9f\t%lu\t%llu\t%s\t%s",Clock(),GetCurrentThreadId(),static_cast<unsigned long long>(frame),stage,detail);
    r.size=DWORD(std::clamp(n,0,int(sizeof(r.text)-2)));
    for(DWORD i=0;i<r.size;i++)if(r.text[i]=='\r'||r.text[i]=='\n')r.text[i]=' ';
    r.text[r.size++]='\n';return r;
}
bool Pop(Record& record) noexcept {
    auto& slot=slots[dequeue&(Capacity-1)];
    if(slot.sequence.load(std::memory_order_acquire)!=dequeue+1)return false;
    record=slot.record;slot.sequence.store(dequeue+Capacity,std::memory_order_release);++dequeue;return true;
}
DWORD WINAPI Run(void*) {
    wchar_t threadName[96];swprintf_s(threadName,L"%ls log writer",GetCoreIdentity().diagnosticLabelW);SetThreadDescription(GetCurrentThread(),threadName);double flushed=Clock();
    std::array<char,65536> batch{};DWORD used=0;
    auto flushBatch=[&]{if(!used)return;DWORD written=0;if(!WriteFile(file,batch.data(),used,&written,nullptr)||written!=used)writeErrors.fetch_add(1,std::memory_order_relaxed);used=0;};
    for(;;){Record record;uint32_t count=0;while(count++<Capacity&&Pop(record)){
            if(used+record.size>batch.size())flushBatch();memcpy(batch.data()+used,record.text,record.size);used+=record.size;
        }flushBatch();
        if(stopping.load(std::memory_order_acquire))break;
        if(Clock()-flushed>=1){char detail[128];snprintf(detail,sizeof(detail),"dropped=%llu write_errors=%llu",static_cast<unsigned long long>(dropped.load()),static_cast<unsigned long long>(writeErrors.load()));Write(Format(0,"log.health",detail));FlushFileBuffers(file);flushed=Clock();}Sleep(5);
    }
    Record record;while(Pop(record))Write(record);
    char detail[128];snprintf(detail,sizeof(detail),"dropped=%llu write_errors=%llu",static_cast<unsigned long long>(dropped.load()),static_cast<unsigned long long>(writeErrors.load()));Write(Format(0,"log.close",detail));FlushFileBuffers(file);return 0;
}
}
bool SessionLogEnabled() noexcept{return enabled.load(std::memory_order_relaxed);}
void SetSessionLogCompact(bool compact) noexcept{compactMode.store(compact,std::memory_order_relaxed);}
bool SessionLogCompact() noexcept{return compactMode.load(std::memory_order_relaxed);}
bool KeepCompactStage(const char* stage) noexcept{
    // No per-frame string formatting, queuing or disk writes in the sweep.
    return !std::strncmp(stage,"sweep.",6)||!std::strncmp(stage,"capture.",8)||
        !std::strncmp(stage,"physics.profile.",16)||
        !std::strncmp(stage,"startup.",8)||!std::strncmp(stage,"session.",8)||
        !std::strncmp(stage,"plugin.transition",17)||!std::strncmp(stage,"home.lamp",9)||!std::strncmp(stage,"home.mechanisms",15)||!std::strncmp(stage,"home.vase",9)||
        !std::strncmp(stage,"gpu.managed_refused",19)||
        std::strstr(stage,"error")||std::strstr(stage,"warning")||
        std::strstr(stage,"fatal");
}
void LogEvent(uint64_t frame,const char* stage,const char* format,...) noexcept {
    if(!SessionLogEnabled())return;
    if(compactMode.load(std::memory_order_relaxed)&&!KeepCompactStage(stage))return;
    char detail[768];va_list args;va_start(args,format);vsnprintf(detail,sizeof(detail),format,args);va_end(args);detail[sizeof(detail)-1]=0;
    // Bounded MPSC enqueue: producers never wait for disk or a free slot.
    uint64_t pos=enqueue.load(std::memory_order_relaxed);
    for(unsigned attempt=0;attempt<8;attempt++){
        auto& slot=slots[pos&(Capacity-1)];auto sequence=slot.sequence.load(std::memory_order_acquire);
        if(sequence==pos){if(enqueue.compare_exchange_weak(pos,pos+1,std::memory_order_relaxed)){
            slot.record=Format(frame,stage,detail);slot.sequence.store(pos+1,std::memory_order_release);return;}
        }else if(sequence<pos){break;}else pos=enqueue.load(std::memory_order_relaxed);
    }
    dropped.fetch_add(1,std::memory_order_relaxed);
}
void LogFatal(const char* message) noexcept {
    // Fatal paths append immediately, including before the worker is started.
    if(file==INVALID_HANDLE_VALUE)return;Write(Format(0,"fatal",message));FlushFileBuffers(file);
}
LONG WINAPI LogUnhandledException(EXCEPTION_POINTERS* info) noexcept {
    char detail[160];snprintf(detail,sizeof(detail),"Unhandled exception code=0x%08lX address=%p",static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),info->ExceptionRecord->ExceptionAddress);LogFatal(detail);return EXCEPTION_CONTINUE_SEARCH;
}
void StartSessionLog(const wchar_t* path){
    if(!path||!*path)return;
    file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)Fail("Cannot open requested session log");
    failureSink=LogFatal;SetUnhandledExceptionFilter(LogUnhandledException);
    for(uint64_t i=0;i<Capacity;i++)slots[i].sequence.store(i,std::memory_order_relaxed);
    char session[256];snprintf(session,sizeof(session),"%s=%s schema=1 fields=QPC_seconds,thread_id,frame_id,stage,details",GetCoreIdentity().applicationName,GetCoreIdentity().version);Write(Format(0,"session.open",session));
    Write(Format(0,"render.route","input -> snapshot -> Copy -> Compute(deform,cull) -> Direct(BLAS,TLAS,depth,shadow) -> Compute(HiZ,clusters) -> Direct(lighting,RayQuery,resolve,denoise,ACES) -> OpenXR; GPU timestamp records are delayed measurements, not pixel readbacks"));
    FlushFileBuffers(file);enabled.store(true,std::memory_order_release);
    worker=CreateThread(nullptr,0,Run,nullptr,0,nullptr);if(!worker)Fail("Cannot start session log writer");
}
void StopSessionLog() noexcept {
    enabled.store(false,std::memory_order_release);stopping.store(true,std::memory_order_release);
    if(worker){WaitForSingleObject(worker,INFINITE);CloseHandle(worker);worker=nullptr;}
    failureSink=nullptr;if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);file=INVALID_HANDLE_VALUE;}
}
}

#endif
