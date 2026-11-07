#include "Assets.h"
#include <ktx.h>
#include <type_traits>
#include <limits>
namespace lab {
namespace {
constexpr uint64_t Magic=0x33454e454353564cull;
struct Header {uint64_t magic{Magic};uint32_t version{3},vertexBytes{sizeof(Vertex)},materialBytes{sizeof(AssetMaterial)},reserved{};uint64_t bytes{},hash{};};
uint64_t Hash(std::span<const std::byte> data){uint64_t h=1469598103934665603ull;for(auto value:data){h^=std::to_integer<uint8_t>(value);h*=1099511628211ull;}return h;}
struct Writer {
    std::vector<std::byte> bytes;
    template<class T>void Pod(const T& value){static_assert(std::is_trivially_copyable_v<T>);auto* p=reinterpret_cast<const std::byte*>(&value);bytes.insert(bytes.end(),p,p+sizeof(T));}
    template<class T>void Vector(const std::vector<T>& values){static_assert(std::is_trivially_copyable_v<T>);Pod(uint64_t(values.size()));if(values.empty())return;auto* p=reinterpret_cast<const std::byte*>(values.data());bytes.insert(bytes.end(),p,p+values.size()*sizeof(T));}
};
struct Reader {
    std::span<const std::byte> bytes;size_t cursor{};
    template<class T>T Pod(){static_assert(std::is_trivially_copyable_v<T>);if(sizeof(T)>bytes.size()-cursor)Fail("cooked scene truncated");T value;memcpy(&value,bytes.data()+cursor,sizeof(T));cursor+=sizeof(T);return value;}
    template<class T>void Vector(std::vector<T>& values){static_assert(std::is_trivially_copyable_v<T>);uint64_t count=Pod<uint64_t>();if(count>(bytes.size()-cursor)/sizeof(T)||count>UINT32_MAX)Fail("cooked scene array size");values.resize(size_t(count));if(count)memcpy(values.data(),bytes.data()+cursor,size_t(count)*sizeof(T));cursor+=size_t(count)*sizeof(T);}
    uint32_t Count(uint32_t limit){auto count=Pod<uint32_t>();if(count>limit)Fail("cooked scene capacity");return count;}
};
template<class Stream>void Plain(Stream& stream,SceneData& scene){
    auto& m=scene.meshes;stream.Vector(m.vertices);stream.Vector(m.indices);stream.Vector(m.remap);stream.Vector(m.triangles);stream.Vector(m.meshlets);stream.Vector(m.ranges);stream.Vector(m.deformations);stream.Vector(m.skin);stream.Vector(m.morphs);stream.Vector(m.motions);
    stream.Vector(scene.materials);stream.Vector(scene.objects);stream.Vector(scene.lights);stream.Vector(scene.samplers);stream.Vector(scene.nodes);stream.Vector(scene.nodeOrder);stream.Vector(scene.bones);stream.Vector(scene.weights);
}
void Validate(const SceneData& scene){
    const auto& m=scene.meshes;
    if(scene.lights.size()>ConsoleSpec::MaxLights||scene.nodes.size()>ConsoleSpec::MaxObjects||scene.objects.size()>ConsoleSpec::MaxObjects||scene.samplers.size()>ConsoleSpec::MaxSamplers||scene.materials.size()>ConsoleSpec::MaxMaterials||scene.textures.size()>ConsoleSpec::MaxTextures)Fail("cooked scene capacity");
    if(m.ranges.size()!=m.motions.size()||m.ranges.size()!=m.deformations.size()||m.vertices.size()!=m.skin.size())Fail("cooked geometry streams");
    auto fits=[](uint64_t start,uint64_t count,size_t size){return start<=size&&count<=size-start;};
    for(const auto& object:scene.objects)if(object.mesh>=m.ranges.size()||object.material>=scene.materials.size())Fail("cooked object assets");
    for(auto index:m.indices)if(index>=m.vertices.size())Fail("cooked vertex index");
    for(auto index:m.remap)if(index>=m.vertices.size())Fail("cooked meshlet vertex");
    for(const auto& meshlet:m.meshlets)if(meshlet.vertexCount>64||meshlet.triangleCount>124||!fits(meshlet.vertexOffset,meshlet.vertexCount,m.remap.size())||!fits(meshlet.triangleOffset,meshlet.triangleCount,m.triangles.size()))Fail("cooked meshlet span");
    for(size_t i=0;i<m.ranges.size();i++){const auto& r=m.ranges[i];const auto& motion=m.motions[i];if(!r.vertexCount||r.indexCount%3||!fits(r.firstVertex,r.vertexCount,m.vertices.size())||!fits(r.firstIndex,r.indexCount,m.indices.size())||!fits(r.firstMeshlet,r.meshletCount,m.meshlets.size()))Fail("cooked mesh span");if(motion.node!=Invalid&&(motion.node>=scene.nodes.size()||!fits(motion.boneStart,motion.boneCount,scene.bones.size())||!fits(motion.morphStart,uint64_t(motion.morphCount)*r.vertexCount,m.morphs.size())||!fits(motion.weightStart,motion.morphCount,scene.weights.size())))Fail("cooked deformation span");}
    for(const auto& material:scene.materials){for(auto texture:{material.albedo,material.normal,material.metalRough,material.emissive,material.occlusion})if(texture!=Invalid&&texture>=scene.textures.size())Fail("cooked material texture");for(auto sampler:material.samplers)if(sampler!=Invalid&&sampler>=scene.samplers.size())Fail("cooked sampler");}
    for(const auto& texture:scene.textures)if(!texture.width||!texture.height||texture.width>ConsoleSpec::MaxTextureEdge||texture.height>ConsoleSpec::MaxTextureEdge||!texture.mips||texture.mips>15||texture.offsets.size()!=texture.mips)Fail("cooked texture size");
    if(scene.nodeOrder.size()!=scene.nodes.size())Fail("cooked hierarchy size");std::vector<bool> visited(scene.nodes.size());
    for(uint32_t index:scene.nodeOrder){if(index>=scene.nodes.size()||visited[index])Fail("cooked hierarchy index");const auto& node=scene.nodes[index];if(node.parent!=Invalid&&(node.parent>=visited.size()||!visited[node.parent]))Fail("cooked hierarchy order");if(!fits(node.weightStart,node.weightCount,scene.weights.size()))Fail("cooked node weights");visited[index]=true;}
    for(const auto& bone:scene.bones)if(bone.node>=scene.nodes.size())Fail("cooked bone node");
    for(const auto& clip:scene.animations)for(const auto& track:clip.tracks){uint32_t multiplier=track.interpolation==Interpolation::Cubic?3:1;if(track.node>=scene.nodes.size()||track.times.empty()||!track.components||track.values.size()!=track.times.size()*track.components*multiplier)Fail("cooked animation channel");if(track.path==AnimationPath::Weights){if(track.components!=scene.nodes[track.node].weightCount)Fail("cooked animated weights");}else if(track.components!=(track.path==AnimationPath::Rotation?4u:3u))Fail("cooked animation components");for(size_t i=0;i<track.times.size();i++)if(!std::isfinite(track.times[i])||(i&&track.times[i]<=track.times[i-1]))Fail("cooked animation times");}
}
}
void SaveCookedScene(const SceneData& scene,const std::wstring& path){
    Validate(scene);Writer output;Plain(output,const_cast<SceneData&>(scene));output.Pod(uint32_t(scene.textures.size()));
    for(const auto& texture:scene.textures){output.Pod(texture.width);output.Pod(texture.height);output.Pod(texture.mips);output.Pod(uint32_t(texture.normal));output.Pod(texture.format);output.Vector(texture.bytes);output.Vector(texture.offsets);}
    output.Pod(uint32_t(scene.animations.size()));for(const auto& clip:scene.animations){output.Pod(clip.duration);output.Pod(uint32_t(clip.tracks.size()));for(const auto& track:clip.tracks){output.Pod(track.node);output.Pod(track.components);output.Pod(track.path);output.Pod(track.interpolation);output.Vector(track.times);output.Vector(track.values);}}
    Header header;header.bytes=output.bytes.size();header.hash=Hash(output.bytes);FILE* file=nullptr;auto temporary=path+L".tmp";_wfopen_s(&file,temporary.c_str(),L"wb");if(!file)Fail("cooked scene output");bool ok=fwrite(&header,1,sizeof(header),file)==sizeof(header)&&fwrite(output.bytes.data(),1,output.bytes.size(),file)==output.bytes.size();ok=fclose(file)==0&&ok;if(!ok||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))Fail("cooked scene write");
}
SceneData LoadCookedScene(const std::wstring& path){
    auto bytes=ReadBytes(path);Reader input{bytes};auto header=input.Pod<Header>();if(header.magic!=Magic||header.version!=3||header.vertexBytes!=sizeof(Vertex)||header.materialBytes!=sizeof(AssetMaterial)||header.bytes!=bytes.size()-sizeof(Header)||header.hash!=Hash(std::span<const std::byte>(bytes).subspan(sizeof(Header))))Fail("cooked scene ABI or checksum");SceneData scene;Plain(input,scene);scene.textures.resize(input.Count(ConsoleSpec::MaxTextures));
    for(auto& texture:scene.textures){texture.width=input.Pod<uint32_t>();texture.height=input.Pod<uint32_t>();texture.mips=input.Pod<uint32_t>();texture.normal=input.Pod<uint32_t>()!=0;texture.format=input.Pod<DXGI_FORMAT>();input.Vector(texture.bytes);input.Vector(texture.offsets);}
    scene.animations.resize(input.Count(4096));for(auto& clip:scene.animations){clip.duration=input.Pod<float>();clip.tracks.resize(input.Count(32768));for(auto& track:clip.tracks){track.node=input.Pod<uint32_t>();track.components=input.Pod<uint32_t>();track.path=input.Pod<AnimationPath>();track.interpolation=input.Pod<Interpolation>();input.Vector(track.times);input.Vector(track.values);}}
    if(input.cursor!=bytes.size())Fail("cooked scene trailing data");Validate(scene);return scene;
}
TextureData CookTexture(const TextureData& input,const std::wstring& path){
    if(input.format!=DXGI_FORMAT_R8G8B8A8_UNORM&&input.format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return input;
    ktxTextureCreateInfo info{};info.vkFormat=input.format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?43:37;info.baseWidth=input.width;info.baseHeight=input.height;info.baseDepth=1;info.numDimensions=2;info.numLevels=input.mips;info.numLayers=1;info.numFaces=1;
    ktxTexture2* texture=nullptr;if(ktxTexture2_Create(&info,KTX_TEXTURE_CREATE_ALLOC_STORAGE,&texture)!=KTX_SUCCESS)Fail("cooked texture create");auto* base=reinterpret_cast<ktxTexture*>(texture);
    for(uint32_t mip=0;mip<input.mips;mip++){size_t bytes=size_t(std::max(1u,input.width>>mip))*std::max(1u,input.height>>mip)*4;if(input.offsets[mip]>input.bytes.size()||bytes>input.bytes.size()-input.offsets[mip])Fail("cooked source mip");if(ktxTexture_SetImageFromMemory(base,mip,0,0,reinterpret_cast<const ktx_uint8_t*>(input.bytes.data()+input.offsets[mip]),bytes)!=KTX_SUCCESS)Fail("cooked texture mip");}
    ktxBasisParams parameters{};parameters.structSize=sizeof(parameters);parameters.uastc=KTX_TRUE;parameters.normalMap=input.normal;parameters.uastcFlags=2;parameters.threadCount=std::max(1u,std::thread::hardware_concurrency()/2);
    if(ktxTexture2_CompressBasisEx(texture,&parameters)!=KTX_SUCCESS)Fail("cooked BasisU compression");
    if(!path.empty()){FILE* file=nullptr;_wfopen_s(&file,path.c_str(),L"wb");if(!file)Fail("KTX2 output file");auto result=ktxTexture_WriteToStdioStream(base,file);bool closed=fclose(file)==0;if(result!=KTX_SUCCESS||!closed)Fail("KTX2 output");}
    if(ktxTexture2_TranscodeBasis(texture,input.normal?KTX_TTF_BC5_RG:KTX_TTF_BC7_RGBA,0)!=KTX_SUCCESS)Fail("cooked BC transcode");
    TextureData result;result.width=input.width;result.height=input.height;result.mips=input.mips;result.normal=input.normal;result.format=input.normal?DXGI_FORMAT_BC5_UNORM:input.format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?DXGI_FORMAT_BC7_UNORM_SRGB:DXGI_FORMAT_BC7_UNORM;result.bytes.resize(ktxTexture_GetDataSize(base));memcpy(result.bytes.data(),ktxTexture_GetData(base),result.bytes.size());for(uint32_t mip=0;mip<result.mips;mip++){ktx_size_t offset=0;if(ktxTexture_GetImageOffset(base,mip,0,0,&offset)!=KTX_SUCCESS)Fail("cooked BC mip");result.offsets.push_back(offset);}ktxTexture_Destroy(base);return result;
}
}
