#include "Animation.h"
namespace lab {
void SceneAnimation::Initialize(const SceneData& scene) {
    baseNodes_=scene.nodes;nodes_=baseNodes_;nodeOrder_=scene.nodeOrder;bindings_=scene.bones;
    baseWeights_=scene.weights;weights_=baseWeights_;animations_=scene.animations;meshMotions_=scene.meshes.motions;
    if(meshMotions_.size()!=scene.meshes.ranges.size())Fail("animation mesh motion table");
    world_.resize(nodes_.size());motions_.resize(scene.meshes.ranges.size());bones_.resize(bindings_.size());Evaluate(0);
}
XMMATRIX SceneAnimation::NodeMatrix(uint32_t node)const {return node<world_.size()?XMLoadFloat4x4(&world_[node]):XMMatrixIdentity();}
void SceneAnimation::Evaluate(double time,uint32_t clipIndex) {
    if(nodes_.size()!=baseNodes_.size()||weights_.size()!=baseWeights_.size())Fail("animation storage shape");
    std::copy(baseNodes_.begin(),baseNodes_.end(),nodes_.begin());std::copy(baseWeights_.begin(),baseWeights_.end(),weights_.begin());
    if(clipIndex<animations_.size()) {
        const auto& clip=animations_[clipIndex];float t=clip.duration>0?float(std::fmod(std::max(0.0,time),double(clip.duration))):0;
        for(const auto& track:clip.tracks){
            if(track.times.empty()||track.node>=nodes_.size())continue;
            auto upper=std::upper_bound(track.times.begin(),track.times.end(),t);
            size_t b=std::min(size_t(upper-track.times.begin()),track.times.size()-1),a=b?b-1:0;
            if(t>=track.times.back())a=b;float dt=track.times[b]-track.times[a];float u=dt>0?std::clamp((t-track.times[a])/dt,0.f,1.f):0;
            auto& node=nodes_[track.node];float value[4]{};
            const uint32_t stride=track.components*(track.interpolation==Interpolation::Cubic?3u:1u);
            for(uint32_t c=0;c<track.components;c++) {
                float x=0;
                if(track.interpolation==Interpolation::Cubic){float u2=u*u,u3=u2*u;x=(2*u3-3*u2+1)*track.values[a*stride+track.components+c]+(u3-2*u2+u)*dt*track.values[a*stride+2*track.components+c]+(-2*u3+3*u2)*track.values[b*stride+track.components+c]+(u3-u2)*dt*track.values[b*stride+c];}
                else {float x0=track.values[a*stride+c],x1=track.values[b*stride+c];x=track.interpolation==Interpolation::Step?x0:x0+(x1-x0)*u;}
                if(track.path==AnimationPath::Weights){uint64_t index=uint64_t(node.weightStart)+c;if(index>=weights_.size())Fail("animation weight range");weights_[size_t(index)]=x;}else value[c]=x;
            }
            if(track.path==AnimationPath::Translation)node.pose.p={value[0],value[1],value[2]};
            if(track.path==AnimationPath::Scale)node.scale={value[0],value[1],value[2]};
            if(track.path==AnimationPath::Rotation){
                XMVECTOR q=XMVectorSet(value[0],value[1],value[2],value[3]);
                if(track.interpolation==Interpolation::Linear){auto* p=track.values.data();q=XMQuaternionSlerp(XMVectorSet(p[a*4],p[a*4+1],p[a*4+2],p[a*4+3]),XMVectorSet(p[b*4],p[b*4+1],p[b*4+2],p[b*4+3]),u);}
                XMStoreFloat4(&node.pose.q,XMQuaternionNormalize(q));
            }
        }
    }
    for(uint32_t index:nodeOrder_){if(index>=nodes_.size())Fail("animation node order");const auto& node=nodes_[index];XMMATRIX local=node.matrixOnly?XMLoadFloat4x4(&node.matrix):XMMatrixScaling(node.scale.x,node.scale.y,node.scale.z)*Matrix(node.pose);if(node.parent!=Invalid){if(node.parent>=world_.size())Fail("animation parent node");local=local*XMLoadFloat4x4(&world_[node.parent]);}XMStoreFloat4x4(&world_[index],local);}
    auto mirror=XMMatrixScaling(1,1,-1);
    for(uint32_t i=0;i<bones_.size();i++){if(bindings_[i].node>=world_.size())Fail("animation bone node");auto matrix=mirror*XMLoadFloat4x4(&bindings_[i].inverseBind)*XMLoadFloat4x4(&world_[bindings_[i].node])*mirror;XMStoreFloat4x4(&bones_[i].transform,matrix);XMStoreFloat4x4(&bones_[i].normalTransform,XMMatrixTranspose(XMMatrixInverse(nullptr,matrix)));}
    for(uint32_t i=0;i<motions_.size();i++){const auto& input=meshMotions_[i];auto& out=motions_[i];if(input.node!=Invalid&&input.node>=world_.size())Fail("animation motion node");auto matrix=input.node==Invalid?XMMatrixIdentity():mirror*XMLoadFloat4x4(&world_[input.node])*mirror;XMStoreFloat4x4(&out.transform,matrix);XMStoreFloat4x4(&out.normalTransform,XMMatrixTranspose(XMMatrixInverse(nullptr,matrix)));out.binding={input.node,input.boneStart,input.boneCount,input.morphStart};out.morph={input.morphCount,input.weightStart,0,0};}
}
}
