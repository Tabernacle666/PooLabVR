#include "Assets.h"
#include <cstddef>
#include <iterator>
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <ktx.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <cfloat>
#include <tuple>
#include <set>
#include <unordered_map>
namespace OASIS {
using Microsoft::WRL::ComPtr;
std::wstring AssetPath(const wchar_t* name) {
    wchar_t path[32768]; DWORD n=GetModuleFileNameW(nullptr,path,32768);
    if(!n||n>=32768) Fail("executable path");
    std::wstring p(path,n); p.resize(p.find_last_of(L"\\/")+1); p+=name; return p;
}
static std::string Utf8(const std::wstring& s);
std::vector<std::byte> ReadBytes(const std::wstring& path) {
    FILE* f=nullptr; _wfopen_s(&f,path.c_str(),L"rb");
    if(!f){
        // The startup failure sink records this complete path in OASIS.log.
        // This also identifies missing staged shaders after an interrupted build.
        const auto message=std::string("Asset file missing: ")+Utf8(path);
        Fail(message.c_str());
    }
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
void FinishRange(MeshData& mesh,MeshRange& range) {
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
static TextureData DecodeWicTexture(std::span<const std::byte> bytes,TextureUse use,uint32_t maxDimension){
    if(bytes.empty()||bytes.size()>MAXDWORD)return {};
    TextureData result;
    result.normal=use==TextureUse::Normal;
    ComPtr<IWICImagingFactory> factory;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))))return {};
    ComPtr<IWICStream> stream;
    if(FAILED(factory->CreateStream(&stream))||
       FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),DWORD(bytes.size()))))
        return {};
    ComPtr<IWICBitmapDecoder> decoder;
    if(FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder)))return {};
    ComPtr<IWICBitmapFrameDecode> frame;
    if(FAILED(decoder->GetFrame(0,&frame))||FAILED(frame->GetSize(&result.width,&result.height))||
       !result.width||!result.height||result.width>maxDimension||result.height>maxDimension)return {};
    ComPtr<IWICFormatConverter> converter;
    if(FAILED(factory->CreateFormatConverter(&converter)))return {};
    const GUID& target=use==TextureUse::Height?GUID_WICPixelFormat16bppGray:GUID_WICPixelFormat32bppRGBA;
    if(FAILED(converter->Initialize(frame.Get(),target,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))
        return {};
    if(use==TextureUse::Height){
        std::vector<uint16_t> pixels(size_t(result.width)*result.height);
        if(FAILED(converter->CopyPixels(nullptr,result.width*2,UINT(pixels.size()*2),reinterpret_cast<BYTE*>(pixels.data()))))
            return {};
        MipmapsR16(result,std::move(pixels));
    }else{
        std::vector<uint8_t> pixels(size_t(result.width)*result.height*4);
        if(FAILED(converter->CopyPixels(nullptr,result.width*4,UINT(pixels.size()),pixels.data())))return {};
        result.format=use==TextureUse::Color?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;
        Mipmaps(result,std::move(pixels),use);
    }
    return result;
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
    result=DecodeWicTexture(bytes,use,16384u);
    if(!result.mips)Fail("WIC texture decode");
    return result;
}
TextureData TryLoadOverrideTexture(const std::filesystem::path& path,TextureUse use){
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    const auto size=std::streamoff(file.tellg());
    if(size<=0||size>128*1024*1024)return {};
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))return {};
    return DecodeWicTexture(bytes,use,4096u);
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
SceneData LoadSceneGLTF(const std::wstring& path,bool keepMeshNodeNames) {
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
    result.materialNames.reserve(data->materials_count+1);
    for(size_t i=0;i<data->materials_count;i++){
        const auto& source=data->materials[i];const auto& p=source.pbr_metallic_roughness;AssetMaterial material;
        result.materialNames.emplace_back(source.name?source.name:"");
        material.colorRoughness={p.base_color_factor[0],p.base_color_factor[1],p.base_color_factor[2],p.roughness_factor};
        float strength=source.has_emissive_strength?source.emissive_strength.emissive_strength:1.f;
        material.emissionMetallic={source.emissive_factor[0]*strength,source.emissive_factor[1]*strength,source.emissive_factor[2]*strength,p.metallic_factor};
        material.albedo=texture(p.base_color_texture,TextureUse::Color,material,0);material.normal=texture(source.normal_texture,TextureUse::Normal,material,1);material.metalRough=texture(p.metallic_roughness_texture,TextureUse::Linear,material,2);material.emissive=texture(source.emissive_texture,TextureUse::Color,material,3);material.occlusion=texture(source.occlusion_texture,TextureUse::Linear,material,4);
        material.alpha=p.base_color_factor[3];material.alphaMode=source.alpha_mode==cgltf_alpha_mode_mask?1u:source.alpha_mode==cgltf_alpha_mode_blend?2u:0u;material.cutoff=source.alpha_cutoff;material.doubleSided=source.double_sided;material.unlit=source.unlit;material.normalScale=source.normal_texture.scale;material.occlusionStrength=source.occlusion_texture.scale;result.materials.push_back(material);
    }
    uint32_t defaultMaterial=uint32_t(result.materials.size());result.materials.push_back({});
    result.materialNames.emplace_back();
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
            if(keepMeshNodeNames)result.meshNodeNames.emplace_back(node->name?node->name:"");
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
namespace {
uint32_t AppendRigidMeshes(SceneData& scene,SceneData& source) {
    auto& dst=scene.meshes;
    auto& src=source.meshes;
    const uint32_t mesh=uint32_t(dst.ranges.size()),vertex=uint32_t(dst.vertices.size());
    const uint32_t indices=uint32_t(dst.indices.size()),meshlet=uint32_t(dst.meshlets.size());
    const uint32_t remap=uint32_t(dst.remap.size()),triangle=uint32_t(dst.triangles.size());
    if(!source.animations.empty()||!source.bones.empty()||!src.morphs.empty())
        Fail("Rigid asset contains animated geometry");
    for(auto& range:src.ranges){
        range.firstVertex+=vertex;range.firstIndex+=indices;range.firstMeshlet+=meshlet;
    }
    for(auto& item:src.meshlets){item.vertexOffset+=remap;item.triangleOffset+=triangle;}
    for(auto& index:src.indices)index+=vertex;
    for(auto& index:src.remap)index+=vertex;
    auto append=[](auto& dest,auto& data){dest.insert(dest.end(),data.begin(),data.end());};
    append(dst.vertices,src.vertices);append(dst.indices,src.indices);
    append(dst.remap,src.remap);append(dst.triangles,src.triangles);
    append(dst.meshlets,src.meshlets);append(dst.ranges,src.ranges);
    append(dst.deformations,src.deformations);append(dst.motions,src.motions);
    dst.skin.resize(dst.vertices.size());
    if(!scene.triangleSolids.empty())scene.triangleSolids.resize(dst.indices.size()/3u,Invalid);
    return mesh;
}

uint32_t AppendMaterials(SceneData& scene,SceneData& source) {
    const uint32_t material=uint32_t(scene.materials.size());
    const uint32_t sampler=uint32_t(scene.samplers.size());
    auto signature=[](const TextureData& texture){
        uint64_t h=14695981039346656037ull;
        auto mix=[&](uint64_t n){h^=n;h*=1099511628211ull;};
        mix(texture.width);mix(texture.height);mix(texture.mips);
        mix(uint32_t(texture.format));mix(texture.normal?1u:0u);
        for(auto offset:texture.offsets)mix(offset);
        for(auto value:texture.bytes)mix(std::to_integer<unsigned char>(value));
        return h;
    };
    auto same=[](const TextureData& a,const TextureData& b){
        return a.width==b.width&&a.height==b.height&&a.mips==b.mips&&
            a.format==b.format&&a.normal==b.normal&&
            a.offsets==b.offsets&&a.bytes==b.bytes;
    };
    // Seed once with base-scene textures so later imports also deduplicate.
    if(scene.textureCache.empty())for(uint32_t i=0;i<scene.textures.size();++i)
        scene.textureCache[signature(scene.textures[i])].push_back(i);
    std::vector<uint32_t> mapping;
    mapping.reserve(source.textures.size());
    for(auto& texture:source.textures){
        const auto key=signature(texture);
        auto& bucket=scene.textureCache[key];
        auto found=std::find_if(bucket.begin(),bucket.end(),[&](uint32_t index){
            return same(scene.textures[index],texture);
        });
        if(found!=bucket.end())mapping.push_back(*found);
        else{
            const uint32_t index=uint32_t(scene.textures.size());
            scene.textures.push_back(std::move(texture));
            bucket.push_back(index);
            mapping.push_back(index);
        }
    }
    for(auto& item:source.materials){
        for(auto* handle:{&item.albedo,&item.normal,&item.metalRough,&item.emissive,&item.occlusion}){
            if(*handle!=Invalid){
                if(*handle>=mapping.size())Fail("glTF material texture index out of bounds");
                *handle=mapping[*handle];
            }
        }
        for(auto& handle:item.samplers)if(handle!=Invalid)handle+=sampler;
        scene.materials.push_back(item);
    }
    scene.materialNames.insert(scene.materialNames.end(),source.materialNames.begin(),source.materialNames.end());
    scene.samplers.insert(scene.samplers.end(),source.samplers.begin(),source.samplers.end());
    return material;
}

}

uint32_t AppendAssetMaterials(SceneData& scene,SceneData& source){return AppendMaterials(scene,source);}

AppendedRigidAsset AppendRigidAsset(SceneData& scene,SceneData& source){
    const auto mesh=AppendRigidMeshes(scene,source);
    const auto material=AppendMaterials(scene,source);
    // Make every rigid non-blended imported GLB part an MG2 source.
    for(const auto& object:source.objects){
        if((object.flags&(DeformedObject|AlphaBlendObject))!=0u||
           source.materials[object.material].alphaMode==2u)continue;
        scene.mg2ForestParts.push_back({mesh+object.mesh,material+object.material});
    }
    return {mesh,material};
}

BuildingSiteTerrain PrepareBuildingSite(SceneData& scene,std::span<const BuildingFootprint> sites){
    BuildingSiteTerrain terrain;
    // Copied source triangles retain materials and UVs. The cutout is limited
    // to the flat upper island layers over the pilot's basement.
    auto& mesh=scene.meshes;
    auto append=[&](const std::vector<Vertex>& polygon){
        if(polygon.size()<3)return;
        const auto first=uint32_t(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),polygon.begin(),polygon.end());
        for(uint32_t i=1;i+1<polygon.size();++i)
            mesh.indices.insert(mesh.indices.end(),{first,first+i,first+i+1});
    };
    const auto mix=[](const Vertex& a,const Vertex& b,float t){
        const auto uv=[&](XMFLOAT2 u,XMFLOAT2 v){return XMFLOAT2{std::lerp(u.x,v.x,t),std::lerp(u.y,v.y,t)};};
        return Vertex{Add(a.position,Scale(Sub(b.position,a.position),t)),Unit(Add(Scale(a.normal,1.f-t),Scale(b.normal,t))),
            uv(a.uv,b.uv),uv(a.uv1,b.uv1)};
    };
    const auto clip=[&](const std::vector<Vertex>& polygon,const auto& dist,bool inside){
        std::vector<Vertex> out;
        if(polygon.empty())return out;
        auto a=polygon.back();
        float ad=dist(a.position);
        for(const auto& b:polygon){
            const float bd=dist(b.position);
            if((ad<0.f)!=(bd<0.f))out.push_back(mix(a,b,ad/(ad-bd)));
            if(inside?bd>=0.f:bd<=0.f)out.push_back(b);
            a=b;
            ad=bd;
        }
        return out;
    };

    constexpr float riverBed=-.030f,riverSurface=.008f,riverRim=.0203f;
    struct ChannelSegment {
        std::array<Vertex,4> rim;
        float minX,maxX,minZ,maxZ;
    };
    const auto readRibbon=[&](const MeshRange& range){
        std::vector<std::array<Vertex,4>> strip;
        for(uint32_t i=range.firstIndex;i+5<range.firstIndex+range.indexCount;i+=6){
            std::array<Vertex,4> quad;
            uint32_t count=0;
            std::array<uint32_t,4> unique{};
            for(uint32_t j=0;j<6;++j){
                const auto index=mesh.indices[i+j];
                if(std::find(unique.begin(),unique.begin()+count,index)!=unique.begin()+count)continue;
                if(count==unique.size())break;
                unique[count]=index;
                quad[count++]=mesh.vertices[index];
            }
            if(count!=4)continue;
            // Imported handedness can reverse the triangle, so restore the
            // authored ribbon's alternating left/right vertex order.
            std::sort(unique.begin(),unique.end());
            for(size_t n=0;n<quad.size();++n)quad[n]=mesh.vertices[unique[n]];
            strip.push_back(quad);
        }
        return strip;
    };
    std::vector<ChannelSegment> channel;
    for(const auto& object:scene.objects){
        if(scene.materialNames[object.material]!="River banks")continue;
        for(const auto& rim:readRibbon(mesh.ranges[object.mesh])){
            const auto& [a,b,c,d]=rim;
            channel.push_back({rim,std::min({a.position.x,b.position.x,c.position.x,d.position.x}),
                std::max({a.position.x,b.position.x,c.position.x,d.position.x}),
                std::min({a.position.z,b.position.z,c.position.z,d.position.z}),
                std::max({a.position.z,b.position.z,c.position.z,d.position.z})});
        }
    }
    const auto carve=[&](std::vector<Vertex> ground,std::vector<std::vector<Vertex>>& dry,
                         std::vector<std::vector<Vertex>>& bed){
        auto minX=ground.front().position.x,maxX=minX,minZ=ground.front().position.z,maxZ=minZ;
        for(const auto& v:ground){
            minX=std::min(minX,v.position.x);maxX=std::max(maxX,v.position.x);
            minZ=std::min(minZ,v.position.z);maxZ=std::max(maxZ,v.position.z);
        }
        dry.push_back(std::move(ground));
        for(const auto& segment:channel){
            if(segment.maxX<minX||segment.minX>maxX||segment.maxZ<minZ||segment.minZ>maxZ)continue;
            const auto& rim=segment.rim;
            const auto side=[&](uint32_t edge,XMFLOAT3 point){
                const auto a=rim[edge].position,b=rim[(edge+1u)%4u].position;
                const auto c=rim[(edge+2u)%4u].position;
                const auto cross=[&](XMFLOAT3 p){return (b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x);};
                return (cross(c)>=0.f?1.f:-1.f)*cross(point);
            };
            std::vector<std::vector<Vertex>> remaining;
            for(auto& region:dry){
                auto interior=region;
                for(uint32_t edge=0;edge<4&&interior.size()>=3;++edge)
                    interior=clip(interior,[&](XMFLOAT3 p){return side(edge,p);},true);
                float area=0.f;
                for(size_t i=2;i<interior.size();++i){
                    const auto a=Sub(interior[i-1].position,interior[0].position);
                    const auto b=Sub(interior[i].position,interior[0].position);
                    area+=a.x*b.z-a.z*b.x;
                }
                if(std::abs(area)<1.e-8f){remaining.push_back(std::move(region));continue;}
                for(auto& v:interior)v.position.y=riverBed;
                bed.push_back(std::move(interior));
                auto rest=std::move(region);
                for(uint32_t edge=0;edge<4&&rest.size()>=3;++edge){
                    const auto dist=[&](XMFLOAT3 p){return side(edge,p);};
                    auto outside=clip(rest,dist,false);
                    if(outside.size()>=3)remaining.push_back(std::move(outside));
                    rest=clip(rest,dist,true);
                }
            }
            dry=std::move(remaining);
        }
    };
    for(auto& object:scene.objects){
        const auto& name=scene.materialNames[object.material];
        const bool letter=name=="Brushed charcoal structural metal"||name=="Warm illuminated glass"||
            name=="Soft amber illuminated glass"||name=="Unlit glass"||name=="Aviation marker lights";
        const bool soil=name=="Island soil";
        const bool banks=name=="River banks",water=name=="Bioluminescent green water";
        const bool ground=soil||banks||water||name=="Island stone"||name=="Low vegetation";
        if(!letter&&!ground)continue;
        if(letter){object.mesh=Invalid;continue;}
        auto* surface=soil?&terrain.soil:banks?&terrain.banks:water?&terrain.waterway:nullptr;
        if(water){
            auto& material=scene.materials[object.material];
            material.colorRoughness={.37f,.25f,.016f,.26f};
            material.emissionMetallic={.22f,.14f,.006f,.04f};
            material.unlit=8u;
            object.flags|=NoCollision|NoShadow;
        }
        const auto source=mesh.ranges[object.mesh];
        MeshRange result;
        result.firstVertex=uint32_t(mesh.vertices.size());
        result.firstIndex=uint32_t(mesh.indices.size());
        result.firstMeshlet=uint32_t(mesh.meshlets.size());
        std::vector<std::vector<Vertex>> polygons;
        if(banks||water){
            float along=0.f;
            for(auto quad:readRibbon(source)){
                auto& [a,b,c,d]=quad;
                const auto center=Scale(Add(a.position,b.position),.5f);
                const float next=along+Length(Sub(Scale(Add(c.position,d.position),.5f),center));
                a.uv={0.f,along};b.uv={1.f,along};c.uv={1.f,next};d.uv={0.f,next};
                if(water){
                    for(auto& v:quad){v.position.y=riverSurface;v.normal={0,1,0};}
                    const auto ab=Sub(b.position,a.position),ac=Sub(c.position,a.position);
                    if(ab.z*ac.x-ab.x*ac.z<0.f)std::reverse(quad.begin(),quad.end());
                    polygons.emplace_back(quad.begin(),quad.end());
                }else{
                    b.position.y=a.position.y=c.position.y=d.position.y=riverRim;
                    constexpr float slope=.24f;
                    auto innerA=mix(a,b,slope),innerB=mix(b,a,slope);
                    auto innerD=mix(d,c,slope),innerC=mix(c,d,slope);
                    innerA.position.y=innerB.position.y=innerC.position.y=innerD.position.y=riverBed;
                    const auto slopeFace=[&](Vertex v,Vertex w,Vertex x,Vertex y){
                        const auto a=Sub(w.position,v.position),b=Sub(x.position,v.position);
                        auto normal=Unit({a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x});
                        if(normal.y<0.f){normal=Scale(normal,-1.f);std::swap(w,y);}
                        v.normal=w.normal=x.normal=y.normal=normal;
                        polygons.push_back({v,w,x,y});
                    };
                    slopeFace(a,innerA,innerD,d);
                    slopeFace(innerB,b,c,innerC);
                }
                along=next;
            }
        }else for(uint32_t i=source.firstIndex;i<source.firstIndex+source.indexCount;i+=3){
            std::vector<Vertex> polygon{mesh.vertices[mesh.indices[i]],mesh.vertices[mesh.indices[i+1]],
                mesh.vertices[mesh.indices[i+2]]};
            if(name=="Island stone"&&(polygon[0].position.y<.018f||
               polygon[1].position.y<.018f||polygon[2].position.y<.018f)){
                append(polygon);continue;
            }
            polygons.push_back(std::move(polygon));
        }
        for(auto& poly:polygons){
            std::vector<std::vector<Vertex>> regions;
            regions.push_back(std::move(poly));
            for(const auto& site:sites){
                std::vector<std::vector<Vertex>> remaining;
                const auto origin=site.origin;
                for(auto shape:regions){
                    for(const auto& [axis,limit,positive]:std::array<std::tuple<int,float,bool>,4>{{
                            {0,origin.x,true},{0,origin.x+site.width,false},
                            {1,origin.z,true},{1,origin.z+site.depth,false}}}){
                        const auto dist=[&](XMFLOAT3 p){return ((axis==0?p.x:p.z)-limit)*(positive?1.f:-1.f);};
                        auto outside=clip(shape,dist,false);
                        if(outside.size()>=3)remaining.push_back(std::move(outside));
                        shape=clip(shape,dist,true);
                    }
                }
                regions=std::move(remaining);
            }
            if((soil||name=="Island stone")&&!channel.empty()){
                std::vector<std::vector<Vertex>> dry,bed;
                for(auto& region:regions){
                    std::vector<std::vector<Vertex>> remainder;
                    carve(std::move(region),remainder,bed);
                    dry.insert(dry.end(),std::make_move_iterator(remainder.begin()),std::make_move_iterator(remainder.end()));
                }
                if(soil)dry.insert(dry.end(),std::make_move_iterator(bed.begin()),std::make_move_iterator(bed.end()));
                regions=std::move(dry);
            }
            for(const auto& region:regions){
                if(region.size()<3)continue;
                const auto transform=XMMatrixScaling(object.scale.x,object.scale.y,object.scale.z)*Matrix(object.current);
                const auto random=[](int32_t x,int32_t z,uint32_t seed){
                    uint32_t h=uint32_t(x)*0x9e3779b9u^uint32_t(z)*0x85ebca6bu^seed;
                    h^=h>>16;h*=0x7feb352du;h^=h>>15;
                    h*=0x846ca68bu;h^=h>>16;
                    return float(h&0xffffffu)/16777215.f;
                };
                const auto noise=[&](float x,float z,uint32_t seed){
                    const int32_t ix=int32_t(std::floor(x)),iz=int32_t(std::floor(z));
                    float fx=x-float(ix),fz=z-float(iz);
                    fx=fx*fx*(3.f-2.f*fx);fz=fz*fz*(3.f-2.f*fz);
                    const float a=std::lerp(random(ix,iz,seed),random(ix+1,iz,seed),fx);
                    const float b=std::lerp(random(ix,iz+1,seed),random(ix+1,iz+1,seed),fx);
                    return std::lerp(a,b,fz)*2.f-1.f;
                };
                const auto lift=[&](Vertex v){
                    if(!soil)return v;
                    const auto p=TransformPoint(v.position,transform);
                    float nearest=std::numeric_limits<float>::max();
                    for(const auto& site:sites){
                        const float dx=std::max({site.origin.x-p.x,p.x-(site.origin.x+site.width),0.f});
                        const float dz=std::max({site.origin.z-p.z,p.z-(site.origin.z+site.depth),0.f});
                        nearest=std::min(nearest,std::hypot(dx,dz));
                    }
                    float fade=std::clamp(nearest/.14f,0.f,1.f);
                    fade=fade*fade*(3.f-2.f*fade);
                    const float relief=.045f*noise(p.x*2.4f,p.z*2.4f,0x713ab07u)
                                      +.014f*noise(p.x*10.f,p.z*10.f,0x19472b5u)
                                      +.0035f*noise(p.x*41.f,p.z*41.f,0x1172acdu);
                    v.position.y+=fade*relief/std::max(.001f,object.scale.y);
                    return v;
                };
                const auto blend=[](const Vertex& a,const Vertex& b,const Vertex& c,float u,float v){
                    const float w=1.f-u-v;
                    Vertex out{};
                    out.position=Add(Add(Scale(a.position,w),Scale(b.position,u)),Scale(c.position,v));
                    out.normal=Unit(Add(Add(Scale(a.normal,w),Scale(b.normal,u)),Scale(c.normal,v)));
                    out.uv={w*a.uv.x+u*b.uv.x+v*c.uv.x,w*a.uv.y+u*b.uv.y+v*c.uv.y};
                    out.uv1={w*a.uv1.x+u*b.uv1.x+v*c.uv1.x,w*a.uv1.y+u*b.uv1.y+v*c.uv1.y};
                    return out;
                };
                const auto emit=[&](Vertex a,Vertex b,Vertex c){
                    const auto e1=Sub(b.position,a.position),e2=Sub(c.position,a.position);
                    const auto n=Unit({e1.y*e2.z-e1.z*e2.y,
                                       e1.z*e2.x-e1.x*e2.z,
                                       e1.x*e2.y-e1.y*e2.x});
                    a.normal=b.normal=c.normal=n;
                    append({a,b,c});
                    if(surface){
                        const auto p=TransformPoint(a.position,transform);
                        const auto q=TransformPoint(b.position,transform);
                        const auto r=TransformPoint(c.position,transform);
                        if(std::abs((q.x-p.x)*(r.z-p.z)-(q.z-p.z)*(r.x-p.x))>1.e-8f)
                            surface->push_back({p,q,r});
                    }
                };
                for(size_t fan=1;fan+1<region.size();++fan){
                    const auto& a=region[0];const auto& b=region[fan];const auto& c=region[fan+1];
                    const unsigned divisions=soil?8u:1u;
                    const auto sample=[&](unsigned i,unsigned j){
                        return lift(blend(a,b,c,float(i)/float(divisions),float(j)/float(divisions)));
                    };
                    for(unsigned i=0;i<divisions;++i)for(unsigned j=0;i+j<divisions;++j){
                        const auto p=sample(i,j),q=sample(i+1,j),r=sample(i,j+1);
                        emit(p,q,r);
                        if(i+j+1<divisions)
                            emit(q,sample(i+1,j+1),r);
                    }
                }
            }
        }
        if(mesh.indices.size()==result.firstIndex){object.mesh=Invalid;continue;}
        FinishRange(mesh,result);
        object.mesh=uint32_t(mesh.ranges.size());
        mesh.ranges.push_back(result);mesh.deformations.emplace_back();mesh.motions.emplace_back();
        if(soil)scene.mg2ForestParts.push_back({object.mesh,object.material});
    }
    std::erase_if(scene.objects,[](const RenderObject& o){return o.mesh==Invalid;});
    mesh.skin.resize(mesh.vertices.size());
    return terrain;
}
uint32_t AppendBuildingBox(SceneData& scene,bool pane){
    auto& m=scene.meshes;
    MeshRange range;
    range.firstVertex=uint32_t(m.vertices.size());
    range.firstIndex=uint32_t(m.indices.size());
    range.firstMeshlet=uint32_t(m.meshlets.size());
    constexpr float a=-.5f,b=.5f;
    auto quad=[&](XMFLOAT3 p,XMFLOAT3 q,XMFLOAT3 r,XMFLOAT3 s,XMFLOAT3 n){
        const uint32_t i=uint32_t(m.vertices.size());
        m.vertices.insert(m.vertices.end(),{{p,n,{0,0}},{q,n,{0,1}},{r,n,{1,1}},{s,n,{1,0}}});
        m.indices.insert(m.indices.end(),{i,i+1,i+2,i,i+2,i+3});
    };
    if(pane){
        quad({a,a,0},{a,b,0},{b,b,0},{b,a,0},{0,0,-1});
    }else{
        quad({a,a,a},{a,b,a},{b,b,a},{b,a,a},{0,0,-1});
        quad({b,a,b},{b,b,b},{a,b,b},{a,a,b},{0,0,1});
        quad({a,a,b},{a,b,b},{a,b,a},{a,a,a},{-1,0,0});
        quad({b,a,a},{b,b,a},{b,b,b},{b,a,b},{1,0,0});
        quad({a,b,a},{a,b,b},{b,b,b},{b,b,a},{0,1,0});
        quad({a,a,b},{a,a,a},{b,a,a},{b,a,b},{0,-1,0});
    }
    FinishRange(m,range);
    const auto index=uint32_t(m.ranges.size());
    m.ranges.push_back(range);m.deformations.emplace_back();m.motions.emplace_back();
    m.skin.resize(m.vertices.size());
    return index;
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
