// Higher marks win when an orb and its sight ray share the same pixel.
static const uint BasaltLaserMark=5u;
static const uint BasaltOrbRimMark=6u;
static const uint BasaltOrbCoreMark=7u;

float3 BlendProjectile(float3 scene,uint marker){
    if(marker==0u)return scene;
    if(marker==BasaltLaserMark)return scene*.65f+float3(.88f,.013f,.009f);
    if(marker==BasaltOrbRimMark)return scene*.25f+float3(1.35f,.028f,.016f);
    if(marker==BasaltOrbCoreMark)return scene*.12f+float3(2.5f,.14f,.063f);
    float3 color=(marker==1u||marker==3u)?float3(.35f,.8f,1.6f):float3(1.6f,.8f,.25f);
    float baseScale=marker>=3u?.55f:.28f;
    return scene*baseScale+color*(marker>=3u?.70f:1.f);
}
