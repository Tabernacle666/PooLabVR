#include "PhysicsCuda.h"
#include "AirflowCuda.h"
#include <cuda_runtime.h>
namespace lab {
using namespace physx;
namespace {
__device__ PxVec3 V(LabVec3V1 v){return {v.x,v.y,v.z};}
__device__ LabVec3V1 L(PxVec3 v){return {v.x,v.y,v.z};}
__device__ PxQuat Q(LabQuatV1 q){return {q.x,q.y,q.z,q.w};}
__device__ PxTransform Interpolate(const PxTransform& a,const PxTransform& b,float t){
    PxQuat q=b.q;if(a.q.dot(q)<0)q=-q;
    return PxTransform(a.p+(b.p-a.p)*t,(a.q*(1-t)+q*t).getNormalized());
}
__device__ PxTransform Pose(uint32_t i,const PxTransform& initial,const PxTransform* previous,const PxTransform* current,float alpha){return i==PhysicsInvalidIndex?initial:Interpolate(previous[i],current[i],alpha);}
__device__ void Add(PxVec3* values,uint32_t index,PxVec3 value){atomicAdd(&values[index].x,value.x);atomicAdd(&values[index].y,value.y);atomicAdd(&values[index].z,value.z);}
__global__ void Aero(const PhysicsAeroGpu* surfaces,uint32_t count,const PxTransform* poses,const PxVec3* velocity,const PxVec3* angular,PxVec3* forces,PxVec3* torques){
    uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;
    auto s=surfaces[i];if(s.wing==PhysicsInvalidIndex||s.carrier==PhysicsInvalidIndex)return;
    const auto& carrier=poses[s.carrier];const auto& wing=poses[s.wing];
    // Cycle-averaged aerodynamic approximation, driven by the simulated relative
    // wing motion. A stalled/struck/stationary wing produces less/zero lift.
    float speed=(angular[s.wing]-angular[s.carrier]).magnitude();
    float lift=fminf(s.surface.maxLift,s.surface.liftCoefficient*speed*speed);
    PxVec3 up=carrier.q.rotate(V(s.surface.liftAxis));
    PxVec3 v=velocity[s.carrier];float vmag=v.magnitude();
    PxVec3 drag=vmag>1e-6f?v*(-fminf(s.surface.maxDrag,s.surface.dragCoefficient*vmag*vmag)/vmag):PxVec3(0);
    PxVec3 f=up*lift+drag;
    // Apply the external load at the simulated wing COM. The actual D6
    // constraint transfers it to the carrier; a struck wing changes both the
    // measured speed and its physical reaction on the rest of the insect.
    Add(forces,s.wing,f);
    (void)wing;(void)torques;
}
__global__ void AirDrag(const PhysicsDragGpu* bodies,uint32_t count,const PhysicsAirflowGpu* fields,uint32_t fieldCount,const PxTransform* poses,const PxVec3* velocity,const PxVec3* angular,PxVec3* forces,PxVec3* torques,float time){
    uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;auto body=bodies[i];auto pose=poses[body.index];const auto& g=body.geometry;
    PxVec3 point=pose.transform(V(g.shapePose.position));PxVec3 air=AirVelocity(fields,fieldCount,poses,angular,point,time);
    // This service adds the forced jet's drag. Still-air damping remains authored.
    if(air.magnitudeSquared()<1e-8f)return;
    PxVec3 relative=air-velocity[body.index]-angular[body.index].cross(point-pose.p);float speed=relative.magnitude();if(speed<1e-6f)return;
    float area=3.14159265f*g.dimensions.x*g.dimensions.x;
    if(g.shape==LAB_PHYSICS_CAPSULE)area+=4*g.dimensions.x*g.dimensions.y;
    else if(g.shape==LAB_PHYSICS_BOX){PxVec3 n=(pose.q*Q(g.shapePose.orientation)).rotateInv(relative/speed);auto d=g.dimensions;
        area=4*(fabsf(n.x)*d.y*d.z+fabsf(n.y)*d.x*d.z+fabsf(n.z)*d.x*d.y);}
    PxVec3 force=relative*(.6125f*area*speed);float limit=g.mass*30.f;
    float length=force.magnitude();if(length>limit)force*=limit/length;
    Add(forces,body.index,force);Add(torques,body.index,(point-pose.p).cross(force));
}
__global__ void Pack(const PhysicsVisualGpu* visuals,uint32_t count,const PhysicsLightGpu* lights,uint32_t lightCount,const PxTransform* previous,const PxTransform* current,float alpha,bool visible,GpuInstance* instances,uint32_t* control,PhysicsGpuLight* outputLights){
    uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i==0){uint32_t active=visible?count:0;control[0]=active;control[1]=count;control[2]=control[3]=0;control[4]=(active+63)/64;control[5]=control[6]=1;control[7]=0;control[8]=(count+63)/64;control[9]=control[10]=1;control[11]=0;}
    if(i<count){auto v=visuals[i];PxTransform p=Pose(v.index,v.initial,previous,current,alpha)*PxTransform(V(v.visual.localPose.position),Q(v.visual.localPose.orientation));
        auto& o=instances[i];o.positionRadius={p.p.x,p.p.y,p.p.z,v.visual.boundingRadius};o.rotation={p.q.x,p.q.y,p.q.z,p.q.w};o.scale={v.visual.scale.x,v.visual.scale.y,v.visual.scale.z,0};o.mesh=v.visual.mesh;o.material=v.visual.material;o.id=v.visual.id;o.flags=v.visual.flags;
        if(!visible){o.flags|=32|4;o.scale={0,0,0,0};o.positionRadius.w=0;}
    }
    if(i<lightCount){auto l=lights[i];auto p=Pose(l.index,l.initial,previous,current,alpha);PxVec3 position=p.transform(V(l.light.localPosition)),direction=p.q.rotate(V(l.light.localDirection));
        outputLights[i]={{position.x,position.y,position.z,l.light.radius},{l.light.color.x,l.light.color.y,l.light.color.z,visible?l.light.power:0},{direction.x,direction.y,direction.z,l.light.outerCos},{l.light.innerCos,-1,5,0}};
    }
}
__global__ void Probes(const PhysicsProbeGpu* probes,uint32_t count,const PxTransform* poses,const PxVec3* velocity,const PxVec3* angular,const PxGpuContactPair* contacts,const uint32_t* contactCount,uint32_t capacity,const PhysicsActorGpu* actors,uint32_t actorCount,LabPhysicsStateV1* out){
    uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;auto probe=probes[i];LabPhysicsStateV1 s{};s.body=probe.handle;
    PxTransform p=probe.index==PhysicsInvalidIndex?probe.initial:poses[probe.index];s.pose={{p.p.x,p.p.y,p.p.z},{p.q.x,p.q.y,p.q.z,p.q.w}};
    if(probe.index!=PhysicsInvalidIndex){s.linearVelocity=L(velocity[probe.index]);s.angularVelocity=L(angular[probe.index]);}
    uint32_t pairs=min(*contactCount,capacity);
    for(uint32_t c=0;c<pairs;c++){const auto& pair=contacts[c];uint64_t a=reinterpret_cast<uint64_t>(pair.actor0),b=reinterpret_cast<uint64_t>(pair.actor1);if(a!=probe.actor&&b!=probe.actor)continue;
        s.contactCount+=pair.nbContacts;uint64_t other=a==probe.actor?b:a;
        for(uint32_t k=0;k<actorCount;k++)if(actors[k].actor==other){s.contactBody=actors[k].handle;break;}
        // The copied pair contains device pointers. Consume them on CUDA before
        // the next simulation, never export PhysX pointers to the public ABI.
        if(pair.nbPatches&&pair.contactPatches){auto* patches=reinterpret_cast<const PxContactPatch*>(pair.contactPatches);s.contactNormal=L(patches[0].normal*(a==probe.actor?1.f:-1.f));}
        if(pair.contactForces)for(uint32_t k=0;k<pair.nbContacts;k++)s.normalImpulse+=fabsf(pair.contactForces[k]);
    }
    out[i]=s;
}
}
void PhysicsApplyAero(cudaStream_t stream,const PhysicsAeroGpu* surfaces,uint32_t count,const PxTransform* poses,const PxVec3* velocity,const PxVec3* angular,PxVec3* forces,PxVec3* torques){if(count)Aero<<<(count+63)/64,64,0,stream>>>(surfaces,count,poses,velocity,angular,forces,torques);}
void PhysicsApplyAirflow(cudaStream_t stream,const PhysicsDragGpu* bodies,uint32_t count,const PhysicsAirflowGpu* fields,uint32_t fieldCount,const PxTransform* poses,const PxVec3* velocity,const PxVec3* angular,PxVec3* forces,PxVec3* torques,float time){if(count&&fieldCount)AirDrag<<<(count+63)/64,64,0,stream>>>(bodies,count,fields,fieldCount,poses,velocity,angular,forces,torques,time);}
void PhysicsPackVisuals(cudaStream_t stream,const PhysicsVisualGpu* visuals,uint32_t count,const PhysicsLightGpu* lights,uint32_t lightCount,const PxTransform* previous,const PxTransform* current,float alpha,bool visible,GpuInstance* instances,uint32_t* control,PhysicsGpuLight* outputLights){uint32_t n=count>lightCount?count:lightCount;if(n)Pack<<<(n+63)/64,64,0,stream>>>(visuals,count,lights,lightCount,previous,current,alpha,visible,instances,control,outputLights);}
void PhysicsPackProbes(cudaStream_t stream,const PhysicsProbeGpu* probes,uint32_t count,const PxTransform* poses,const PxVec3* velocity,const PxVec3* angular,const PxGpuContactPair* contacts,const uint32_t* contactCount,uint32_t capacity,const PhysicsActorGpu* actors,uint32_t actorCount,LabPhysicsStateV1* out){if(count)Probes<<<(count+31)/32,32,0,stream>>>(probes,count,poses,velocity,angular,contacts,contactCount,capacity,actors,actorCount,out);}
}
