// Generate a compute dispatch from a GPU-produced element count.
cbuffer IndirectParameters:register(b0){
    uint controlDescriptor,argumentsDescriptor,countOffset,overflowOffset;
    uint ambiguityOffset,threadsPerGroup,outputOffset,auxiliaryControl;
};
[numthreads(1,1,1)]
void MakeCutDispatch(uint3 dispatch:SV_DispatchThreadID){
    RWByteAddressBuffer control=ResourceDescriptorHeap[controlDescriptor];
    RWByteAddressBuffer arguments=ResourceDescriptorHeap[argumentsDescriptor];
    if(outputOffset==36u){
        // Preserve the completed exterior before cap triangles are appended.
        RWByteAddressBuffer surface=ResourceDescriptorHeap[auxiliaryControl];
        control.Store(28,surface.Load(0));
    }else if(outputOffset==48u){
        // A failed cap must not discard a successfully clipped exterior.
        RWByteAddressBuffer cap=ResourceDescriptorHeap[auxiliaryControl];
        if(cap.Load(12)!=0||cap.Load(16)!=0)control.Store(0,cap.Load(28));
    }
    uint count=control.Load(countOffset);uint groups=0;
    if(threadsPerGroup!=0&&!control.Load(overflowOffset)&&!control.Load(ambiguityOffset))
        groups=count/threadsPerGroup+(count%threadsPerGroup!=0?1:0);
    uint x=min(groups,65535u),y=groups==0?1:groups/65535u+(groups%65535u!=0?1:0);
    arguments.Store3(outputOffset,uint3(x,y,1));
}
