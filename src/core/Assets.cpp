#include "Assets.h"
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <ktx.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <limits>
#include <cfloat>
namespace lab {
using Microsoft::WRL::ComPtr;
std::wstring AssetPath(const wchar_t* name) {
    wchar_t path[32768]; DWORD n=GetModuleFileNameW(nullptr,path,32768);
    if(!n||n>=32768) Fail("executable path");
    std::wstring p(path,n); p.resize(p.find_last_of(L"\\/")+1); p+=name; return p;
}
std::vector<std::byte> ReadBytes(const std::wstring& path) {
    FILE* f=nullptr; _wfopen_s(&f,path.c_str(),L"rb"); if(!f) Fail("Asset file missing");
    _fseeki64(f,0,SEEK_END); auto size=_ftelli64(f); _fseeki64(f,0,SEEK_SET);
    if(size<0 || uint64_t(size)>2ull*1024*1024*1024) Fail("Asset size");
    std::vector<std::byte> b(static_cast<size_t>(size));
    if(fread(b.data(),1,b.size(),f)!=b.size()) Fail("Asset read"); fclose(f); return b;
}
static std::string Utf8(const std::wstring& s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.c_str(),-1,nullptr,0,nullptr,nullptr);
    if(!n) Fail("UTF8 path"); std::string a(size_t(n),0);
    WideCharToMultiByte(CP_UTF8,0,s.c_str(),-1,a.data(),n,nullptr,nullptr); a.pop_back(); return a;
}
static std::wstring Wide(const char* s) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,nullptr,0);
    if(!n) Fail("glTF URI encoding"); std::wstring result(size_t(n),0);
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,result.data(),n); result.pop_back(); return result;
}
struct Document {
    cgltf_data* data{};
    explicit Document(const std::wstring& path) {
        cgltf_options options{}; auto name=Utf8(path);
        if(cgltf_parse_file(&options,name.c_str(),&data)!=cgltf_result_success) Fail("glTF 2.0 parse");
        if(cgltf_load_buffers(&options,data,name.c_str())!=cgltf_result_success) Fail("glTF buffers");
        if(cgltf_validate(data)!=cgltf_result_success) Fail("glTF structure");
    }
    ~Document(){ cgltf_free(data); }
};
static void FinishRange(MeshData& mesh,MeshRange& range) {
    range.vertexCount=uint32_t(mesh.vertices.size())-range.firstVertex;
    range.indexCount=uint32_t(mesh.indices.size())-range.firstIndex;
    if(!range.vertexCount || !range.indexCount) Fail("empty glTF primitive");
    XMFLOAT3 lo{FLT_MAX,FLT_MAX,FLT_MAX},hi{-FLT_MAX,-FLT_MAX,-FLT_MAX};
    for(uint32_t i=range.firstVertex;i<range.firstVertex+range.vertexCount;i++) {
        const auto& p=mesh.vertices[i].position;
        lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
        hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
    }
    auto center=Scale(Add(lo,hi),.5f); float radius=0;
    for(uint32_t i=range.firstVertex;i<range.firstVertex+range.vertexCount;i++) radius=std::max(radius,Length(Sub(mesh.vertices[i].position,center)));
    range.bounds={center.x,center.y,center.z,radius};
    std::array<uint32_t,64> remap{}; uint32_t vertices=0,triangles=0;
    Meshlet current{uint32_t(mesh.remap.size()),uint32_t(mesh.triangles.size()),0,0};
    auto flush=[&] {
        if(!triangles) return;
        current.vertexCount=vertices; current.triangleCount=triangles; mesh.meshlets.push_back(current);
        mesh.remap.insert(mesh.remap.end(),remap.begin(),remap.begin()+vertices); vertices=triangles=0;
        current={uint32_t(mesh.remap.size()),uint32_t(mesh.triangles.size()),0,0};
    };
    for(uint32_t t=range.firstIndex;t<range.firstIndex+range.indexCount;t+=3) {
        uint32_t needed=0;
        for(uint32_t j=0;j<3;j++) if(std::find(remap.begin(),remap.begin()+vertices,mesh.indices[t+j])==remap.begin()+vertices) needed++;
        if(vertices+needed>64 || triangles==124) flush();
        std::array<uint32_t,3> local{};
        for(uint32_t j=0;j<3;j++) {
            uint32_t index=mesh.indices[t+j]; auto found=std::find(remap.begin(),remap.begin()+vertices,index);
            if(found==remap.begin()+vertices) remap[vertices++]=index;
            local[j]=uint32_t(found-remap.begin());
        }
        mesh.triangles.push_back(local); triangles++;
    }
    flush(); range.meshletCount=uint32_t(mesh.meshlets.size())-range.firstMeshlet;
}
static std::vector<float> Floats(const cgltf_accessor* accessor,uint32_t components) {
    if(!accessor)return {};
    if(cgltf_num_components(accessor->type)!=components||accessor->count>SIZE_MAX/components)Fail("glTF accessor shape");
    std::vector<float> values(accessor->count*components);
    if(cgltf_accessor_unpack_floats(accessor,values.data(),values.size())!=values.size())Fail("glTF accessor decode");
    for(float value:values)if(!std::isfinite(value))Fail("glTF nonfinite accessor");return values;
}
static void Primitive(MeshData& result,const cgltf_primitive& p,FXMMATRIX transform) {
    if(p.has_draco_mesh_compression) Fail("Draco glTF requires decompression before import");
    const cgltf_accessor *position=nullptr,*normal=nullptr,*uv=nullptr,*uv1=nullptr;
    for(size_t i=0;i<p.attributes_count;i++) {
        auto& a=p.attributes[i];
        if(a.type==cgltf_attribute_type_position) position=a.data;
        else if(a.type==cgltf_attribute_type_normal) normal=a.data;
        else if(a.type==cgltf_attribute_type_texcoord && a.index==0) uv=a.data;
        else if(a.type==cgltf_attribute_type_texcoord && a.index==1) uv1=a.data;
    }
    if(!position || position->count>UINT32_MAX-result.vertices.size()) Fail("glTF positions");
    auto positions=Floats(position,3),normals=Floats(normal,3),uvs=Floats(uv,2),uvs1=Floats(uv1,2);
    for(auto* a:{normal,uv,uv1})if(a&&a->count!=position->count)Fail("glTF vertex attribute count");
    uint32_t base=uint32_t(result.vertices.size());
    auto normalTransform=XMMatrixTranspose(XMMatrixInverse(nullptr,transform));
    for(size_t i=0;i<position->count;i++) {
        Vertex v{};
        memcpy(&v.position,positions.data()+i*3,sizeof(XMFLOAT3));
        if(normal)memcpy(&v.normal,normals.data()+i*3,sizeof(XMFLOAT3));
        if(uv)memcpy(&v.uv,uvs.data()+i*2,sizeof(XMFLOAT2));
        if(uv1)memcpy(&v.uv1,uvs1.data()+i*2,sizeof(XMFLOAT2));
        XMStoreFloat3(&v.position,XMVector3TransformCoord(XMLoadFloat3(&v.position),transform));
        if(normal) XMStoreFloat3(&v.normal,XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&v.normal),normalTransform)));
        if(!std::isfinite(v.position.x)||!std::isfinite(v.position.y)||!std::isfinite(v.position.z)) Fail("glTF nonfinite vertex");
        result.vertices.push_back(v);
    }
    size_t count=p.indices?p.indices->count:position->count;
    auto index=[&](size_t i) { size_t v=p.indices?cgltf_accessor_read_index(p.indices,i):i; if(v>=position->count) Fail("glTF index"); return base+uint32_t(v); };
    bool flipped=XMVectorGetX(XMMatrixDeterminant(transform))<0;
    auto triangle=[&](size_t a,size_t b,size_t c) {
        uint32_t ia=index(a),ib=index(b),ic=index(c); if(flipped) std::swap(ib,ic);
        if(ia==ib||ib==ic||ia==ic) return;
        result.indices.insert(result.indices.end(),{ia,ib,ic});
        if(!normal) {
            auto& va=result.vertices[ia]; auto& vb=result.vertices[ib]; auto& vc=result.vertices[ic];
            XMFLOAT3 n; XMStoreFloat3(&n,XMVector3Cross(XMVectorSubtract(XMLoadFloat3(&vb.position),XMLoadFloat3(&va.position)),XMVectorSubtract(XMLoadFloat3(&vc.position),XMLoadFloat3(&va.position))));
            va.normal=Add(va.normal,n); vb.normal=Add(vb.normal,n); vc.normal=Add(vc.normal,n);
        }
    };
    if(p.type==cgltf_primitive_type_triangles) { if(count%3) Fail("glTF triangle count"); for(size_t i=0;i<count;i+=3) triangle(i,i+1,i+2); }
    else if(p.type==cgltf_primitive_type_triangle_strip) { for(size_t i=2;i<count;i++) triangle(i-2+(i&1),i-1-(i&1),i); }
    else if(p.type==cgltf_primitive_type_triangle_fan) { for(size_t i=2;i<count;i++) triangle(0,i-1,i); }
    else Fail("glTF primitive must contain triangles");
    if(!normal) for(uint32_t i=base;i<result.vertices.size();i++) result.vertices[i].normal=Unit(result.vertices[i].normal);
}
MeshData LoadGLTF(const std::wstring& path) {
    Document doc(path); MeshData result;
    for(size_t m=0;m<doc.data->meshes_count;m++) {
        const auto& mesh=doc.data->meshes[m]; MeshRange r{};
        r.firstVertex=uint32_t(result.vertices.size()); r.firstIndex=uint32_t(result.indices.size()); r.firstMeshlet=uint32_t(result.meshlets.size());
        for(size_t p=0;p<mesh.primitives_count;p++) Primitive(result,mesh.primitives[p],XMMatrixIdentity());
        FinishRange(result,r); result.ranges.push_back(r); result.deformations.push_back({});result.motions.push_back({});
    }
    result.skin.resize(result.vertices.size());return result;
}
static float ToLinear(float v) { return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f); }
static float ToSRGB(float v) { return v<=.0031308f?v*12.92f:1.055f*std::pow(v,1.f/2.4f)-.055f; }
static void Mipmaps(TextureData& result,std::vector<uint8_t> pixels,TextureUse use) {
    uint32_t w=result.width,h=result.height;
    for(;;) {
        result.offsets.push_back(result.bytes.size()); ++result.mips;
        auto* p=reinterpret_cast<const std::byte*>(pixels.data()); result.bytes.insert(result.bytes.end(),p,p+pixels.size());
        if(w==1&&h==1) break;
        uint32_t nw=std::max(1u,w/2),nh=std::max(1u,h/2); std::vector<uint8_t> next(size_t(nw)*nh*4);
        for(uint32_t y=0;y<nh;y++) for(uint32_t x=0;x<nw;x++) {
            float sum[4]{}; uint32_t samples=0;
            for(uint32_t sy=y*h/nh;sy<(y+1)*h/nh;sy++) for(uint32_t sx=x*w/nw;sx<(x+1)*w/nw;sx++) {
                for(uint32_t c=0;c<4;c++) { float v=pixels[(size_t(sy)*w+sx)*4+c]/255.f; sum[c]+=use==TextureUse::Color&&c<3?ToLinear(v):v; } ++samples;
            }
            for(float& c:sum) c/=float(samples);
            if(use==TextureUse::Normal) { auto n=Unit({sum[0]*2-1,sum[1]*2-1,sum[2]*2-1}); sum[0]=n.x*.5f+.5f;sum[1]=n.y*.5f+.5f;sum[2]=n.z*.5f+.5f; }
            for(uint32_t c=0;c<4;c++) { float v=use==TextureUse::Color&&c<3?ToSRGB(sum[c]):sum[c]; next[(size_t(y)*nw+x)*4+c]=uint8_t(std::clamp(v,0.f,1.f)*255+.5f); }
        }
        pixels=std::move(next);w=nw;h=nh;
    }
}
static void MipmapsR16(TextureData& result,std::vector<uint16_t> pixels) {
    result.format=DXGI_FORMAT_R16_UNORM;uint32_t w=result.width,h=result.height;
    for(;;) {
        result.offsets.push_back(result.bytes.size());++result.mips;
        const auto* p=reinterpret_cast<const std::byte*>(pixels.data());result.bytes.insert(result.bytes.end(),p,p+pixels.size()*sizeof(uint16_t));
        if(w==1&&h==1)break;
        uint32_t nw=std::max(1u,w/2),nh=std::max(1u,h/2);std::vector<uint16_t> next(size_t(nw)*nh);
        for(uint32_t y=0;y<nh;++y)for(uint32_t x=0;x<nw;++x){
            uint64_t sum=0;uint32_t samples=0;
            for(uint32_t sy=y*h/nh;sy<(y+1)*h/nh;++sy)for(uint32_t sx=x*w/nw;sx<(x+1)*w/nw;++sx){sum+=pixels[size_t(sy)*w+sx];++samples;}
            next[size_t(y)*nw+x]=uint16_t((sum+samples/2)/samples);
        }
        pixels=std::move(next);w=nw;h=nh;
    }
}
TextureData DecodeTexture(std::span<const std::byte> bytes,TextureUse use) {
    TextureData result; result.normal=use==TextureUse::Normal;
    static constexpr uint8_t magic[]={0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,0x0d,0x0a,0x1a,0x0a};
    if(bytes.size()>=sizeof(magic)&&!memcmp(bytes.data(),magic,sizeof(magic))) {
        ktxTexture2* t=nullptr;
        if(ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(bytes.data()),bytes.size(),KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,&t)!=KTX_SUCCESS) Fail("KTX2 texture");
        if(t->numDimensions!=2||t->numLayers>1||t->numFaces!=1) Fail("KTX2 must be a 2D material texture");
        if(ktxTexture2_NeedsTranscoding(t)) {
            if(ktxTexture2_TranscodeBasis(t,result.normal?KTX_TTF_BC5_RG:KTX_TTF_BC7_RGBA,0)!=KTX_SUCCESS) Fail("BasisU transcode");
            result.format=result.normal?DXGI_FORMAT_BC5_UNORM:use==TextureUse::Color?DXGI_FORMAT_BC7_UNORM_SRGB:DXGI_FORMAT_BC7_UNORM;
        } else {
            switch(t->vkFormat) {
                case 37:case 43:result.format=use==TextureUse::Color?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;break;
                case 70:result.format=DXGI_FORMAT_R16_UNORM;break;
                case 141:result.format=DXGI_FORMAT_BC5_UNORM;break;
                case 145:case 146:result.format=use==TextureUse::Color?DXGI_FORMAT_BC7_UNORM_SRGB:DXGI_FORMAT_BC7_UNORM;break;
                default:Fail("Unsupported KTX2 format; cook to BasisU, BC7, BC5 or RGBA8");
            }
        }
        result.width=t->baseWidth; result.height=t->baseHeight; result.mips=t->numLevels;
        auto* base=reinterpret_cast<ktxTexture*>(t);auto size=ktxTexture_GetDataSize(base); result.bytes.resize(size);memcpy(result.bytes.data(),ktxTexture_GetData(base),size);
        for(uint32_t i=0;i<result.mips;i++){ktx_size_t offset=0;if(ktxTexture_GetImageOffset(base,i,0,0,&offset)!=KTX_SUCCESS)Fail("KTX2 mip");result.offsets.push_back(offset);}
        ktxTexture_Destroy(base); return result;
    }
    if(bytes.size()>MAXDWORD) Fail("WIC texture size");
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory");
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream),"WIC stream");Hr(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),DWORD(bytes.size())),"WIC bytes");
    ComPtr<IWICBitmapDecoder> decoder;Hr(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder),"PNG/JPEG decode");
    ComPtr<IWICBitmapFrameDecode> frame;Hr(decoder->GetFrame(0,&frame),"image frame");Hr(frame->GetSize(&result.width,&result.height),"image dimensions");
    if(!result.width||!result.height||result.width>16384||result.height>16384) Fail("texture dimensions");
    ComPtr<IWICFormatConverter> converter;Hr(factory->CreateFormatConverter(&converter),"image conversion");
    const GUID& target=use==TextureUse::Height?GUID_WICPixelFormat16bppGray:GUID_WICPixelFormat32bppRGBA;
    Hr(converter->Initialize(frame.Get(),target,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"texture conversion");
    if(use==TextureUse::Height){
        std::vector<uint16_t> pixels(size_t(result.width)*result.height);Hr(converter->CopyPixels(nullptr,result.width*2,UINT(pixels.size()*2),reinterpret_cast<BYTE*>(pixels.data())),"height pixels");MipmapsR16(result,std::move(pixels));
    }else{
        std::vector<uint8_t> pixels(size_t(result.width)*result.height*4);Hr(converter->CopyPixels(nullptr,result.width*4,UINT(pixels.size()),pixels.data()),"image pixels");
        result.format=use==TextureUse::Color?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;Mipmaps(result,std::move(pixels),use);
    }
    return result;
}
TextureData LoadTexture(const std::wstring& path,TextureUse use){return DecodeTexture(ReadBytes(path),use);}
TextureData LoadKTX2(const std::wstring& path,bool normal){return LoadTexture(path,normal?TextureUse::Normal:TextureUse::Color);}
static std::vector<std::byte> ImageBytes(const cgltf_image& image,const std::filesystem::path& folder) {
    if(image.buffer_view) { auto& v=*image.buffer_view;auto* p=static_cast<const std::byte*>(v.buffer->data)+v.offset;return {p,p+v.size}; }
    if(!image.uri) Fail("glTF image URI");
    std::string uri=image.uri;
    if(uri.rfind("data:",0)==0) {
        auto comma=uri.find(','); if(comma==std::string::npos||uri.substr(0,comma).find(";base64")==std::string::npos) Fail("glTF data URI");
        std::vector<std::byte> data; uint32_t bits=0;int count=0;
        for(size_t i=comma+1;i<uri.size();i++) {
            char c=uri[i]; if(c=='=') break; int v=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='+'?62:c=='/'?63:-1;
            if(v<0) Fail("glTF base64"); bits=(bits<<6)|uint32_t(v);count+=6;if(count>=8){count-=8;data.push_back(std::byte((bits>>count)&255));}
        }
        return data;
    }
    if(uri.find("://")!=std::string::npos) Fail("glTF remote image must be downloaded locally");
    auto hex=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
    std::string decoded;for(size_t i=0;i<uri.size();i++){if(uri[i]=='%'&&i+2<uri.size()){int a=hex(uri[i+1]),b=hex(uri[i+2]);if(a<0||b<0)Fail("glTF URI escape");decoded.push_back(char(a*16+b));i+=2;}else decoded.push_back(uri[i]);}
    return ReadBytes((folder/Wide(decoded.c_str())).wstring());
}
SceneData LoadSceneGLTF(const std::wstring& path) {
    Document doc(path);auto* data=doc.data;SceneData result;auto folder=std::filesystem::path(path).parent_path();
    if(data->nodes_count>8192||data->materials_count>4096||data->samplers_count>1021)Fail("glTF scene capacity");
    for(size_t i=0;i<data->samplers_count;i++){const auto& sampler=data->samplers[i];result.samplers.push_back({uint32_t(sampler.min_filter?sampler.min_filter:9987),uint32_t(sampler.mag_filter?sampler.mag_filter:9729),uint32_t(sampler.wrap_s),uint32_t(sampler.wrap_t)});}
    struct CachedImage {const cgltf_image* image;TextureUse use;uint32_t index;};std::vector<CachedImage> loaded;
    auto texture=[&](const cgltf_texture_view& view,TextureUse use,AssetMaterial& material,uint32_t slot)->uint32_t {
        if(!view.texture)return Invalid;
        int coordinate=view.has_transform&&view.transform.has_texcoord?view.transform.texcoord:view.texcoord;
        if(coordinate<0||coordinate>1)Fail("glTF material requires TEXCOORD_0 or TEXCOORD_1");
        auto& mapping=material.mapping[slot];mapping.rotationSet.z=float(coordinate);
        if(view.has_transform){mapping.offsetScale={view.transform.offset[0],view.transform.offset[1],view.transform.scale[0],view.transform.scale[1]};mapping.rotationSet.x=std::cos(view.transform.rotation);mapping.rotationSet.y=std::sin(view.transform.rotation);}
        if(view.texture->sampler)material.samplers[slot]=uint32_t(view.texture->sampler-data->samplers);
        const auto* image=view.texture->has_basisu?view.texture->basisu_image:view.texture->image;if(!image)Fail("glTF image source");
        for(const auto& item:loaded)if(item.image==image&&item.use==use)return item.index;
        uint32_t index=uint32_t(result.textures.size());result.textures.push_back(DecodeTexture(ImageBytes(*image,folder),use));loaded.push_back({image,use,index});return index;
    };
    for(size_t i=0;i<data->materials_count;i++){
        const auto& source=data->materials[i];const auto& p=source.pbr_metallic_roughness;AssetMaterial material;
        material.colorRoughness={p.base_color_factor[0],p.base_color_factor[1],p.base_color_factor[2],p.roughness_factor};
        float strength=source.has_emissive_strength?source.emissive_strength.emissive_strength:1.f;
        material.emissionMetallic={source.emissive_factor[0]*strength,source.emissive_factor[1]*strength,source.emissive_factor[2]*strength,p.metallic_factor};
        material.albedo=texture(p.base_color_texture,TextureUse::Color,material,0);material.normal=texture(source.normal_texture,TextureUse::Normal,material,1);material.metalRough=texture(p.metallic_roughness_texture,TextureUse::Linear,material,2);material.emissive=texture(source.emissive_texture,TextureUse::Color,material,3);material.occlusion=texture(source.occlusion_texture,TextureUse::Linear,material,4);
        material.alpha=p.base_color_factor[3];material.alphaMode=source.alpha_mode==cgltf_alpha_mode_mask?1u:source.alpha_mode==cgltf_alpha_mode_blend?2u:0u;material.cutoff=source.alpha_cutoff;material.doubleSided=source.double_sided;material.unlit=source.unlit;material.normalScale=source.normal_texture.scale;material.occlusionStrength=source.occlusion_texture.scale;result.materials.push_back(material);
    }
    uint32_t defaultMaterial=uint32_t(result.materials.size());result.materials.push_back({});
    result.nodes.resize(data->nodes_count);std::vector<bool> animated(data->nodes_count);
    for(size_t i=0;i<data->nodes_count;i++){
        const auto& source=data->nodes[i];auto& node=result.nodes[i];node.parent=source.parent?uint32_t(source.parent-data->nodes):Invalid;node.matrixOnly=source.has_matrix;
        node.pose.p={source.translation[0],source.translation[1],source.translation[2]};node.pose.q={source.rotation[0],source.rotation[1],source.rotation[2],source.rotation[3]};node.scale={source.scale[0],source.scale[1],source.scale[2]};memcpy(&node.matrix,source.matrix,sizeof(node.matrix));
        node.weightStart=uint32_t(result.weights.size());node.weightCount=source.mesh&&source.mesh->primitives_count?uint32_t(source.mesh->primitives[0].targets_count):0;
        for(uint32_t w=0;w<node.weightCount;w++)result.weights.push_back(w<source.weights_count?source.weights[w]:source.mesh&&w<source.mesh->weights_count?source.mesh->weights[w]:0);
    }
    for(size_t a=0;a<data->animations_count;a++){
        AnimationClip clip;const auto& source=data->animations[a];
        for(size_t c=0;c<source.channels_count;c++){
            const auto& channel=source.channels[c];if(!channel.target_node||!channel.sampler)Fail("glTF animation target");
            AnimationTrack track;track.node=uint32_t(channel.target_node-data->nodes);auto& node=result.nodes[track.node];
            if(node.matrixOnly)Fail("glTF animation on matrix node");
            switch(channel.target_path){case cgltf_animation_path_type_translation:track.path=AnimationPath::Translation;track.components=3;break;case cgltf_animation_path_type_rotation:track.path=AnimationPath::Rotation;track.components=4;break;case cgltf_animation_path_type_scale:track.path=AnimationPath::Scale;track.components=3;break;case cgltf_animation_path_type_weights:track.path=AnimationPath::Weights;track.components=node.weightCount;break;default:Fail("glTF animation path");}
            const auto& sampler=*channel.sampler;track.interpolation=sampler.interpolation==cgltf_interpolation_type_cubic_spline?Interpolation::Cubic:sampler.interpolation==cgltf_interpolation_type_step?Interpolation::Step:Interpolation::Linear;
            track.times=Floats(sampler.input,1);track.values=Floats(sampler.output,track.path==AnimationPath::Weights?1u:track.components);
            if(track.times.empty()||!track.components||track.values.size()!=track.times.size()*track.components*(track.interpolation==Interpolation::Cubic?3:1))Fail("glTF animation data");
            for(size_t k=1;k<track.times.size();k++)if(track.times[k]<=track.times[k-1])Fail("glTF animation key order");
            clip.duration=std::max(clip.duration,track.times.back());animated[track.node]=true;clip.tracks.push_back(std::move(track));
        }
        result.animations.push_back(std::move(clip));
    }
    std::vector<uint8_t> visited(data->nodes_count);
    auto hierarchy=[&](auto&& self,uint32_t index)->void{if(visited[index]==2)return;if(visited[index]==1)Fail("glTF node cycle");visited[index]=1;auto parent=result.nodes[index].parent;if(parent!=Invalid){self(self,parent);animated[index]=animated[index]||animated[parent];}result.nodeOrder.push_back(index);visited[index]=2;};
    for(uint32_t i=0;i<data->nodes_count;i++)hierarchy(hierarchy,i);
    std::vector<uint32_t> skinOffset(data->skins_count);
    for(size_t i=0;i<data->skins_count;i++){
        const auto& skin=data->skins[i];skinOffset[i]=uint32_t(result.bones.size());auto matrices=Floats(skin.inverse_bind_matrices,16);if(skin.inverse_bind_matrices&&skin.inverse_bind_matrices->count!=skin.joints_count)Fail("glTF inverse bind count");
        for(size_t j=0;j<skin.joints_count;j++){BoneBinding bone;bone.node=uint32_t(skin.joints[j]-data->nodes);if(matrices.empty())XMStoreFloat4x4(&bone.inverseBind,XMMatrixIdentity());else memcpy(&bone.inverseBind,matrices.data()+j*16,sizeof(bone.inverseBind));result.bones.push_back(bone);}
    }
    auto visit=[&](auto&& self,const cgltf_node* node)->void {
        uint32_t nodeIndex=uint32_t(node-data->nodes);float matrix[16];cgltf_node_transform_world(node,matrix);XMFLOAT4X4 raw;memcpy(&raw,matrix,sizeof(raw));auto world=XMLoadFloat4x4(&raw)*XMMatrixScaling(1,1,-1);
        if(node->mesh)for(size_t j=0;j<node->mesh->primitives_count;j++){
            const auto& primitive=node->mesh->primitives[j];bool dynamic=animated[nodeIndex]||node->skin||primitive.targets_count;
            MeshRange range{};range.firstVertex=uint32_t(result.meshes.vertices.size());range.firstIndex=uint32_t(result.meshes.indices.size());range.firstMeshlet=uint32_t(result.meshes.meshlets.size());
            Primitive(result.meshes,primitive,dynamic?XMMatrixScaling(1,1,-1):world);FinishRange(result.meshes,range);MeshMotion motion;
            result.meshes.skin.resize(result.meshes.vertices.size());
            if(dynamic){
                motion.node=nodeIndex;motion.weightStart=result.nodes[nodeIndex].weightStart;motion.morphCount=uint32_t(primitive.targets_count);motion.morphStart=uint32_t(result.meshes.morphs.size());
                if(motion.morphCount!=result.nodes[nodeIndex].weightCount)Fail("glTF morph target count");
                if(node->skin){motion.boneStart=skinOffset[node->skin-data->skins];motion.boneCount=uint32_t(node->skin->joints_count);const cgltf_accessor *joints[2]{},*weights[2]{};
                    for(size_t k=0;k<primitive.attributes_count;k++){auto& attr=primitive.attributes[k];if(attr.index<0||attr.index>1)continue;if(attr.type==cgltf_attribute_type_joints)joints[attr.index]=attr.data;if(attr.type==cgltf_attribute_type_weights)weights[attr.index]=attr.data;}
                    if(!joints[0]||!weights[0])Fail("glTF skin influences");
                    for(uint32_t set=0;set<2;set++){if(bool(joints[set])!=bool(weights[set]))Fail("glTF skin attribute pair");if(!joints[set])continue;if(joints[set]->count!=range.vertexCount||weights[set]->count!=range.vertexCount)Fail("glTF skin vertex count");auto jointData=Floats(joints[set],4),weightData=Floats(weights[set],4);for(uint32_t v=0;v<range.vertexCount;v++){auto& skinned=result.meshes.skin[range.firstVertex+v];auto* ji=set?&skinned.joints1.x:&skinned.joints.x;auto* we=set?&skinned.weights1.x:&skinned.weights.x;for(uint32_t k=0;k<4;k++){ji[k]=uint32_t(jointData[v*4+k]);we[k]=weightData[v*4+k];if(ji[k]>=motion.boneCount||we[k]<0)Fail("glTF joint index or weight");}}}
                    for(uint32_t v=0;v<range.vertexCount;v++){auto& skin=result.meshes.skin[range.firstVertex+v];float total=skin.weights.x+skin.weights.y+skin.weights.z+skin.weights.w+skin.weights1.x+skin.weights1.y+skin.weights1.z+skin.weights1.w;if(total<=0)Fail("glTF zero skin weights");XMStoreFloat4(&skin.weights,XMVectorScale(XMLoadFloat4(&skin.weights),1/total));XMStoreFloat4(&skin.weights1,XMVectorScale(XMLoadFloat4(&skin.weights1),1/total));}
                }
                for(size_t target=0;target<primitive.targets_count;target++){const auto& morph=primitive.targets[target];const cgltf_accessor *position=nullptr,*normal=nullptr;for(size_t k=0;k<morph.attributes_count;k++){const auto& attr=morph.attributes[k];if(attr.type==cgltf_attribute_type_position)position=attr.data;if(attr.type==cgltf_attribute_type_normal)normal=attr.data;}for(auto* accessor:{position,normal})if(accessor&&accessor->count!=range.vertexCount)Fail("glTF morph vertex count");auto positions=Floats(position,3),normals=Floats(normal,3);for(uint32_t v=0;v<range.vertexCount;v++){MorphVertex delta;if(position)delta.position={positions[v*3],positions[v*3+1],-positions[v*3+2]};if(normal)delta.normal={normals[v*3],normals[v*3+1],-normals[v*3+2]};result.meshes.morphs.push_back(delta);}}
            }
            RenderObject object;object.mesh=uint32_t(result.meshes.ranges.size());object.material=primitive.material?uint32_t(primitive.material-data->materials):defaultMaterial;object.id=uint32_t(result.objects.size());object.flags=dynamic?DynamicObject|DeformedObject|NoCollision:0;
            result.meshes.ranges.push_back(range);result.meshes.deformations.push_back({});result.meshes.motions.push_back(motion);result.objects.push_back(object);
        }
        if(node->light){const auto& source=*node->light;SceneLight light;XMStoreFloat3(&light.position,XMVector3TransformCoord(XMVectorZero(),world));XMStoreFloat3(&light.direction,XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0,0,-1,0),world)));light.color={source.color[0],source.color[1],source.color[2]};light.power=source.intensity;light.radius=source.range>0?source.range:FarZ;light.animationNode=animated[nodeIndex]?nodeIndex:Invalid;
            if(source.type==cgltf_light_type_directional)light.flags=3;
            if(source.type==cgltf_light_type_spot){light.outerCos=std::cos(source.spot_outer_cone_angle);light.innerCos=std::cos(source.spot_inner_cone_angle);}result.lights.push_back(light);}
        for(size_t i=0;i<node->children_count;i++)self(self,node->children[i]);
    };
    if(data->scene)for(size_t i=0;i<data->scene->nodes_count;i++)visit(visit,data->scene->nodes[i]);else for(size_t i=0;i<data->nodes_count;i++)if(!data->nodes[i].parent)visit(visit,&data->nodes[i]);
    return result;
}
void AppendAcousticGeometry(const SceneData& scene,std::vector<XMFLOAT3>& vertices,std::vector<std::array<uint32_t,3>>& triangles) {
    for(const auto& object:scene.objects) {
        if(object.flags&NoCollision)continue;
        const auto& range=scene.meshes.ranges[object.mesh];uint32_t base=uint32_t(vertices.size());auto world=XMMatrixScaling(object.scale.x,object.scale.y,object.scale.z)*Matrix(object.current);
        for(uint32_t i=0;i<range.vertexCount;i++){XMFLOAT3 v;XMStoreFloat3(&v,XMVector3TransformCoord(XMLoadFloat3(&scene.meshes.vertices[range.firstVertex+i].position),world));vertices.push_back(v);}
        for(uint32_t i=0;i<range.indexCount;i+=3)triangles.push_back({base+scene.meshes.indices[range.firstIndex+i]-range.firstVertex,base+scene.meshes.indices[range.firstIndex+i+1]-range.firstVertex,base+scene.meshes.indices[range.firstIndex+i+2]-range.firstVertex});
    }
}
}
