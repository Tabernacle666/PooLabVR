// Target only: procedural 3x5 numeric glyphs.
uint TargetGlyph(uint code){
    switch(code){
    case 0u:return 31599u;
    case 1u:return 29850u;
    case 2u:return 29671u;
    case 3u:return 31207u;
    case 4u:return 18925u;
    case 5u:return 31183u;
    case 6u:return 31695u;
    case 7u:return 9383u;
    case 8u:return 31727u;
    case 9u:return 31215u;
    case 10u:return 448u;
    case 11u:return 27u;
    case 12u:return 24512u;
    default:return 0u;
    }
}
int TargetDigits(uint n){return n>=100u?3:n>=10u?2:1;}
float TargetText(float2 p,int value,bool metres){
    if(any(p<0.f)||p.y>=5.f)return 0.f;
    const uint n=uint(abs(value));
    const int digits=TargetDigits(n),negative=value<0?1:0;
    const int character=int(floor(p.x/4.f)),col=int(floor(p.x))%4;
    if(col>=3||character>=digits+negative+1)return 0.f;
    uint code=0u;
    if(negative!=0&&character==0)code=10u;
    else if(character==digits+negative)code=metres?12u:11u;
    else{
        const int exponent=digits-1-(character-negative);
        const uint divisor=exponent==2?100u:exponent==1?10u:1u;
        code=(n/divisor)%10u;
    }
    return float((TargetGlyph(code)>>uint(int(floor(p.y))*3+col))&1u);
}
bool TargetMark(int m){
    return m>=0&&m<=160&&(m<=20||(m<=50&&m%5==0)||m%10==0);
}
float3 TargetOverlay(float3 color,float2 screenUv,float2 worldUv,float sceneDepth,uint view){
    // Fixed world-space ruler, projected per eye, occluded by nearer geometry.
    const float unit=targetOrigin.w;
    const float3 origin=eye[view].xyz;
    const float3 ray=ViewRay(worldUv,view);
    if(ray.y<-.00001f&&origin.y>targetOrigin.y){
        const float t=(targetOrigin.y-origin.y)/ray.y;
        const float sceneT=sceneDepth>0.f?length(Unproject(worldUv,sceneDepth,view)-origin):1.e20f;
        if(t<=sceneT+.035f*unit){
            const float3 local=(origin+t*ray-targetOrigin.xyz)/unit;
            const float along=local.x,side=-local.z;
            if(along>=0.f&&along<=160.5f&&abs(side)<1.25f){
                const int m=int(round(along));
                const bool mark=TargetMark(m);
                const float width=m%5==0?.22f:.13f;
                float a=(abs(side)<.022f&&along<=160.f)?1.f:0.f;
                if(mark&&abs(along-float(m))<.023f&&abs(side)<width)a=1.f;
                const bool named=m<=10||m==15||m==20||m==25||m==30||m==40||m==50||m==60||m==80||m==100||m==120||m==140||m==160;
                if(mark&&named){
                    const float2 p=float2(side-.26f,float(m)+.28f-along)/.05f;
                    a=max(a,TargetText(p,m,true));
                }
                color=lerp(color,float3(.25f,1.f,.18f),a*.82f);
            }
        }
    }
    // Per-eye runtime asymmetric FOV: equal angles are not equal pixel steps.
    const float4 fov=targetFov[view];
    const float2 angles=degrees(atan(float2(lerp(fov.x,fov.y,screenUv.x),
                                             lerp(fov.w,fov.z,screenUv.y))));
    const float2 tick=round(angles/5.f)*5.f;
    const float2 diff=angles-tick;
    const float axis=.045f;
    float a=(abs(angles.x)<axis||abs(angles.y)<axis)?1.f:0.f;
    if(abs(diff.x)<.055f&&abs(angles.y)<.38f)a=1.f;
    if(abs(diff.y)<.055f&&abs(angles.x)<.38f)a=1.f;
    if(tick.x!=0.f){
        const float2 p=float2(diff.x+.9f,1.65f-angles.y)/.22f;
        a=max(a,TargetText(p,int(tick.x),false));
    }
    if(tick.y!=0.f){
        const float2 p=float2(angles.x-.52f,-diff.y+.55f)/.22f;
        a=max(a,TargetText(p,int(tick.y),false));
    }
    a=max(a,TargetText(float2(angles.x-.48f,1.65f-angles.y)/.22f,0,false));
    return lerp(color,float3(.15f,.94f,1.f),a*.78f);
}
