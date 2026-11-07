#pragma once
#include "../cartridge_api/LaboVRStorageService.h"
#include <string>
namespace lab {
class ProfileStorage {
    std::string owner_;
    LabU32 Transfer(bool,LabU32,const char*,void*,LabU32,LabStorageResultV1*);
public:
    void SetOwner(std::string owner){owner_=std::move(owner);}
    LabStorageServiceV1 Service();
};
}
