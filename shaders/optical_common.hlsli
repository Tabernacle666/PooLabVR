#ifndef OASIS_OPTICAL_COMMON
#define OASIS_OPTICAL_COMMON
struct PopulationOpticalField {
    uint4 map;
    uint4 selection;
    float4 bodyWing;
    float4 controls;
    float4 history;
    uint4 work;
    uint4 input;
};
float2 OpticalSign(float2 v){return float2(v.x>=0?1:-1,v.y>=0?1:-1);}
float2 OpticalUV(float3 direction){
    float2 p=direction.xy/max(dot(abs(direction),1.xxx),1e-8);
    if(direction.z<0)p=(1-abs(p.yx))*OpticalSign(p);
    return p*.5+.5;
}
float3 OpticalDirection(float2 uv){
    float2 p=uv*2-1;float3 d=float3(p,1-abs(p.x)-abs(p.y));
    if(d.z<0)d.xy=(1-abs(d.yx))*OpticalSign(d.xy);
    return normalize(d);
}
int2 OpticalPixel(int2 p,PopulationOpticalField s){


    int w=(int)s.map.y,h=(int)s.map.z;
    if(p.x<0){p.x=-p.x-1;p.y=h-1-p.y;}else if(p.x>=w){p.x=2*w-1-p.x;p.y=h-1-p.y;}
    if(p.y<0){p.y=-p.y-1;p.x=w-1-p.x;}else if(p.y>=h){p.y=2*h-1-p.y;p.x=w-1-p.x;}
    return p;
}
float OpticalSolidAngle(float3 direction,PopulationOpticalField s){


    float l1=dot(abs(direction),1.xxx);return 4*l1*l1*l1/(s.map.y*s.map.z);
}
float OpticalRadial(float distance,PopulationOpticalField s){
    return log(max(distance,s.history.y)/s.history.y)/log(s.history.z/s.history.y)*(s.map.w-1);
}
float3 OpticalLoad(PopulationOpticalField s,float3 direction,uint layer){
    Texture2DArray<float4> field=ResourceDescriptorHeap[NonUniformResourceIndex(s.map.x)];
    float2 p=OpticalUV(direction)*float2(s.map.yz)-.5;int2 base=int2(floor(p));float2 f=frac(p);float3 value=0;
    [unroll]for(uint y=0;y<2;y++)[unroll]for(uint x=0;x<2;x++){
        int2 pixel=OpticalPixel(base+int2(x,y),s);
        value+=field.Load(int4(pixel,layer,0)).xyz*(x?f.x:1-f.x)*(y?f.y:1-f.y);
    }return value;
}
float OpticalTransmission(PopulationOpticalField s,float3 receiver,uint channel){
    if(!s.selection.x||s.selection.y>=LightCount)return 1;
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];Light light=lights[s.selection.y];
    if(light.colorPower.w<=0)return 1;
    float3 delta=receiver-light.positionRadius.xyz;float distance=length(delta);if(distance<=s.history.y)return 1;
    float3 direction=delta/distance;uint layer=(uint)clamp(floor(OpticalRadial(distance,s)),0.0,(float)(s.map.w-1));
    float3 central=OpticalLoad(s,direction,layer);


    float inverseDistance=central.x>1e-8?central.z/(central.x*s.history.y):0;
    float angularRadius=min(.45,s.controls.y*max(0,inverseDistance-1/distance));
    float direct=exp(-min(80,central[channel]));
    if(central.x<=1e-8||angularRadius<sqrt(OpticalSolidAngle(direction,s))*.25)return direct;
    float3 right=normalize(cross(direction,abs(direction.y)<.95?float3(0,1,0):float3(1,0,0))),up=cross(right,direction);
    const float2 disk[8]={float2(.15,.32),float2(-.34,.11),float2(.38,-.29),float2(-.13,-.52),float2(-.59,.43),float2(.61,.46),float2(.04,.79),float2(-.55,-.66)};


    float result=direct;
    [unroll]for(uint k=0;k<8;k++){
        float3 ray=normalize(direction+(right*disk[k].x+up*disk[k].y)*angularRadius);
        result+=exp(-min(80,OpticalLoad(s,ray,layer)[channel]));
    }return saturate(result/9);
}
float PopulationTransmission(uint light,float3 receiver){
    if(resourceAbi.w==0)return 1;StructuredBuffer<PopulationOpticalField> fields=ResourceDescriptorHeap[resourceAbi.z];float transmission=1;
    [loop]for(uint i=0;i<resourceAbi.w;i++){PopulationOpticalField s=fields[i];if(s.selection.y==light)transmission*=OpticalTransmission(s,receiver,0);}return transmission;
}
float PopulationSourceTransmission(uint source,float3 eyePosition){
    if(resourceAbi.w==0)return 1;StructuredBuffer<PopulationOpticalField> fields=ResourceDescriptorHeap[resourceAbi.z];float transmission=1;


    [loop]for(uint i=0;i<resourceAbi.w;i++){PopulationOpticalField s=fields[i];if(s.selection.z==source)transmission*=OpticalTransmission(s,eyePosition,1);}return transmission;
}
#endif
