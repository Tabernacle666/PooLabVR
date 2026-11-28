#pragma once
#include "Assets.h"
#include "RuntimeConfig.h"
#include <array>
#include <functional>
#include <cstddef>
#include <memory>

namespace OASIS {
struct Mg2LodEye {
    XMFLOAT3 position;
    float focalPixels,nearPlane;
    // Predicted displacement over one publication interval, in mesh space.
    XMFLOAT3 motion{};
};

// xyz identify a persistent instance; w is the selected variant ordinal.
using Mg2InstanceRoute=std::array<uint32_t,4>;
inline constexpr uint32_t Mg2ResidentKeyBit=0x80000000u;
struct Mg2LodInstance {
    Mg2InstanceRoute key{};
    uint32_t mesh{},eyeCount{};
    std::array<Mg2LodEye,RuntimeConfig::MaxViews> eyes{};
    float distanceWorld{};
    bool visible{};
};
struct Mg2LodCluster {
    uint32_t id,firstVertex,vertexCount,firstIndex,indexCount,material,mesh,part;
    bool opaque;
    uint32_t sourceFirstVertex{},sourceFirstIndex{};
};

struct Mg2LodCut {
    std::vector<Mg2LodCluster> clusters;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    size_t pages{};
    std::vector<uint32_t> selection;
    std::vector<Mg2InstanceRoute> routes;
    uint32_t variants{},candidates{},eligible{},selectedRoutes{};
};

// Selection-only GPU ABI. The source geometry and downstream cut ABI are unchanged.
struct Mg2GpuPage {
    XMFLOAT3 center; float radius;
    float error; uint32_t isRoot,clusterCount,triangles;
    uint32_t opaque,firstId,firstCluster{},pad1{};
};
struct Mg2GpuCluster { uint32_t page,refined,triangles,local; };
// Preorder spatial tree over the DAG's groups; escape skips a whole subtree.
struct Mg2GpuNode {
    XMFLOAT3 center; float radius;
    float error; uint32_t force,page,escape;
};
struct Mg2GpuInstance {
    uint32_t first,count,structuralFirst,structuralCount;
    uint32_t outputOffset,eyeCount,visible,previousCluster;
    uint32_t nodeRoot,nodeEnd,pad0{},pad1{};
    struct Eye { XMFLOAT4 positionFocal,motionNear; };
    std::array<Eye,RuntimeConfig::MaxViews> eyes{};
};
static_assert(RuntimeConfig::MaxViews==4);
static_assert(sizeof(Mg2GpuPage)==48&&sizeof(Mg2GpuCluster)==16&&sizeof(Mg2GpuNode)==32&&sizeof(Mg2GpuInstance)==176);
// Camera-independent records, resident on the GPU between selections.
struct Mg2GpuRegistryEntry {
    Mg2InstanceRoute key{};
    uint32_t first{},count{},structuralFirst{},structuralCount{};
    uint32_t nodeRoot{},nodeEnd{},pad0{},pad1{};
    XMFLOAT4 rootRadius{},rotation{},scale{};
};
static_assert(sizeof(Mg2GpuRegistryEntry)==96);
using Mg2GpuDispatch=std::function<void(std::span<uint32_t>,float)>;

class Mg2Pages {
    struct Cluster {
        uint32_t id,vertexCount;
        int refined;
        // Immutable scene-local source geometry; no decode on selection.
        std::vector<Vertex> vertices;
        std::vector<unsigned char> indices;
        uint32_t sourceFirstVertex{},sourceFirstIndex{};
    };
    struct Page {
        XMFLOAT3 center;
        float radius,error;
        uint32_t material,mesh;
        bool opaque;
        std::vector<Cluster> clusters;
        bool structural{};
        uint64_t structuralTriangles{};
    };
    // Published pages are shared across catalog snapshots. Only a freshly
    // baked or newly copied segment may mutate its own pages.
    std::vector<std::shared_ptr<Page>> pages_;
    std::unordered_map<uint32_t,std::vector<size_t>> structuralPages_;
    std::unordered_map<uint32_t,std::vector<size_t>> pagesByMesh_;
    std::vector<uint8_t> hasParent_;
    struct GpuMesh { uint32_t first,count,structuralFirst,structuralCount,nodeRoot,nodeEnd; };
    std::unordered_map<uint32_t,GpuMesh> gpuMeshes_;
    std::vector<Mg2GpuPage> gpuPages_;
    std::vector<Mg2GpuNode> gpuNodes_;
    std::vector<Mg2GpuCluster> gpuClusters_;
    std::vector<uint32_t> gpuStructural_;
    void BuildGpuCatalog();
    uint64_t sourceBytes_{};
    uint32_t vertexCount_{},indexCount_{};
    uint64_t revision_{1};
    // Bake-order catalog index, independent of residency and instance cuts.
    std::vector<std::pair<size_t,size_t>> sourceClusters_;
public:
    uint32_t VertexCount()const{return vertexCount_;}
    uint32_t IndexCount()const{return indexCount_;}
    enum class SourceStream:uint32_t { Vertices=0, Indices=1 };
    static constexpr uint32_t SourceStreamCount=2;
    static constexpr uint64_t SourceTileBytes=65536u;
    static constexpr uint64_t SourceStride(SourceStream stream){
        return stream==SourceStream::Vertices?sizeof(Vertex):sizeof(uint32_t);
    }
    static constexpr uint64_t SourceAlign(uint64_t bytes){
        return (bytes+SourceTileBytes-1u)&~(SourceTileBytes-1u);
    }
    uint64_t SourceBytes(SourceStream stream)const{
        return uint64_t(stream==SourceStream::Vertices?vertexCount_:indexCount_)*SourceStride(stream);
    }
    // Independent, stable virtual address streams. This is the used
    // extent, not the reservation capacity nor the physical VRAM demand.
    uint64_t SourceVirtualBytes(SourceStream stream)const{
        return SourceAlign(SourceBytes(stream));
    }
    static constexpr uint64_t SourceAddressLimit(SourceStream stream){
        return SourceAlign(uint64_t(UINT32_MAX)*SourceStride(stream));
    }
    void PackSourceTile(SourceStream stream,uint64_t byteOffset,std::span<std::byte> destination)const;
    explicit Mg2Pages(const SceneData&);
    Mg2Pages(const SceneData&,std::span<const Mg2ForestPart>);
    Mg2Pages(Mg2Pages&&)=default;
    Mg2Pages& operator=(Mg2Pages&&)=default;
    // Append a self-contained baked segment. Optional remap translates its
    // local mesh IDs into never-before-used representation IDs. Old geometry
    // and GPU catalog prefixes are shared/preserved, not rebaked.
    std::shared_ptr<const Mg2Pages> Extend(const Mg2Pages& segment,
        std::span<const std::pair<uint32_t,uint32_t>> meshRemap={})const;
    bool IsPrefixOf(const Mg2Pages& next)const;
    uint64_t Revision()const{return revision_;}
    size_t PageCount()const{return pages_.size();}
    uint64_t SourceBytes()const{return sourceBytes_;}
    void AppendOpacityTriangles(uint32_t mesh,std::vector<std::array<XMFLOAT2,3>>& out)const;
    const auto& GpuPages()const{return gpuPages_;}
    const auto& GpuNodes()const{return gpuNodes_;}
    const auto& GpuClusters()const{return gpuClusters_;}
    const auto& GpuStructural()const{return gpuStructural_;}
    bool RegistryMesh(uint32_t mesh,Mg2GpuRegistryEntry&)const;
    Mg2LodCut Select(std::span<const Mg2GpuRegistryEntry>,uint64_t scratchWords,float errorPixels,
        const Mg2GpuDispatch&,std::span<const uint32_t> previous,
        std::span<const Mg2InstanceRoute> previousRoutes)const;
private:
    Mg2Pages(const Mg2Pages&)=default;
    Mg2Pages& operator=(const Mg2Pages&)=delete;
    void Bake(const SceneData&,std::span<const Mg2ForestPart>);
};
}
