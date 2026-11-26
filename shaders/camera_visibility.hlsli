#ifndef OASIS_CAMERA_VISIBILITY
#define OASIS_CAMERA_VISIBILITY
// Opt-in isolation only; all culling stages remain enabled by default.
static const uint VisibilityInstanceFrustumBypass=2u,VisibilityInstanceHiZBypass=4u,
    VisibilityMeshletFrustumBypass=8u,VisibilityMeshletHiZBypass=16u;
bool Frustum(float4 sphere,uint view,out float2 lo,out float2 hi,out float nearest){float3 p=sphere.xyz;float radius=sphere.w;float4 c=mul(float4(p,1),vp[view]);float rx=radius*length(float3(vp[view][0][0],vp[view][1][0],vp[view][2][0]));float ry=radius*length(float3(vp[view][0][1],vp[view][1][1],vp[view][2][1]));float rz=radius*length(float3(vp[view][0][2],vp[view][1][2],vp[view][2][2]));float rw=radius;if(c.x+rx<-(c.w+rw)||c.x-rx>c.w+rw||c.y+ry<-(c.w+rw)||c.y-ry>c.w+rw||c.z+rz<0||c.z-rz>c.w+rw){lo=0;hi=1;nearest=1;return false;}
    lo=1;hi=0;nearest=0;bool clipped=false;[unroll]for(uint k=0;k<8;k++){float3 offset=float3((k&1)?1:-1,(k&2)?1:-1,(k&4)?1:-1)*radius;float4 h=mul(float4(p+offset,1),vp[view]);if(h.w<=timing.x){clipped=true;break;}float2 uv=h.xy/h.w*float2(.5,-.5)+.5;lo=min(lo,uv);hi=max(hi,uv);nearest=max(nearest,h.z/h.w);}if(clipped){lo=0;hi=1;nearest=1;}return true;}
// BuildingSession::Visual emits the centered unit cube (or the centered
// pane, contained by that cube). MG2 copies the original source vertices;
// its selected cuts cannot extend beyond the source cube. Destruction
// publishes new parts with their own transforms. No new buffer or ABI.
bool StructuralCameraBox(GpuInstance instance){
    // BuildingObject describes ownership, not shape (trees and GLTF props
    // carry it too). A tight OBB is legal only for an explicit unit-box producer.
    return (instance.flags&OASIS_INSTANCE_UNIT_BOX)!=0u &&
           (instance.flags&(2u|ShuttleSpeedDustFlag|OASIS_INSTANCE_FOREST))==0u;
}

// xyz is the oriented world half-extent; w distinguishes sphere, measured
// box and certified unit box. Keep the validated foliage/deformation path.
float4 CameraBox(GpuInstance instance,float4 sphere){
    const float3 p=max(abs(instance.positionRadius.xyz),abs(sphere.xyz));
    const float margin=max(.0001f,2.e-6f*max(p.x,max(p.y,p.z)));
    if(StructuralCameraBox(instance))return float4(.5f*abs(instance.scale)+margin,2.f);
    if((instance.flags&(2u|ShuttleSpeedDustFlag|OASIS_INSTANCE_FOREST))!=0u)return 0.f;
    const uint slot=IsForestPage(instance.mesh)?Mg2CameraBoxes:MeshCameraBoxes;
    StructuredBuffer<uint2> table=ResourceDescriptorHeap[NonUniformResourceIndex(DomainTableDescriptor(ResourceDomainMeshes))];
    const uint2 binding=table[slot];
    if(binding.y==0u)return 0.f;
    StructuredBuffer<float4> boxes=ResourceDescriptorHeap[NonUniformResourceIndex(binding.x)];
    const float4 local=boxes[instance.mesh&~Mg2MeshBit];
    if(local.w<=0.f||!all(isfinite(local)))return 0.f;
    // Newly certified native ranges in a writable scene are architecture-only.
    if(local.w>1.5f&&(instance.flags&OASIS_INSTANCE_BUILDING)==0u)return 0.f;
    const float3 halfExtent=local.xyz*abs(instance.scale)+margin;
    // Architecture benefits from tight projected rectangles for occlusion.
    // For other rigid meshes prefer a sphere if its enclosing volume is smaller.
    const float r=sphere.w;
    if((instance.flags&OASIS_INSTANCE_BUILDING)==0u&&
       halfExtent.x*halfExtent.y*halfExtent.z>0.5235988f*r*r*r)return 0.f;
    return float4(halfExtent,1.f);
}
float3 CameraBoxCenter(GpuInstance instance,float4 sphere,float4 box){
    return box.w>1.5f?instance.positionRadius.xyz:sphere.xyz;
}

// Clip-space projection of the actual oriented box. A common outside
// plane rejects the entire convex volume. A near-plane intersection
// deliberately disables Hi-Z rejection, including when the eye is inside.
bool OrientedBoxFrustum(GpuInstance instance,float3 boxCenter,float3 halfExtent,uint view,out float2 lo,
                       out float2 hi,out float nearest){
    const float4 center=mul(float4(boxCenter,1.f),vp[view]);
    const float4 dx=mul(float4(Rotate(float3(halfExtent.x,0.f,0.f),instance.rotation),0.f),vp[view]);
    const float4 dy=mul(float4(Rotate(float3(0.f,halfExtent.y,0.f),instance.rotation),0.f),vp[view]);
    const float4 dz=mul(float4(Rotate(float3(0.f,0.f,halfExtent.z),instance.rotation),0.f),vp[view]);
    lo=1.f;hi=0.f;nearest=0.f;
    uint outsideAll=63u;
    bool clipped=false;
    [unroll]for(uint k=0u;k<8u;++k){
        const float4 h=center+((k&1u)?dx:-dx)+((k&2u)?dy:-dy)+((k&4u)?dz:-dz);
        if(!all(isfinite(h))){lo=0.f;hi=1.f;nearest=1.f;return true;}
        const float eps=1.e-5f*max(1.f,max(abs(h.w),max(abs(h.x),max(abs(h.y),abs(h.z)))));
        uint outside=0u;
        if(h.x < -h.w-eps)outside|=1u;
        if(h.x >  h.w+eps)outside|=2u;
        if(h.y < -h.w-eps)outside|=4u;
        if(h.y >  h.w+eps)outside|=8u;
        if(h.z < -eps)outside|=16u;
        if(h.z >  h.w+eps)outside|=32u;
        outsideAll&=outside;
        // Reverse-Z: z==w is the near plane.
        if(h.w<=timing.x+eps||h.z>=h.w-eps){clipped=true;continue;}
        const float2 uv=h.xy/h.w*float2(.5f,-.5f)+.5f;
        lo=min(lo,uv);hi=max(hi,uv);
        nearest=max(nearest,h.z/h.w);
    }
    if(outsideAll!=0u){lo=0.f;hi=1.f;nearest=1.f;return false;}
    if(clipped){lo=0.f;hi=1.f;nearest=1.f;return true;}
    // Expand the rectangle and the nearest reverse-Z bound, never shrink.
    lo-=1.e-5f;hi+=1.e-5f;nearest=min(1.f,nearest+1.e-5f);
    return true;
}

bool CameraInstanceFrustum(GpuInstance instance,float4 sphere,float4 box,
                           uint view,out float2 lo,out float2 hi,out float nearest){
    if((dimensions.w&VisibilityInstanceFrustumBypass)!=0u){lo=0.f;hi=1.f;nearest=1.f;return true;}
    [branch]if(box.w>0.f)return OrientedBoxFrustum(instance,CameraBoxCenter(instance,sphere,box),box.xyz,view,lo,hi,nearest);
    return Frustum(sphere,view,lo,hi,nearest);
}

// One opaque AABB per building. A tree is rejected when the segment from
// the eye to its planted root intersects any box. Bark and foliage share
// this point. Ignore holes, sphere coverage and the eye being inside a box.
struct BuildingOcclusionBox {float4 lo,hi;};
bool ForestBehindBuildingBox(float3 root,float3 camera,BuildingOcclusionBox box){
    const float3 delta=root-camera;
    float enter=0.f,leave=1.f;
    [unroll]for(uint axis=0u;axis<3u;++axis){
        const float d=delta[axis];
        if(d==0.f){
            if(camera[axis]<box.lo[axis]||camera[axis]>box.hi[axis])return false;
        }else{
            const float inv=rcp(d);
            const float t0=(box.lo[axis]-camera[axis])*inv;
            const float t1=(box.hi[axis]-camera[axis])*inv;
            enter=max(enter,min(t0,t1));
            leave=min(leave,max(t0,t1));
        }
    }
    return enter<=leave&&enter<1.f;
}
bool ForestOccludedByBuilding(float3 root,uint view){
    if((dimensions.w&VisibilityInstanceHiZBypass)!=0u)return false;
    const uint descriptor=DomainResource(ResourceDomainVisibility,VisibilityBuildingBoxes);
    if(descriptor==0xffffffffu)return false;
    StructuredBuffer<BuildingOcclusionBox> boxes=ResourceDescriptorHeap[descriptor];
    uint count,stride;
    boxes.GetDimensions(count,stride);
    [loop]for(uint i=0u;i<count;++i)
        if(ForestBehindBuildingBox(root,eye[view].xyz,boxes[i]))return true;
    return false;
}

bool CameraNearBuilding(GpuInstance instance,float4 sphere,float4 box,uint view){
    if((instance.flags&OASIS_INSTANCE_BUILDING)==0u)return false;
    [branch]if(box.w>0.f){
        // Use the same bound for near protection and projected visibility.
        const float3 local=Rotate(eye[view].xyz-CameraBoxCenter(instance,sphere,box),
                                  float4(-instance.rotation.xyz,instance.rotation.w));
        const float3 delta=max(abs(local)-box.xyz,0.f);
        return dot(delta,delta)<=.28f*.28f;
    }
    const float3 delta=sphere.xyz-eye[view].xyz;
    const float reach=sphere.w+.28f;
    return dot(delta,delta)<reach*reach;
}

bool Occluded(float2 lo,float2 hi,float nearest,uint view){if(any(lo<0)||any(hi>1)||nearest>=1)return false;Texture2DArray<float> hiz=ResourceDescriptorHeap[Hierarchy];float2 extent=(hi-lo)*float2(Width,Height);uint mip=min(HiZMips-1,(uint)ceil(log2(max(1,max(extent.x,extent.y)))));uint w,h,layers,levels;w=max(1u,Width>>mip);h=max(1u,Height>>mip);uint2 a=min(uint2(lo*float2(w,h)),uint2(w-1,h-1));uint2 b=min(uint2(hi*float2(w,h)),uint2(w-1,h-1));float depth=min(min(hiz.Load(int4(a,view,mip)),hiz.Load(int4(b,view,mip))),min(hiz.Load(int4(a.x,b.y,view,mip)),hiz.Load(int4(b.x,a.y,view,mip))));return nearest<depth-.0003;}
bool InstanceOccluded(float2 lo,float2 hi,float nearest,uint view){
    return (dimensions.w&VisibilityInstanceHiZBypass)==0u&&Occluded(lo,hi,nearest,view);
}
#endif
