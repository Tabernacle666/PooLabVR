#pragma once
#include "LaboVRVisualService.h"
#include <array>
#include <span>
#include <cstddef>
namespace labovr {
using VisualTexture=LabVisualTextureV1;
using VisualMesh=LabVisualMeshV1;
using VisualMaterial=LabVisualMaterialV1;
using VisualObject=LabVisualObjectV1;
using VisualStream=LabVisualStreamV1;
using VisualEffect=LabVisualEffectV1;
using VisualCapture=LabVisualCaptureV1;
using VisualLight=LabVisualLightV1;
constexpr VisualTexture Texture(LabU32 id,const char* path,LabU32 use){return {sizeof(VisualTexture),id,use,0,path};}
constexpr VisualTexture TextureColor(LabU32 id,const char* path){return Texture(id,path,LABOVR_VISUAL_TEXTURE_COLOR_V1);}
constexpr VisualTexture TextureNormal(LabU32 id,const char* path){return Texture(id,path,LABOVR_VISUAL_TEXTURE_NORMAL_V1);}
constexpr VisualTexture TextureOrm(LabU32 id,const char* path){return Texture(id,path,LABOVR_VISUAL_TEXTURE_ORM_V1);}
constexpr VisualTexture TextureHeight(LabU32 id,const char* path){return Texture(id,path,LABOVR_VISUAL_TEXTURE_HEIGHT_V1);}
constexpr VisualTexture TextureDetail(LabU32 id,const char* path){return Texture(id,path,LABOVR_VISUAL_TEXTURE_DETAIL_V1);}
constexpr VisualMesh MeshFile(LabU32 id,const char* path){return {sizeof(VisualMesh),id,LABOVR_VISUAL_MESH_FILE_V1,0,path};}
constexpr VisualMesh MeshBox(LabU32 id){return {sizeof(VisualMesh),id,LABOVR_VISUAL_MESH_BOX_V1,0,nullptr};}
constexpr VisualMesh MeshSphere(LabU32 id){return {sizeof(VisualMesh),id,LABOVR_VISUAL_MESH_SPHERE_V1,0,nullptr};}
constexpr VisualMesh MeshCylinder(LabU32 id){return {sizeof(VisualMesh),id,LABOVR_VISUAL_MESH_CYLINDER_V1,0,nullptr};}
inline VisualMesh MeshMemory(LabU32 id,const LabVisualMeshDataV1* data){VisualMesh mesh{sizeof(VisualMesh),id,LABOVR_VISUAL_MESH_MEMORY_V1,0,nullptr};mesh.data=data;return mesh;}
constexpr VisualMaterial Material(LabU32 id,LabU32 model){
    VisualMaterial m{};m.structSize=sizeof(m);m.id=id;m.model=model;
    m.albedoTexture=m.normalTexture=m.ormTexture=m.heightTexture=m.detailTexture=LABOVR_VISUAL_INVALID_ID_V1;
    m.tint[0]=m.tint[1]=m.tint[2]=m.tint[3]=1;m.uvScale=m.normalScale=m.detailScale=m.tessFactor=1;
    m.parallaxMinSteps=8;m.parallaxMaxSteps=32;return m;
}
constexpr VisualMaterial MaterialPbr(LabU32 id){return Material(id,LABOVR_VISUAL_MATERIAL_PBR_V1);}
constexpr VisualMaterial MaterialHeightPbr(LabU32 id){auto m=Material(id,LABOVR_VISUAL_MATERIAL_HEIGHT_PBR_V1);m.parallaxMaxSteps=96;return m;}
constexpr VisualMaterial MaterialGel(LabU32 id){auto m=Material(id,LABOVR_VISUAL_MATERIAL_REFRACTIVE_GEL_V1);m.refractionStrength=.02f;m.thickness=.1f;return m;}
constexpr VisualMaterial MaterialVertexColor(LabU32 id){return Material(id,LABOVR_VISUAL_MATERIAL_VERTEX_COLOR_V1);}
constexpr VisualObject Object(LabU32 id,LabU32 mesh,LabU32 material,LabPoseV1 pose={{0,0,0},{0,0,0,1}},LabVec3V1 scale={1,1,1},LabU32 visibility=LABOVR_VISUAL_MAIN_V1|LABOVR_VISUAL_CAPTURE_V1){
    VisualObject object{};object.structSize=sizeof(object);object.id=id;object.mesh=mesh;object.material=material;object.visibilityMask=visibility;object.pose=pose;object.scale=scale;object.emissionScale=1;object.updateMask=LABOVR_VISUAL_OBJECT_STATIC_V1;return object;
}
constexpr VisualStream Stream(LabU32 id,LabU32 material,LabU32 capacity,LabU32 primitive,LabU32 visibility,LabU32 sort,float pointSize){return {sizeof(VisualStream),id,primitive,material,capacity,visibility,sort,pointSize};}
constexpr VisualStream Triangles(LabU32 id,LabU32 material,LabU32 capacity,LabU32 visibility=LABOVR_VISUAL_MAIN_V1|LABOVR_VISUAL_CAPTURE_V1,LabU32 sort=LABOVR_VISUAL_SORT_NONE_V1){return Stream(id,material,capacity,LABOVR_VISUAL_STREAM_TRIANGLES_V1,visibility,sort,1);}
constexpr VisualStream Lines(LabU32 id,LabU32 material,LabU32 capacity,LabU32 visibility=LABOVR_VISUAL_MAIN_V1|LABOVR_VISUAL_CAPTURE_V1){return Stream(id,material,capacity,LABOVR_VISUAL_STREAM_LINES_V1,visibility,LABOVR_VISUAL_SORT_NONE_V1,1);}
constexpr VisualStream Points(LabU32 id,LabU32 material,LabU32 capacity,float pointSizePixels=1,LabU32 visibility=LABOVR_VISUAL_MAIN_V1|LABOVR_VISUAL_CAPTURE_V1){return Stream(id,material,capacity,LABOVR_VISUAL_STREAM_POINTS_V1,visibility,LABOVR_VISUAL_SORT_NONE_V1,pointSizePixels);}
constexpr VisualEffect Effect(LabU32 id,LabU32 type,LabU32 visibility,std::array<float,8> values){VisualEffect e{sizeof(VisualEffect),id,type,visibility,{}};for(size_t i=0;i<values.size();++i)e.values[i]=values[i];return e;}
constexpr VisualEffect Cosmos(LabU32 id,LabU32 visibility=LABOVR_VISUAL_MAIN_V1|LABOVR_VISUAL_CAPTURE_V1,std::array<float,8> values={}){return Effect(id,LABOVR_VISUAL_EFFECT_COSMOS_V1,visibility,values);}
constexpr VisualEffect LiquidOrb(LabU32 id,LabU32 visibility=LABOVR_VISUAL_MAIN_V1|LABOVR_VISUAL_CAPTURE_V1,std::array<float,8> values={0,1,1,1,.1f,1,1,0}){return Effect(id,LABOVR_VISUAL_EFFECT_LIQUID_ORB_V1,visibility,values);}
constexpr VisualEffect Bloom(LabU32 id,LabU32 visibility=LABOVR_VISUAL_MAIN_V1|LABOVR_VISUAL_CAPTURE_V1,std::array<float,8> values={2.5f,1,.58f}){return Effect(id,LABOVR_VISUAL_EFFECT_BLOOM_V1,visibility,values);}
constexpr VisualCapture Capture(LabU32 id,LabU32 width,LabU32 height){return {sizeof(VisualCapture),id,width,height};}
template<class T,size_t N>constexpr LabU32 Count(const std::array<T,N>&){static_assert(N<=0xffffffffu);return LabU32(N);}
inline LabVisualPackageV1 Package(std::span<const LabVisualTextureV1> textures,std::span<const LabVisualMeshV1> meshes,std::span<const LabVisualMaterialV1> materials,std::span<const LabVisualObjectV1> objects,std::span<const LabVisualLightV1> lights,std::span<const LabVisualStreamV1> streams,std::span<const LabVisualEffectV1> effects,std::span<const LabVisualCaptureV1> captures,const char* externalScenePath=nullptr){
    return {LABOVR_VISUAL_PACKAGE_VERSION_V1,sizeof(LabVisualPackageV1),textures.data(),LabU32(textures.size()),meshes.data(),LabU32(meshes.size()),materials.data(),LabU32(materials.size()),objects.data(),LabU32(objects.size()),lights.data(),LabU32(lights.size()),streams.data(),LabU32(streams.size()),effects.data(),LabU32(effects.size()),captures.data(),LabU32(captures.size()),externalScenePath};
}
inline LabVisualStreamFrameV1 StreamFrame(LabU32 stream,std::span<const LabVisualStreamVertexV1> vertices,float pointSizePixels=0){return {sizeof(LabVisualStreamFrameV1),stream,vertices.data(),LabU32(vertices.size()),pointSizePixels};}
inline LabVisualBatchV1 Batch(std::span<const LabVisualObjectStateV1> objects,std::span<const LabVisualStreamFrameV1> streams,std::span<const LabVisualEffectStateV1> effects,std::span<const LabVisualLightModifierV1> lights,std::span<const LabVisualCaptureRequestV1> captures){return {LABOVR_VISUAL_BATCH_VERSION_V1,sizeof(LabVisualBatchV1),objects.data(),LabU32(objects.size()),streams.data(),LabU32(streams.size()),effects.data(),LabU32(effects.size()),lights.data(),LabU32(lights.size()),captures.data(),LabU32(captures.size())};}
}
