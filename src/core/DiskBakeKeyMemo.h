#pragma once
#include "DiskBake.h"
#include <cwchar>

namespace OASIS::DiskBakeKeyMemo {
namespace fs=std::filesystem;
using Digest=std::array<unsigned char,32>;
struct Counters {
    std::atomic<uint64_t> hits{},misses{},unavailable{},recordsWritten{};
};
inline Counters stats;
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
inline bool Identity(DiskBake::Hash& hash,const fs::path& path){
    const auto& name=path.native();
    const uint64_t chars=name.size();
    if(!hash.Add(&chars,sizeof(chars))||!hash.Add(name.data(),name.size()*sizeof(wchar_t)))return false;
    Handle file;
    if(!path.empty())file.value=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(file.value==INVALID_HANDLE_VALUE){
        const auto error=path.empty()?ERROR_FILE_NOT_FOUND:GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return false;
        const uint8_t present=0;return hash.Add(&present,sizeof(present));
    }
    // On other filesystems the change token may not reliably detect a
    // same-size edit with a restored last-write timestamp. Hash bytes instead.
    wchar_t filesystem[32]{};
    if(!GetVolumeInformationByHandleW(file.value,nullptr,0,nullptr,nullptr,nullptr,
        filesystem,DWORD(std::size(filesystem)))||
        (_wcsicmp(filesystem,L"NTFS")!=0&&_wcsicmp(filesystem,L"ReFS")!=0))return false;
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    FILE_ID_INFO id{};
    if(!GetFileInformationByHandleEx(file.value,FileBasicInfo,&basic,sizeof(basic))||
       !GetFileInformationByHandleEx(file.value,FileStandardInfo,&standard,sizeof(standard))||
       !GetFileInformationByHandleEx(file.value,FileIdInfo,&id,sizeof(id))||
       standard.Directory||standard.DeletePending||!basic.ChangeTime.QuadPart)return false;
    const uint8_t present=1;
    return hash.Add(&present,sizeof(present))&&
        hash.Add(&id.VolumeSerialNumber,sizeof(id.VolumeSerialNumber))&&
        hash.Add(id.FileId.Identifier,sizeof(id.FileId.Identifier))&&
        hash.Add(&standard.EndOfFile.QuadPart,sizeof(standard.EndOfFile.QuadPart))&&
        hash.Add(&basic.CreationTime.QuadPart,sizeof(basic.CreationTime.QuadPart))&&
        hash.Add(&basic.LastWriteTime.QuadPart,sizeof(basic.LastWriteTime.QuadPart))&&
        hash.Add(&basic.ChangeTime.QuadPart,sizeof(basic.ChangeTime.QuadPart));
}
inline bool Signature(const wchar_t* group,const char* version,const std::vector<fs::path>& files,Digest& digest){
    DiskBake::Hash hash;
    const uint64_t groupChars=std::wcslen(group),versionBytes=std::strlen(version),count=files.size();
    if(!hash.Add(&groupChars,sizeof(groupChars))||!hash.Add(group,size_t(groupChars)*sizeof(wchar_t))||
       !hash.Add(&versionBytes,sizeof(versionBytes))||!hash.Add(version,size_t(versionBytes))||
       !hash.Add(&count,sizeof(count)))return false;
    for(const auto& path:files)if(!Identity(hash,path))return false;
    return hash.Finish(digest);
}
inline std::wstring Hex(const Digest& digest){
    static constexpr wchar_t hex[]=L"0123456789abcdef";
    std::wstring name;name.reserve(64);
    for(auto c:digest){name+=hex[c>>4];name+=hex[c&15];}
    return name;
}
inline fs::path Key(const wchar_t* group,const char* version,const std::vector<fs::path>& files){
    wchar_t root[32768]{};
    const DWORD n=GetEnvironmentVariableW(L"OASIS_BAKE_CACHE",root,DWORD(std::size(root)));
    if(!n||n>=std::size(root))return {};
    Digest before{},after{};
    if(!Signature(group,version,files,before)){
        ++stats.unavailable;return DiskBake::Key(group,version,files);
    }
    const auto cacheRoot=fs::path(root);
    const auto record=cacheRoot/L"key-memo-v1"/group/(Hex(before)+L".bin");
    Digest stored{};
    {
        DiskBake::Reader in(record);
        if(in.Pod(stored)&&in.Finish()){
            ++stats.hits;return cacheRoot/group/(Hex(stored)+L".bin");
        }
    }
    ++stats.misses;
    // Preserve the original content-addressed bake name exactly; never
    // substitute metadata for the actual asset content key.
    const auto key=DiskBake::Key(group,version,files);
    if(key.empty())return {};
    if(!Signature(group,version,files,after)||before!=after)return {};
    const auto name=key.stem().native();
    if(name.size()!=64)return key;
    auto digit=[](wchar_t c)->int{return c>=L'0'&&c<=L'9'?int(c-L'0'):c>=L'a'&&c<=L'f'?int(c-L'a')+10:-1;};
    for(size_t i=0;i<stored.size();++i){
        const int hi=digit(name[i*2]),lo=digit(name[i*2+1]);
        if(hi<0||lo<0)return key;
        stored[i]=static_cast<unsigned char>((hi<<4)|lo);
    }
    DiskBake::Writer out(record);
    if(out.Pod(stored)&&out.Finish())++stats.recordsWritten;
    return key;
}
}
