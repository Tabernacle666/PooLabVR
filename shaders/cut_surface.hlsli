#ifndef OASIS_CUT_SURFACE
#define OASIS_CUT_SURFACE

struct CutVertex {
    float3 position,normal;
    float2 uv,uv1;
};
struct CutTriangle {
    CutVertex a,b,c;
    uint material,instanceId,pad0,pad1;
};
struct CutSpatialNode {float3 lower;uint leftOrBlock;float3 upper;uint rightChild;};
struct CutSpatialBlock {
    float3 lower;uint firstTriangle;float3 upper;uint triangleCount;
    uint instanceId,geometryState,activeFirst,activeCount;
};

#endif
