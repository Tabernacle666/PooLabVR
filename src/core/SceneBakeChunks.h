#pragma once
#include "DiskBake.h"

namespace OASIS::SceneBakeChunks {
namespace fs=std::filesystem;
using Digest=std::array<unsigned char,32>;
// Storage granularity, not a budget or a limit on asset size.
inline constexpr size_t InlineBytes=256u*1024u,ChunkBytes=4u*1024u*1024u;
struct Counters {
    std::atomic<uint64_t> payloadRead{},payloadWritten{},payloadReused{},
        manifestRead{},manifestWritten{},readMs{},writeMs{};
};
inline Counters stats;
inline bool DigestOf(const void* data,size_t bytes,Digest& digest){
    DiskBake::Hash hash;
    return hash.Add(data,bytes)&&hash.Finish(digest);
}
inline fs::path PayloadRoot(const fs::path& manifest){
    return manifest.parent_path().parent_path()/L"scene-payload-v1";
}
inline fs::path PayloadPath(const fs::path& root,const Digest& digest){
    static constexpr wchar_t hex[]=L"0123456789abcdef";
    std::wstring name;name.reserve(64);
    for(auto c:digest){name+=hex[c>>4];name+=hex[c&15];}
    return root/name.substr(0,2)/(name.substr(2)+L".bin");
}
inline bool Store(const fs::path& root,const Digest& digest,const void* data,size_t bytes){
    const auto path=PayloadPath(root,digest);
    std::error_code error;
    if(fs::file_size(path,error)==bytes&&!error){
        stats.payloadReused.fetch_add(bytes,std::memory_order_relaxed);
        return true;
    }
    error.clear();fs::create_directories(path.parent_path(),error);
    if(error)return false;
    auto temp=path;
    temp+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetCurrentThreadId())+L".tmp";
    bool written=false;
    {
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);
        if(out&&out.write(static_cast<const char*>(data),std::streamsize(bytes))){
            stats.payloadWritten.fetch_add(bytes,std::memory_order_relaxed);
            out.close();written=bool(out);
        }
    }
    if(written&&MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        return true;
    fs::remove(temp,error);
    return false;
}
inline bool Load(const fs::path& root,const Digest& digest,void* data,size_t bytes){
    const auto path=PayloadPath(root,digest);
    std::error_code error;
    if(fs::file_size(path,error)!=bytes||error)return false;
    std::ifstream in(path,std::ios::binary);
    if(!in||!in.read(static_cast<char*>(data),std::streamsize(bytes)))return false;
    stats.payloadRead.fetch_add(bytes,std::memory_order_relaxed);
    Digest actual{};
    if(!DigestOf(data,bytes,actual))return false;
    if(actual==digest)return true;
    // Derived cache only. An invalid payload must not be reused by Store.
    in.close();fs::remove(path,error);
    return false;
}
struct Reader : DiskBake::Reader {
    fs::path root;
    explicit Reader(const fs::path& path):DiskBake::Reader(path),root(PayloadRoot(path)){}
    template<class T> bool Vec(std::vector<T>& v){
        static_assert(std::is_trivially_copyable_v<T>);
        uint64_t n{};
        if(!Pod(n)||n>v.max_size()||n>(std::numeric_limits<size_t>::max)()/sizeof(T))return ok=false;
        const size_t bytes=size_t(n)*sizeof(T);
        if(bytes<InlineBytes){
            if(bytes>remaining)return ok=false;
            v.resize(size_t(n));return Bytes(v.data(),bytes);
        }
        const size_t chunks=1+(bytes-1)/ChunkBytes;
        if(chunks>remaining/sizeof(Digest))return ok=false;
        v.resize(size_t(n));
        auto* dst=reinterpret_cast<unsigned char*>(v.data());
        for(size_t offset=0;offset<bytes;){
            Digest digest{};
            const size_t length=(std::min)(ChunkBytes,bytes-offset);
            if(!Bytes(digest.data(),digest.size())||!Load(root,digest,dst+offset,length))return ok=false;
            offset+=length;
        }
        return true;
    }
};
struct Writer : DiskBake::Writer {
    fs::path root;
    explicit Writer(const fs::path& path):DiskBake::Writer(path),root(PayloadRoot(path)){}
    template<class T> bool Vec(const std::vector<T>& v){
        static_assert(std::is_trivially_copyable_v<T>);
        if(!Pod(uint64_t(v.size())))return false;
        const size_t bytes=v.size()*sizeof(T);
        if(bytes<InlineBytes)return Bytes(v.data(),bytes);
        const auto* src=reinterpret_cast<const unsigned char*>(v.data());
        for(size_t offset=0;offset<bytes;){
            Digest digest{};
            const size_t length=(std::min)(ChunkBytes,bytes-offset);
            if(!DigestOf(src+offset,length,digest)||!Store(root,digest,src+offset,length)||
               !Bytes(digest.data(),digest.size()))return ok=false;
            offset+=length;
        }
        return true;
    }
};
struct Timing {
    std::atomic<uint64_t>& target;
    ULONGLONG start=GetTickCount64();
    ~Timing(){target.fetch_add(GetTickCount64()-start,std::memory_order_relaxed);}
};
inline void ManifestBytes(const fs::path& path,std::atomic<uint64_t>& target){
    std::error_code error;const auto bytes=fs::file_size(path,error);
    if(!error)target.fetch_add(bytes,std::memory_order_relaxed);
}
}
