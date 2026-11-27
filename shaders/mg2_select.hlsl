// GPU cut selection, matching Mg2Pages' existing projected-error policy.
// One workgroup per instance. Each 32-lane wave emits one membership word;
// the existing CPU publication path consumes selected cluster IDs.
struct Page {
    float3 center; float radius;
    float error; uint isRoot,clusterCount,triangles;
    uint opaque,firstId,pad0,pad1;
};
struct Cluster { uint page,refined,triangles,localIndex; };
struct Eye { float4 positionFocal,motionNear; };
struct Instance {
    uint first,count,structuralFirst,structuralCount;
    uint outputOffset,eyeCount,visible,previousCluster;
    Eye eyes[4];
};
StructuredBuffer<Page> Pages : register(t0);
StructuredBuffer<Cluster> Clusters : register(t1);
StructuredBuffer<Instance> Instances : register(t2);
StructuredBuffer<uint> Structural : register(t3);
RWStructuredBuffer<uint> Output : register(u0);
cbuffer Settings : register(b0) { float ErrorPixels; };
groupshared uint TriangleSums[64];
groupshared uint ClusterSums[64];
bool Refine(Page p,Instance inst){
    if(p.isRoot!=0)return true;
    for(uint eye=0;eye<inst.eyeCount;++eye){
        Eye v=inst.eyes[eye];
        float depth=max(v.motionNear.w,length(p.center-v.positionFocal.xyz)-p.radius);
        if(p.error>ErrorPixels*depth/v.positionFocal.w)return true;
    }
    return false;
}
float StructuralError(Page p,Instance inst){
    float error=0;
    for(uint eye=0;eye<inst.eyeCount;++eye){
        Eye v=inst.eyes[eye];
        float3 delta=p.center-v.positionFocal.xyz;
        float3 motion=v.motionNear.xyz;
        float motion2=dot(motion,motion);
        float t=motion2>0?saturate(dot(delta,motion)/motion2):0;
        float depth=max(v.motionNear.w,length(delta-motion*t)-p.radius);
        error=max(error,p.error*v.positionFocal.w/depth);
    }
    return error;
}
[WaveSize(32)]
[numthreads(64,1,1)]
void Mg2SelectMain(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    Instance inst=Instances[group.x];
    uint triangles=0,selected=0;
    for(uint base=0;base<inst.count;base+=64u){
        uint localIndex=base+lane;
        bool hit=false;
        if(localIndex<inst.count){
            Cluster c=Clusters[inst.first+localIndex];
            hit=Refine(Pages[c.page],inst);
            if(hit&&c.refined!=0xffffffffu)hit=!Refine(Pages[c.refined],inst);
            if(hit){triangles+=c.triangles;++selected;}
        }
        // All lanes participate, including the tail of the last wave.
        uint mask=WaveActiveBallot(hit).x;
        if(WaveIsFirstLane()&&localIndex<inst.count)
            Output[inst.outputOffset+1u+localIndex/32u]=mask;
    }
    TriangleSums[lane]=triangles;ClusterSums[lane]=selected;
    GroupMemoryBarrierWithGroupSync();
    for(uint stride=32u;stride>0;stride>>=1u){
        if(lane<stride){
            TriangleSums[lane]+=TriangleSums[lane+stride];
            ClusterSums[lane]+=ClusterSums[lane+stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0){
        uint best=0;
        triangles=TriangleSums[0];selected=ClusterSums[0];
        if(inst.visible!=0){
            for(uint i=0;i<inst.structuralCount;++i){
                uint index=Structural[inst.structuralFirst+i];
                Page p=Pages[index];
                if(p.triangles>=triangles||p.clusterCount>selected)continue;
                bool retained=p.firstId==inst.previousCluster;
                float budget=ErrorPixels*(p.opaque!=0?1.0:0.5)*(retained?1.0:0.8);
                if(StructuralError(p,inst)<=budget){
                    best=index+1u;triangles=p.triangles;selected=p.clusterCount;
                }
            }
        }
        Output[inst.outputOffset]=best;
    }
}
