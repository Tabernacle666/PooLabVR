#pragma once
#include "Core.h"
#include <dxgiformat.h>
#include <vector>
#include <string>
#include <span>
namespace lab {
struct Vertex { XMFLOAT3 position,normal; XMFLOAT2 uv,uv1{}; };
struct Meshlet { uint32_t vertexOffset,triangleOffset,vertexCount,triangleCount; };
struct MeshRange { uint32_t firstMeshlet{},meshletCount{},firstVertex{},vertexCount{},firstIndex{},indexCount{}; XMFLOAT4 bounds{}; };
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
struct AssetMaterial {
    XMFLOAT4 colorRoughness{1,1,1,1},emissionMetallic{};
    uint32_t albedo{Invalid},normal{Invalid},metalRough{Invalid},emissive{Invalid};float normalScale{1};
    uint32_t occlusion{Invalid},alphaMode{},doubleSided{};float alpha{1},cutoff{.5f},occlusionStrength{1};uint32_t unlit{};
    std::array<TextureMapping,5> mapping{};
    std::array<uint32_t,5> samplers{Invalid,Invalid,Invalid,Invalid,Invalid};
};
struct SceneNode {uint32_t parent{Invalid},weightStart{},weightCount{},matrixOnly{};Pose pose;XMFLOAT3 scale{1,1,1};XMFLOAT4X4 matrix{};};
struct BoneBinding {uint32_t node{};XMFLOAT4X4 inverseBind{};};
enum class AnimationPath:uint32_t {Translation,Rotation,Scale,Weights};
enum class Interpolation:uint32_t {Linear,Step,Cubic};
struct AnimationTrack {uint32_t node{},components{};AnimationPath path{};Interpolation interpolation{};std::vector<float> times,values;};
struct AnimationClip {float duration{};std::vector<AnimationTrack> tracks;};
struct SceneData {
    MeshData meshes;std::vector<TextureData> textures;std::vector<AssetMaterial> materials;std::vector<RenderObject> objects;std::vector<SceneLight> lights;
    std::vector<SamplerData> samplers;
    std::vector<SceneNode> nodes;std::vector<uint32_t> nodeOrder;std::vector<BoneBinding> bones;std::vector<float> weights;std::vector<AnimationClip> animations;
};
std::wstring AssetPath(const wchar_t* name);
std::vector<std::byte> ReadBytes(const std::wstring&);
MeshData LoadGLTF(const std::wstring&);
TextureData LoadKTX2(const std::wstring&,bool);
TextureData DecodeTexture(std::span<const std::byte>,TextureUse);
TextureData LoadTexture(const std::wstring&,TextureUse);
SceneData LoadSceneGLTF(const std::wstring&);
void AppendAcousticGeometry(const SceneData&,std::vector<XMFLOAT3>&,std::vector<std::array<uint32_t,3>>&);
void SaveCookedScene(const SceneData&,const std::wstring&);
SceneData LoadCookedScene(const std::wstring&);
TextureData CookTexture(const TextureData&,const std::wstring& ktxPath={});
}
