#include "ClientAssets.h"
#include "ContentIds.h"
#include <utility>
namespace lab {
SceneData CreateClientAssetSet(){
    SceneData scene;scene.meshes=LoadGLTF(AssetPath(L"assets/primitives.gltf"));if(scene.meshes.ranges.size()!=uint32_t(Mesh::Count))Fail("primitive library");
    scene.meshes.deformations[uint32_t(Mesh::Sphere)]={{1,0,0,.07f},{27.36f,0,0,0}};scene.materials.resize(uint32_t(Material::Count));return scene;
}
void AppendClientScene(SceneData& a,const SceneData& b){
    uint32_t vertex=uint32_t(a.meshes.vertices.size()),index=uint32_t(a.meshes.indices.size()),remap=uint32_t(a.meshes.remap.size()),triangle=uint32_t(a.meshes.triangles.size()),meshlet=uint32_t(a.meshes.meshlets.size()),range=uint32_t(a.meshes.ranges.size()),morph=uint32_t(a.meshes.morphs.size()),material=uint32_t(a.materials.size()),texture=uint32_t(a.textures.size()),sampler=uint32_t(a.samplers.size()),node=uint32_t(a.nodes.size()),bone=uint32_t(a.bones.size()),weight=uint32_t(a.weights.size());
    auto& x=a.meshes;const auto& y=b.meshes;
    x.vertices.insert(x.vertices.end(),y.vertices.begin(),y.vertices.end());for(auto i:y.indices)x.indices.push_back(i+vertex);for(auto i:y.remap)x.remap.push_back(i+vertex);x.triangles.insert(x.triangles.end(),y.triangles.begin(),y.triangles.end());
    for(auto m:y.meshlets){m.vertexOffset+=remap;m.triangleOffset+=triangle;x.meshlets.push_back(m);}for(auto r:y.ranges){r.firstVertex+=vertex;r.firstIndex+=index;r.firstMeshlet+=meshlet;x.ranges.push_back(r);}x.deformations.insert(x.deformations.end(),y.deformations.begin(),y.deformations.end());
    for(auto m:y.motions){if(m.node!=Invalid)m.node+=node;if(m.boneCount)m.boneStart+=bone;if(m.morphCount)m.morphStart+=morph;if(m.weightStart||m.morphCount)m.weightStart+=weight;x.motions.push_back(m);}x.skin.insert(x.skin.end(),y.skin.begin(),y.skin.end());x.morphs.insert(x.morphs.end(),y.morphs.begin(),y.morphs.end());
    for(auto t:b.textures)a.textures.push_back(std::move(t));for(auto m:b.materials){for(auto* t:{&m.albedo,&m.normal,&m.metalRough,&m.emissive,&m.occlusion})if(*t!=Invalid)*t+=texture;for(auto& value:m.samplers)if(value!=Invalid)value+=sampler;a.materials.push_back(m);}for(auto value:b.samplers)a.samplers.push_back(value);
    for(auto n:b.nodes){if(n.parent!=Invalid)n.parent+=node;if(n.weightCount)n.weightStart+=weight;a.nodes.push_back(n);}for(auto n:b.nodeOrder)a.nodeOrder.push_back(n+node);for(auto binding:b.bones){binding.node+=node;a.bones.push_back(binding);}a.weights.insert(a.weights.end(),b.weights.begin(),b.weights.end());
    for(auto clip:b.animations){for(auto& track:clip.tracks)track.node+=node;a.animations.push_back(std::move(clip));}
    for(auto o:b.objects){o.mesh+=range;o.material+=material;a.objects.push_back(o);}for(auto l:b.lights){if(l.animationNode!=Invalid)l.animationNode+=node;a.lights.push_back(l);}if(a.lights.size()>MaxLights)Fail("scene light capacity");
}
}
