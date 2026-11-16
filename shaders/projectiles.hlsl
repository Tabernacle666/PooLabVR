// GPU projectile step. The resulting projectile and impact buffers are staged:
// if the impact buffer overflows, neither buffer may be published.
struct Projectile {
    float3 position;float age;
    float3 velocity;float radius;
    float3 acceleration;float lifetime;
    uint id;uint kind;uint alive;uint _pad;
};
struct ProjectileImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId;uint primitiveIndex;uint kind;uint _pad;
};
cbuffer ProjectileParameters : register(b0) {
    uint sourceProjectiles,destinationProjectiles,impactBuffer,controlBuffer;
    uint sceneDescriptor,projectileCount,impactCapacity,instanceMask;
    float deltaTime,maxSegmentLength,_reserved0,_reserved1;
};

[numthreads(64,1,1)]
void StepProjectiles(uint3 dispatch:SV_DispatchThreadID){
    uint index=dispatch.x+dispatch.y*4194240u;if(index>=projectileCount)return;
    StructuredBuffer<Projectile> source=ResourceDescriptorHeap[sourceProjectiles];
    RWStructuredBuffer<Projectile> destination=ResourceDescriptorHeap[destinationProjectiles];
    RWStructuredBuffer<ProjectileImpact> impacts=ResourceDescriptorHeap[impactBuffer];
    RWByteAddressBuffer control=ResourceDescriptorHeap[controlBuffer];
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[sceneDescriptor];
    Projectile p=source[index];
    if(p.alive==0){destination[index]=p;return;}
    float remaining=max(0,min(deltaTime,p.lifetime-p.age));
    if(maxSegmentLength<=0){uint ignored;control.InterlockedOr(8,1,ignored);return;}
    // Integrate each segment analytically. Subdivision bounds the distance
    // traced in a single chord; acceleration affects only this projectile.
    while(remaining>0){
        float speed=length(p.velocity);
        float accel=length(p.acceleration);
        float step=remaining;
        if(accel>1e-6){
            float distanceStep=2*maxSegmentLength/(sqrt(speed*speed+2*accel*maxSegmentLength)+speed);
            step=min(step,distanceStep);
        }else if(speed>1e-6)step=min(step,maxSegmentLength/speed);
        if(step<=0){uint ignored;control.InterlockedOr(8,1,ignored);return;}
        float3 delta=p.velocity*step+0.5*p.acceleration*step*step;
        float distance=length(delta);
        if(distance>1e-8){
            RayDesc ray;ray.Origin=p.position;ray.Direction=delta/distance;ray.TMin=0;ray.TMax=distance;
            RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
            query.TraceRayInline(scene,RAY_FLAG_NONE,instanceMask,ray);
            while(query.Proceed()){}
            if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT){
                uint slot=control.Load(0);
                for(;;){
                    if(slot>=impactCapacity){uint ignored;control.InterlockedOr(4,1,ignored);return;}
                    uint observed;control.InterlockedCompareExchange(0,slot,slot+1,observed);
                    if(observed==slot)break;slot=observed;
                }
                ProjectileImpact hit;hit.position=ray.Origin+ray.Direction*query.CommittedRayT();
                hit.radius=p.radius;hit.direction=ray.Direction;hit.projectileId=p.id;
                hit.instanceId=query.CommittedInstanceID();hit.primitiveIndex=query.CommittedPrimitiveIndex();
                hit.kind=p.kind;hit._pad=0;impacts[slot]=hit;
                p.position=hit.position;p.alive=0;destination[index]=p;return;
            }
        }
        p.position+=delta;p.velocity+=p.acceleration*step;p.age+=step;remaining-=step;
    }
    if(p.age>=p.lifetime)p.alive=0;
    destination[index]=p;
}
