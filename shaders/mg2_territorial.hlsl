// A root owns a disjoint preorder subtree. Emit only accepted nodes.
struct TerritorialNode {
    float4 sphere;
    float geometric,optical;
    uint escape,selectable;
    float4 supportBox; // xyz: AABB half extent, w: architectural support
};
StructuredBuffer<TerritorialNode> Nodes : register(t0);
StructuredBuffer<uint> Roots : register(t1);
StructuredBuffer<float4> Eyes : register(t2);
RWStructuredBuffer<uint> Output : register(u0);
cbuffer Settings : register(b0) {
    uint rootCount,eyeCount,dispatchX;
    float rasterErrorPixels,rayErrorPixels;
};
[numthreads(64,1,1)]
void Mg2TerritorialMain(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    const uint root=(group.x+group.y*dispatchX)*64u+lane;
    if(root>=rootCount)return;
    const float errorPixels=group.z==0u?rasterErrorPixels:rayErrorPixels;
    uint at=Roots[root],end=Nodes[at].escape,visited=0u;
    while(at<end){
        const TerritorialNode node=Nodes[at];++visited;
        // bit 0 = raster, bit 1 = RTX. Vegetation retains both bits.
        bool accept=(node.selectable&(1u<<group.z))!=0u&&eyeCount!=0u;
        [loop]for(uint eye=0u;eye<eyeCount&&accept;++eye){
            const float4 view=Eyes[eye*2u],motion=Eyes[eye*2u+1u];
            const float3 delta=node.sphere.xyz-view.xyz;
            float distance;
            if(node.supportBox.w!=0.f){
                const float3 a=-delta,b=a+motion.xyz;
                const float3 gap=max(0.f,max(min(a,b)-node.supportBox.xyz,
                    -node.supportBox.xyz-max(a,b)));
                distance=length(gap)-1.e-5*(length(a)+length(motion.xyz)+length(node.supportBox.xyz)+1.f);
            }else{
                const float travel2=dot(motion.xyz,motion.xyz);
                const float t=travel2>0.0?saturate(dot(delta,motion.xyz)/travel2):0.0;
                const float swept=length(delta-motion.xyz*t);
                const float margin=1.e-5*(swept+node.sphere.w+1.0+length(motion.xyz));
                distance=swept-node.sphere.w-margin;
            }
            // Both eyes, including off-screen ray contributors.
            accept=distance>motion.w&&max(node.geometric,node.optical)*view.w<=errorPixels*distance;
        }
        if(accept){
            uint slot;InterlockedAdd(Output[0],1u,slot);
            Output[slot+2u]=at;
            at=node.escape;
        }else ++at;
    }
    uint ignored;InterlockedAdd(Output[1],visited,ignored);
}
