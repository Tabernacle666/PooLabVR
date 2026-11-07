#include "GpuValidation.h"
#include "Core.h"
#include "SessionLog.h"

namespace lab {
void __stdcall GpuValidation::Message(D3D12_MESSAGE_CATEGORY category,
    D3D12_MESSAGE_SEVERITY severity,D3D12_MESSAGE_ID id,LPCSTR text,void* context) noexcept {
    auto& self=*static_cast<GpuValidation*>(context);const char* stage=nullptr;
    if(severity==D3D12_MESSAGE_SEVERITY_CORRUPTION||severity==D3D12_MESSAGE_SEVERITY_ERROR){
        self.errors_.fetch_add(1,std::memory_order_relaxed);stage="gpu.validation_error";
    }else if(severity==D3D12_MESSAGE_SEVERITY_WARNING){
        self.warnings_.fetch_add(1,std::memory_order_relaxed);stage="gpu.validation_warning";
    }
    if(stage)LogEvent(0,stage,"category=%u id=%u message=%s",unsigned(category),unsigned(id),text?text:"");
}
void GpuValidation::Enable(){
    Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_)),"D3D12 debug layer unavailable");
    debug_->EnableDebugLayer();debug_->SetEnableGPUBasedValidation(TRUE);
    debug_->SetEnableSynchronizedCommandQueueValidation(TRUE);enabled_=true;
}
void GpuValidation::Attach(ID3D12Device* device){
    if(!enabled_)return;
    Hr(device->QueryInterface(IID_PPV_ARGS(&messages_)),"D3D12 validation message callbacks unavailable");
    // Include device-creation diagnostics emitted before callback registration.
    auto count=messages_->GetNumStoredMessagesAllowedByRetrievalFilter();
    for(UINT64 i=0;i<count;++i){SIZE_T size=0;Hr(messages_->GetMessage(i,nullptr,&size),"D3D12 validation message size");
        std::vector<std::byte> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        Hr(messages_->GetMessage(i,message,&size),"D3D12 validation message");
        Message(message->Category,message->Severity,message->ID,message->pDescription,this);
    }
    messages_->ClearStoredMessages();
    Hr(messages_->RegisterMessageCallback(&Message,D3D12_MESSAGE_CALLBACK_FLAG_NONE,this,&cookie_),"D3D12 validation callback");registered_=true;
    LogEvent(0,"gpu.validation_ready","debug_layer=1 gpu_validation=1 synchronized_queue_validation=1");
}
GpuValidationState GpuValidation::State(ID3D12Device* device)const {
    return {enabled_,device&&FAILED(device->GetDeviceRemovedReason()),
        errors_.load(std::memory_order_relaxed),warnings_.load(std::memory_order_relaxed)};
}
GpuValidation::~GpuValidation(){if(registered_)messages_->UnregisterMessageCallback(cookie_);}
}
