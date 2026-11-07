#include "VrCapture.h"
#include "Core.h"
#include "SessionLog.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <objbase.h>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <deque>
#include <map>
#include <thread>
#include <algorithm>
#include <cstdio>
namespace lab {
namespace {
using Microsoft::WRL::ComPtr;
std::string Utf8(const std::wstring& w){if(w.empty())return {};int n=WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);std::string s(n,'\0');if(n)WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),s.data(),n,nullptr,nullptr);return s;}
}
struct VrCapture::Impl {
    struct Job {VrCaptureTicket ticket;LabU64 frame{};uint32_t width{},height{};bool bgra{};std::vector<unsigned char> pixels;};
    std::filesystem::path directory,fallbackDirectory;
    mutable std::mutex mutex;std::condition_variable cv;bool stop{};LabU64 next{1};
    std::map<LabU64,LabCaptureStatusV1> status;std::deque<VrCaptureTicket> pending;std::deque<Job> jobs;std::thread worker;
    Impl(){
        wchar_t path[32768]{};DWORD n=GetModuleFileNameW(nullptr,path,DWORD(std::size(path)));
        if(n&&n<std::size(path))fallbackDirectory=std::filesystem::path(path).parent_path()/L"Captures";
        n=GetEnvironmentVariableW(L"LOCALAPPDATA",path,DWORD(std::size(path)));
        if(n&&n<std::size(path))directory=std::filesystem::path(path)/L"ConsoleVR"/L"Captures";
        // Explicit per-run destination, used only when a diagnostic launcher opts in.
        n=GetEnvironmentVariableW(L"CONSOLEVR_CAPTURE_DIR",path,DWORD(std::size(path)));
        if(n&&n<std::size(path))directory=std::filesystem::path(path);
        if(directory.empty())directory=fallbackDirectory;
        worker=std::thread([this]{Run();});
    }
    ~Impl(){{std::lock_guard lock(mutex);stop=true;}cv.notify_one();worker.join();}
    void Set(const VrCaptureTicket& t,LabU32 state,const char* msg,const std::wstring& file={}){std::lock_guard lock(mutex);auto it=status.find(t.owner);if(it==status.end()||it->second.requestId!=t.request)return;it->second.state=state;snprintf(it->second.message,sizeof(it->second.message),"%s",msg);auto s=Utf8(file);snprintf(it->second.fileUtf8,sizeof(it->second.fileUtf8),"%s",s.c_str());
        if(!file.empty()){auto dir=Utf8(std::filesystem::path(file).parent_path().wstring());snprintf(it->second.directoryUtf8,sizeof(it->second.directoryUtf8),"%s",dir.c_str());}
        LogEvent(it->second.frameId,"capture.status","request=%llu state=%u message=%s file=%s",static_cast<unsigned long long>(t.request),state,msg,s.c_str());
    }
    bool Save(Job& j,std::wstring& file,std::string& error){
        auto checked=[&](HRESULT hr,const char* stage){if(SUCCEEDED(hr))return true;char message[128];snprintf(message,sizeof(message),"%s / HRESULT 0x%08lX",stage,static_cast<unsigned long>(hr));error=message;return false;};
        auto winError=[&](const char* stage,DWORD code){char message[128];snprintf(message,sizeof(message),"%s / WIN32 %lu",stage,static_cast<unsigned long>(code));error=message;};
        if(!j.width||!j.height||uint64_t(j.width)*j.height*4!=j.pixels.size()||j.pixels.size()>MAXDWORD){error="INVALID CAPTURE PIXEL BUFFER";return false;}
        ComPtr<IWICImagingFactory> factory;ComPtr<IStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> props;
        if(!checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC FACTORY")||
           !checked(CreateStreamOnHGlobal(nullptr,TRUE,&stream),"PNG MEMORY STREAM")||
           !checked(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG ENCODER")||
           !checked(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG INITIALIZE")||
           !checked(encoder->CreateNewFrame(&frame,&props),"PNG FRAME")||
           !checked(frame->Initialize(props.Get()),"PNG FRAME INITIALIZE")||
           !checked(frame->SetSize(j.width,j.height),"PNG SIZE"))return false;
        // OpenXR's final eye is an opaque image, even if its alpha channel is unused.
        for(size_t i=0;i<j.pixels.size();i+=4){if(!j.bgra)std::swap(j.pixels[i],j.pixels[i+2]);j.pixels[i+3]=255;}
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
        if(!checked(frame->SetPixelFormat(&format),"PNG PIXEL FORMAT"))return false;
        if(!IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA)){error="PNG BGRA FORMAT UNAVAILABLE";return false;}
        if(!checked(frame->WritePixels(j.height,j.width*4,UINT(j.pixels.size()),j.pixels.data()),"PNG PIXELS")||
           !checked(frame->Commit(),"PNG FRAME COMMIT")||!checked(encoder->Commit(),"PNG COMMIT"))return false;
        HGLOBAL storage{};STATSTG stat{};
        if(!checked(GetHGlobalFromStream(stream.Get(),&storage),"PNG STORAGE")||!checked(stream->Stat(&stat,STATFLAG_NONAME),"PNG STREAM SIZE"))return false;
        if(!stat.cbSize.QuadPart||stat.cbSize.QuadPart>MAXDWORD){error="INVALID PNG STREAM SIZE";return false;}
        const auto* data=GlobalLock(storage);if(!data){winError("PNG MEMORY LOCK",GetLastError());return false;}
        FILETIME timestamp;GetSystemTimeAsFileTime(&timestamp);ULARGE_INTEGER ticks{};ticks.LowPart=timestamp.dwLowDateTime;ticks.HighPart=timestamp.dwHighDateTime;
        bool written=false;
        for(const auto& destination:{directory,fallbackDirectory}){
            if(destination.empty())continue;
            std::error_code ec;std::filesystem::create_directories(destination,ec);
            if(ec){error="CAPTURE DIRECTORY / "+ec.message();continue;}
            for(uint32_t attempt=0;attempt<10000;attempt++){
                ULARGE_INTEGER candidate=ticks;candidate.QuadPart+=LabU64(attempt)*10000;FILETIME utc{candidate.LowPart,candidate.HighPart},local{};SYSTEMTIME st{};
                if(!FileTimeToLocalFileTime(&utc,&local)||!FileTimeToSystemTime(&local,&st)){winError("CAPTURE TIMESTAMP",GetLastError());break;}
                wchar_t name[128];swprintf_s(name,L"ConsoleVR_%04u-%02u-%02u_%02u-%02u-%02u_%03u.png",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,st.wMilliseconds);
                auto path=destination/name;HANDLE output=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(output==INVALID_HANDLE_VALUE){auto code=GetLastError();if(code==ERROR_FILE_EXISTS||code==ERROR_ALREADY_EXISTS)continue;winError("CREATE PNG",code);break;}
                DWORD n{};written=WriteFile(output,data,DWORD(stat.cbSize.QuadPart),&n,nullptr)!=0;
                if(!written)winError("WRITE PNG",GetLastError());else if(n!=stat.cbSize.QuadPart){written=false;error="INCOMPLETE PNG WRITE";}
                if(written&&!FlushFileBuffers(output)){written=false;winError("FLUSH PNG",GetLastError());}
                if(!CloseHandle(output)&&written){written=false;winError("CLOSE PNG",GetLastError());}
                if(written)file=path.wstring();else DeleteFileW(path.c_str());break;
            }
            if(written)break;
        }
        GlobalUnlock(storage);if(!written&&error.empty())error="CAPTURE DIRECTORY UNAVAILABLE";return written;
    }
    void Run(){HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        for(;;){Job j;{std::unique_lock lock(mutex);cv.wait(lock,[&]{return stop||!jobs.empty();});if(stop&&jobs.empty())break;j=std::move(jobs.front());jobs.pop_front();}
            std::wstring file;std::string error;bool ok=false;
            try{if(SUCCEEDED(com))ok=Save(j,file,error);else{char message[128];snprintf(message,sizeof(message),"WIC COM INITIALIZE / HRESULT 0x%08lX",static_cast<unsigned long>(com));error=message;}}
            catch(const std::exception& e){error="PNG EXCEPTION / "+std::string(e.what());}
            catch(...){error="PNG EXCEPTION";}
            Set(j.ticket,ok?LAB_CAPTURE_SAVED_V1:LAB_CAPTURE_FAILED_V1,ok?"PNG SAVED":error.c_str(),file);
        }if(SUCCEEDED(com))CoUninitialize();
    }

};
VrCapture::VrCapture():impl_(std::make_unique<Impl>()){}VrCapture::~VrCapture()=default;
bool VrCapture::Request(LabU64 owner,const LabCaptureRequestV1& options,LabU64& out){
    out=0;if(!owner||options.structSize<sizeof(options)||options.eye>1||options.includeInterface>1)return false;std::lock_guard lock(impl_->mutex);auto it=impl_->status.find(owner);
    if(it!=impl_->status.end()&&it->second.state>=LAB_CAPTURE_QUEUED_V1&&it->second.state<=LAB_CAPTURE_ENCODING_V1)return false;
    if(impl_->pending.size()+impl_->jobs.size()>=8)return false;
    LabCaptureStatusV1 s{sizeof(s),LAB_CAPTURE_QUEUED_V1};s.requestId=impl_->next++;auto dir=Utf8(impl_->directory.wstring());snprintf(s.directoryUtf8,sizeof(s.directoryUtf8),"%s",dir.c_str());snprintf(s.message,sizeof(s.message),"%s",impl_->directory.empty()?"USER DIRECTORY UNAVAILABLE":"CAPTURE QUEUED");
    if(impl_->directory.empty())s.state=LAB_CAPTURE_FAILED_V1;impl_->status[owner]=s;out=s.requestId;if(s.state==LAB_CAPTURE_FAILED_V1)return false;
    impl_->pending.push_back({owner,out,options,QpcNowSeconds()});LogEvent(0,"capture.request","request=%llu eye=%u interface=%u",static_cast<unsigned long long>(out),options.eye,options.includeInterface);return true;
}
bool VrCapture::Status(LabU64 owner,LabCaptureStatusV1& out)const{if(out.structSize<sizeof(out))return false;std::lock_guard lock(impl_->mutex);auto it=impl_->status.find(owner);if(it!=impl_->status.end())out=it->second;else{out={sizeof(out)};auto dir=Utf8(impl_->directory.wstring());snprintf(out.directoryUtf8,sizeof(out.directoryUtf8),"%s",dir.c_str());}return true;}
bool VrCapture::Take(VrCaptureTicket& out){std::lock_guard lock(impl_->mutex);while(!impl_->pending.empty()){out=impl_->pending.front();impl_->pending.pop_front();auto it=impl_->status.find(out.owner);if(it!=impl_->status.end()&&it->second.requestId==out.request){it->second.state=LAB_CAPTURE_GPU_V1;snprintf(it->second.message,sizeof(it->second.message),"%s","GPU COPY / WAITING FOR FENCE");return true;}}return false;}
void VrCapture::Pump(){
    std::lock_guard lock(impl_->mutex);double now=QpcNowSeconds();
    for(auto it=impl_->pending.begin();it!=impl_->pending.end();){
        auto status=impl_->status.find(it->owner);
        if(status==impl_->status.end()||status->second.requestId!=it->request)it=impl_->pending.erase(it);
        else if(now-it->queuedAt>30){status->second.state=LAB_CAPTURE_FAILED_V1;snprintf(status->second.message,sizeof(status->second.message),"%s","NO RENDERED EYE AVAILABLE");it=impl_->pending.erase(it);}
        else ++it;
    }
}
void VrCapture::Encode(const VrCaptureTicket& t,LabU64 frame,uint32_t w,uint32_t h,bool bgra,std::vector<unsigned char> pixels){
    std::lock_guard lock(impl_->mutex);auto it=impl_->status.find(t.owner);if(it==impl_->status.end()||it->second.requestId!=t.request)return;it->second.state=LAB_CAPTURE_ENCODING_V1;it->second.frameId=frame;snprintf(it->second.message,sizeof(it->second.message),"%s","ENCODING PNG");impl_->jobs.push_back({t,frame,w,h,bgra,std::move(pixels)});impl_->cv.notify_one();
}
void VrCapture::Failed(const VrCaptureTicket& t,const char* message){impl_->Set(t,LAB_CAPTURE_FAILED_V1,message);}
void VrCapture::Clear(LabU64 owner){std::lock_guard lock(impl_->mutex);impl_->status.erase(owner);}
}
