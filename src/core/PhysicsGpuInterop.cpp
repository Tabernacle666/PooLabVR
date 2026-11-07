#include "PhysicsGpuInterop.h"
#include "Core.h"

namespace lab::physics_detail {
namespace {
void Cuda(cudaError_t result,const char* operation){if(result!=cudaSuccess){char message[512];snprintf(message,sizeof(message),"PhysX CUDA: %s: %s",operation,cudaGetErrorString(result));Fail(message);}}
void Gpu(bool result,const char* operation){if(!result)Fail(operation);}
}
void SharedBuffer::Create(const LabGpuInteropServiceV1& interop,uint64_t size){
        bytes=size;Gpu(interop.CreateSharedBuffer(interop.serviceContext,size,&handle)!=0,"PhysX: shared DX12 buffer creation failed");
        LabGpuInteropMemoryExportV1 out{sizeof(out)};Gpu(interop.GetCudaMemoryHandle(interop.serviceContext,handle,&out)!=0,"PhysX: DX12 memory export failed");
        cudaExternalMemoryHandleDesc d{};d.type=cudaExternalMemoryHandleTypeD3D12Resource;d.handle.win32.handle=out.win32Handle;d.size=out.allocationBytes;d.flags=cudaExternalMemoryDedicated;
        auto result=cudaImportExternalMemory(&memory,&d);CloseHandle(out.win32Handle);Cuda(result,"import DX12 resource");
        cudaExternalMemoryBufferDesc b{};b.size=out.resourceBytes;Cuda(cudaExternalMemoryGetMappedBuffer(&pointer,memory,&b),"map DX12 resource");
    }
void SharedBuffer::Destroy(const LabGpuInteropServiceV1& interop){if(pointer)Cuda(cudaFree(pointer),"unmap DX12 buffer");if(memory)Cuda(cudaDestroyExternalMemory(memory),"release DX12 import");if(handle)interop.ReleaseResource(interop.serviceContext,handle);*this={};}
void SharedFence::Create(const LabGpuInteropServiceV1& interop){
        Gpu(interop.CreateSharedFence(interop.serviceContext,0,&handle)!=0,"PhysX: shared DX12 fence creation failed");
        LabGpuInteropFenceExportV1 out{sizeof(out)};Gpu(interop.GetCudaFenceHandle(interop.serviceContext,handle,1,&out)!=0,"PhysX: DX12 fence export failed");
        cudaExternalSemaphoreHandleDesc d{};d.type=cudaExternalSemaphoreHandleTypeD3D12Fence;d.handle.win32.handle=out.win32Handle;
        auto result=cudaImportExternalSemaphore(&semaphore,&d);CloseHandle(out.win32Handle);Cuda(result,"import DX12 fence");
    }
uint64_t SharedFence::Completed(const LabGpuInteropServiceV1& interop){LabGpuInteropFenceExportV1 out{sizeof(out)};Gpu(interop.GetCudaFenceHandle(interop.serviceContext,handle,0,&out)!=0,"PhysX: query DX12 fence failed");return out.completedValue;}
void SharedFence::Wait(cudaStream_t stream,uint64_t value){if(value){cudaExternalSemaphoreWaitParams p{};p.params.fence.value=value;Cuda(cudaWaitExternalSemaphoresAsync(&semaphore,&p,1,stream),"wait DX12 consumer");}}
void SharedFence::Signal(cudaStream_t stream,uint64_t value){cudaExternalSemaphoreSignalParams p{};p.params.fence.value=value;Cuda(cudaSignalExternalSemaphoresAsync(&semaphore,&p,1,stream),"signal CUDA producer");}
void SharedFence::Destroy(const LabGpuInteropServiceV1& interop){if(semaphore)Cuda(cudaDestroyExternalSemaphore(semaphore),"release fence import");if(handle)interop.ReleaseFence(interop.serviceContext,handle);*this={};}
}
