#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <array>
#include <type_traits>
#include <cstdint>
#include <limits>
#include <algorithm>
#include <cstring>
#include <iterator>
#pragma comment(lib,"bcrypt.lib")

namespace OASIS::DiskBake {
namespace fs=std::filesystem;
struct Hash {
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<unsigned char> object;
    bool valid{};
    Hash(){
        DWORD size=0,got=0;
        if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return;
        if(BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&got,0)<0)return;
        object.resize(size);
        valid=BCryptCreateHash(alg,&hash,object.data(),size,nullptr,0,0)>=0;
    }
    Hash(const Hash&)=delete;
    ~Hash(){if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);}
    bool Add(const void* data,size_t n){
        const auto* p=static_cast<const unsigned char*>(data);
        while(valid&&n){
            const ULONG chunk=ULONG((std::min)(n,size_t(64)*1024*1024));
            valid=BCryptHashData(hash,const_cast<PUCHAR>(p),chunk,0)>=0;
            p+=chunk;n-=chunk;
        }
        return valid;
    }
    bool Finish(std::array<unsigned char,32>& out){
        return valid&&BCryptFinishHash(hash,out.data(),ULONG(out.size()),0)>=0;
    }
};
inline fs::path Key(const wchar_t* group,const char* version,const std::vector<fs::path>& files){
    wchar_t root[32768]{};
    const DWORD n=GetEnvironmentVariableW(L"OASIS_BAKE_CACHE",root,DWORD(std::size(root)));
    if(!n||n>=std::size(root))return {};
    Hash hash;
    if(!hash.Add(version,std::strlen(version)))return {};
    std::vector<char> buffer(1024*1024);
    for(const auto& path:files){
        std::error_code error;
        const bool exists=!path.empty()&&fs::exists(path,error);
        if(error)return {};
        const uint8_t present=exists?1:0;
        if(!hash.Add(&present,sizeof(present)))return {};
        if(!exists)continue;
        const auto size=fs::file_size(path,error);
        if(error||!hash.Add(&size,sizeof(size)))return {};
        std::ifstream in(path,std::ios::binary);
        if(!in)return {};
        uintmax_t total=0;
        while(in){
            in.read(buffer.data(),std::streamsize(buffer.size()));
            const auto count=in.gcount();
            if(count>0){if(!hash.Add(buffer.data(),size_t(count)))return {};total+=uintmax_t(count);}
        }
        if(!in.eof()||total!=size)return {};
    }
    std::array<unsigned char,32> digest{};
    if(!hash.Finish(digest))return {};
    static constexpr wchar_t hex[]=L"0123456789abcdef";
    std::wstring name;
    for(auto c:digest){name+=hex[c>>4];name+=hex[c&15];}
    return fs::path(root)/group/(name+L".bin");
}
struct Reader {
    std::ifstream stream;
    Hash hash;
    uint64_t remaining{};
    bool ok{};
    explicit Reader(const fs::path& path):stream(path,std::ios::binary|std::ios::ate){
        if(!stream)return;
        const auto length=stream.tellg();
        if(length<std::streamoff(32))return;
        remaining=uint64_t(length)-32;stream.seekg(0);ok=true;
    }
    bool Bytes(void* p,size_t n){
        if(!ok||n>remaining)return ok=false;
        if(n&&!stream.read(static_cast<char*>(p),std::streamsize(n)))return ok=false;
        remaining-=n;return ok=hash.Add(p,n);
    }
    template<class T> bool Pod(T& x){
        static_assert(std::is_trivially_copyable_v<T>);
        return Bytes(&x,sizeof(x));
    }
    template<class T> bool Vec(std::vector<T>& v){
        static_assert(std::is_trivially_copyable_v<T>);
        uint64_t n{};
        if(!Pod(n)||n>remaining/sizeof(T)||n>v.max_size())return ok=false;
        v.resize(size_t(n));return Bytes(v.data(),v.size()*sizeof(T));
    }
    bool String(std::string& value){
        uint64_t n{};
        if(!Pod(n)||n>remaining||n>1024*1024)return ok=false;
        value.resize(size_t(n));return Bytes(value.data(),value.size());
    }
    bool Count(uint64_t& n,uint64_t max){return Pod(n)&&n<=max;}
    bool Finish(){
        std::array<unsigned char,32> actual{},expected{};
        return ok&&!remaining&&hash.Finish(actual)&&
            bool(stream.read(reinterpret_cast<char*>(expected.data()),expected.size()))&&actual==expected;
    }
};
struct Writer {
    std::ofstream stream;
    fs::path path,temp;
    Hash hash;
    bool ok{};
    explicit Writer(const fs::path& target):path(target){
        if(path.empty())return;
        std::error_code error;fs::create_directories(path.parent_path(),error);
        if(error)return;
        temp=path;temp+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetCurrentThreadId())+L".tmp";
        stream.open(temp,std::ios::binary|std::ios::trunc);ok=bool(stream);
    }
    ~Writer(){
        if(stream.is_open())stream.close();
        if(!temp.empty()){std::error_code error;fs::remove(temp,error);}
    }
    bool Bytes(const void* p,size_t n){
        if(!ok)return false;
        if(n&&!stream.write(static_cast<const char*>(p),std::streamsize(n)))return ok=false;
        return ok=hash.Add(p,n);
    }
    template<class T> bool Pod(const T& x){
        static_assert(std::is_trivially_copyable_v<T>);return Bytes(&x,sizeof(x));
    }
    template<class T> bool Vec(const std::vector<T>& v){
        static_assert(std::is_trivially_copyable_v<T>);
        return Pod(uint64_t(v.size()))&&Bytes(v.data(),v.size()*sizeof(T));
    }
    bool String(const std::string& x){return Pod(uint64_t(x.size()))&&Bytes(x.data(),x.size());}
    bool Finish(){
        std::array<unsigned char,32> digest{};
        if(!ok||!hash.Finish(digest)||!stream.write(reinterpret_cast<const char*>(digest.data()),digest.size()))return false;
        stream.flush();if(!stream)return false;stream.close();if(!stream)return false;
        return MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    }
};
}
