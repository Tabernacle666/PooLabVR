#ifndef LAB_PRESENTATION_HLSLI
#define LAB_PRESENTATION_HLSLI
#define LAB_PRESENTATION_MAX_VIEWS 4
cbuffer LabPresentationFrame : register(b0)
{
    row_major float4x4 vp[LAB_PRESENTATION_MAX_VIEWS];
    row_major float4x4 inverseVP[LAB_PRESENTATION_MAX_VIEWS];
    row_major float4x4 previousVP[LAB_PRESENTATION_MAX_VIEWS];
    row_major float4x4 shadowVP[24];
    float4 eye[LAB_PRESENTATION_MAX_VIEWS];
    uint4 resourceAbi;
    uint4 dimensions;
    uint4 outputDimensions;
    uint4 counts;
    uint4 topology;
    float4 timing;
};
cbuffer LabPresentationRoot : register(b1)
{
    uint presentationDrawIndex;
    uint presentationDrawTable;
    uint presentationSceneSnapshot;
    uint presentationResourceTable;
    uint presentationFlags;
};
uint PresentationResource(uint slot)
{
    StructuredBuffer<uint2> table=ResourceDescriptorHeap[NonUniformResourceIndex(presentationResourceTable)];
    return table[slot].x;
}
#define PresentationSceneSnapshot presentationSceneSnapshot
#endif
