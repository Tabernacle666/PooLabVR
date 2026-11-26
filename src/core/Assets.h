#pragma once
#include "Core.h"
#include <filesystem>
#include <dxgiformat.h>
#include <vector>
#include <string>
#include <span>
#include <unordered_map>
#include <stdexcept>
namespace OASIS {
class AssetImportError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
struct Vertex { XMFLOAT3 position,normal; XMFLOAT2 uv,uv1{}; };
struct Meshlet { uint32_t vertexOffset,triangleOffset,vertexCount,triangleCount; };
struct MeshRange { uint32_t firstMeshlet{},meshletCount{},firstVertex{},vertexCount{},firstIndex{},indexCount{}; XMFLOAT4 bounds{}; };
// Indexed by meshlet ID. Bounds are in the same coordinate space as its vertices.
struct Deformation { XMFLOAT4 axisAmplitude{1,0,0,0}; XMFLOAT4 frequencyPhase{0,0,0,0}; };
struct SkinVertex {XMUINT4 joints{},joints1{};XMFLOAT4 weights{1,0,0,0},weights1{};};
struct MorphVertex {XMFLOAT3 position{},normal{};};
struct MeshMotion {uint32_t node{Invalid},boneStart{},boneCount{},morphStart{},morphCount{},weightStart{};};
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices,remap;
    std::vector<std::array<uint32_t,3>> triangles;
    std::vector<Meshlet> meshlets;
    std::vector<MeshRange> ranges;
    std::vector<Deformation> deformations;
    std::vector<SkinVertex> skin;
    std::vector<MorphVertex> morphs;
    std::vector<MeshMotion> motions;
};
enum class TextureUse:uint32_t {Color,Normal,Linear,Height};
struct TextureData {uint32_t width{},height{},mips{};bool normal{};DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};std::vector<std::byte> bytes;std::vector<uint64_t> offsets;};
struct TextureMapping {XMFLOAT4 offsetScale{0,0,1,1};XMFLOAT4 rotationSet{1,0,0,0};};
struct SamplerData {uint32_t minFilter{9987},magFilter{9729},wrapS{10497},wrapT{10497};bool operator==(const SamplerData&)const=default;};
// Canonical identity is separate from source glTF labels and texture indices.
struct AssetMaterial {
    XMFLOAT4 colorRoughness{1,1,1,1},emissionMetallic{};
    uint32_t albedo{Invalid},normal{Invalid},metalRough{Invalid},emissive{Invalid};float normalScale{1};
    XMFLOAT2 heightUvScale{};float macroVariation{};
    uint32_t occlusion{Invalid},alphaMode{},doubleSided{};float alpha{1},cutoff{.5f},occlusionStrength{1};uint32_t unlit{};
    uint32_t roughnessOverride{Invalid},authoringSlot{Invalid};
    uint64_t physicalMaterialId{};
    uint32_t profileInstalled{};
    uint32_t viewRole{}; // functional view-model shader role, not physical composition
    // policy: structured reflection eligibility, dielectric F0, roughness floor, metallic ceiling
    XMFLOAT4 opticalPolicy{0.f,.04f,.04f,1.f};
    // Declared layered/extended properties; individual shader features use explicit support.
    XMFLOAT4 opticalLayers{0.f,1.f,0.f,0.f}; // clearcoat, coat roughness, anisotropy, diffuse transmission
    XMFLOAT2 opticalTransmission{1.5f,0.f}; // IOR, transmission
    std::array<TextureMapping,5> mapping{};
    std::array<uint32_t,5> samplers{Invalid,Invalid,Invalid,Invalid,Invalid};
};
struct SceneNode {uint32_t parent{Invalid},weightStart{},weightCount{},matrixOnly{};Pose pose;XMFLOAT3 scale{1,1,1};XMFLOAT4X4 matrix{};};
struct BoneBinding {uint32_t node{};XMFLOAT4X4 inverseBind{};};
enum class AnimationPath:uint32_t {Translation,Rotation,Scale,Weights};
enum class Interpolation:uint32_t {Linear,Step,Cubic};
struct AnimationTrack {uint32_t node{},components{};AnimationPath path{};Interpolation interpolation{};std::vector<float> times,values;};
struct AnimationClip {float duration{};std::vector<AnimationTrack> tracks;};
struct Mg2ForestPart {uint32_t mesh{},material{};};
// Botanical source recipes: shared primitive topology, tree-local placement,
// and cooked mesh/material routes. These are source data, never WorldObjects.
enum class CanopyPartKind : uint8_t { Trunk, Root, Branch, Twig, Needles, Leaves };
struct CanopyPart {
    CanopyPartKind kind{};
    uint32_t sides{};
    // The final ring sample is at 2*pi, not a wrapped copy of sample zero.
    std::vector<XMFLOAT2> ring;
};
struct CanopyPartPlacement {
    uint32_t part{};
    XMFLOAT3 root{},tip{},transverse{};
    float radius0{},radius1{};
    // The placement ordinal is its stable assembly-local identity.
    // Parent ordinals precede children and never depend on GPU LOD routes.
    uint32_t parent{Invalid};
};
struct CanopyAssembly {
    std::vector<CanopyPartPlacement> placements;
    std::array<uint32_t,2> meshes{Invalid,Invalid};
    std::array<uint32_t,2> materials{Invalid,Invalid};
};
struct CanopyLibrary {
    std::vector<CanopyPart> parts;
    std::array<CanopyAssembly,3> assemblies;
};
// Camera-only opaque building envelope. Not a scene object or ray primitive.
struct BuildingOcclusionBox {XMFLOAT4 lo,hi;};
static_assert(sizeof(BuildingOcclusionBox)==32);
struct SceneData {
    MeshData meshes;std::vector<TextureData> textures;std::vector<AssetMaterial> materials;std::vector<RenderObject> objects;std::vector<SceneLight> lights;
    // One source-volume identifier per triangle in meshes.indices. A value of
    // Invalid denotes a surface that does not bound a closed volume.
    std::vector<uint32_t> triangleSolids;
    bool writableStaticGeometry{};
    bool assetDegraded{};
    std::vector<SamplerData> samplers;
    std::vector<SceneNode> nodes;std::vector<uint32_t> nodeOrder;std::vector<BoneBinding> bones;std::vector<float> weights;std::vector<AnimationClip> animations;
    std::vector<std::string> materialNames;
    std::vector<std::string> meshNodeNames;
    // Authoring-time texture deduplication: across GLBs, all furniture instances
    // reuse the exact uploaded image when their decoded bytes match.
    std::unordered_map<uint64_t,std::vector<uint32_t>> textureCache;
    std::vector<Mg2ForestPart> mg2ForestParts;
    std::array<uint32_t,2> laserWallMaterials{Invalid,Invalid};
    // Contiguous static object ranges, one per body, followed by the end.
    std::vector<uint32_t> sphereObjectOffsets;
    bool solarSystem{};
    std::vector<BuildingOcclusionBox> buildingOcclusionBoxes;
    CanopyLibrary canopy;
};
std::wstring AssetPath(const wchar_t* name);
std::vector<std::byte> ReadBytes(const std::wstring&);
MeshData LoadGLTF(const std::wstring&);
TextureData LoadKTX2(const std::wstring&,bool);
TextureData DecodeTexture(std::span<const std::byte>,TextureUse);
TextureData TryLoadOverrideTexture(const std::filesystem::path&,TextureUse);
TextureData LoadTexture(const std::wstring&,TextureUse);
SceneData LoadSceneGLTF(const std::wstring&,bool keepMeshNodeNames=false);
SceneData BuildEmergencySite();
struct AppendedRigidAsset {uint32_t firstMesh{},firstMaterial{};};
AppendedRigidAsset AppendRigidAsset(SceneData& scene,SceneData& source);
uint32_t AppendAssetMaterials(SceneData& scene,SceneData& source); // shared texture+material import, no geometry
// Append native rigid, indexed geometry with the same meshlet encoding as glTF.
void FinishRange(MeshData&,MeshRange&);
struct BuildingFootprint { XMFLOAT3 origin; float width,depth; };
using TerrainTriangle=std::array<XMFLOAT3,3>;
struct BuildingSiteTerrain {
    std::vector<TerrainTriangle> soil,banks,waterway;
};
BuildingSiteTerrain PrepareBuildingSite(SceneData&,std::span<const BuildingFootprint>);
uint32_t AppendBuildingBox(SceneData&,bool pane=false);
void PrepareSpectacleGeometry(SceneData&);
void AppendSpectacleCity(SceneData&);
void AppendSolarPlanets(SceneData&);
void InstantiateSpectacleCities(SceneData&,uint32_t count);
void AppendAcousticGeometry(const SceneData&,std::vector<XMFLOAT3>&,std::vector<std::array<uint32_t,3>>&);
TextureData CookTexture(const TextureData&,const std::wstring& ktxPath={});
}
