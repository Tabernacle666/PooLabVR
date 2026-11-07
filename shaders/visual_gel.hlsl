#include "visual_common.hlsli"
VisualPixel VisualGelMeshVS(VisualMeshIn i,uint view:SV_ViewID){VisualDrawData d=VisualDraw();float2 uv=i.uv*max(.001,d.materialParams.x);float h=VisualSampleLevel(d.texture0.w,uv,0,float4(.5,.5,.5,1)).r;
    float wave=sin(timing.z*(.34+d.deform.y*.58)+d.deform.z+dot(i.position,float3(2.3,3.1,1.7)));
    float relief=(h-.5)*2*max(0.,d.materialParams.w);float shell=max(.0008,d.advancedParams.w)*(.88+wave*d.deform.x*.08);
    VisualPixel o=VisualProject(VisualWorld(i.position+i.normal*(relief+shell),d),float4(1,1,1,1),view);o.normal=VisualNormal(i.normal,d);o.tangent=normalize(VisualRotate(i.tangent,d.rotation));o.uv=uv;return o;}
VisualPixel VisualGelStreamVS(VisualStreamIn i,uint view:SV_ViewID){VisualPixel o=VisualProject(i.position,i.color,view);o.normal=normalize(i.position);o.tangent=normalize(float3(o.normal.z,.0001,-o.normal.x));return o;}
float VisualHash12(float2 p){float3 p3=frac(float3(p.xyx)*.1031);p3+=dot(p3,p3.yzx+33.33);return frac((p3.x+p3.y)*p3.z);}
float4 VisualGelPS(VisualPixel i,bool front:SV_IsFrontFace):SV_TARGET{
    VisualDrawData d=VisualDraw();float2 uvDx=ddx(i.uv),uvDy=ddy(i.uv);float2 detailUV=i.uv*max(1.,d.detailParams.x)+timing.z*float2(.013,-.009);float2 detailDx=ddx(detailUV),detailDy=ddy(detailUV);float2 suv=i.clip.xy/ max(abs(i.clip.w),.0001)*float2(.5,-.5)+.5;float3 gn=normalize(i.normal);if(!front)gn=-gn;
    float3 view=normalize(eye[i.view].xyz-i.world);float fres=pow(1-saturate(dot(gn,view)),2.2);float waveA=sin(dot(i.world,float3(.41,.29,.37))*max(.01,d.deform.y)+timing.z*2.1+d.deform.z),waveB=cos(dot(i.world,float3(.17,.43,.31))*max(.01,d.deform.y)-timing.z*1.7);
    float h=VisualSampleGrad(d.texture0.w,i.uv,uvDx,uvDy,float4(.5,.5,.5,1)).r,rough=VisualSampleGrad(d.texture0.z,i.uv,uvDx,uvDy,float4(1,.2,0,1)).g;
    float3 n0=VisualSampleGrad(d.texture0.y,i.uv,uvDx,uvDy,float4(.5,.5,1,1)).xyz*2-1;float3 n1=VisualSampleGrad(d.texture1.x,detailUV,detailDx,detailDy,float4(.5,.5,1,1)).xyz*2-1;
    float3 t=normalize(i.tangent-gn*dot(gn,i.tangent)),b=normalize(cross(gn,t));n0.xy=n0.xy*d.materialParams.y+n1.xy*d.detailParams.y;n0.z=max(.05,n0.z);float3 n=normalize(t*n0.x+b*n0.y+gn*n0.z);
    float phase=VisualHash12(floor(i.uv*7.7))*6.28318,wave2=sin(timing.z*.17+phase*1.73+sin(i.uv.x*13+i.uv.y*7)*2.1);
    float2 crawl=float2(sin(i.uv.y*19+phase+wave2),cos(i.uv.x*23-phase-waveA*.7));float wet=saturate((1-rough)*1.35+(.5-abs(h-.5))*.95);
    float2 flow=d.texture0.y==0xffffffffu?normalize(float2(gn.z+waveA*.45,gn.y+waveB*.45)+.0001):float2(n.x,-n.y)+crawl*(.20+.12*wave2);
    float refr=max(0.,d.advancedParams.z)*wet;float2 off=flow*refr*(d.texture0.y==0xffffffffu?(.42+saturate(.25+length(float2(waveA,waveB))*.34)):.050*(.28+.72*fres));
    float2 texel=1./max(float2(dimensions.xy),1.);float3 base=VisualSnapshot(suv,i.view),refracted=VisualSnapshot(clamp(suv+off,texel,1-texel),i.view);float3 tint=d.tint.rgb*i.color.rgb;
    float thickness=saturate(d.advancedParams.w*(.45+i.color.a*.55));float3 filtered=lerp(base,refracted,saturate(.34+refr*.78));filtered*=exp(-max(0.,1-tint)*thickness);
    filtered=lerp(filtered*(1+tint*(.22+thickness*.42)),filtered*tint+filtered*.65,.25+thickness*.35);
    float highlight=pow(saturate(.55+.45*waveA*waveB),3.);float3 rgb=filtered+tint*(.10+.28*highlight)+highlight*.08;
    return float4(max(rgb,0),saturate(i.color.a*d.tint.a*(.44+thickness*.38)));}
