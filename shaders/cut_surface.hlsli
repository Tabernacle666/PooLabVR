#ifndef OASIS_CUT_SURFACE
#define OASIS_CUT_SURFACE

struct CutImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId,primitiveIndex,tick,recess;
    float hitTime;
    uint blockId,owner,pending;
};
static const uint CutImpactSlots=128u;
static const uint TerrainRecess=2u;
static const uint GroundCraterHit=0xfffffffeu;
struct BuildingImpact {
    float3 position;uint renderId;
    float3 direction;uint projectile;
    float3 origin;float hitTime;
    uint4 cpuMetadata;
};

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
