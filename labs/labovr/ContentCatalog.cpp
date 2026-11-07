#include "ContentCatalog.h"
#include "LaboVRPlugin.h"
#include "LaboVRGpuService.h"
#include "SessionLog.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>

namespace lab {
namespace {
std::filesystem::path ExecutableDirectory(){
    std::wstring path(32768,L'\0');DWORD n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!n||n>=path.size())return {};path.resize(n);return std::filesystem::path(path).parent_path();
}
std::optional<std::string> ReadText(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);if(!file)return std::nullopt;
    file.seekg(0,std::ios::end);auto end=file.tellg();if(end==std::streampos(-1))return std::nullopt;auto size=static_cast<std::streamoff>(end);if(size<0||size>1024*1024)return std::nullopt;
    file.seekg(0,std::ios::beg);std::string text(static_cast<size_t>(size),'\0');if(!text.empty())file.read(text.data(),std::streamsize(text.size()));if(!file&&!text.empty())return std::nullopt;return text;
}
void SkipSpace(std::string_view s,size_t& p){while(p<s.size()&&(s[p]==' '||s[p]=='\t'||s[p]=='\r'||s[p]=='\n'))++p;}
bool ParseString(std::string_view s,size_t& p,std::string& out){
    SkipSpace(s,p);if(p>=s.size()||s[p]!='"')return false;++p;out.clear();
    while(p<s.size()){
        char c=s[p++];if(c=='"')return true;if(c=='\\'){
            if(p>=s.size())return false;char e=s[p++];switch(e){case '"':case '\\':case '/':out.push_back(e);break;case 'b':out.push_back('\b');break;case 'f':out.push_back('\f');break;case 'n':out.push_back('\n');break;case 'r':out.push_back('\r');break;case 't':out.push_back('\t');break;default:return false;}
        }else{if(static_cast<unsigned char>(c)<0x20)return false;out.push_back(c);}
    }return false;
}
std::optional<size_t> MemberValue(std::string_view s,std::string_view key,size_t begin=0,size_t end=std::string_view::npos){
    if(end==std::string_view::npos)end=s.size();size_t p=begin;
    while(p<end){SkipSpace(s,p);if(p>=end)break;if(s[p]=='"'){
        size_t q=p;std::string found;if(!ParseString(s,q,found))return std::nullopt;SkipSpace(s,q);
        if(q<end&&s[q]==':'){++q;SkipSpace(s,q);if(found==key)return q;}p=q;
    }else ++p;}return std::nullopt;
}
bool BalancedRange(std::string_view s,size_t p,char open,char close,size_t& contentBegin,size_t& contentEnd){
    SkipSpace(s,p);if(p>=s.size()||s[p]!=open)return false;contentBegin=p+1;int depth=1;bool inString=false,escape=false;
    for(size_t i=p+1;i<s.size();++i){char c=s[i];if(inString){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')inString=false;continue;}if(c=='"'){inString=true;continue;}if(c==open)++depth;else if(c==close&&--depth==0){contentEnd=i;return true;}}
    return false;
}
bool StringMember(std::string_view s,std::string_view key,std::string& out,size_t begin=0,size_t end=std::string_view::npos){auto p=MemberValue(s,key,begin,end);if(!p)return false;size_t q=*p;return ParseString(s,q,out);}
bool UIntMember(std::string_view s,std::string_view key,uint32_t& out){auto p=MemberValue(s,key);if(!p)return false;size_t q=*p;SkipSpace(s,q);auto first=s.data()+q,last=s.data()+s.size();auto [end,ec]=std::from_chars(first,last,out);return ec==std::errc{}&&end!=first;}
bool BoolMember(std::string_view s,std::string_view key,bool& out,size_t begin=0,size_t end=std::string_view::npos){auto p=MemberValue(s,key,begin,end);if(!p)return false;auto q=*p;if(s.substr(q,4)=="true"){out=true;return true;}if(s.substr(q,5)=="false"){out=false;return true;}return false;}
bool StringArrayMember(std::string_view s,std::string_view key,std::vector<std::string>& out){
    auto p=MemberValue(s,key);if(!p)return false;size_t b{},e{};if(!BalancedRange(s,*p,'[',']',b,e))return false;out.clear();size_t q=b;
    while(q<e){SkipSpace(s,q);if(q>=e)break;std::string value;if(!ParseString(s,q,value))return false;out.push_back(std::move(value));SkipSpace(s,q);if(q<e){if(s[q]!=',')return false;++q;}}
    return true;
}
bool SafeRelativeDll(const std::filesystem::path& value){if(value.empty()||value.is_absolute())return false;for(const auto& part:value)if(part==std::filesystem::path(".."))return false;return value.extension()==std::filesystem::path(".dll");}
bool ValidServices(const std::vector<std::string>& services){
    static const std::set<std::string> known{"visual_v1","input_v1","audio_v1","gpu_v1","gpu_interop_v1","physics_v1","storage_v1"};std::set<std::string> seen;
    for(const auto& service:services)if(!known.contains(service)||!seen.insert(service).second)return false;return true;
}
bool ValidInputActions(const std::vector<std::string>& actions){std::set<std::string> seen;for(const auto& name:actions){if(name.empty()||name.size()>127||!seen.insert(name).second)return false;for(unsigned char c:name)if(!(std::islower(c)||std::isdigit(c)||c=='_'||c=='-'||c=='.'))return false;}return true;}
std::optional<ClientWorldEntry> ParseManifest(const std::filesystem::path& manifest){
    auto text=ReadText(manifest);if(!text)return std::nullopt;std::string_view json=*text;ClientWorldEntry entry;uint32_t schema{};std::string dll;
    if(!UIntMember(json,"schemaVersion",schema)||schema!=1u||!StringMember(json,"id",entry.id)||entry.id.empty()||!StringMember(json,"displayName",entry.name)||entry.name.empty()||!StringMember(json,"dll",dll)||!UIntMember(json,"pluginAbi",entry.pluginAbi)||!UIntMember(json,"gpuAbi",entry.gpuAbi)||!StringArrayMember(json,"requiredServices",entry.requiredServices))return std::nullopt;
    if(entry.pluginAbi!=LABOVR_PLUGIN_ABI_V1||entry.gpuAbi!=LABOVR_GPU_ABI_V1||!ValidServices(entry.requiredServices))return std::nullopt;
    if(MemberValue(json,"requiredInputActions")){if(!StringArrayMember(json,"requiredInputActions",entry.requiredInputActions)||!ValidInputActions(entry.requiredInputActions))return std::nullopt;}
    std::filesystem::path dllRelative=std::filesystem::u8path(dll);if(!SafeRelativeDll(dllRelative))return std::nullopt;
    auto optionsPos=MemberValue(json,"options");if(!optionsPos)return std::nullopt;size_t ob{},oe{};if(!BalancedRange(json,*optionsPos,'{','}',ob,oe))return std::nullopt;
    if(!BoolMember(json,"smooth_locomotion",entry.navigation.smoothLocomotion,ob,oe)||!BoolMember(json,"height_adjust",entry.navigation.heightAdjust,ob,oe))return std::nullopt;
    if(MemberValue(json,"retain_home_ambient",ob,oe)&&!BoolMember(json,"retain_home_ambient",entry.navigation.retainHomeAmbient,ob,oe))return std::nullopt;
    entry.manifestPath=manifest;entry.manifestJson=*text;entry.packageRoot=manifest.parent_path();entry.dllPath=entry.packageRoot/dllRelative;
    std::error_code ec;if(!std::filesystem::is_regular_file(entry.dllPath,ec)||ec)return std::nullopt;return entry;
}
}

ContentCatalog DiscoverWorldCatalog(){
    ContentCatalog catalog;auto root=ExecutableDirectory()/L"worlds";std::error_code ec;
    auto rootUtf8=root.u8string();LogEvent(0,"content.catalog_root","path=%s",reinterpret_cast<const char*>(rootUtf8.c_str()));
    if(!std::filesystem::is_directory(root,ec)||ec){LogEvent(0,"content.catalog_missing","error=%d",int(ec.value()));return catalog;}
    std::vector<std::filesystem::path> manifests;std::filesystem::directory_iterator it(root,ec),end;if(ec){LogEvent(0,"content.catalog_iter_error","error=%d",int(ec.value()));return catalog;}for(;it!=end;it.increment(ec)){if(ec)break;std::error_code itemEc;if(!it->is_directory(itemEc)||itemEc)continue;auto manifest=it->path()/L"world.json";if(std::filesystem::is_regular_file(manifest,itemEc)&&!itemEc)manifests.push_back(std::move(manifest));}
    std::sort(manifests.begin(),manifests.end());std::set<std::string> ids;for(const auto& manifest:manifests){auto manifestUtf8=manifest.u8string();if(auto entry=ParseManifest(manifest)){if(ids.insert(entry->id).second){LogEvent(0,"content.world_discovered","id=%s name=%s manifest=%s",entry->id.c_str(),entry->name.c_str(),reinterpret_cast<const char*>(manifestUtf8.c_str()));catalog.Add(std::move(*entry));}else LogEvent(0,"content.world_rejected","reason=duplicate_id manifest=%s",reinterpret_cast<const char*>(manifestUtf8.c_str()));}else LogEvent(0,"content.world_rejected","reason=invalid_manifest_or_missing_dll manifest=%s",reinterpret_cast<const char*>(manifestUtf8.c_str()));}
    LogEvent(0,"content.catalog_ready","worlds=%u",uint32_t(catalog.size()));return catalog;
}
}
