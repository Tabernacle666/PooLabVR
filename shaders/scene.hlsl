#include "common.hlsli"
#include "camera_visibility.hlsli"
#include "cut_ray.hlsli"
#include "solar_visibility.hlsli"
#ifndef OASIS_MATERIAL_MASK
#define OASIS_MATERIAL_MASK -1
#endif
#define OASIS_SHUTTLE_CRT (OASIS_MATERIAL_MASK == 32)
struct Out {
    float4 position:SV_Position;
    float3 world:POSITION0;
    float3 normal:NORMAL0;
    float2 uv:TEXCOORD0;
    float2 uv1:TEXCOORD3;
    nointerpolation uint material:TEXCOORD1;
    nointerpolation uint view:TEXCOORD2;
    nointerpolation uint receiver:TEXCOORD5;
#if OASIS_SHUTTLE_CRT
    nointerpolation uint crt:TEXCOORD4;
#endif
};
float3 ShuttleDustVertex(Vertex vertex,GpuInstance instance,uint view){
    const uint packed=instance.visualData;
    const float density=float(packed>>18u)/63.f;
    const float phase=float(packed&4095u)/4096.f;
    const float speed=float((packed>>12u)&63u)/63.f;
    float3 local=vertex.position;
    local.x=(frac((local.x+20.f)/40.f-phase)-.5f)*40.f;
    const float3 center=World(local,instance);
    const float3 delta=center-eye[view].xyz;
    const float distance2=dot(delta,delta);
    const float3 axis=Rotate(float3(1,0,0),instance.rotation);
    const float3 across=cross(axis,delta);
    const float edge=saturate((20.f-abs(local.x))*.6f)*saturate((distance2-7.f)*.28f);
    const float visible=vertex.uv1.x<density?edge:0.f;
    const float3 side=across*rsqrt(max(1.e-6f,dot(across,across)));
    const float width=.006f+min(.045f,sqrt(distance2)*.00112f);
    return center+visible*(axis*(.085f+.92f*speed)*vertex.uv.x+side*width*vertex.uv.y);
}

float3 ForestInsectVertex(Vertex vertex,GpuInstance instance,uint view){
    const float units=1.f;
    const float phase=6.2831853f*frac(vertex.uv1.y+float(instance.id&65535u)*.61803399f);
    const float t=timing.z;
    const float pulse=.5f+.5f*sin(t*(1.3f+vertex.uv1.y)+phase);
    float3 local=vertex.position;
    local.x+=.25f*sin(t*2.8f+phase)+.12f*sin(t*6.1f+phase*2.f)+.07f*sin(t*.45f);
    local.z+=.27f*cos(t*2.3f+phase)+.11f*sin(t*5.7f+phase*3.f)+.05f*cos(t*.38f);
    local.y+=.20f*sin(t*(1.8f+vertex.uv1.y)+phase)+.08f*cos(t*7.f+phase);
    const float3 center=World(local,instance);
    const float3 player=float3(tileViewer.x,tileViewer.z,tileViewer.y);
    const float3 away=center-player;
    const float d=length(away)/units;
    const float3 flightCenter=center+away/max(length(away),.00001f)*units*.20f*(1.f-smoothstep(.25f,1.1f,d));
    const float3 delta=eye[view].xyz-flightCenter;
    const float3 facing=normalize(delta+float3(0,0,.000001f));
    const float3 right=normalize(cross(float3(0,1,0),facing)+float3(.000001f,0,0));
    const float3 up=cross(facing,right);
    const float gate=frac(vertex.uv1.x+float(instance.id&65535u)*.75487766f);
    const float visible=gate<forestAtmosphere.z?1.f:0.f;
    const float distance=length(delta)/units;
    const float size=lerp(.001f,.005f,vertex.uv1.y)*(.75f+.25f*pulse);
    const float radius=units*size*visible*smoothstep(.05f,.22f,distance)*(1.f-smoothstep(12.f,22.f,distance));
    return flightCenter+radius*(right*vertex.uv.x+up*vertex.uv.y);
}

float4 WorldMeshletSphere(GpuInstance instance,float4 local){
    const float3 scale=abs(instance.scale.xyz);
    return float4(World(local.xyz,instance),local.w*max(scale.x,max(scale.y,scale.z))+0.00291262136f);
}
bool MeshletSphereInAnyView(float4 sphere){
    const float4 center=float4(sphere.xyz,1.f);
    const float radius=sphere.w;
    // The mesh output counts and triangle indices are shared by all view instances.
    [loop]for(uint eyeIndex=0;eyeIndex<ViewCount;++eyeIndex){
        bool inside=true;
        [unroll]for(uint plane=0;plane<6u;++plane)
            inside=inside&&(dot(center,meshletClipPlanes[eyeIndex][plane])>=-radius);
        if(inside)return true;
    }
    return false;
}
bool PagedMeshletInAnyView(GpuInstance instance,uint meshlet){
    [branch]if(!IsForestPage(instance.mesh)||(dimensions.w&VisibilityMeshletFrustumBypass)!=0u)return true;
    StructuredBuffer<float4> bounds=ResourceDescriptorHeap[DomainResource(ResourceDomainMeshes,Mg2Bounds)];
    return MeshletSphereInAnyView(WorldMeshletSphere(instance,bounds[meshlet]));
}

[outputtopology("triangle")]
[numthreads(64,1,1)]
void MeshMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,uint view:SV_ViewID,
              out vertices Out verts[128],out indices uint3 tris[128]){
    uint4 generation=0;
    if(resourceAbi.w!=0xffffffffu){
        StructuredBuffer<uint4> state=ResourceDescriptorHeap[resourceAbi.w];
        generation=state[0];
    }
    uint vertexCount=0u,triangleCount=0u;
    GpuInstance inst=(GpuInstance)0;
    Meshlet m=(Meshlet)0;
    const uint drawOrdinal=objectIndex+group.y;
    uint instanceIndex=drawOrdinal;
    if(param2==3u)instanceIndex=OpaqueRenderIndex(drawOrdinal);
    else if(param2==4u){
        StructuredBuffer<uint> compact=ResourceDescriptorHeap[TileMarksSRV];
        instanceIndex=compact[drawOrdinal];
    }
    if(param2==3u||param2==4u)inst=LoadCameraInstance(instanceIndex);
    else inst=LoadInstance(instanceIndex);
    const bool replaced=generation.y!=0u&&
        instanceIndex<cutGeometry.w&&
        (inst.id&0x80000000u)!=0u&&(inst.flags&19u)==0u;
    if(!replaced&&(inst.flags&128u)==0u){
        const MeshRange range=LoadMeshRange(inst.mesh);
        StructuredBuffer<Meshlet> meshlets=ResourceDescriptorHeap[
            NonUniformResourceIndex(MeshResource(inst.mesh,MeshMeshlets,Mg2Meshlets))];
        const uint meshlet=MeshletOrdinal(group);
        if(meshlet<range.meshletCount){
            const uint index=range.firstMeshlet+meshlet;
            if(PagedMeshletInAnyView(inst,index)){
                m=meshlets[index];
                vertexCount=m.vertexCount;
                triangleCount=m.triangleCount;
            }
        }
    }
    // DXIL mesh validation requires exactly one dominating output-count call.
    SetMeshOutputCounts(vertexCount,triangleCount);
    for(uint vertex=tid;vertex<vertexCount;vertex+=64u){
        StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[
            NonUniformResourceIndex(MeshResource(inst.mesh,MeshVertices,Mg2Vertices))];
        const uint index=MeshletVertexIndex(inst.mesh,m.vertexOffset+vertex);
        Vertex v=vertices[index];
        if((inst.flags&2u)!=0u){
            StructuredBuffer<Vertex> deformed=
                ResourceDescriptorHeap[DynamicVertices];
            v=deformed[index];
        }
        float3 world=World(v.position,inst);
#if !OASIS_SHUTTLE_CRT
        if((inst.flags&ShuttleSpeedDustFlag)!=0u)world=ShuttleDustVertex(v,inst,view);
        if((inst.flags&OASIS_INSTANCE_FOREST_INSECTS)!=0u)world=ForestInsectVertex(v,inst,view);
#endif
        verts[vertex].position=mul(float4(world,1),vp[view]);
        verts[vertex].world=world;
        verts[vertex].normal=normalize(Rotate(v.normal/
            (sign(inst.scale.xyz)*max(abs(inst.scale.xyz),.000001)),
            inst.rotation));
        verts[vertex].uv=v.uv*InstanceSurfaceUVScale(inst);verts[vertex].uv1=v.uv1;
        verts[vertex].material=inst.material;verts[vertex].view=view;verts[vertex].receiver=inst.id;
#if OASIS_SHUTTLE_CRT
        verts[vertex].crt=(inst.flags&ShuttleCrtFlag)!=0u?inst.visualData:0u;
#endif
    }
    for(uint t=tid;t<triangleCount;t+=64u){
        tris[t]=MeshletTriangle(inst.mesh,m.triangleOffset,t);
    }
}
[outputtopology("triangle")]
[numthreads(64,1,1)]
void ShadowMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,
                out vertices Out verts[128],out indices uint3 tris[128]){
    uint4 generation=0;
    if(resourceAbi.w!=0xffffffffu){
        StructuredBuffer<uint4> state=ResourceDescriptorHeap[resourceAbi.w];
        generation=state[0];
    }
    uint vertexCount=0u,triangleCount=0u;
    GpuInstance inst=(GpuInstance)0;
    Meshlet m=(Meshlet)0;
    inst=LoadInstanceStable(objectIndex);
    const bool replaced=generation.y!=0u&&
        objectIndex<cutGeometry.w&&
        (inst.id&0x80000000u)!=0u&&(inst.flags&19u)==0u;
    if(!replaced){
        StructuredBuffer<Meshlet> meshlets=ResourceDescriptorHeap[
            NonUniformResourceIndex(MeshResource(inst.mesh,MeshMeshlets,Mg2Meshlets))];
        const MeshRange range=LoadMeshRange(inst.mesh);
        const uint meshlet=MeshletOrdinal(group);
        if(meshlet<range.meshletCount){
            m=meshlets[range.firstMeshlet+meshlet];
            vertexCount=m.vertexCount;
            triangleCount=m.triangleCount;
        }
    }
    // One call dominates every possible mesh output store.
    SetMeshOutputCounts(vertexCount,triangleCount);
    for(uint vertex=tid;vertex<vertexCount;vertex+=64u){
        StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[
            NonUniformResourceIndex(MeshResource(inst.mesh,MeshVertices,Mg2Vertices))];
        const uint index=MeshletVertexIndex(inst.mesh,m.vertexOffset+vertex);
        Vertex v=vertices[index];
        float3 world=World(v.position,inst);
        verts[vertex].position=mul(float4(world,1),shadowVP[passIndex]);
        verts[vertex].world=world;
        verts[vertex].normal=Rotate(v.normal,inst.rotation);
        verts[vertex].uv=v.uv*InstanceSurfaceUVScale(inst);verts[vertex].uv1=v.uv1;
        verts[vertex].material=inst.material;verts[vertex].view=0;verts[vertex].receiver=inst.id;
    }
    for(uint t=tid;t<triangleCount;t+=64u){
        tris[t]=MeshletTriangle(inst.mesh,m.triangleOffset,t);
    }
}


// Dedicated step-4d raster path. Each indirect command supplies a stable
// block ID and its triangle count. Unselected owners emit no geometry.
[outputtopology("triangle")]
[numthreads(64,1,1)]
void CutMeshMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,uint view:SV_ViewID,
                 out vertices Out verts[64],out indices uint3 tris[124]){
    uint local=(group.x+group.y*65535u)*21u;
    StructuredBuffer<uint4> cacheState=ResourceDescriptorHeap[cutRaster.x];
    uint blockDescriptor=cacheState[0].x==0u?cutRaster.y:cutRaster.z;
    StructuredBuffer<CutRasterBlock> blocks=
        ResourceDescriptorHeap[NonUniformResourceIndex(blockDescriptor)];
    CutRasterBlock block=blocks[objectIndex];
    const uint faceCount=cacheState[0].y!=0u&&local<param0?min(21u,param0-local):0u;
    SetMeshOutputCounts(faceCount*3u,faceCount);
    uint descriptor=block.geometryState==0u?cutGeometry.x:cutRaster.w;
    uint start=(block.geometryState==0u?block.firstTriangle:block.activeFirst)+local;
    if(tid<faceCount*3u){
        StructuredBuffer<CutTriangle> source=
            ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
        CutTriangle cutFace=source[start+tid/3u];
        Vertex v=cutFace.a;
        if(tid%3u==1u)v=cutFace.b;
        else if(tid%3u==2u)v=cutFace.c;
        verts[tid].position=mul(float4(v.position,1),vp[view]);
        verts[tid].world=v.position;
        verts[tid].normal=normalize(v.normal);
        verts[tid].uv=v.uv;
        verts[tid].uv1=v.uv1;
        verts[tid].material=cutFace.material;
        verts[tid].receiver=cutFace.instanceId;
        verts[tid].view=view;
    }
    if(tid<faceCount)
        tris[tid]=uint3(tid*3u,tid*3u+1u,tid*3u+2u);
}

[outputtopology("triangle")]
[numthreads(64,1,1)]
void CutShadowMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,
                   out vertices Out verts[64],out indices uint3 tris[124]){
    uint local=(group.x+group.y*65535u)*21u;
    StructuredBuffer<uint4> cacheState=ResourceDescriptorHeap[cutRaster.x];
    uint faceCount=cacheState[0].y!=0u&&local<param0?min(21u,param0-local):0u;
    SetMeshOutputCounts(faceCount*3u,faceCount);
    uint blockDescriptor=cacheState[0].x==0u?cutRaster.y:cutRaster.z;
    StructuredBuffer<CutRasterBlock> blocks=
        ResourceDescriptorHeap[NonUniformResourceIndex(blockDescriptor)];
    CutRasterBlock block=blocks[objectIndex];
    uint descriptor=block.geometryState==0u?cutGeometry.x:cutRaster.w;
    uint start=(block.geometryState==0u?block.firstTriangle:block.activeFirst)+local;
    if(tid<faceCount*3u){
        StructuredBuffer<CutTriangle> source=
            ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
        CutTriangle cutFace=source[start+tid/3u];
        Vertex v=cutFace.a;
        if(tid%3u==1u)v=cutFace.b;
        else if(tid%3u==2u)v=cutFace.c;
        verts[tid].position=mul(float4(v.position,1),shadowVP[passIndex]);
        verts[tid].world=v.position;
        verts[tid].normal=normalize(v.normal);
        verts[tid].uv=v.uv;
        verts[tid].uv1=v.uv1;
        verts[tid].material=cutFace.material;
        verts[tid].receiver=cutFace.instanceId;
        verts[tid].view=0u;
    }
    if(tid<faceCount){
        StructuredBuffer<CutTriangle> source=
            ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
        tris[tid]=(source[start+tid].pad0&4u)!=0u?
            uint3(0u,0u,0u):uint3(3u*tid,3u*tid+1u,3u*tid+2u);
    }
}

// Depth carries only coverage inputs. Keep the legacy shadow shader ABI intact.
// The seed depth is built from the exact selected meshes for both eyes.
bool DepthMeshletVisible(GpuInstance inst,uint index){
    const bool paged=IsForestPage(inst.mesh);
    // The initial seed and published meshes must not read an incomplete Hi-Z.
    // Dynamic/deformed/dust bounds are not the immutable meshlet bounds.
    if(passIndex!=9u||(inst.flags&(2u|ShuttleSpeedDustFlag))!=0u)
        return PagedMeshletInAnyView(inst,index);
    const uint descriptor=DomainResource(ResourceDomainMeshes,
        paged?Mg2Bounds:MeshNativeBounds);
    if(descriptor==0xffffffffu)return true;
    StructuredBuffer<float4> bounds=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
    uint count,stride;bounds.GetDimensions(count,stride);
    if(index>=count)return true;
    const float4 local=bounds[index];
    if(!all(isfinite(local))||local.w<0.f)return true;
    // Both tests used to transform the same immutable MG2 bound separately.
    const float4 sphere=WorldMeshletSphere(inst,local);
    if(paged&&(dimensions.w&VisibilityMeshletFrustumBypass)!=0u)return true;
    if(paged&&!MeshletSphereInAnyView(sphere))return false;
    [loop]for(uint eyeIndex=0;eyeIndex<ViewCount;++eyeIndex){
        const float3 delta=sphere.xyz-eye[eyeIndex].xyz;
        const float reach=sphere.w+.28f;
        if(dot(delta,delta)<=reach*reach)return true;
        float2 lo,hi;float nearest;
        if(Frustum(sphere,eyeIndex,lo,hi,nearest)&&
           ((dimensions.w&VisibilityMeshletHiZBypass)!=0u||!Occluded(lo,hi,nearest,eyeIndex)))
            return true;
    }
    return false;
}
struct DepthOut {
    float4 position:SV_Position;
    float2 uv:TEXCOORD0;
    float2 uv1:TEXCOORD3;
    nointerpolation uint material:TEXCOORD1;
#if OASIS_MATERIAL_MASK == 64
    float3 normal:NORMAL0;
    nointerpolation uint receiver:TEXCOORD5;
#endif
};
[outputtopology("triangle")]
[numthreads(64,1,1)]
void DepthMeshMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,uint view:SV_ViewID,
              out vertices DepthOut verts[128],out indices uint3 tris[128]){
    uint4 generation=0;
    if(resourceAbi.w!=0xffffffffu){
        StructuredBuffer<uint4> state=ResourceDescriptorHeap[resourceAbi.w];
        generation=state[0];
    }
    uint vertexCount=0u,triangleCount=0u;
    GpuInstance inst=(GpuInstance)0;
    Meshlet m=(Meshlet)0;
    const uint drawOrdinal=objectIndex+group.y;
    const uint instanceIndex=param2==3u?OpaqueRenderIndex(drawOrdinal):drawOrdinal;
    inst=LoadCameraInstance(instanceIndex);
    const bool replaced=generation.y!=0u&&
        instanceIndex<cutGeometry.w&&
        (inst.id&0x80000000u)!=0u&&(inst.flags&19u)==0u;
    if(!replaced&&(inst.flags&128u)==0u){
        const MeshRange range=LoadMeshRange(inst.mesh);
        StructuredBuffer<Meshlet> meshlets=ResourceDescriptorHeap[
            NonUniformResourceIndex(MeshResource(inst.mesh,MeshMeshlets,Mg2Meshlets))];
        const uint meshlet=MeshletOrdinal(group);
        if(meshlet<range.meshletCount){
            const uint index=range.firstMeshlet+meshlet;
            if(DepthMeshletVisible(inst,index)){
                m=meshlets[index];
                vertexCount=m.vertexCount;
                triangleCount=m.triangleCount;
            }
        }
    }
    // DXIL mesh validation requires exactly one dominating output-count call.
    SetMeshOutputCounts(vertexCount,triangleCount);
    for(uint vertex=tid;vertex<vertexCount;vertex+=64u){
        StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[
            NonUniformResourceIndex(MeshResource(inst.mesh,MeshVertices,Mg2Vertices))];
        const uint index=MeshletVertexIndex(inst.mesh,m.vertexOffset+vertex);
        Vertex v=vertices[index];
        if((inst.flags&2u)!=0u){
            StructuredBuffer<Vertex> deformed=
                ResourceDescriptorHeap[DynamicVertices];
            v=deformed[index];
        }
        float3 world=World(v.position,inst);
#if !OASIS_SHUTTLE_CRT
        if((inst.flags&ShuttleSpeedDustFlag)!=0u)world=ShuttleDustVertex(v,inst,view);
        if((inst.flags&OASIS_INSTANCE_FOREST_INSECTS)!=0u)world=ForestInsectVertex(v,inst,view);
#endif
        verts[vertex].position=mul(float4(world,1),vp[view]);
        verts[vertex].uv=v.uv*InstanceSurfaceUVScale(inst);verts[vertex].uv1=v.uv1;
        verts[vertex].material=inst.material;
#if OASIS_MATERIAL_MASK == 64
        verts[vertex].normal=normalize(Rotate(v.normal/
            (sign(inst.scale.xyz)*max(abs(inst.scale.xyz),.000001)),inst.rotation));
        verts[vertex].receiver=inst.id;
#endif
    }
    for(uint t=tid;t<triangleCount;t+=64u){
        tris[t]=MeshletTriangle(inst.mesh,m.triangleOffset,t);
    }
}
// The seed classifier admits only alphaMode==opaque. No PS is needed.
#if OASIS_MATERIAL_MASK == 64
#define DepthSeedOut DepthOut
#else
struct DepthSeedOut {float4 position:SV_Position;};
#endif
[outputtopology("triangle")]
[numthreads(64,1,1)]
void DepthSeedMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,uint view:SV_ViewID,
              out vertices DepthSeedOut verts[128],out indices uint3 tris[128]){
    uint4 generation=0;
    if(resourceAbi.w!=0xffffffffu){
        StructuredBuffer<uint4> state=ResourceDescriptorHeap[resourceAbi.w];
        generation=state[0];
    }
    uint vertexCount=0u,triangleCount=0u;
    GpuInstance inst=(GpuInstance)0;
    Meshlet m=(Meshlet)0;
    const uint drawOrdinal=objectIndex+group.y;
    const uint instanceIndex=param2==3u?OpaqueRenderIndex(drawOrdinal):drawOrdinal;
    inst=LoadCameraInstance(instanceIndex);
    const bool replaced=generation.y!=0u&&
        instanceIndex<cutGeometry.w&&
        (inst.id&0x80000000u)!=0u&&(inst.flags&19u)==0u;
    if(!replaced&&(inst.flags&128u)==0u){
        const MeshRange range=LoadMeshRange(inst.mesh);
        StructuredBuffer<Meshlet> meshlets=ResourceDescriptorHeap[
            NonUniformResourceIndex(MeshResource(inst.mesh,MeshMeshlets,Mg2Meshlets))];
        const uint meshlet=MeshletOrdinal(group);
        if(meshlet<range.meshletCount){
            const uint index=range.firstMeshlet+meshlet;
            if(PagedMeshletInAnyView(inst,index)){
                m=meshlets[index];
                vertexCount=m.vertexCount;
                triangleCount=m.triangleCount;
            }
        }
    }
    // DXIL mesh validation requires exactly one dominating output-count call.
    SetMeshOutputCounts(vertexCount,triangleCount);
    for(uint vertex=tid;vertex<vertexCount;vertex+=64u){
        StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[
            NonUniformResourceIndex(MeshResource(inst.mesh,MeshVertices,Mg2Vertices))];
        const uint index=MeshletVertexIndex(inst.mesh,m.vertexOffset+vertex);
        Vertex v=vertices[index];
        if((inst.flags&2u)!=0u){
            StructuredBuffer<Vertex> deformed=
                ResourceDescriptorHeap[DynamicVertices];
            v=deformed[index];
        }
        float3 world=World(v.position,inst);
#if !OASIS_SHUTTLE_CRT
        if((inst.flags&ShuttleSpeedDustFlag)!=0u)world=ShuttleDustVertex(v,inst,view);
        if((inst.flags&OASIS_INSTANCE_FOREST_INSECTS)!=0u)world=ForestInsectVertex(v,inst,view);
#endif
        verts[vertex].position=mul(float4(world,1),vp[view]);
#if OASIS_MATERIAL_MASK == 64
        verts[vertex].uv=v.uv*InstanceSurfaceUVScale(inst);verts[vertex].uv1=v.uv1;
        verts[vertex].material=inst.material;
        verts[vertex].normal=normalize(Rotate(v.normal/
            (sign(inst.scale.xyz)*max(abs(inst.scale.xyz),.000001)),inst.rotation));
        verts[vertex].receiver=inst.id;
#endif
    }
    for(uint t=tid;t<triangleCount;t+=64u){
        tris[t]=MeshletTriangle(inst.mesh,m.triangleOffset,t);
    }
}
[outputtopology("triangle")]
[numthreads(64,1,1)]
void CutDepthMeshMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,uint view:SV_ViewID,
                 out vertices DepthOut verts[64],out indices uint3 tris[124]){
    uint local=(group.x+group.y*65535u)*21u;
    StructuredBuffer<uint4> cacheState=ResourceDescriptorHeap[cutRaster.x];
    uint blockDescriptor=cacheState[0].x==0u?cutRaster.y:cutRaster.z;
    StructuredBuffer<CutRasterBlock> blocks=
        ResourceDescriptorHeap[NonUniformResourceIndex(blockDescriptor)];
    CutRasterBlock block=blocks[objectIndex];
    const uint faceCount=cacheState[0].y!=0u&&local<param0?min(21u,param0-local):0u;
    SetMeshOutputCounts(faceCount*3u,faceCount);
    uint descriptor=block.geometryState==0u?cutGeometry.x:cutRaster.w;
    uint start=(block.geometryState==0u?block.firstTriangle:block.activeFirst)+local;
    if(tid<faceCount*3u){
        StructuredBuffer<CutTriangle> source=
            ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
        CutTriangle cutFace=source[start+tid/3u];
        Vertex v=cutFace.a;
        if(tid%3u==1u)v=cutFace.b;
        else if(tid%3u==2u)v=cutFace.c;
        verts[tid].position=mul(float4(v.position,1),vp[view]);
        verts[tid].uv=v.uv;
        verts[tid].uv1=v.uv1;
        verts[tid].material=cutFace.material;
#if OASIS_MATERIAL_MASK == 64
        verts[tid].normal=v.normal;verts[tid].receiver=0xffffffffu;
#endif
    }
    if(tid<faceCount)
        tris[tid]=uint3(tid*3u,tid*3u+1u,tid*3u+2u);
}

// Camera coverage uses the undisplaced surface UVs in every raster pass.
// The hardware OMM remains the mip-zero ray-intersection representation.
float CameraCoverageAlpha(Surface surface,float2 uv,float2 uv1){
    float alpha=surface.opacity.x;
    if(surface.albedo!=0xffffffffu)alpha*=SampleMaterial(surface.albedo,surface,0,uv,uv1).a;
    return alpha;
}
#if OASIS_MATERIAL_MASK == 64
uint2 SolarPublishReceiver(DepthOut i,bool front,Surface surface,bool opaqueSeed){
    // H1 keeps solar visibility in the geometric-normal domain. Opaque
    // PBR architecture can now use the same result even with normal maps
    // and procedural relief; the actual BRDF remains in the color shader.
    // Modified-normal cutouts retain the original fragment fallback.
    const uint kind=surface.extra.w;
    const bool geometricReceiver=opaqueSeed||surface.extra.y==0u||solarCacheInfo.w==4u||
        (surface.normal==0xffffffffu&&surface.procedural.y<=0.f);
    const bool materialAdmitsSun=kind!=1u&&kind!=7u&&kind!=9u&&
        (solarCacheInfo.w==4u||(kind!=3u&&kind!=4u&&kind!=5u&&kind!=6u&&
            kind!=8u&&kind!=10u&&kind!=11u));
    // Marker 1 distinguishes a visible excluded material from empty depth.
    if(!geometricReceiver||!materialAdmitsSun||(!opaqueSeed&&surface.extra.y==2u))return uint2(1u,0u);
    float3 n=normalize(i.normal);
    if(surface.extra.z&&!front)n=-n;
    if(!all(isfinite(n))||dot(n,n)<.5f)return uint2(1u,0u);
    // Bit 30 is spare in the packed 2x15-bit normal. Thin foliage can receive
    // sunlight on either side; the compute pass orients the ray to the light.
    const uint thin=(surface.extra.z!=0u&&surface.opticalLayers.w>0.f)?0x40000000u:0u;
    return uint2(SolarPackReceiverNormal(n)|thin,i.receiver);
}
uint2 CameraSolarDepthPixel(DepthOut i,bool front:SV_IsFrontFace):SV_Target0{
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    const Surface surface=materials[i.material];
    if(surface.extra.y)clip(CameraCoverageAlpha(surface,i.uv,i.uv1)-surface.opacity.y);
    return SolarPublishReceiver(i,front,surface,false);
}
// The seed list contains only opaque geometry. No alpha texture access.
[earlydepthstencil]
uint2 CameraSolarSeedPixel(DepthOut i,bool front:SV_IsFrontFace):SV_Target0{
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    return SolarPublishReceiver(i,front,materials[i.material],true);
}
#endif
void CameraDepthPixel(DepthOut i){StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];Surface surface=materials[i.material];if(surface.extra.y)clip(CameraCoverageAlpha(surface,i.uv,i.uv1)-surface.opacity.y);}
void DepthPixel(Out i){StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];Surface surface=materials[i.material];if(surface.extra.y)clip(CameraCoverageAlpha(surface,i.uv,i.uv1)-surface.opacity.y);}
float StaticShadow(float3 p,Light light,uint slot) {
    float3 d=p-light.positionRadius.xyz;float3 ad=abs(d);uint face=ad.x>=ad.y&&ad.x>=ad.z?(d.x>=0?0:1):ad.y>=ad.z?(d.y>=0?2:3):(d.z>=0?4:5);
    float4 clip=mul(float4(p,1),shadowVP[slot*6+face]);if(clip.w<=0)return 1;float3 ndc=clip.xyz/clip.w;if(ndc.z<0||ndc.z>1)return 1;
    float2 uv=ndc.xy*float2(.5,-.5)+.5,tile=float2((slot%2)*3+face%3,(slot/2)*2+face/3);float result=0;
    Texture2D<float> atlas=ResourceDescriptorHeap[ShadowAtlas];SamplerComparisonState comparison=SamplerDescriptorHeap[1];
    [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++){float2 q=clamp(uv+float2(x,y)/512,.5/512,1-.5/512);result+=atlas.SampleCmpLevelZero(comparison,(q+tile)/float2(6,4),ndc.z+.0006);}return result/9;
}
float Visibility(float3 p,float3 n,Light light,uint index,uint opticalSample=OpticalNoSample,
                 uint receiver=0xffffffffu,uint receiverMaterial=0xffffffffu,float3 receiverNormal=float3(0,0,0),float footprint=0.f,
                  float2 screen=float2(0,0),uint view=0xffffffffu,bool opaqueReceiver=false,float receiverDepth=0.f) {
    // Derivatives are evaluated by the caller; helpers have no color output.
    if(IsHelperLane())return 1.f;
    if(!((uint)light.parameters.z&1))return 1;
    bool directional=((uint)light.parameters.z&2)!=0;float3 direction=directional?-light.directionOuter.xyz:light.positionRadius.xyz-p;float distance=directional?timing.y:length(direction);
    int slot=(int)light.parameters.y;
    const bool sun=HasGlobalSun&&index==LightCount&&slot<0;
    // The BRDF is evaluated with the mapped shading normal in ShadeLight.
    // Global solar visibility belongs to the actual raster surface instead.
    const bool geometricSun=sun&&opaqueReceiver&&view<ViewCount&&
        all(isfinite(receiverNormal))&&dot(receiverNormal,receiverNormal)>.5f;
    const float3 visibilityNormal=geometricSun?receiverNormal:n;
    // A fully blocked static shadow cannot be made visible by a dynamic ray.
    // Evaluate the atlas once, before any BVH traversal; the product is unchanged.
    const float staticVisibility=slot>=0?StaticShadow(p+n*0.0582524271f,light,(uint)slot):1.f;
    if(staticVisibility<=0.f)return 0.f;
    if((Quality==0&&index>=4)||RayInstanceCount==0)return staticVisibility;
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];RayDesc ray;ray.Origin=p+visibilityNormal*0.0582524271f;ray.Direction=normalize(direction);ray.TMin=0.0582524271f;ray.TMax=max(0.0873786407f,distance-0.291262136f);
    if(solarSun.w>0.f&&sphereScene.z==0u){
        // Only the active body's geometry occupies this canonical space.
        const float3 local=ray.Origin-atmosphereSphere.xyz;
        const float along=dot(local,ray.Direction);
        const float disc=along*along+atmosphereSphere.w*atmosphereSphere.w-dot(local,local);
        ray.TMax=clamp(-along+sqrt(max(0.f,disc))+0.0873786407f,0.0873786407f,ray.TMax);
    }
    const uint rayMask=slot>=0?2u:3u;
    const bool useCut=CutRayActive();
    if(geometricSun)ray.TMax=SolarTraceLimit(ray.Origin,ray.Direction);
    if(sun)OpticalCount(opticalSample,0u);
    // 0: adaptive two-anchor visibility, 1: original exact per-fragment DXR,
    // 2: diagnostic bypass of global solar shadows only. Local lights unchanged.
    if(sun&&reflectionSettings.w==2.f){
        OpticalCount(opticalSample,20u);
        return 1.f;
    }
    if(sun&&reflectionSettings.w==0.f&&!useCut&&opaqueReceiver&&view<ViewCount){
        float sampled;
        if(SolarResolvedVisibility(screen,view,receiverDepth,visibilityNormal,receiver,opticalSample,sampled))
            return sampled;
        OpticalCount(opticalSample,27u);
        if(DomainResource(ResourceDomainLighting,LightingSolarVisibility)!=0xffffffffu)
            OpticalCount(opticalSample,31u);
        // A missing receiver is handled by the pre-existing material wave
        // sharing path. Exact mode and live cut tracing remain untouched.
        return AdaptiveSolarVisibility(ray,receiver,receiverMaterial,visibilityNormal,
            footprint,screen,view,opticalSample);
    }
    if(sun)OpticalCount(opticalSample,2u);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene,RAY_FLAG_NONE,HardwareRayMask(rayMask,useCut),ray);
    float transmission=1.f;
    while(q.Proceed()){
        if(q.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
        const uint rayIndex=q.CandidateInstanceID();
        if(useCut&&RayUsesCutReplacement(rayIndex))continue;
        if(sun)OpticalCount(opticalSample,23u);
        const float2 opacity=RayOpacity(rayIndex,q.CandidatePrimitiveIndex(),q.CandidateGeometryIndex(),q.CandidateTriangleBarycentrics());
        if(opacity.y==2)transmission*=1.f-opacity.x;
        else if(opacity.x>0.f)q.CommitNonOpaqueTriangleHit();
    }
    if(q.CommittedStatus()==COMMITTED_TRIANGLE_HIT)return 0.f;
    if(useCut&&slot<0&&TraceCutRay(ray,rayMask,true).descriptor!=0xffffffffu)return 0.f;
    return transmission*staticVisibility;
}
float BarkSpecularScale(Surface s){return s.extra.w==15u?saturate(float(forestFogBinding.y)*.01f):1.f;}
float3 BRDF(float3 base,float roughness,float metal,float3 n,float3 v,float3 l,float3 irradiance,float dielectricF0,float specularScale){float3 h=normalize(v+l);float nl=saturate(dot(n,l)),nv=saturate(dot(n,v)),nh=saturate(dot(n,h)),vh=saturate(dot(v,h));float a=max(.045,roughness*roughness),a2=a*a;float denominator=nh*nh*(a2-1)+1;float d=a2/max(Pi*denominator*denominator,.0001);float k=(roughness+1)*(roughness+1)/8;float g=(nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k));float3 fresnel=lerp(dielectricF0.xxx,base,metal)+(1-lerp(dielectricF0.xxx,base,metal))*pow(1-vh,5);return ((1-fresnel)*(1-metal)*base/Pi+specularScale*fresnel*d*g/max(4*nv*nl,.001))*irradiance*nl;}
// Energy-partitioned thin diffuse sheet. The existing dielectric specular
// lobe stays on the illuminated face; diffuse transmission is not emission
// and does not change the geometric/OMM alpha coverage.
float3 ThinSurfaceBRDF(float3 base,float roughness,float metal,float3 n,float3 v,
                       float3 l,float3 irradiance,float dielectricF0,float diffuseTransmission,float specularScale){
    [branch]if(diffuseTransmission<=0.f)
        return BRDF(base,roughness,metal,n,v,l,irradiance,dielectricF0,specularScale);
    const float nl=dot(n,l);
    if(nl>0.f)
        return BRDF(base*(1.f-diffuseTransmission),roughness,metal,n,v,l,irradiance,dielectricF0,specularScale);
    return base*irradiance*((1.f-dielectricF0)*(1.f-metal)*diffuseTransmission*
                            saturate(-nl)/Pi);
}
float3 ShadeLight(float3 p,float3 base,float roughness,float metal,float3 n,float3 v,Light light,uint index,bool indoors,float dielectricF0,inout float4 roomRadiance,float diffuseTransmission=0.f,float specularScale=1.f,uint opticalSample=OpticalNoSample,
                  uint receiver=0xffffffffu,uint receiverMaterial=0xffffffffu,float3 receiverNormal=float3(0,0,0),float footprint=0.f,
                  float2 screen=float2(0,0),uint view=0xffffffffu,bool opaqueReceiver=false,float receiverDepth=0.f){


    if(light.colorPower.w<=0||all(light.colorPower.rgb==0))return 0;
    const bool room=IsRoomLight(light);
    if((indoors&&!room)||(room&&!RoomLightReaches(light,p)))return 0;
    float3 bounce=0.f;
    if(room){
        // Reuse the already evaluated low-frequency room lighting. No second
        // BVH or probe traversal is required to approximate a broad reflection.
        const float3 irradiance=RoomDiffuseBounce(light,index,p,n,1.f.xxx,0.f);
        bounce=base*(1.f-metal)*irradiance;
        roomRadiance+=float4(irradiance,1.f);
    }
    bool directional=((uint)light.parameters.z&2)!=0;
    float3 delta=directional?-light.directionOuter.xyz:light.positionRadius.xyz-p;
    if(diffuseTransmission<=0.f&&dot(n,delta)<=0.f)return bounce;


    float distanceSquared=dot(delta,delta);
    if(!directional&&distanceSquared>=light.positionRadius.w*light.positionRadius.w)return bounce;
    float distance=sqrt(distanceSquared);
    float3 direction=delta/max(distance,0.0291262136f);
    float falloff=1;
    if(!directional){


        float ratio=distance/light.positionRadius.w;
        float ratioSquared=ratio*ratio;
        float fade=saturate(1-ratioSquared*ratioSquared);
        falloff=fade*fade/max(16.9667263f,distanceSquared);
        if(!room&&light.directionOuter.w>-1)
            falloff*=saturate((dot(-direction,light.directionOuter.xyz)-light.directionOuter.w)/max(.00001,light.parameters.x-light.directionOuter.w));
        if(falloff<=0)return bounce;
    }
    const float3 unshadowed=ThinSurfaceBRDF(base,roughness,metal,n,v,direction,light.colorPower.rgb*light.colorPower.w*falloff,dielectricF0,diffuseTransmission,specularScale);
    if(all(unshadowed<=0.f))return bounce;
    const float3 lightSideNormal=diffuseTransmission>0.f&&dot(n,direction)<0.f?-n:n;
    float shadow=Visibility(p,lightSideNormal,light,index,opticalSample,receiver,receiverMaterial,receiverNormal,footprint,screen,view,opaqueReceiver,receiverDepth);
    return bounce+unshadowed*shadow;
}


float2 UrbanHeightUV(Surface s,float2 uv,float3 world,float3 n,float3 view){
    if(s.extra.w!=2||s.parameters.y<=0||s.metalRough==0xffffffffu)
        return uv;
    float3 dpdx=ddx(world),dpdy=ddy(world);
    float2 duvdx=ddx(uv),duvdy=ddy(uv);
    float det=duvdx.x*duvdy.y-duvdx.y*duvdy.x;
    float facing=abs(dot(view,n));
    if(abs(det)<1.e-7||facing<.09||facing>.985)
        return uv;
    float orientation=det<0?-1:1;
    float3 t=normalize((dpdx*duvdy.y-dpdy*duvdx.y)*orientation);
    float3 b=normalize((dpdy*duvdx.x-dpdx*duvdy.x)*orientation);
    float2 shift=float2(dot(view,t),dot(view,b))*
        s.parameters.yz/max(.12,facing);
    float steps=ceil(lerp(6.,14.,1.-facing));
    float2 delta=shift/steps;
    Texture2D<float4> orm=ResourceDescriptorHeap[NonUniformResourceIndex(s.metalRough)];
    SamplerState sampler=SamplerDescriptorHeap[NonUniformResourceIndex((uint)s.mapping[2].rotationSet.w)];
    float2 current=uv;
    float depth=0,stepDepth=1./steps;
    [loop]for(int index=0;index<14;index++){
        float height=orm.SampleGrad(sampler,MaterialUV(s,2,current,current),duvdx,duvdy).a;
        if(index>=int(steps)||depth>=1.-height)break;
        current-=delta;
        depth+=stepDepth;
    }
    return current;
}
float UrbanMacro(float3 p){
    return .62*sin(dot(p,float3(0.00387966667f,0.00127033333f,0.00295266667f)))*
               sin(dot(p,float3(-0.001648f,0.00312433334f,0.00209433334f)))+
           .38*sin(dot(p,float3(0.00995666668f,0.00260933334f,-0.00755333334f)));
}
struct ColorOut{float4 direct:SV_Target0;float4 reflection:SV_Target1;};
#if OASIS_SHUTTLE_CRT
#include "shuttle_crt.hlsli"
[earlydepthstencil]
ColorOut PixelMain(Out i){
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    ColorOut screen;
    if(solarCacheInfo.w==5u){screen.direct=float4(.5f,.5f,.5f,1.f);screen.reflection=0.f;return screen;}
    screen.direct=float4(ShuttleCrt(materials[i.material],i.uv,i.uv1,i.crt),1.f);
    screen.reflection=0.f;
    return screen;
}
#else
float3 MappedSurfaceNormal(Surface s,float3 n,float3 world,float2 uv0,float2 uv1){
    float2 xy=(SampleMaterial(s.normal,s,1,uv0,uv1).xy*2-1)*s.parameters.x;
    float3 dp1=ddx(world),dp2=ddy(world);
    float2 uv=MaterialUV(s,1,uv0,uv1),duv1=ddx(uv),duv2=ddy(uv);
    float determinant=duv1.x*duv2.y-duv1.y*duv2.x;
    if(abs(determinant)<=.0000001f)return n;
    float direction=determinant<0?-1:1;
    float3 tangent=normalize((dp1*duv2.y-dp2*duv1.y)*direction);
    float3 bitangent=normalize((dp2*duv1.x-dp1*duv2.x)*direction);
    return normalize(tangent*xy.x+bitangent*xy.y+n*sqrt(saturate(1-dot(xy,xy))));
}
#include "reflection_hit.hlsli"
// The prepass writes depth. Color passes only read it, including alpha-masked surfaces.
[earlydepthstencil]
ColorOut PixelMain(Out i,bool front:SV_IsFrontFace){
    
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];Surface s=materials[i.material];float3 base=s.colorRoughness.rgb,emission=s.emissionMetallic.rgb;float roughness=s.colorRoughness.a,metal=s.emissionMetallic.a,alpha=s.opacity.x;float3 n=normalize(i.normal);if(s.extra.z&&!front)n=-n;float3 v=normalize(eye[i.view].xyz-i.world);
    const uint prism=solarCacheInfo.w;
    [branch]if(prism==5u){
        if(s.extra.y==1u){
            clip(CameraCoverageAlpha(s,i.uv,i.uv1)-s.opacity.y);
        }
        ColorOut minimal;minimal.direct=float4(.5f,.5f,.5f,s.extra.y==2u?saturate(alpha):1.f);
        minimal.reflection=0.f;return minimal;
    }
    // Derivatives before divergent material/ray branches, in the actual per-eye render space.
    const float2 worldDerivatives=float2(length(ddx(i.world)),length(ddy(i.world)));
    const float pixelWidth=max(worldDerivatives.x,worldDerivatives.y);
    const float solarFootprint=max(worldDerivatives.x,worldDerivatives.y);
    const float diffuseTransmission=s.extra.z!=0u?saturate(s.opticalLayers.w):0.f;
    const float3 solarNormal=diffuseTransmission>0.f&&HasGlobalSun&&dot(n,SolarDirection(i.world))<0.f?-n:n;
    const float pixelAngle=max(length(ddx(v)),length(ddy(v)));
    float2 surfaceUV=i.uv;
    float occlusion=1.f;
    // PRISM E: retain alpha coverage and normal lighting, skip material detail.
    [branch]if(prism==4u){
        if(s.extra.y==1u){
            clip(CameraCoverageAlpha(s,i.uv,i.uv1)-s.opacity.y);
        }
        if(s.extra.y!=2u)alpha=1.f;
        roughness=max(s.opticalPolicy.z,roughness);
        metal=min(s.opticalPolicy.w,saturate(metal+s.metallicBias));
    }else{
    if(s.extra.w==8u){
        const float t=timing.z;
        const float2 uv=i.uv;
        const float a=sin(uv.y*71.f-3.1f*t+uv.x*4.7f);
        const float b=sin(uv.y*29.f-1.8f*t-uv.x*12.f);
        const float detail=1.f-saturate(max(fwidth(uv.x)*5.f,fwidth(uv.y)*35.f));
        const float shore=1.f-smoothstep(.018f,.16f,min(uv.x,1.f-uv.x));
        const float3 rippled=normalize(n+float3(a,.0f,b)*.16f*detail);
        const float angle=1.f-saturate(abs(dot(rippled,v)));
        const float sheen=.07f+.32f*angle*angle;
        const float3 sulfur=lerp(float3(.29f,.148f,.009f),float3(.96f,.70f,.038f),
            saturate(.56f+(.15f*a+.12f*b)*detail-.17f*shore));
        const float3 foam=float3(.86f,.74f,.29f)*shore*smoothstep(.48f,.92f,a*.5f+b*.34f)*detail;
        ColorOut river;
        river.direct=float4(sulfur*(.92f-sheen)+float3(.32f,.38f,.42f)*sheen+foam,1.f);
        river.reflection=0.f;
        return river;
    }
    // MG2 workstation CRT-era ambient screensaver. Original procedural
    // visual, exclusively on monitor screen material (not the plastic bezel).
    if(s.extra.w==10u){
        const float2 uv=saturate(i.uv);
        const float t=timing.z;
        const float2 centered=uv*2.f-1.f;
        const float angle=t*.19f;
        const float2 axis=float2(cos(angle),sin(angle));
        const float2 rotated=float2(dot(centered,axis),
            dot(centered,float2(-axis.y,axis.x)));
        const float2 boundary=abs(rotated)-float2(.39f,.27f);
        const float outline=min(abs(boundary.x),abs(boundary.y));
        const float other=max(boundary.x,boundary.y);
        const float frame=(1.f-smoothstep(.005f,.029f,outline))*
            (1.f-smoothstep(-.02f,.06f,other));
        const float horizontal=saturate(1.f-abs(centered.y*.53f-
            sin(centered.x*7.f+t*.43f)*.11f)*28.f)*.14f;
        const float stars=pow(saturate(sin(uv.x*132.7f+uv.y*237.1f)*
            sin(uv.y*193.3f-uv.x*113.5f)),46.f);
        const float scan=.92f+.08f*sin(uv.y*630.f);
        const float3 night=float3(.014f,.022f,.042f);
        const float3 neon=float3(.09f,.64f,.56f)*frame+
                          float3(.10f,.18f,.65f)*horizontal+
                          stars*float3(.12f,.29f,.34f);
        ColorOut monitor;
        monitor.direct=float4((night+neon)*scan,1.f);
        monitor.reflection=0.f;
        return monitor;
    }

    // Rackmount status lights blink at independent, stable pseudo-random
    // phases based on physical LED location, without per-LED CPU updates.
    if(s.extra.w==11u){
        const float3 grid=floor(i.world*0.274666667f);
        const float phase=frac(sin(dot(grid,float3(12.9898,78.233,37.719)))*43758.5453);
        const float frequency=1.1+phase*3.7;
        const float beat=frac(timing.z*frequency+phase);
        const float luminance=beat<.19f?.12f:beat<.37f?2.5f:.65f;
        ColorOut led;
        led.direct=float4(s.colorRoughness.xyz*luminance,1.f);
        led.reflection=0.f;
        return led;
    }
#if OASIS_MATERIAL_MASK < 0
    if(s.extra.w==5u){
        const float fresnel=.04f+.96f*pow(1.f-saturate(abs(dot(n,v))),5.f);
        const float3 mirror=reflect(-v,n);
        const float spread=.25f+.75f*saturate(AuthorRoughness(s,roughness,i.uv,false));
        const float sky=smoothstep(-spread,spread,mirror.y);
        const float3 tint=AuthorColor(s,base);
        const float3 environment=lerp(float3(.047f,.079f,.087f),float3(.19f,.28f,.36f),sky);
        const float opacity=saturate(fresnel+alpha*(1.f-fresnel)*.55f);
        ColorOut pane;
        const float4 glass=float4((environment*fresnel+tint*(opacity-fresnel))/max(opacity,.001f),opacity);
        // Genuine transparent PBR glazing: the geometry rasterized behind
        // the pane supplies the interior. Neither streaming nor distance
        // may substitute a painted room, silhouette or opaque proxy.
        pane.direct=glass;
        pane.reflection=float4(0,0,0,pane.direct.a);
        return pane;
    }
    if(s.extra.w==3u||s.extra.w==4u){
        const bool crtGlass=s.extra.w==4u;
        if(s.normal!=0xffffffffu)n=MappedSurfaceNormal(s,n,i.world,i.uv,i.uv1);
        const float sharpness=exp2(-2.f*(AuthorRoughness(s,roughness,i.uv,false)-roughness));
        float4 glassTexture=1.f;
        if(s.albedo!=0xffffffffu)glassTexture=SampleMaterial(s.albedo,s,0,i.uv,i.uv1);
        if(s.emissive!=0xffffffffu)emission*=SampleMaterial(s.emissive,s,3,i.uv,i.uv1).rgb;
        const float facing=saturate(abs(dot(n,v)));
        const float edge=pow(1.f-facing,5.f);
        const float3 reflected=reflect(-v,n);
        const float sky=smoothstep(-.40f,.70f,reflected.y);
        const float3 sun=normalize(solarSun.xyz-i.world+float3(.0001f,.0002f,0.f));
        const float sunGlint=pow(saturate(dot(reflected,sun)),27.f*sharpness);
        const float panelGlint=pow(saturate(dot(reflected,normalize(float3(-.19f,.87f,.46f)))),13.f*sharpness);
        const float horizon=1.f-smoothstep(.03f,.52f,abs(reflected.y-.09f));
        const float3 cool=float3(.15f,.24f,.34f)*(.32f+.68f*sky);
        const float3 amber=float3(.42f,.24f,.085f);
        const float3 sheen=crtGlass?cool*.54f+amber*panelGlint*.50f:
            cool+float3(.14f,.18f,.23f)*horizon+amber*sunGlint;
        ColorOut glass;
        glass.direct=float4(AuthorColor(s,sheen*glassTexture.rgb)*(.40f+.60f*edge)*(1.f+.4f*s.metallicBias)+emission,
            saturate(glassTexture.a*s.opacity.w*(crtGlass?.073f+.36f*edge+.12f*panelGlint:.085f+.53f*edge+.18f*sunGlint)));
        glass.reflection=float4(0,0,0,glass.direct.a);
        return glass;
    }
#endif

    if(s.extra.w==6u){
        ColorOut fitting;
        fitting.direct=float4(base*.12f+emission,1.f);
        fitting.reflection=0;
        return fitting;
    }

    if(s.extra.w==2u&&s.extra.y==2u&&s.colorRoughness.w<.12)
        surfaceUV+=float2(-.031,.012)*timing.z;
#if OASIS_MATERIAL_MASK < 0 || (OASIS_MATERIAL_MASK & 4)
    surfaceUV=UrbanHeightUV(s,surfaceUV,i.world,n,v);
#endif
#if OASIS_MATERIAL_MASK < 0
    if(s.albedo!=0xffffffff){float4 color=SampleMaterial(s.albedo,s,0,surfaceUV,i.uv1);base*=color.rgb;alpha*=color.a;}
#elif (OASIS_MATERIAL_MASK & 1)
    {float4 color=SampleMaterial(s.albedo,s,0,surfaceUV,i.uv1);base*=color.rgb;alpha*=color.a;}
#endif
    base=AuthorColor(s,base);
    if(s.extra.y==1u){
        // POM modifies material appearance, not the depth silhouette.
        // Ordinary foliage and other cutouts reuse the existing albedo sample.
        if(s.extra.w==2u&&s.parameters.y>0.f&&s.metalRough!=0xffffffffu)
            alpha=CameraCoverageAlpha(s,i.uv,i.uv1);
        clip(alpha-s.opacity.y);
    }
    if(s.extra.y!=2u)alpha=1.f;
#if OASIS_MATERIAL_MASK < 0
    if(s.normal!=0xffffffff)n=MappedSurfaceNormal(s,n,i.world,surfaceUV,i.uv1);
#elif (OASIS_MATERIAL_MASK & 2)
    n=MappedSurfaceNormal(s,n,i.world,surfaceUV,i.uv1);
#endif
    if(s.procedural.y>0){
        float3 q=i.world*s.procedural.y*6.2831853;float footprint=max(length(ddx(q)),length(ddy(q)));float filtered=exp(-footprint*footprint*.08);
        float3 gradient=float3(cos(q.x)*sin(q.z*.79),cos(q.y*.83)*sin(q.x*.71),cos(q.z*.79)*sin(q.y*.83));
        n=normalize(n+(gradient-n*dot(gradient,n))*s.procedural.x*filtered);
        base*=1+.035*filtered*sin(q.x)*sin(q.y*.83)*sin(q.z*.79);


        float edge=min(min(i.uv.x,1-i.uv.x),min(i.uv.y,1-i.uv.y));float aa=max(fwidth(edge),.0001);
        float seam=1-smoothstep(s.procedural.z-aa,s.procedural.z+aa,edge);base*=1-seam*s.procedural.w;
        roughness=clamp(roughness+seam*.08,.04,1);
    }
    float surfaceAo=1.;
#if OASIS_MATERIAL_MASK < 0
    if(s.metalRough!=0xffffffff){
#elif (OASIS_MATERIAL_MASK & 4)
    {
#endif
#if OASIS_MATERIAL_MASK < 0 || (OASIS_MATERIAL_MASK & 4)
        float4 sample=SampleMaterial(s.metalRough,s,2,surfaceUV,i.uv1);
        roughness*=sample.g;metal*=sample.b;
        if(s.extra.w==2)surfaceAo=lerp(1.,sample.r,.82);
    }
#endif
    if(s.parameters.w>0){
        float variation=UrbanMacro(i.world)*s.parameters.w;
        base*=1.+variation;
        roughness=saturate(roughness*(1.-variation*.45));
    }
#if OASIS_MATERIAL_MASK < 0
    if(s.emissive!=0xffffffff)emission*=SampleMaterial(s.emissive,s,3,i.uv,i.uv1).rgb;
#elif (OASIS_MATERIAL_MASK & 8)
    emission*=SampleMaterial(s.emissive,s,3,i.uv,i.uv1).rgb;
#endif
    roughness=max(s.opticalPolicy.z,AuthorRoughness(s,roughness,surfaceUV,false));
    // Morning moisture changes the existing material response.
    [branch]if(forestAtmosphere.y>0.f&&(s.extra.w==12u||s.extra.w==13u||s.extra.w==14u||s.extra.w==15u)){
        const float3 p=i.world;
        const float patch=.65f+.35f*sin(p.x*1.17f+p.z*.83f)*sin(p.z*1.43f-p.x*.37f);
        const float wet=forestAtmosphere.y*patch;
        base*=1.f-wet*(s.extra.w==12u?.12f:.30f);
        roughness=lerp(roughness,s.extra.w==12u?.26f:.43f,wet);
        s.opticalPolicy.y=lerp(s.opticalPolicy.y,.04f,wet);
    }
    // PBR plaster must not reflect like varnished metal even when a scanned
    // roughness map contains local dark texels.
    if(s.extra.w==9u)roughness=max(roughness,.82f);
    metal=min(s.opticalPolicy.w,saturate(metal+s.metallicBias));
    if(atmosphereSphere.w>0&&atmosphereLight.w>0&&any(emission>0)){
        float3 delta=atmosphereLight.xyz-i.world;
        float3 incoming=delta*rsqrt(max(dot(delta,delta),.000001));
        float3 boundary=dot(n,incoming)>=0?n:-n;
        float3 refracted=refract(-incoming,boundary,1.0/1.45);
        float alignment=saturate(dot(-refracted,v));
        emission*=atmosphereParams.y*(.9+.1*alignment);
    }
#if OASIS_MATERIAL_MASK < 0
    occlusion=(s.extra.x==0xffffffff?1:
        lerp(1,SampleMaterial(s.extra.x,s,4,surfaceUV,i.uv1).r,s.opacity.z))*surfaceAo;
#elif (OASIS_MATERIAL_MASK & 16)
    occlusion=lerp(1,SampleMaterial(s.extra.x,s,4,surfaceUV,i.uv1).r,s.opacity.z)*surfaceAo;
#else
    occlusion=surfaceAo;
#endif
    } // Full material path.
    // Last implicit-LOD material sample and all derivatives are above.
    // Helpers have no render-target contribution: no light-list traversal,
    // room probes, BRDF evaluation, optical bookkeeping or environment work.
    [branch]if(IsHelperLane()){
        ColorOut helper;helper.direct=0.f;helper.reflection=0.f;return helper;
    }
    float3 color=base*(.026+.018*saturate(n.y))*occlusion+emission;
    // Low-frequency interior ambient fill for plaster and ceiling surfaces.
    if(s.extra.w==9u)color+=base*(.075f+.035f*saturate(n.y))*occlusion;
    if((sphereScene.y!=0u||sphereScene.z!=0u)&&s.extra.w!=7u&&s.extra.w!=9u)color+=base*(.095f*occlusion);


    if(s.extra.w==2){


        float hemisphere=.27+.49*saturate(n.y);
        color+=base*occlusion*hemisphere*float3(.32,.39,.52);
    }
    const float specularScale=BarkSpecularScale(s);
    const uint opticalSample=OpticalSampleIndex(i.position.xy,i.view);
    float4 roomRadiance=0.f;
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    if(s.extra.w==1)color=base+emission;
    else if(prism==3u){} // PRISM D: skip local light/probe shading, preserve global sun.
    else if(LightCount<=4){[loop]for(uint index=0;index<LightCount;index++)color+=ShadeLight(i.world,base,roughness,metal,n,v,lights[index],index,(s.extra.w==7u||s.extra.w==9u),s.opticalPolicy.y,roomRadiance,diffuseTransmission,specularScale);}
    else{
        StructuredBuffer<uint> directionalCount=ResourceDescriptorHeap[DirectionalCount];
        StructuredBuffer<uint> directional=ResourceDescriptorHeap[DirectionalIndices];
        [loop]for(uint j=0;j<directionalCount[0];++j){
            uint index=directional[j];
            color+=ShadeLight(i.world,base,roughness,metal,n,v,lights[index],index,(s.extra.w==7u||s.extra.w==9u),s.opticalPolicy.y,roomRadiance,diffuseTransmission,specularScale);
        }
        StructuredBuffer<uint2> clusters=ResourceDescriptorHeap[Clusters];
        StructuredBuffer<uint> indices=ResourceDescriptorHeap[ClusterIndices];
        uint2 span=clusters[ClusterIndex(uint2(i.position.xy),i.position.z,i.view)];
        if(span.y==ClusterOverflow){


            uint cursor=1u;
            [loop]while(true){
                uint index=LightBvhNextLight(cursor,LightBvhLeafCount,
                                            i.world,0.0f);
                if(index==0xffffffffu)break;
                if(((uint)lights[index].parameters.z&2u)==0u)
                    color+=ShadeLight(i.world,base,roughness,metal,n,v,
                                      lights[index],index,(s.extra.w==7u||s.extra.w==9u),s.opticalPolicy.y,roomRadiance,diffuseTransmission,specularScale);
            }
        }else{
            [loop]for(uint j=0;j<span.y;++j){
                uint index=indices[span.x+j];
                color+=ShadeLight(i.world,base,roughness,metal,n,v,
                                  lights[index],index,(s.extra.w==7u||s.extra.w==9u),s.opticalPolicy.y,roomRadiance,diffuseTransmission,specularScale);
            }
        }
    }


    if(HasGlobalSun&&s.extra.w!=1u)
        color+=ShadeLight(i.world,base,roughness,metal,n,v,globalSun,LightCount,
                          s.extra.w==7u||s.extra.w==9u,s.opticalPolicy.y,roomRadiance,diffuseTransmission,specularScale,opticalSample,
                          i.receiver,i.material,solarNormal,solarFootprint,i.position.xy,i.view,s.extra.y!=2u,i.position.z);

    // Admission and reflected detail are separate. Diffuse materials never
    // request a scene image. Broad finishes use the existing low-frequency
    // illumination; a structured finish may still be too weak or too blurred
    // to justify a ray at this particular pixel.
    bool reflective=false;
    float3 reflection=0.f;
    if(s.opticalPolicy.x>0.f&&s.extra.w!=1u&&max(emission.x,max(emission.y,emission.z))<1.f){
        const float3 f0=lerp(s.opticalPolicy.y.xxx,saturate(base),metal);
        const float3 weight=(f0+(1.f-f0)*pow(1.f-saturate(dot(n,v)),5.f))*(1.f-saturate(roughness));
        const float peak=max(weight.x,max(weight.y,weight.z));
        const float3 mirrorDirection=reflect(-v,n);
        const float coneSpread=roughness*roughness*smoothstep(.05f,.09f,roughness);
        const float coneAngle=pixelAngle+coneSpread;
        const float footprintPixels=coneSpread/max(pixelAngle,1.e-5f);
        const float sharp=1.f-smoothstep(.055f,.12f,roughness);
        const float resolvedDetail=1.f-smoothstep(2.f,12.f,footprintPixels);
        // Weak, spread-out dielectric detail is not worth tracing. True mirrors,
        // strong metal reflections and meaningful grazing reflections survive.
        const float strong=smoothstep(.05f,.16f,peak);
        float detail=max(sharp,max(resolvedDetail,strong));
        detail*=smoothstep(.004f,.012f,detail*peak);
        const bool traceEligible=topology.z!=0&&s.opticalPolicy.x>.5f&&
            roughness<reflectionSettings.x&&reflectionSettings.y>0.f&&RayInstanceCount!=0;
        if(s.opticalPolicy.x>.5f)OpticalCount(opticalSample,4u);
        if(!traceEligible)detail=0.f;
        const bool indoors=s.extra.w==7u||s.extra.w==9u||roomRadiance.w>0.f;
        const float3 environment=indoors?roomRadiance.rgb:
            lerp(float3(.018f,.021f,.025f),float3(.035f,.049f,.075f),
                 smoothstep(-.55f,.55f,mirrorDirection.y));
        color+=environment*weight*(1.f-detail);
        [branch]if(detail>0.f){
            reflective=true;
            RayDesc ray;
            ray.Origin=i.world+n*0.0873786407f;ray.Direction=mirrorDirection;
            ray.TMin=0.0873786407f;ray.TMax=reflectionSettings.y;
            const float4 incident=SharedReflectionIncident(ray,CutRayActive(),i.receiver,i.material,i.view,
                i.position.xy,n,roughness,pixelWidth,coneAngle,coneSpread,footprintPixels,
                s.extra.y==0u,opticalSample);
            reflection=incident.w>0.f?incident.rgb:environment;
            reflection*=weight*detail;
        }
    }
    ColorOut output;output.direct=float4(color,saturate(alpha));output.reflection=float4(reflection,s.extra.y==2?saturate(alpha):(reflective?1:0));return output;
}
#endif
