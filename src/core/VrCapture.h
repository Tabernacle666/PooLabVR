#pragma once
#include "../cartridge_api/LaboVRCapture.h"
#include <memory>
#include <vector>
namespace lab {
struct VrCaptureTicket {LabU64 owner{},request{};LabCaptureRequestV1 options{};double queuedAt{};};
class VrCapture {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    VrCapture();~VrCapture();
    bool Request(LabU64,const LabCaptureRequestV1&,LabU64&);
    bool Status(LabU64,LabCaptureStatusV1&)const;
    bool Take(VrCaptureTicket&);
    void Pump();
    void Encode(const VrCaptureTicket&,LabU64,uint32_t,uint32_t,bool,std::vector<unsigned char>);
    void Failed(const VrCaptureTicket&,const char*);
    void Clear(LabU64);
};
}
