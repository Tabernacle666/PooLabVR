#include "GraphicsInternal.h"

namespace OASIS {
    void Graphics::Impl::EnsureCutPipelines(){
        if(cutPipelines[0])return;
        constexpr std::array<const wchar_t*,size_t(CutKernel::Count)> names={
            L"StepProjectiles",L"ResolveImpact",L"BuildImpactVolume",L"CutSurface",
            L"FindCutSegments",L"PartitionCap",L"EmitCap",L"MakeCutDispatch",
            L"PackCutGeometry",L"FinalizeCutRange"
        };
        std::array<ComPtr<ID3D12PipelineState>,size_t(CutKernel::Count)> prepared;
        for(size_t i=0;i<names.size();++i){
            auto bytes=Shader(names[i]);ComputePipeline stream{};
            stream.root.data=root.Get();stream.cs.data={bytes.data(),bytes.size()};
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream),&stream};
            prepared[i]=Pipeline(names[i],desc,Hash(bytes));
        }
        cutPipelines=std::move(prepared);
    }

    void Graphics::Impl::BindCutPipeline(ID3D12GraphicsCommandList7* list,CutKernel kernel,D3D12_GPU_VIRTUAL_ADDRESS parameters){
        if(!list||!parameters||(parameters&255))Fail("Invalid cut pipeline arguments");
        const auto index=size_t(kernel);if(index>=cutPipelines.size()||!cutPipelines[index])Fail("Cut pipeline is not initialized");
        ID3D12DescriptorHeap* heaps[]={descriptors.Get(),samplers.Get()};list->SetDescriptorHeaps(2,heaps);
        list->SetComputeRootSignature(root.Get());list->SetComputeRootConstantBufferView(0,parameters);
        list->SetPipelineState(cutPipelines[index].Get());
    }
}
