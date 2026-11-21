#pragma once
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <windows.h>

namespace OASIS {

inline std::string_view TrimProfile(std::string_view value) {
    const auto first=value.find_first_not_of(" \t\r");
    if(first==std::string_view::npos)return {};
    return value.substr(first,value.find_last_not_of(" \t\r")-first+1);
}

template<class Read>
void ReadLocalProfile(const std::filesystem::path& path,Read read) {
    std::ifstream file(path);
    for(std::string line;std::getline(file,line);){
        if(line.starts_with("\xEF\xBB\xBF"))line.erase(0,3);
        if(const auto comment=line.find_first_of("#;");comment!=std::string::npos)line.resize(comment);
        const auto equals=line.find('=');
        if(equals!=std::string::npos)
            read(TrimProfile(std::string_view(line).substr(0,equals)),TrimProfile(std::string_view(line).substr(equals+1)));
    }
}

inline bool ProfileInt(std::string_view text,int& value) {
    if(text.empty())return false;
    const auto end=text.data()+text.size();
    const auto result=std::from_chars(text.data(),end,value);
    return result.ec==std::errc{}&&result.ptr==end;
}

template<class Write>
bool WriteLocalProfile(const std::filesystem::path& path,Write write) {
    if(path.empty())return true;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(),error);
    if(error)return false;
    auto pending=path;
    pending+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    std::ofstream file(pending,std::ios::trunc);
    write(file);
    file.close();
    if(file&&MoveFileExW(pending.wstring().c_str(),path.wstring().c_str(),
                        MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
    std::filesystem::remove(pending,error);
    return false;
}

}
