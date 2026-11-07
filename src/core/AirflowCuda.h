#pragma once
#include "PhysicsCuda.h"
#include <cuda_runtime.h>
namespace lab {
__device__ inline physx::PxVec3 AirVelocity(const PhysicsAirflowGpu* fields,uint32_t count,const physx::PxTransform* poses,const physx::PxVec3* angular,physx::PxVec3 p,float time){
    using namespace physx;PxVec3 result(0);
    for(uint32_t i=0;i<count;i++){
        auto f=fields[i];if(!f.field.enabled||f.emitter==PhysicsInvalidIndex)continue;
        auto pose=poses[f.emitter];auto local=f.field.localDirection;
        PxVec3 axis=pose.q.rotate(PxVec3(local.x,local.y,local.z));
        float rpm=fabsf(angular[f.emitter].dot(axis))*9.5492966f;
        float speed=fminf(20.f,rpm/f.field.referenceRpm*f.field.speedAtReferenceRpm);
        PxVec3 delta=p-pose.p;float along=delta.dot(axis);if(along<=0)continue;
        PxVec3 radial=delta-axis*along;float width=f.field.radius+along*.16f;
        float plume=expf(-radial.magnitudeSquared()/(width*width))*fminf(1.f,along/.15f)*expf(-along/f.field.length);
        PxVec3 swirl=axis.cross(radial)/fmaxf(width,.05f)*sinf(along*5-time*2);
        PxVec3 noise(sinf(p.y*2+p.z+time),sinf(p.z*2+p.x-time*.7f),sinf(p.x*2+p.y+time*.8f));
        result+=(axis+swirl*.18f+noise*(f.field.turbulence*.12f))*speed*plume;
    }
    return result;
}
}
