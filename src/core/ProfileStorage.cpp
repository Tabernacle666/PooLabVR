#include "ProfileStorage.h"
#include "Core.h"
#include "Assets.h"
#include "BuildConfig.h"
#include <filesystem>
#include <algorithm>
#include <cstdio>
namespace lab {
namespace {
bool Name(const char* s){if(!s||!*s)return false;size_t n=0;for(;*s;s++,n++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_')||n>63)return false;return true;}
}
LabU32 ProfileStorage::Transfer(bool write,LabU32 scope,const char* name,void* data,LabU32 bytes,LabStorageResultV1* out){
    if(!out||out->structSize<sizeof(*out))return 0;
    *out={sizeof(*out),LAB_STORAGE_INVALID_V1};auto finish=[&](LabU32 status,const char* text){out->status=status;snprintf(out->message,sizeof(out->message),"%s",text);return status==LAB_STORAGE_OK_V1?1u:0u;};
    if(!Name(owner_.c_str())||!Name(name)||scope>LAB_PROFILE_USER_V1||bytes>1024*1024||(write&&(!data||!bytes)))return finish(LAB_STORAGE_INVALID_V1,"INVALID PROFILE REQUEST");
    std::filesystem::path root;std::error_code ec;
    const std::string directory=owner_=="home"?"Home":owner_;
    if(scope==LAB_PROFILE_SYSTEM_V1){
        root=std::filesystem::path(LAB_PROJECT_PROFILE_ROOT_W)/directory;
        if(!std::filesystem::is_directory(root,ec)){
            if(write)return finish(LAB_STORAGE_UNAVAILABLE_V1,"SYSTEM SOURCE UNAVAILABLE");
            root=std::filesystem::path(AssetPath(L"profiles"))/directory;
        }
    }else{
        wchar_t local[32768];DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,DWORD(std::size(local)));
        if(!n||n>=std::size(local))return finish(LAB_STORAGE_UNAVAILABLE_V1,"USER DIRECTORY UNAVAILABLE");
        root=std::filesystem::path(local)/L"ConsoleVR"/directory;
        if(write){std::filesystem::create_directories(root,ec);if(ec)return finish(LAB_STORAGE_IO_V1,"USER DIRECTORY WRITE FAILED");}
    }
    auto path=root/(std::string(name)+".json");
    if(!write){
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return finish(GetLastError()==ERROR_FILE_NOT_FOUND?LAB_STORAGE_MISSING_V1:LAB_STORAGE_IO_V1,"PROFILE READ FAILED");
        LARGE_INTEGER size{};bool ok=GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<=1024*1024;
        if(!ok){CloseHandle(file);return finish(LAB_STORAGE_INVALID_V1,"INVALID PROFILE SIZE");}out->bytes=LabU32(size.QuadPart);
        if(!data){CloseHandle(file);return finish(LAB_STORAGE_OK_V1,"PROFILE SIZE OK");}
        if(bytes<out->bytes){CloseHandle(file);return finish(LAB_STORAGE_CAPACITY_V1,"PROFILE BUFFER TOO SMALL");}
        DWORD got{};ok=ReadFile(file,data,out->bytes,&got,nullptr)&&got==out->bytes;CloseHandle(file);return finish(ok?LAB_STORAGE_OK_V1:LAB_STORAGE_IO_V1,ok?"PROFILE READ OK":"PROFILE READ FAILED");
    }
    // CREATE_NEW plus owner-specific process suffix avoids overwriting an
    // unrelated temp file. A failed write leaves the previous profile intact.
    auto temporary=path;temporary+=L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return finish(LAB_STORAGE_IO_V1,"PROFILE SOURCE NOT WRITABLE");
    DWORD written{};bool ok=WriteFile(file,data,bytes,&written,nullptr)&&written==bytes&&FlushFileBuffers(file);ok=CloseHandle(file)&&ok;
    if(ok)ok=MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if(!ok){DeleteFileW(temporary.c_str());return finish(LAB_STORAGE_IO_V1,"PROFILE ATOMIC WRITE FAILED");}
    out->bytes=bytes;return finish(LAB_STORAGE_OK_V1,"PROFILE SAVED");
}
LabStorageServiceV1 ProfileStorage::Service(){
    LabStorageServiceV1 s{1,sizeof(s),this};
    s.ReadProfile=[](void* c,LabU32 scope,const char* name,void* data,LabU32 n,LabStorageResultV1* out)->LabU32{return c?static_cast<ProfileStorage*>(c)->Transfer(false,scope,name,data,n,out):0;};
    s.WriteProfile=[](void* c,LabU32 scope,const char* name,const void* data,LabU32 n,LabStorageResultV1* out)->LabU32{return c?static_cast<ProfileStorage*>(c)->Transfer(true,scope,name,const_cast<void*>(data),n,out):0;};return s;
}
}
