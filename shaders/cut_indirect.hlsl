// Generate a compute dispatch from a GPU-produced element count.
cbuffer IndirectParameters:register(b0){
    uint controlDescriptor,argumentsDescriptor,countOffset,overflowOffset;
    uint ambiguityOffset,threadsPerGroup,outputOffset,_reserved;
};
[numthreads(1,1,1)]
void MakeCutDispatch(uint3 dispatch:SV_DispatchThreadID){
    ByteAddressBuffer control=ResourceDescriptorHeap[controlDescriptor];
    RWByteAddressBuffer arguments=ResourceDescriptorHeap[argumentsDescriptor];
    uint count=control.Load(countOffset);uint groups=0;
    if(threadsPerGroup!=0&&!control.Load(overflowOffset)&&!control.Load(ambiguityOffset))
        groups=count/threadsPerGroup+(count%threadsPerGroup!=0?1:0);
    uint x=min(groups,65535u),y=groups==0?1:groups/65535u+(groups%65535u!=0?1:0);
    arguments.Store3(outputOffset,uint3(x,y,1));
}
