#include "GraphicsInternal.h"

namespace OASIS {
    void Graphics::Impl::PublishFrameObjects(Frame& f){
        rayScene.UpdateDynamic(sceneCache.StaticCount(),std::span<const GpuInstance>(f.objects,f.dynamicCount),{});
        if(f.snapshotCount)
            std::memcpy(f.uploadGeneration->mapped+f.objectsOffset,f.objects,size_t(f.snapshotCount)*sizeof(GpuInstance));
    }

    void Graphics::Impl::RebaseSolarFrame(Frame& f,XRFrame& frame,FXMMATRIX bodyToCosmos,CXMMATRIX cosmosToBody){
        // Static meshes, cuts and the view bank use this body's canonical space.
        for(uint32_t eye=0;eye<frame.viewCount;++eye){
            XMStoreFloat4x4(&frame.vp[eye],bodyToCosmos*XMLoadFloat4x4(&frame.vp[eye]));
            XMStoreFloat4x4(&frame.inverseVP[eye],XMLoadFloat4x4(&frame.inverseVP[eye])*cosmosToBody);
            frame.eyes[eye]=TransformPoint(frame.eyes[eye],cosmosToBody);
        }
        auto pose=[&](Pose& p){p=FromMatrix(Matrix(p)*cosmosToBody);};
        auto ray=[&](LaserRay& r){
            r.origin=TransformPoint(r.origin,cosmosToBody);
            r.end=TransformPoint(r.end,cosmosToBody);
        };
        pose(frame.input.head);
        for(auto& glove:frame.glovePoses)if(glove)pose(*glove);
        if(frame.toolExit)pose(*frame.toolExit);
        for(auto& shot:frame.shotHands)if(shot.aim.tracked)pose(shot.aim.pose);
        if(frame.basaltRay)ray(*frame.basaltRay);
        if(frame.previousLaserRay)ray(*frame.previousLaserRay);
        for(uint32_t i=0;i<frame.laserAfterimageCount;++i)ray(frame.laserAfterimages[i].ray);
        // Visual-only contributors publish cosmos poses before the native body is selected.
        const auto rotation=XMQuaternionRotationMatrix(cosmosToBody);
        for(uint32_t i=0;i<f.snapshotCount;++i){
            auto& object=f.objects[i];
            if((object.flags&SolarWorldVisual)==0u)continue;
            const auto p=TransformPoint({object.positionRadius.x,object.positionRadius.y,
                object.positionRadius.z},cosmosToBody);
            object.positionRadius={p.x,p.y,p.z,object.positionRadius.w};
            const auto& packedRotation=object.rotation;
            XMFLOAT4 quaternion{packedRotation.x,packedRotation.y,packedRotation.z,packedRotation.w};
            XMStoreFloat4(&quaternion,XMQuaternionNormalize(
                XMQuaternionMultiply(XMLoadFloat4(&quaternion),rotation)));
            object.rotation={quaternion.x,quaternion.y,quaternion.z,quaternion.w};
        }
    }
}
