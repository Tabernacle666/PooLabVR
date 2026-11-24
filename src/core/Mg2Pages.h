#pragma once
#include "Assets.h"
#include "RuntimeConfig.h"
#include <array>

namespace OASIS {
struct Mg2LodEye {
    XMFLOAT3 position;
    float focalPixels,nearPlane;
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
};

struct Mg2LodCut {
    std::vector<Mg2LodCluster> clusters;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    size_t pages{};
    std::vector<uint32_t> selection;
    std::vector<Mg2InstanceRoute> routes;
    uint32_t variants{},candidates{};
};

class Mg2Pages {
    struct Cluster {
        uint32_t id,vertexCount;
        int refined;
        std::vector<unsigned char> vertices,indices;
    };
    struct Page {
        XMFLOAT3 center;
        float radius,error;
        uint32_t material,mesh;
        bool opaque;
        std::vector<Cluster> clusters;
    };
    std::vector<Page> pages_;
public:
    explicit Mg2Pages(const SceneData&);
    Mg2Pages(const SceneData&,std::span<const Mg2ForestPart>);
    size_t PageCount()const{return pages_.size();}
    Mg2LodCut Select(std::span<const Mg2LodInstance>,float errorPixels,
        std::span<const uint32_t> previous={},
        std::span<const Mg2InstanceRoute> previousRoutes={})const;
private:
    void Bake(const SceneData&,std::span<const Mg2ForestPart>);
};
}
