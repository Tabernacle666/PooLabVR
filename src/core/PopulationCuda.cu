#include "PopulationCuda.h"
#include "AirflowCuda.h"
#include <cuda_runtime.h>
namespace lab {
using namespace physx;
namespace {
__device__ PxVec3 V(LabVec3V1 p){return {p.x,p.y,p.z};}
__device__ LabVec3V1 L(PxVec3 p){return {p.x,p.y,p.z};}
__device__ uint32_t Hash(uint32_t a){a^=a>>16;a*=0x7feb352du;a^=a>>15;a*=0x846ca68bu;return a^(a>>16);}
__device__ float Unit(uint32_t a){return float(Hash(a)&0xffffffu)/16777216.f;}
__device__ PxVec3 Safe(PxVec3 p,PxVec3 fallback=PxVec3(0,0,1)){float d=p.magnitudeSquared();return d>1e-12f?p*rsqrtf(d):fallback;}
__device__ PxVec3 ClampLength(PxVec3 p,float max){float n=p.magnitudeSquared();return n>max*max?p*(max*rsqrtf(n)):p;}
struct PopulationClock {float phase[3];uint32_t failureEpoch;float realSeconds;float flowSeconds;};
__device__ PxVec3 Fourier(uint32_t seed,PopulationClock clock){
    // Common irrational-frequency phases are reduced once per batch on the host;
    // individual seeded phases remain GPU-only. No per-insect double remainder.
    PxVec3 out(0);float* a=&out.x;
    for(int axis=0;axis<3;axis++)for(int k=0;k<3;k++){
        float phase=clock.phase[k]+Unit(seed+axis*7919+k*1013)*6.2831853f;
        a[axis]+=__sinf(phase)*(k==0?.55f:k==1?.30f:.15f);
    }return out;
}
__device__ PxTransform BodyPose(const PopulationBodyGpu& b,const PxTransform* poses){return (b.index==PhysicsInvalidIndex||!poses?b.initial:poses[b.index])*b.local;}
__device__ PxVec3 Abs(PxVec3 p){return {fabsf(p.x),fabsf(p.y),fabsf(p.z)};}
__device__ PxVec3 Min(PxVec3 a,PxVec3 b){return {fminf(a.x,b.x),fminf(a.y,b.y),fminf(a.z,b.z)};}
__device__ PxVec3 Max(PxVec3 a,PxVec3 b){return {fmaxf(a.x,b.x),fmaxf(a.y,b.y),fmaxf(a.z,b.z)};}
__device__ int Cell(float p){return int(floorf((p+8.f)*.5f));}
__device__ PxVec3 Extent(const PopulationBodyGpu& b,const PxTransform& p){
    if(b.shape==LAB_PHYSICS_BOX)return Abs(p.q.rotate(PxVec3(1,0,0)))*b.dimensions.x+Abs(p.q.rotate(PxVec3(0,1,0)))*b.dimensions.y+Abs(p.q.rotate(PxVec3(0,0,1)))*b.dimensions.z;
    PxVec3 e(b.dimensions.x);if(b.shape==LAB_PHYSICS_CAPSULE)e+=Abs(p.q.rotate(PxVec3(1,0,0)))*b.dimensions.y;return e;
}
__global__ void PrepareBodies(const PopulationBodyGpu* bodies,PopulationBodyPoseGpu* world,uint32_t* grid,uint32_t count,const PxTransform* poses){
    uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;auto b=bodies[i];auto& w=world[i];auto current=BodyPose(b,poses);
    w.previous=w.handle==b.handle?w.current:current;w.current=current;w.handle=b.handle;if(!b.enabled)return;
    PxVec3 e=Extent(b,current),pe=Extent(b,w.previous);PxVec3 lower=Min(current.p-e,w.previous.p-pe),upper=Max(current.p+e,w.previous.p+pe);
    int x0=max(0,Cell(lower.x)),x1=min(7,Cell(upper.x)),y0=max(0,Cell(lower.y)),y1=min(7,Cell(upper.y)),z0=max(0,Cell(lower.z)),z1=min(7,Cell(upper.z));
    for(int z=z0;z<=z1;z++)for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++)atomicOr(&grid[((z*8+y)*8+x)*4+i/32],1u<<(i&31));
}
__device__ void Candidates(const uint32_t* grid,PxVec3 from,PxVec3 to,float radius,uint32_t masks[4]){
    PxVec3 lo=Min(from,to)-PxVec3(radius),hi=Max(from,to)+PxVec3(radius);
    int x0=Cell(lo.x),x1=Cell(hi.x),y0=Cell(lo.y),y1=Cell(hi.y),z0=Cell(lo.z),z1=Cell(hi.z);
    // Outside the accelerator volume, or for very wide avoidance zones, query
    // all scene colliders on GPU. No out-of-grid collision can be dropped.
    if(x0<0||y0<0||z0<0||x1>7||y1>7||z1>7||(x1-x0+1)*(y1-y0+1)*(z1-z0+1)>64){for(int k=0;k<4;k++)masks[k]=0xffffffff;return;}
    for(int k=0;k<4;k++)masks[k]=0;
    for(int z=z0;z<=z1;z++)for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++)for(int k=0;k<4;k++)masks[k]|=grid[((z*8+y)*8+x)*4+k];
}
__device__ float Distance(const PopulationBodyGpu& b,PxTransform pose,PxVec3 world,PxVec3& normal){
    PxVec3 p=pose.transformInv(world),n(0);float d;
    if(b.shape==LAB_PHYSICS_BOX){
        PxVec3 q(fabsf(p.x)-b.dimensions.x,fabsf(p.y)-b.dimensions.y,fabsf(p.z)-b.dimensions.z);
        PxVec3 outside(fmaxf(q.x,0),fmaxf(q.y,0),fmaxf(q.z,0));
        float length=outside.magnitude();d=length+fminf(fmaxf(q.x,fmaxf(q.y,q.z)),0);
        if(length>1e-8f)n=PxVec3(copysignf(outside.x,p.x),copysignf(outside.y,p.y),copysignf(outside.z,p.z))/length;
        else if(q.x>=q.y&&q.x>=q.z)n.x=copysignf(1,p.x);else if(q.y>=q.z)n.y=copysignf(1,p.y);else n.z=copysignf(1,p.z);
    }else{
        if(b.shape==LAB_PHYSICS_CAPSULE)p.x-=fminf(b.dimensions.y,fmaxf(-b.dimensions.y,p.x));
        float length=p.magnitude();d=length-b.dimensions.x;n=Safe(p,PxVec3(0,1,0));
    }
    normal=pose.q.rotate(n);return d;
}
__device__ void Add(PxVec3* v,uint32_t i,PxVec3 a){atomicAdd(&v[i].x,a.x);atomicAdd(&v[i].y,a.y);atomicAdd(&v[i].z,a.z);}
__device__ bool Sweep(const PopulationBodyGpu& b,PxTransform pose,PxVec3 from,PxVec3 to,float radius,PxVec3& hit,PxVec3& normal){
    PxVec3 delta=to-from;float length=delta.magnitude(),t=0;
    // Conservative advancement of the swept insect sphere. The signed distance
    // is 1-Lipschitz for every supported primitive; thin table/cable cannot tunnel.
    for(int k=0;k<12;k++){
        PxVec3 point=from+delta*t;float d=Distance(b,pose,point,normal)-radius;
        if(d<=.00005f){hit=point+normal*fmaxf(0,-d+.0001f);return true;}
        if(length<1e-8f)return false;t+=d/length;if(t>1)return false;
    }
    // Conservative stop on budget exhaustion: never allow an unresolved sweep
    // to cross the solid. This response is not a navigation target/teleport.
    hit=from+delta*fminf(t,1.f);Distance(b,pose,hit,normal);return true;
}
__global__ void Init(PopulationStateGpu* states,uint32_t first,uint32_t count,LabPopulationDescV1 d,uint32_t birth,const PopulationBodyGpu* bodies,uint32_t nb,const PxTransform* poses){
    uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;uint32_t h=Hash(d.flight.seed+first+i+birth*19349663u);
    PopulationStateGpu s{};
    for(uint32_t attempt=0;attempt<32;attempt++){
        uint32_t candidate=h+attempt*104729u;s.position=V(d.spawnCenter)+PxVec3((Unit(candidate)-.5f)*2*d.spawnHalfExtent.x,(Unit(candidate+1)-.5f)*2*d.spawnHalfExtent.y,(Unit(candidate+2)-.5f)*2*d.spawnHalfExtent.z);
        bool clear=true;for(uint32_t k=0;k<nb;k++)if(bodies[k].enabled){PxVec3 normal;if(Distance(bodies[k],BodyPose(bodies[k],poses),s.position,normal)<d.flight.bodyRadius+.003f){clear=false;break;}}
        if(clear)break;
    }
    // Complete initial placement before any publication. Even an unusually
    // obstructed spawn volume cannot publish insects already inside its floor.
    for(uint32_t pass=0;pass<4;pass++)for(uint32_t k=0;k<nb;k++)if(bodies[k].enabled){PxVec3 normal;float gap=Distance(bodies[k],BodyPose(bodies[k],poses),s.position,normal)-d.flight.bodyRadius;if(gap<0)s.position+=normal*(-gap+.0001f);}
    s.previous=s.position;s.heading=Safe(PxVec3(Unit(h+3)-.5f,(Unit(h+4)-.5f)*.4f,Unit(h+5)-.5f));s.velocity=s.heading*d.flight.nominalSpeed;s.activity=.6f;s.liftAxis=PxVec3(0,1,0);s.wingPhase=Unit(h+6)*6.2831853f;s.birth=h;states[first+i]=s;
}
__global__ void Step(PopulationStateGpu* states,uint32_t count,LabPopulationDescV1 d,LabPopulationInputV1 input,LabPopulationEffectsV1 effects,LabPopulationHandsV1 hands,const PopulationBodyGpu* bodies,const PopulationBodyPoseGpu* world,const uint32_t* grid,uint32_t nb,const PxTransform* previous,const PxTransform* poses,const PxVec3* velocities,const PxVec3* angular,PxVec3* forces,PxVec3* torques,const PhysicsLightGpu* lights,uint32_t nl,const PhysicsAirflowGpu* airflows,uint32_t airflowCount,LabPopulationSpaceV1 space,float dt,float reactionDt,PopulationClock clock,bool observerPresent){
    uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=count||!input.enabled)return;
    auto s=states[i];const auto& f=d.flight;PxVec3 old=s.position;PxVec3 noise=Fourier(s.birth,clock);
    // Local swept capsules share the tracking input for both hands. Acceleration
    // is integrated at fixed dt, so repeated substeps do not multiply an impulse.
    PxVec3 sweepAcceleration(0);
    for(uint32_t h=0;h<2;h++)if(observerPresent&&hands.hand[h].tracked){
        auto hand=hands.hand[h];PxVec3 end=V(hand.position),start=V(hand.previous),v=V(hand.velocity);
        float speed=v.magnitude();if(speed<.2f)continue;
        PxVec3 segment=end-start;float len=segment.magnitudeSquared();
        float t=len>1e-10f?fminf(1.f,fmaxf(0.f,(s.position-start).dot(segment)/len)):1.f;
        PxVec3 away=s.position-(start+segment*t);float distance=away.magnitude();
        if(distance>=effects.sweepRadius)continue;
        PxVec3 direction=Safe(v),radial=Safe(away,Safe(s.position-V(input.playerPosition)));
        float proximity=1-distance/effects.sweepRadius;
        float angle=.35f+.65f*fmaxf(0.f,direction.dot(radial));
        sweepAcceleration+=(direction+radial*.65f+Safe(s.position-V(input.playerPosition))*.45f)
            *(effects.impulse*speed*proximity*proximity*angle*12.f);
        if(effects.impulse>0){s.impact=clock.realSeconds+effects.fleeSeconds;s.chased=1;}
    }
    s.velocity=ClampLength(s.velocity+sweepAcceleration*dt,f.maxSpeed);
    float aversion=s.chased?(s.impact>clock.realSeconds?1.f:effects.residualAversion):0.f;
    PxVec3 steer=s.heading*.45f+noise*f.wander;steer.y*=f.vertical;
    if(space.enabled){PxVec3 p=s.position-V(space.center),extent=V(space.halfExtent);
        PxVec3 outside(fmaxf(0.f,fabsf(p.x)-extent.x)*copysignf(1.f,p.x),fmaxf(0.f,fabsf(p.y)-extent.y)*copysignf(1.f,p.y),fmaxf(0.f,fabsf(p.z)-extent.z)*copysignf(1.f,p.z));
        steer-=outside*space.returnStrength;}

    for(uint32_t k=0;k<nl;k++)if(lights[k].light.power>0){auto l=lights[k];PxTransform p=l.index==PhysicsInvalidIndex?l.initial:poses[l.index];PxVec3 light=p.transform(V(l.light.localPosition)),delta=light-s.position;
        float r=delta.magnitude();if(r<l.light.radius){PxVec3 dir=Safe(delta);float influence=f.lightAttraction*fminf(2.f,l.light.power/26.f)/(1+r*.65f)*(1-.85f*aversion);
            // Finite focus radius and individual tangential drift prevent all
            // insects being nailed to the light's emitter position.
            float focus=fmaxf(.2f,f.turnRadius*1.8f);float pull=fminf(1.f,fmaxf(-.8f,(r-focus)/focus));
            steer+=dir*(influence*pull)+Safe(dir.cross(noise),PxVec3(1,0,0))*(influence*.45f);
        }
    }
    float failure=Unit(s.birth+clock.failureEpoch*1664525u);float authority=failure<f.avoidanceFailure?.12f:1.f;
    float look=fminf(f.anticipationDistance,fmaxf(f.comfortDistance,s.velocity.magnitude()*.22f));PxVec3 future=s.position+Safe(s.velocity,s.heading)*look;
    uint32_t candidates[4];Candidates(grid,s.position,future,f.anticipationDistance*(s.impact>clock.realSeconds?f.impactMultiplier:1.f)+f.maxSpeed*dt+f.bodyRadius,candidates);
    for(uint32_t word=0;word<4;word++){uint32_t mask=candidates[word];while(mask){uint32_t k=word*32+__ffs(mask)-1;mask&=mask-1;if(k>=nb)continue;auto b=bodies[k];if(!b.enabled)continue;PxTransform p=world[k].current;
        float extra=b.hand&&s.impact>clock.realSeconds?f.impactMultiplier:1.f,range=f.anticipationDistance*extra;
        if((p.p-s.position).magnitudeSquared()>(b.radius+range+look)*(b.radius+range+look))continue;
        PxVec3 n,n2;float now=Distance(b,p,s.position,n)-f.bodyRadius, ahead=Distance(b,p,future,n2)-f.bodyRadius;
        float comfort=f.comfortDistance*extra;
        float weight=fmaxf(0,(range-fminf(now,ahead))/fmaxf(.01f,range-comfort));weight=fminf(weight,6.f);
        steer+=Safe(n+n2,n)*(weight*authority*(1.2f+f.turnIntensity));
    }}
    // Player capsule extends from the tracked head toward the floor. It has the
    // same post-impact multiplier as the hands but does not invent hand impacts.
    PxVec3 head=V(input.playerPosition);
    if(observerPresent){PxVec3 axis=s.position-head;axis.y=s.position.y-fminf(head.y,fmaxf(.25f,s.position.y));
        float playerDistance=axis.magnitude()-.24f;float playerRange=fmaxf(f.anticipationDistance*(s.impact>clock.realSeconds?f.impactMultiplier:1.f),effects.avoidanceRadius*aversion);
        if(playerDistance<playerRange)steer+=Safe(axis,PxVec3(1,0,0))*fminf(6.f,(playerRange-playerDistance)/fmaxf(.05f,playerRange-f.comfortDistance))*fmaxf(authority,aversion)*(1+aversion*2);
    }
    for(uint32_t h=0;h<2;h++)if(observerPresent&&hands.hand[h].tracked&&aversion>0){
        PxVec3 away=s.position-V(hands.hand[h].position);float distance=away.magnitude();
        float radius=effects.avoidanceRadius*aversion;
        if(distance<radius)steer+=Safe(away)*(1-distance/fmaxf(radius,.01f))*aversion*5;
    }
    PxVec3 desired=Safe(steer,s.heading);float cosine=fminf(1.f,fmaxf(-1.f,s.heading.dot(desired)));float angle=acosf(cosine);
    float rate=fminf(f.turnIntensity/f.responseSeconds,fmaxf(.1f,s.velocity.magnitude())/f.turnRadius);
    float fraction=angle>1e-5f?fminf(1.f,rate*dt/angle):1.f;
    // Rodrigues rotation remains well-defined even for antiparallel desired headings.
    PxVec3 axisTurn=Safe(s.heading.cross(desired),Safe(s.heading.cross(PxVec3(0,1,0)),PxVec3(1,0,0)));float turn=angle*fraction;
    s.heading=Safe(s.heading*cosf(turn)+axisTurn.cross(s.heading)*sinf(turn)+axisTurn*(axisTurn.dot(s.heading)*(1-cosf(turn))));
    float speed=f.nominalSpeed*(.85f+.3f*Unit(s.birth+17));PxVec3 airVelocity=AirVelocity(airflows,airflowCount,poses,angular,s.position,clock.flowSeconds);
    PxVec3 desiredVelocity=ClampLength(s.heading*fminf(speed,f.maxSpeed)+airVelocity,f.maxSpeed);
    PxVec3 desiredAcceleration=ClampLength((desiredVelocity-s.velocity)/f.responseSeconds,18.f);
    // Mean lift: L = .5 rho S CL (2 pi f A R)^2. Activity controls
    // frequency squared; required thrust includes gravity and drag. The rendered
    // wings and acoustic pitch use this actual, bounded actuation state.
    float air=1.225f;float baseSpeed=6.2831853f*f.wingFrequency*.85f*.0035f;
    float liftBase=.5f*air*f.wingArea*f.liftCoefficient*baseSpeed*baseSpeed;
    PxVec3 relativeAir=s.velocity-airVelocity;
    PxVec3 drag=relativeAir*(-.5f*air*f.wingArea*f.dragCoefficient*relativeAir.magnitude()/f.mass);
    PxVec3 required=desiredAcceleration-PxVec3(0,-9.81f,0)-drag;
    float requested=required.magnitude()*f.mass/fmaxf(liftBase,1e-9f);
    s.activity+=(fminf(2.8f,requested)-s.activity)*(1-expf(-dt/.045f));
    s.liftAxis=Safe(s.liftAxis+(Safe(required,PxVec3(0,1,0))-s.liftAxis)*(1-expf(-dt/.035f)),PxVec3(0,1,0));
    PxVec3 acceleration=s.liftAxis*(liftBase*s.activity/f.mass)+PxVec3(0,-9.81f,0)+drag;
    s.velocity=ClampLength(s.velocity+acceleration*dt,f.maxSpeed);s.previous=old;
    PxVec3 to=old+s.velocity*dt;bool contactResolved=false;
    for(uint32_t word=0;word<4;word++){uint32_t mask=candidates[word];while(mask){uint32_t k=word*32+__ffs(mask)-1;mask&=mask-1;if(k>=nb)continue;auto b=bodies[k];if(!b.enabled)continue;PxTransform p=world[k].current,before=world[k].previous;
        PxVec3 movement=p.p-before.p; // covers tracked hand sweeps, not only insect motion
        if((p.p-to).magnitudeSquared()>(b.radius+f.bodyRadius+(to-old).magnitude()+movement.magnitude())*(b.radius+f.bodyRadius+(to-old).magnitude()+movement.magnitude()))continue;
        PxVec3 hit,normal;
        float rotationMargin=2*b.radius*sqrtf(fmaxf(0.f,1.f-fabsf(p.q.dot(before.q))));
        if(!Sweep(b,p,old+movement,to,f.bodyRadius+rotationMargin,hit,normal))continue;
        PxVec3 contactVelocity=b.index==PhysicsInvalidIndex?PxVec3(0):velocities[b.index]+angular[b.index].cross(hit-p.p);
        PxVec3 relative=s.velocity-contactVelocity;float inward=relative.dot(normal);PxVec3 impulse(0);
        if(inward<0){impulse=normal*(-(1+b.restitution)*inward*f.mass);s.velocity+=impulse/f.mass;}
        to=hit;contactResolved=true;
        if(b.hand){s.impact=clock.realSeconds+effects.fleeSeconds;s.chased=1;}
        s.velocity=ClampLength(s.velocity,f.maxSpeed);
        if(b.index!=PhysicsInvalidIndex&&!b.kinematic&&impulse.magnitudeSquared()>0){Add(forces,b.index,-impulse/reactionDt);Add(torques,b.index,(hit-p.p).cross(-impulse/reactionDt));}
    }}
    // Solid player contact, independent of imperfect navigation.
    if(observerPresent){PxVec3 closest=head;closest.y=fminf(head.y,fmaxf(.25f,to.y));PxVec3 delta=to-closest;float pr=.24f+f.bodyRadius;
        if(delta.magnitudeSquared()<pr*pr){contactResolved=true;PxVec3 n=Safe(delta,PxVec3(1,0,0));to=closest+n*pr;float v=s.velocity.dot(n);if(v<0)s.velocity-=n*(1.2f*v);}
    }
    if(contactResolved)for(uint32_t word=0;word<4;word++){uint32_t mask=candidates[word];while(mask){uint32_t k=word*32+__ffs(mask)-1;mask&=mask-1;if(k>=nb)continue;auto b=bodies[k];
        if(!b.enabled||b.index!=PhysicsInvalidIndex||b.shape!=LAB_PHYSICS_BOX)continue;
        auto pose=world[k].current;PxVec3 before=pose.transformInv(old),after=pose.transformInv(to);
        // Reapply support-plane constraints after a hand/player response. A hand
        // cannot push an insect through a floor/table whose top it approached
        // from above. The plane derives from the actual collider, not a Y clamp.
        float top=b.dimensions.y+f.bodyRadius;
        if(before.y>=top-.0002f&&after.y<top&&fabsf(after.x)<b.dimensions.x+f.bodyRadius&&fabsf(after.z)<b.dimensions.z+f.bodyRadius){PxVec3 normal=pose.q.rotate(PxVec3(0,1,0));to+=normal*(top-after.y+.0001f);float inward=s.velocity.dot(normal);if(inward<0)s.velocity-=normal*((1+b.restitution)*inward);}
    }}
    s.position=to;s.wingPhase=fmodf(s.wingPhase+6.2831853f*f.wingFrequency*sqrtf(fmaxf(.01f,s.activity))*dt,6.2831853f);states[i]=s;
}
__device__ PxQuat Orientation(PxVec3 heading,PxVec3 liftAxis){
    PxVec3 forward=Safe(heading),right=Safe(liftAxis.cross(forward),Safe(PxVec3(0,1,0).cross(forward),PxVec3(1,0,0))),up=Safe(forward.cross(right));
    return PxQuat(PxMat33(right,up,forward)).getNormalized();
}
__device__ PxQuat StrokeOrientation(PxVec3 heading,PxVec3 liftAxis){
    PxVec3 up=Safe(liftAxis,PxVec3(0,1,0));PxVec3 right=Safe(up.cross(heading),Safe(up.cross(PxVec3(0,0,1)),PxVec3(1,0,0)));PxVec3 forward=Safe(right.cross(up));
    return PxQuat(PxMat33(right,up,forward)).getNormalized();
}
__device__ void Instance(GpuInstance& o,PxVec3 position,PxQuat q,PxVec3 scale,uint32_t mesh,uint32_t material,uint32_t id,bool enabled){
    o.positionRadius={position.x,position.y,position.z,enabled?.0045f*fmaxf(scale.x,fmaxf(scale.y,scale.z)):0};o.rotation={q.x,q.y,q.z,q.w};o.scale={enabled?scale.x:0,enabled?scale.y:0,enabled?scale.z:0,0};o.mesh=mesh;o.material=material;o.id=id;o.flags=1u|8u|4u|32u; // dynamic, no CPU collision/shadow, no TLAS
}
__global__ void ResetSummary(PopulationSummaryGpu* s,uint32_t* control,uint32_t base,uint32_t capacity,uint32_t count,bool visible,uint64_t frame,uint64_t tick){
    *s={};s->targetKey=0xffffffff;for(int i=0;i<8;i++)s->voiceKeys[i]=0xffffffff;for(int i=0;i<64;i++)s->detailKeys[i]=0xffffffff;
    s->snapshot.structSize=sizeof(LabPopulationSnapshotV1);s->snapshot.count=count;s->snapshot.capacity=capacity;s->snapshot.sourceFrame=frame;s->snapshot.simulationTick=tick;s->snapshot.targetId=0xffffffff;
    uint32_t slots=base+count+LAB_PHYSICS_POPULATION_DETAIL_MAX_V1*4+LAB_POPULATION_SHADOW_MAX_V1;
    control[0]=visible?base+count:0;control[1]=slots;control[2]=control[3]=0;control[4]=(control[0]+63)/64;control[5]=control[6]=1;control[7]=0;control[8]=(slots+63)/64;control[9]=control[10]=1;control[11]=0;
}
__global__ void Pack(const PopulationStateGpu* states,uint32_t count,uint32_t capacity,LabPopulationDescV1 d,LabPopulationInputV1 input,bool visible,GpuInstance* instances,uint32_t base,PopulationSummaryGpu* summary,float alpha){
    uint32_t i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=capacity)return;
    if(i>=count){Instance(instances[base+i],PxVec3(0),PxQuat(PxIdentity),PxVec3(0),d.proxyMesh,d.bodyMaterial,0,false);return;}
    auto s=states[i];PxVec3 position=s.previous+(s.position-s.previous)*alpha;PxQuat q=Orientation(s.heading,s.liftAxis);float distance=(s.position-V(input.playerPosition)).magnitude();
    uint32_t warpMask=__activemask(),lane=threadIdx.x&31,leader=__ffs(warpMask)-1;
    uint32_t nearMask=__ballot_sync(warpMask,distance<d.flight.nearDistance);
    if(lane==leader&&nearMask)atomicAdd(&summary->nearCount,uint32_t(__popc(nearMask)));
    float unit=d.flight.bodyRadius/.0035f;
    Instance(instances[base+i],position,q,PxVec3(unit),d.proxyMesh,d.bodyMaterial,0x71000000u+i,visible);
    instances[base+i].flags|=128u;
    if(visible&&distance<d.flight.detailDistance){
        PxVec3 relative=s.position-V(input.playerPosition),direction=Safe(relative);
        uint32_t azimuth=min(15u,uint32_t((atan2f(direction.z,direction.x)+3.14159265f)*(16.f/6.28318531f)));
        uint32_t elevation=min(3u,uint32_t((direction.y+1)*2));
        uint32_t key=(uint32_t(fminf(4094.f,distance*4096.f))<<20)|i;
        atomicMin(&summary->detailKeys[elevation*16+azimuth],key);
    }
    // Eight nearest spatial sectors bound acoustic work independently of count.
    PxVec3 relative=s.position-V(input.playerPosition);uint32_t sector=(relative.x>0?1:0)|(relative.y>0?2:0)|(relative.z>0?4:0);
    uint32_t key=(uint32_t(fminf(4094.f,distance*256.f))<<20)|i;uint32_t sectorMask=__match_any_sync(warpMask,sector),nearest=__reduce_min_sync(sectorMask,key);
    if(lane==uint32_t(__ffs(sectorMask)-1))atomicMin(&summary->voiceKeys[sector],nearest);
    if(input.aimTracked){PxVec3 aim=Safe(V(input.aimDirection)),offset=s.position-V(input.aimOrigin);float along=offset.dot(aim),side=(offset-aim*along).magnitude();
        if(along>0&&along<8&&side<d.flight.bodyRadius+.022f)atomicMin(&summary->targetKey,(uint32_t(along*256.f)<<20)|i);
    }
}
__global__ void Details(const PopulationStateGpu* states,uint32_t count,LabPopulationDescV1 d,LabPopulationInputV1 input,bool visible,GpuInstance* instances,uint32_t base,PopulationSummaryGpu* summary,float alpha){
    uint32_t slot=threadIdx.x;if(slot>=LAB_PHYSICS_POPULATION_DETAIL_MAX_V1)return;
    uint32_t start=base+count+slot*4;
    for(uint32_t k=0;k<4;k++)Instance(instances[start+k],PxVec3(0),PxQuat(PxIdentity),PxVec3(0),d.proxyMesh,d.bodyMaterial,0,false);
    uint32_t key=summary->detailKeys[slot];if(!visible||key==0xffffffff)return;
    uint32_t i=key&0xfffff;if(i>=count)return;auto s=states[i];
    float distance=(s.position-V(input.playerPosition)).magnitude();
    float x=fminf(1.f,fmaxf(0.f,(d.flight.detailDistance-distance)/fmaxf(.01f,d.flight.detailDistance*.5f))),blend=x*x*(3-2*x);
    if(blend<=0)return;atomicAdd(&summary->detailedCount,1u);
    float unit=d.flight.bodyRadius/.0035f;PxVec3 position=s.previous+(s.position-s.previous)*alpha;PxQuat q=Orientation(s.heading,s.liftAxis);
    instances[base+i].scale.w=blend;
    Instance(instances[start],position+q.rotate(PxVec3(0,.0002f,.0023f)*unit),q,PxVec3(unit*blend),d.headMesh,d.headMaterial,0x72000000u+i,true);
    Instance(instances[start+1],position+q.rotate(PxVec3(0,-.0003f,-.0017f)*unit),q,PxVec3(unit*blend),d.abdomenMesh,d.bodyMaterial,0x73000000u+i,true);
    float flap=sinf(s.wingPhase)*.85f;
    for(uint32_t side=0;side<2;side++){float sign=side?1.f:-1.f;PxQuat wing=StrokeOrientation(s.heading,s.liftAxis)*PxQuat(sign*flap,PxVec3(0,0,1));Instance(instances[start+2+side],position+q.rotate(PxVec3(sign*.001f,.0005f,-.0003f)*unit),wing,PxVec3(unit*blend),side?d.rightWingMesh:d.leftWingMesh,d.wingMaterial,0x74000000u+i,true);}
}
__global__ void Shadows(const PopulationStateGpu* states,uint32_t count,LabPopulationDescV1 d,LabPopulationEffectsV1 e,
    const PopulationBodyGpu* bodies,const PopulationBodyPoseGpu* world,uint32_t nb,const PhysicsLightGpu* lights,uint32_t nl,const PxTransform* poses,
    GpuInstance* instances,uint32_t base,bool visible,float alpha){
    uint32_t slot=blockIdx.x*blockDim.x+threadIdx.x;if(slot>=LAB_POPULATION_SHADOW_MAX_V1)return;
    auto& out=instances[base+count+4*LAB_PHYSICS_POPULATION_DETAIL_MAX_V1+slot];
    Instance(out,PxVec3(0),PxQuat(PxIdentity),PxVec3(0),d.proxyMesh,d.bodyMaterial,0,false);out.flags|=64u;
    uint32_t sampled=min(count,LAB_POPULATION_SHADOW_MAX_V1);
    if(!visible||slot>=sampled||e.shadowDensity<=0||Unit(slot+7919)>e.shadowDensity||!nl||e.shadowIntensity<=0)return;
    uint32_t insect=uint32_t(uint64_t(slot)*count/sampled);auto s=states[insect];
    PxVec3 p=s.previous+(s.position-s.previous)*alpha;
    // One dominant active light. Other lights do not duplicate the population.
    uint32_t li=0;for(uint32_t k=1;k<nl;k++)if(lights[k].light.power>lights[li].light.power)li=k;
    auto l=lights[li];if(l.light.power<=0)return;
    PxTransform lp=l.index==PhysicsInvalidIndex||!poses?l.initial:poses[l.index];PxVec3 origin=lp.transform(V(l.light.localPosition)),ray=p-origin;
    float closest=1e6f;PxVec3 hit(0),normal(0,1,0);PxQuat rotation(PxIdentity);
    for(uint32_t k=0;k<nb;k++){auto b=bodies[k];if(!b.enabled||b.hand||b.shape!=LAB_PHYSICS_BOX)continue;
        auto pose=world[k].current;PxVec3 o=pose.transformInv(origin),v=pose.q.rotateInv(ray);
        if(v.y>=-.00001f)continue;
        float t=(b.dimensions.y-o.y)/v.y;if(t<1||t>=closest||t>8)continue;
        PxVec3 q=o+v*t;if(fabsf(q.x)>b.dimensions.x||fabsf(q.z)>b.dimensions.z)continue;
        closest=t;normal=pose.q.rotate(PxVec3(0,1,0));hit=pose.transform(q)+normal*.0004f;rotation=pose.q;
    }
    if(closest==1e6f)return;
    float height=(hit-p).magnitude(),distance=(hit-origin).magnitude();
    if(distance>l.light.radius)return;
    float cone=l.light.outerCos>-1?fminf(1.f,fmaxf(0.f,(Safe(ray).dot(lp.q.rotate(V(l.light.localDirection)))-l.light.outerCos)/fmaxf(.00001f,l.light.innerCos-l.light.outerCos))):1.f;
    float attenuation=expf(-height*.9f)*fminf(1.f,l.light.power/26.f)*cone;
    float radius=fminf(fmaxf(.024f,d.flight.bodyRadius*8),d.flight.bodyRadius*closest+.002f*height);
    Instance(out,hit,rotation,PxVec3(radius,.0001f,radius),d.proxyMesh,d.bodyMaterial,0,true);
    out.positionRadius.w=radius;out.flags|=64u;out.scale.w=e.shadowIntensity*attenuation;
}
__global__ void Finish(const PopulationStateGpu* states,uint32_t count,uint32_t capacity,LabPopulationDescV1 d,bool visible,GpuInstance* instances,uint32_t* control,uint32_t base,PopulationSummaryGpu* s,float realSeconds){
    uint32_t i=threadIdx.x,detail=min(s->detailedCount,LAB_PHYSICS_POPULATION_DETAIL_MAX_V1);
    if(i)return;
    auto& out=s->snapshot;out.nearCount=s->nearCount;out.detailedCount=detail;out.simplifiedCount=count-detail;
    // The render range follows active count, independently of reserved storage.
    uint32_t active=base+count+LAB_PHYSICS_POPULATION_DETAIL_MAX_V1*4;control[0]=visible?active:0;control[4]=(control[0]+63)/64;
    if(s->targetKey!=0xffffffff){uint32_t id=s->targetKey&0xfffff;if(id<count){out.targetId=id;out.targetImpactRemaining=fmaxf(0,states[id].impact-realSeconds);out.targetActivity=states[id].activity;}}
    for(uint32_t k=0;k<8;k++)if(s->voiceKeys[k]!=0xffffffff){uint32_t id=s->voiceKeys[k]&0xfffff;if(id<count){auto a=states[id];auto& voice=out.voices[out.voiceCount++];voice.position=L(a.position);voice.velocity=L(a.velocity);voice.activity=a.activity;voice.frequency=d.flight.wingFrequency*sqrtf(fmaxf(.01f,a.activity));voice.insect=id;}}
}
}
void PopulationInitialize(cudaStream_t s,PopulationStateGpu* p,uint32_t first,uint32_t n,const LabPopulationDescV1& d,uint32_t birth,const PopulationBodyGpu* bodies,uint32_t nb,const PxTransform* poses){if(n)Init<<<(n+255)/256,256,0,s>>>(p,first,n,d,birth,bodies,nb,poses);}
void PopulationStep(cudaStream_t s,PopulationStateGpu* p,uint32_t n,const LabPopulationDescV1& d,const LabPopulationInputV1& in,const LabPopulationEffectsV1& effects,const LabPopulationHandsV1& hands,const PopulationBodyGpu* bodies,PopulationBodyPoseGpu* world,uint32_t* grid,uint32_t nb,const PxTransform* previous,const PxTransform* poses,const PxVec3* v,const PxVec3* a,PxVec3* forces,PxVec3* torques,const PhysicsLightGpu* lights,uint32_t nl,const PhysicsAirflowGpu* airflows,uint32_t airflowCount,const LabPopulationSpaceV1& space,float dt,float reactionDt,double time,float realSeconds,bool observerPresent){if(n){if(nb)PrepareBodies<<<(nb+63)/64,64,0,s>>>(bodies,world,grid,nb,poses);PopulationClock clock{{float(fmod(time*.713,6.283185307)),float(fmod(time*1.117,6.283185307)),float(fmod(time*1.733,6.283185307))},uint32_t(fmod(time*2.,4294967296.)),realSeconds,float(time)};Step<<<(n+255)/256,256,0,s>>>(p,n,d,in,effects,hands,bodies,world,grid,nb,previous,poses,v,a,forces,torques,lights,nl,airflows,airflowCount,space,dt,reactionDt,clock,observerPresent);}}
void PopulationPack(cudaStream_t s,const PopulationStateGpu* p,uint32_t n,uint32_t capacity,const LabPopulationDescV1& d,const LabPopulationInputV1& in,const LabPopulationEffectsV1& effects,const PopulationBodyGpu* bodies,const PopulationBodyPoseGpu* world,uint32_t nb,const PhysicsLightGpu* lights,uint32_t nl,const PxTransform* poses,bool visible,GpuInstance* instances,uint32_t* control,uint32_t base,PopulationSummaryGpu* summary,uint64_t frame,uint64_t tick,float alpha,float realSeconds){
    ResetSummary<<<1,1,0,s>>>(summary,control,base,capacity,n,visible,frame,tick);if(n)Pack<<<(n+255)/256,256,0,s>>>(p,n,capacity,d,in,visible,instances,base,summary,alpha);Details<<<1,64,0,s>>>(p,n,d,in,visible,instances,base,summary,alpha);Shadows<<<(LAB_POPULATION_SHADOW_MAX_V1+255)/256,256,0,s>>>(p,n,d,effects,bodies,world,nb,lights,nl,poses,instances,base,visible,alpha);Finish<<<1,64,0,s>>>(p,n,capacity,d,visible,instances,control,base,summary,realSeconds);
}
}
