#pragma once
#include "Assets.h"

namespace OASIS {
struct Mg2LodEye {
    XMFLOAT3 position;
    float focalPixels,nearPlane;
};

struct Mg2LodCluster {
    uint32_t id,firstVertex,vertexCount,firstIndex,indexCount,material;
    bool opaque;
};

struct Mg2LodCut {
    std::vector<Mg2LodCluster> clusters;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    size_t pages{};
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
        uint32_t material;
        bool opaque;
        std::vector<Cluster> clusters;
    };
    std::vector<Page> pages_;
public:
    explicit Mg2Pages(const SceneData&);
    size_t PageCount()const{return pages_.size();}
    Mg2LodCut Select(std::span<const Mg2LodEye>,float errorPixels)const;
};
}
