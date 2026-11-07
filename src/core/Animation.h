#pragma once
#include "Assets.h"
namespace lab {
struct GpuMotion {XMFLOAT4X4 transform{},normalTransform{};XMUINT4 binding{},morph{};};
struct GpuBone {XMFLOAT4X4 transform{},normalTransform{};};
class SceneAnimation {
    // Own the animation inputs used after Initialize(). Graphics::ReplaceScene()
    // may be given a temporary composed SceneData, so retaining its address would
    // leave Evaluate() with a dangling pointer as soon as ReplaceScene returns.
    std::vector<SceneNode> baseNodes_,nodes_;
    std::vector<uint32_t> nodeOrder_;
    std::vector<BoneBinding> bindings_;
    std::vector<float> baseWeights_,weights_;
    std::vector<AnimationClip> animations_;
    std::vector<MeshMotion> meshMotions_;
    std::vector<XMFLOAT4X4> world_;
    std::vector<GpuMotion> motions_;
    std::vector<GpuBone> bones_;
public:
    void Initialize(const SceneData&);
    void Evaluate(double time,uint32_t clip=0);
    const auto& Motions()const{return motions_;}
    const auto& Bones()const{return bones_;}
    const auto& Weights()const{return weights_;}
    XMMATRIX NodeMatrix(uint32_t node)const;
};
}
