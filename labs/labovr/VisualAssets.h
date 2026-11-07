#pragma once
#include "Assets.h"
#include "LaboVRVisualService.h"
#include <filesystem>
namespace lab {
struct VisualMeshVertex {float px,py,pz,nx,ny,nz,tx,ty,tz,u,v;};
struct VisualMeshData {std::vector<VisualMeshVertex> vertices;std::vector<uint32_t> indices;};
std::filesystem::path ResolveVisualAsset(const std::filesystem::path& root,const char* path);
VisualMeshData LoadVisualObj(const std::filesystem::path&);
TextureData LoadVisualImage(const std::filesystem::path&,LabU32 use);
VisualMeshData VisualPrimitive(LabU32 source);
uint32_t AppendVisualMesh(MeshData&,const VisualMeshData&);
}
