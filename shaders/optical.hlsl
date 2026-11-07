#include "common.hlsli"
struct OpticalInstance {float4 positionRadius,rotation,scale;uint4 reserved;};
struct OpticalInput {uint descriptor,first,count,reserved;};
PopulationOpticalField Settings(){StructuredBuffer<PopulationOpticalField> fields=ResourceDescriptorHeap[objectIndex];return fields[passIndex];}
uint OpticalCell(uint2 p,uint layer,PopulationOpticalField s){return (layer*s.map.z+p.y)*s.map.y+p.x;}
[numthreads(256,1,1)]
void OpticalClear(uint3 id:SV_DispatchThreadID){
    PopulationOpticalField s=Settings();if(id.x>=s.map.y*s.map.z)return;
    RWStructuredBuffer<uint2> bins=ResourceDescriptorHeap[s.work.x];
    [loop]for(uint z=0;z<s.map.w;z++){uint cell=(z*s.map.y*s.map.z+id.x)*3;bins[cell]=0;bins[cell+1]=0;bins[cell+2]=0;}
}
void OpticalAdd(RWStructuredBuffer<uint2> bins,uint cell,float3 value){
    // Exact 64-bit fixed-receiver sum using mandatory 32-bit atomics and carry.
    // It does not depend on optional heap-resource int64 atomic support. Every
    // valid contribution is retained even under heavy contention; no capped CAS
    // retries or overflowing 32-bit optical-depth counters.
    [unroll]for(uint c=0;c<3;c++){
        uint64_t fixed=(uint64_t)round(max(0,value[c])*1048576.0);uint low=(uint)fixed,high=(uint)(fixed>>32),before;
        InterlockedAdd(bins[cell*3+c].x,low,before);
        if(high||before+low<before)InterlockedAdd(bins[cell*3+c].y,high+(before+low<before?1:0));
    }
}
[numthreads(256,1,1)]
void OpticalSplat(uint3 id:SV_DispatchThreadID){
    PopulationOpticalField s=Settings();StructuredBuffer<OpticalInput> inputs=ResourceDescriptorHeap[param0];OpticalInput input=inputs[param1];if(id.x>=input.count||s.selection.y>=LightCount)return;
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];Light light=lights[s.selection.y];if(light.colorPower.w<=0)return;
    StructuredBuffer<OpticalInstance> particles=ResourceDescriptorHeap[input.descriptor];OpticalInstance particle=particles[input.first+id.x];
    if(!isfinite(particle.positionRadius.w)||particle.positionRadius.w<=0||any(!isfinite(particle.positionRadius.xyz))||any(!isfinite(particle.rotation))||!isfinite(particle.scale.w)||abs(dot(particle.rotation,particle.rotation)-1)>.002)return;
    float3 delta=particle.positionRadius.xyz-light.positionRadius.xyz;float actualDistance=length(delta);if(actualDistance<1e-8)return;float distance=max(actualDistance,s.history.y);if(distance>=s.history.z)return;
    float3 extent=s.bodyWing.xyz*abs(particle.scale.xyz);float wingArea=s.bodyWing.w*abs(particle.scale.x*particle.scale.z);
    // Public small-occluder contract bounds the entire 64-bit accumulator even
    // for 1,048,576 coincident maximum silhouettes at the nearest radial layer.
    if(any(!isfinite(extent))||any(extent<=0)||any(extent>.05)||wingArea>.01||!isfinite(wingArea))return;
    float3 direction=delta/actualDistance;float4 inverse=float4(-particle.rotation.xyz,particle.rotation.w);float3 local=Rotate(direction,inverse);
    float body=Pi*length(float3(extent.y*extent.z*local.x,extent.x*extent.z*local.y,extent.x*extent.y*local.z));
    float wings=wingArea*s.controls.x*abs(local.y),solidAngle=OpticalSolidAngle(direction,s);
    float tau=(body+wings)*s.controls.z/(distance*distance);
    float residual=wings*(1-saturate(particle.scale.w))*s.controls.z/(distance*distance);
    float3 optical=float3(tau,residual,tau*s.history.y/distance);
    // Deposit at the upper boundary; lookup uses the lower boundary. Thus an
    // occluder behind a queried receiver can never leak into its prefix sum.
    uint layer=(uint)clamp(ceil(OpticalRadial(distance,s)),0.0,(float)(s.map.w-1));
    // Four area-weighted silhouette samples reproduce the covariance of the
    // projected ellipsoid, rather than concentrating a nearby fly into one cell.
    float3 right=normalize(cross(direction,abs(direction.y)<.95?float3(0,1,0):float3(1,0,0))),up=cross(right,direction);
    float3 ax=Rotate(float3(extent.x,0,0),particle.rotation),ay=Rotate(float3(0,extent.y,0),particle.rotation),az=Rotate(float3(0,0,extent.z),particle.rotation);
    float3 rx=float3(dot(right,ax),dot(right,ay),dot(right,az)),uy=float3(dot(up,ax),dot(up,ay),dot(up,az));
    float xx=sqrt(dot(rx,rx)),yx=dot(rx,uy)/max(xx,1e-8),yy=sqrt(max(0,dot(uy,uy)-yx*yx));
    float spread=sqrt(1+wings/max(body,1e-12))/distance;
    uint samples=max(xx,sqrt(dot(uy,uy)))*spread>sqrt(solidAngle)*.35?4:1;
    RWStructuredBuffer<uint2> bins=ResourceDescriptorHeap[s.work.x];
    [loop]for(uint sample=0;sample<samples;sample++){
        float2 disk=samples==1?0:float2(sample&1?.5:-.5,sample&2?.5:-.5);
        float2 offset=float2(xx*disk.x,yx*disk.x+yy*disk.y)*spread;
        float3 ray=normalize(direction+right*offset.x+up*offset.y);
        float2 p=OpticalUV(ray)*float2(s.map.yz)-.5;int2 base=int2(floor(p));float2 f=frac(p);
        [unroll]for(uint y=0;y<2;y++)[unroll]for(uint x=0;x<2;x++){
            int2 pixel=OpticalPixel(base+int2(x,y),s);
            float cellAngle=OpticalSolidAngle(OpticalDirection((float2(pixel)+.5)/float2(s.map.yz)),s);
            float3 contribution=optical*(x?f.x:1-f.x)*(y?f.y:1-f.y)/(samples*cellAngle);uint cell=OpticalCell((uint2)pixel,layer,s);
            // Coincident insects in a wave need one accumulator update per
            // component, without dropping contributions from distinct cells.
            if(WaveActiveAllEqual(cell)){contribution=WaveActiveSum(contribution);if(WaveIsFirstLane())OpticalAdd(bins,cell,contribution);}
            else OpticalAdd(bins,cell,contribution);
        }
    }
}
float OpticalValue(uint2 v){return (float(v.y)*4294967296.0+float(v.x))/1048576.0;}
[numthreads(8,8,1)]
void OpticalPrefix(uint3 id:SV_DispatchThreadID){
    PopulationOpticalField s=Settings();if(any(id.xy>=s.map.yz))return;
    StructuredBuffer<uint2> bins=ResourceDescriptorHeap[s.input.y];RWTexture2DArray<float4> output=ResourceDescriptorHeap[s.work.z];
    Texture2DArray<float4> old=ResourceDescriptorHeap[s.work.y];float weight=0;
    if(s.history.w>.5&&s.selection.y<LightCount){
        StructuredBuffer<float4> origin=ResourceDescriptorHeap[s.work.w];StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];Light light=lights[s.selection.y];
        if(distance(origin[0].xyz,light.positionRadius.xyz)<.001&&origin[0].w==light.colorPower.w)weight=s.history.x;
    }
    float3 sum=0;
    [loop]for(uint z=0;z<s.map.w;z++){
        uint cell=OpticalCell(id.xy,z,s)*3;sum+=float3(OpticalValue(bins[cell]),OpticalValue(bins[cell+1]),OpticalValue(bins[cell+2]));
        float3 value=sum;
        if(weight>0){float3 previous=old.Load(int4(id.xy,z,0)).xyz;previous=min(previous,sum*1.1);value=lerp(sum,previous,weight);}
        // Empty/decreased cells promptly follow current positions. This clamp
        // also prevents old near occluders from darkening a now-clear depth.
        output[uint3(id.xy,z)]=float4(max(0,value),0);
    }
}
[numthreads(1,1,1)]
void OpticalOrigin(uint3 id:SV_DispatchThreadID){
    PopulationOpticalField s=Settings();RWStructuredBuffer<float4> origin=ResourceDescriptorHeap[s.input.x];StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    origin[0]=s.selection.y<LightCount?float4(lights[s.selection.y].positionRadius.xyz,lights[s.selection.y].colorPower.w):0;
}
struct OpticalDebugOut {float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
OpticalDebugOut OpticalDebugVS(uint id:SV_VertexID,uint view:SV_ViewID){OpticalDebugOut o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);o.view=view;return o;}
float4 OpticalDebugPS(OpticalDebugOut i):SV_Target0{
    PopulationOpticalField s=Settings();if(s.selection.y>=LightCount)return 0;float3 ray=normalize(Unproject(i.uv,.0001,i.view)-eye[i.view].xyz);StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    float3 receiver=eye[i.view].xyz+ray*8;float3 delta=receiver-lights[s.selection.y].positionRadius.xyz;
    float3 value=OpticalLoad(s,normalize(delta),s.map.w-1);float t=exp(-min(80,value.x));
    return float4(s.selection.w==1?float3(1-t,t*.4,.03):t.xxx,.55);
}
