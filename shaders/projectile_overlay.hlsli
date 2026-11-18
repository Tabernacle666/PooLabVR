// Higher marks win when an orb and its sight ray share the same pixel.
static const uint ScoriaGuideMark=5u;
static const uint ScoriaOrbRimMark=6u;
static const uint ScoriaOrbCoreMark=7u;

float3 BlendProjectile(float3 scene,uint marker){
    if(marker==0u)return scene;
    if(marker==ScoriaGuideMark)return scene*.65f+float3(.88f,.013f,.009f);
    if(marker==ScoriaOrbRimMark)return scene*.25f+float3(1.35f,.028f,.016f);
    if(marker==ScoriaOrbCoreMark)return scene*.12f+float3(2.5f,.14f,.063f);
    if(marker>=256u){
        const float strength=float(marker&255u)/255.f;
        const uint kind=marker>>8u;
        const float3 light=kind==4u?float3(3.2f,.54f,.14f):
                           kind==1u?float3(.91f,.245f,.071f):float3(1.4f,.036f,.018f);
        return scene+strength*light;
    }
    float3 color=(marker==1u||marker==3u)?float3(.35f,.8f,1.6f):float3(1.6f,.8f,.25f);
    float baseScale=marker>=3u?.55f:.28f;
    return scene*baseScale+color*(marker>=3u?.70f:1.f);
}
