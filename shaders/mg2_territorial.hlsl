// A root owns a disjoint preorder subtree. Emit only accepted nodes.
struct TerritorialNode {
    float4 sphere;
    float geometric,optical;
    uint escape,selectable;
};
StructuredBuffer<TerritorialNode> Nodes : register(t0);
StructuredBuffer<uint> Roots : register(t1);
StructuredBuffer<float4> Eyes : register(t2);
RWStructuredBuffer<uint> Output : register(u0);
cbuffer Settings : register(b0) {
    uint rootCount,eyeCount,dispatchX;
    float errorPixels;
};
[numthreads(64,1,1)]
void Mg2TerritorialMain(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    const uint root=(group.x+group.y*dispatchX)*64u+lane;
    if(root>=rootCount)return;
    uint at=Roots[root],end=Nodes[at].escape,visited=0u;
    while(at<end){
        const TerritorialNode node=Nodes[at];++visited;
        bool accept=node.selectable!=0u&&eyeCount!=0u;
        [loop]for(uint eye=0u;eye<eyeCount&&accept;++eye){
            const float4 view=Eyes[eye*2u],motion=Eyes[eye*2u+1u];
            const float3 delta=node.sphere.xyz-view.xyz;
            const float travel2=dot(motion.xyz,motion.xyz);
            const float t=travel2>0.0?saturate(dot(delta,motion.xyz)/travel2):0.0;
            const float swept=length(delta-motion.xyz*t);
            const float margin=1.e-5*(swept+node.sphere.w+1.0+length(motion.xyz));
            const float distance=swept-node.sphere.w-margin;
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
