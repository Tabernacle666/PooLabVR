#include "PhysicsRuntimeInternal.h"

namespace lab {
uint32_t PhysicsRuntime::Impl::Scene::CreateBody(const LabPhysicsBodyDescV1& d,LabPhysicsBodyV1* out){
    if(!out||paused&&started)return 0;*out=0;
    if(d.structSize<sizeof(d)||d.kind>LAB_PHYSICS_KINEMATIC||d.shape>LAB_PHYSICS_CONVEX||!Valid(d.pose)||!Valid(d.shapePose)||!Valid(d.dimensions)||d.layer==0||!Finite(d.mass)||!Finite(d.contactOffset)||d.contactOffset<=d.restOffset||d.contactOffset<=0||!Finite(d.staticFriction)||!Finite(d.dynamicFriction)||d.staticFriction<0||d.dynamicFriction<0||!Finite(d.restitution)||d.restitution<0||d.restitution>1||!Finite(d.linearDamping)||!Finite(d.angularDamping)||d.linearDamping<0||d.angularDamping<0)return 0;
    if(d.kind!=LAB_PHYSICS_STATIC&&d.mass<=0)return 0;
    uint32_t slot=0;while(slot<bodies.size()&&bodies[slot].actor)++slot;if(slot>=desc.maxBodies)return 0;
    {PxScopedCudaLock lock(*runtime.cuda);Cuda(cudaStreamSynchronize(stream),"retire actor creation consumers");}
    if(slot==bodies.size())bodies.emplace_back();Body b;b.geometry=d;b.geometry.convexVertices=nullptr;b.initial=P(d.pose).getNormalized();b.committed=b.initial;b.lastTarget=b.initial;b.kinematic=d.kind==LAB_PHYSICS_KINEMATIC;b.collisionMask=d.collisionMask;
    b.material=runtime.physics->createMaterial(d.staticFriction,d.dynamicFriction,d.restitution);if(!b.material)return 0;
    PxShape* shape{};
    if(d.shape==LAB_PHYSICS_BOX&&d.dimensions.x>0&&d.dimensions.y>0&&d.dimensions.z>0)shape=runtime.physics->createShape(PxBoxGeometry(V(d.dimensions)),*b.material,true);
    if(d.shape==LAB_PHYSICS_SPHERE&&d.dimensions.x>0)shape=runtime.physics->createShape(PxSphereGeometry(d.dimensions.x),*b.material,true);
    if(d.shape==LAB_PHYSICS_CAPSULE&&d.dimensions.x>0&&d.dimensions.y>=0)shape=runtime.physics->createShape(PxCapsuleGeometry(d.dimensions.x,d.dimensions.y),*b.material,true);
    if(d.shape==LAB_PHYSICS_CONVEX&&d.convexVertices&&d.convexVertexCount>=4&&d.convexVertexCount<=64){
        PxCookingParams params(runtime.physics->getTolerancesScale());params.buildGPUData=true;PxConvexMeshDesc mesh;mesh.points.count=d.convexVertexCount;mesh.points.stride=sizeof(LabVec3V1);mesh.points.data=d.convexVertices;mesh.flags=PxConvexFlag::eCOMPUTE_CONVEX;mesh.vertexLimit=64;
        b.convex=PxCreateConvexMesh(params,mesh,runtime.physics->getPhysicsInsertionCallback());
        if(b.convex&&b.convex->isGpuCompatible())shape=runtime.physics->createShape(PxConvexMeshGeometry(b.convex),*b.material,true);
    }
    if(!shape){if(b.convex)b.convex->release();b.material->release();return 0;}
    shape->setLocalPose(P(d.shapePose));shape->setContactOffset(d.contactOffset);shape->setRestOffset(d.restOffset);shape->setSimulationFilterData(PxFilterData(d.layer,d.collisionMask,d.collisionGroup,0));
    if(d.kind==LAB_PHYSICS_STATIC)b.actor=runtime.physics->createRigidStatic(b.initial);else b.actor=runtime.physics->createRigidDynamic(b.initial);
    if(!b.actor){shape->release();if(b.convex)b.convex->release();b.material->release();return 0;}b.actor->attachShape(*shape);shape->release();
    if(d.kind!=LAB_PHYSICS_STATIC){auto* dynamic=static_cast<PxRigidDynamic*>(b.actor);Gpu(PxRigidBodyExt::setMassAndUpdateInertia(*dynamic,d.mass),"PhysX: failed body inertia calculation");if(d.inertia.x>0&&d.inertia.y>0&&d.inertia.z>0&&Valid(d.inertia))dynamic->setMassSpaceInertiaTensor(V(d.inertia));dynamic->setLinearDamping(d.linearDamping);dynamic->setAngularDamping(d.angularDamping);dynamic->setMaxAngularVelocity(d.maxAngularVelocity>0?d.maxAngularVelocity:200.f);dynamic->setSolverIterationCounts(SolverPositionIterations,SolverVelocityIterations);dynamic->setSleepThreshold(0);if(b.kinematic)dynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC,true);if(d.reserved[0]&LAB_PHYSICS_BODY_SPECULATIVE_CCD)dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_SPECULATIVE_CCD,true);}
    b.handle=(uint64_t(runtime.Generation())<<32)|(slot+1);scene->addActor(*b.actor);bodies[slot]=b;topologyDirty=descriptorsDirty=true;*out=b.handle;return 1;
}
uint32_t PhysicsRuntime::Impl::Scene::DestroyBody(LabPhysicsBodyV1 h){
    auto* b=FindBody(h);if(!b||paused&&started)return 0;
    for(const auto& v:visuals)if(v.body==h)return 0;for(const auto& l:lights)if(l.body==h)return 0;
    {PxScopedCudaLock lock(*runtime.cuda);Cuda(cudaStreamSynchronize(stream),"retire body mutation consumers");}
    for(auto& j:joints)if(j.joint){PxRigidActor *a{},*other{};j.joint->getActors(a,other);if(a==b->actor||other==b->actor){j.joint->release();j={};}}
    std::erase_if(airflows,[&](const auto& f){return f.emitter==h;});std::erase_if(commands,[&](auto& c){return c.body==h;});std::erase_if(aero,[&](auto& a){return a.wing==h||a.carrier==h;});std::erase(probes,h);
    // A visible/probed body cannot be replaced with a fictitious pose. Clients
    // must detach/reconfigure its visuals before destroying that actor.
    b->actor->release();b->material->release();if(b->convex)b->convex->release();*b={};topologyDirty=descriptorsDirty=true;return 1;
}
uint32_t PhysicsRuntime::Impl::Scene::SetBodyCollisionEnabled(LabPhysicsBodyV1 h,bool enabled){
    auto* b=FindBody(h);if(!b)return 0;if(b->collisionEnabled==enabled)return 1;
    PxScopedCudaLock lock(*runtime.cuda);Cuda(cudaStreamSynchronize(stream),"retire collision filter consumers");
    PxShape* shape{};if(b->actor->getShapes(&shape,1)!=1||b->actor->getNbShapes()!=1)return 0;
    auto filter=shape->getSimulationFilterData();filter.word1=enabled?b->collisionMask:0;
    shape->setSimulationFilterData(filter);
    Gpu(scene->resetFiltering(*b->actor),"PhysX GPU body collision filter update failed");runtime.Check();
    b->collisionEnabled=enabled;descriptorsDirty=true;return 1;
}
uint32_t PhysicsRuntime::Impl::Scene::SetBodySpeculativeCCD(LabPhysicsBodyV1 h,bool enabled){
    auto* b=FindBody(h);if(!b||b->kinematic)return 0;
    auto* dynamic=b->actor->is<PxRigidDynamic>();if(!dynamic)return 0;
    PxScopedCudaLock lock(*runtime.cuda);
    Cuda(cudaStreamSynchronize(stream),"retire CCD flag consumers");
    dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_SPECULATIVE_CCD,enabled);
    runtime.Check();return 1;
}
uint32_t PhysicsRuntime::Impl::Scene::CreateD6(const LabPhysicsD6DescV1& d,LabPhysicsJointV1* out){
    if(!out)return 0;*out=0;if(d.structSize<sizeof(d)||!Valid(d.frameA)||!Valid(d.frameB)||d.bodyA==d.bodyB)return 0;
    auto* a=d.bodyA?FindBody(d.bodyA):nullptr;auto* b=FindBody(d.bodyB);if((d.bodyA&&!a)||!b)return 0;for(auto m:d.motion)if(m>LAB_PHYSICS_FREE)return 0;
    auto* joint=PxD6JointCreate(*runtime.physics,a?a->actor:nullptr,P(d.frameA),b->actor,P(d.frameB));if(!joint)return 0;
    uint32_t slot=0;while(slot<joints.size()&&joints[slot].joint)++slot;if(slot==joints.size())joints.emplace_back();auto& j=joints[slot];j.handle=(uint64_t(runtime.Generation())<<32)|(slot+1);j.joint=joint;if(!ConfigureD6(j.handle,d)){joint->release();j={};return 0;}*out=j.handle;return 1;
}
uint32_t PhysicsRuntime::Impl::Scene::ConfigureD6(LabPhysicsJointV1 h,const LabPhysicsD6DescV1& d){
    auto* j=FindJoint(h);if(!j||d.structSize<sizeof(d)||!Valid(d.frameA)||!Valid(d.frameB))return 0;
    auto* a=d.bodyA?FindBody(d.bodyA):nullptr;auto* b=FindBody(d.bodyB);
    PxRigidActor *actualA{},*actualB{};j->joint->getActors(actualA,actualB);
    if((d.bodyA&&!a)||!b||(a?a->actor:nullptr)!=actualA||b->actor!=actualB)return 0;
    for(uint32_t i=0;i<6;i++)if(d.motion[i]>LAB_PHYSICS_FREE||!Finite(d.drive[i].stiffness)||!Finite(d.drive[i].damping)||!Finite(d.drive[i].maxForce)||d.drive[i].stiffness<0||d.drive[i].damping<0||d.drive[i].maxForce<0)return 0;
    if(!Finite(d.limitStiffness)||!Finite(d.limitDamping)||d.limitStiffness<0||d.limitDamping<0)return 0;
    for(uint32_t i=0;i<3;i++)if(d.motion[i]==LAB_PHYSICS_LIMITED&&(!Finite(d.linearLower[i])||!Finite(d.linearUpper[i])||d.linearUpper[i]<=d.linearLower[i]))return 0;
    if(d.motion[3]==LAB_PHYSICS_LIMITED&&(!Finite(d.twistLower)||!Finite(d.twistUpper)||d.twistUpper<=d.twistLower||d.twistLower<=-6.28f||d.twistUpper>=6.28f))return 0;
    if((d.motion[4]==LAB_PHYSICS_LIMITED||d.motion[5]==LAB_PHYSICS_LIMITED)&&(!Finite(d.swingY)||!Finite(d.swingZ)||d.swingY<=0||d.swingZ<=0||d.swingY>=3.14f||d.swingZ>=3.14f))return 0;
    const auto& slerp=d.drive[LAB_PHYSICS_DRIVE_SLERP];
    bool useSlerp=slerp.stiffness>0||slerp.damping>0;
    if(useSlerp){
        for(uint32_t i=3;i<6;i++)if(d.motion[i]==LAB_PHYSICS_LOCKED)return 0;
        if(d.drive[LAB_PHYSICS_DRIVE_SWING].stiffness||d.drive[LAB_PHYSICS_DRIVE_SWING].damping||d.drive[LAB_PHYSICS_DRIVE_TWIST].stiffness||d.drive[LAB_PHYSICS_DRIVE_TWIST].damping)return 0;
    }
    auto* joint=j->joint;joint->setLocalPose(PxJointActorIndex::eACTOR0,P(d.frameA));joint->setLocalPose(PxJointActorIndex::eACTOR1,P(d.frameB));
    for(uint32_t i=0;i<6;i++)joint->setMotion(PxD6Axis::Enum(i),PxD6Motion::Enum(d.motion[i]));
    for(uint32_t i=0;i<3;i++)if(d.motion[i]==LAB_PHYSICS_LIMITED){PxJointLinearLimitPair limit(d.linearLower[i],d.linearUpper[i],PxSpring(d.limitStiffness,d.limitDamping));joint->setLinearLimit(PxD6Axis::Enum(i),limit);}
    if(d.motion[3]==LAB_PHYSICS_LIMITED)joint->setTwistLimit(PxJointAngularLimitPair(d.twistLower,d.twistUpper,PxSpring(d.limitStiffness,d.limitDamping)));
    if(d.motion[4]==LAB_PHYSICS_LIMITED||d.motion[5]==LAB_PHYSICS_LIMITED)joint->setSwingLimit(PxJointLimitCone(d.swingY,d.swingZ,PxSpring(d.limitStiffness,d.limitDamping)));
    // Public V1 drive indices are independent of PhysX's seven drive indices.
    // In PhysX 5.9, TWIST is 3 and SLERP is 6; a cast drives the wrong axes.
    auto drive=[&](uint32_t i){const auto& v=d.drive[i];return PxD6JointDrive(v.stiffness,v.damping,v.maxForce,v.acceleration!=0);};
    joint->setAngularDriveConfig(useSlerp?PxD6AngularDriveConfig::eSLERP:PxD6AngularDriveConfig::eSWING_TWIST);
    joint->setDrive(PxD6Drive::eX,drive(LAB_PHYSICS_DRIVE_X));joint->setDrive(PxD6Drive::eY,drive(LAB_PHYSICS_DRIVE_Y));joint->setDrive(PxD6Drive::eZ,drive(LAB_PHYSICS_DRIVE_Z));
    if(useSlerp)joint->setDrive(PxD6Drive::eSLERP,drive(LAB_PHYSICS_DRIVE_SLERP));
    else{joint->setDrive(PxD6Drive::eSWING1,drive(LAB_PHYSICS_DRIVE_SWING));joint->setDrive(PxD6Drive::eSWING2,drive(LAB_PHYSICS_DRIVE_SWING));joint->setDrive(PxD6Drive::eTWIST,drive(LAB_PHYSICS_DRIVE_TWIST));}
    joint->setConstraintFlag(PxConstraintFlag::eCOLLISION_ENABLED,d.collideConnected!=0);
    j->description=d;j->committedFrameA=d.frameA;j->committedFrameB=d.frameB;
    j->lastDriveValid=false;
    return 1;
}
uint32_t PhysicsRuntime::Impl::Scene::DestroyJoint(LabPhysicsJointV1 h){auto* j=FindJoint(h);if(!j)return 0;j->joint->release();*j={};return 1;}
} // namespace lab
