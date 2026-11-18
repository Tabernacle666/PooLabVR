float3 BlendProjectile(float3 scene,uint marker){
    if(marker==0u)return scene;
    float3 color=(marker==1u||marker==3u)?float3(.35f,.8f,1.6f):float3(1.6f,.8f,.25f);
    float baseScale=marker>=3u?.55f:.28f;
    return scene*baseScale+color*(marker>=3u?.70f:1.f);
}
