#pragma once
#include "Assets.h"

namespace OASIS {
struct Mg2LodEye {
    XMFLOAT3 position;
    float focalPixels,nearPlane;
};

struct Mg2LodCluster {
    uint32_t id,firstVertex,vertexCount,firstIndex,indexCount,material,mesh;
    bool opaque;
};

struct Mg2LodCut {
    std::vector<Mg2LodCluster> clusters;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    size_t pages{};
    std::vector<uint32_t> selection;
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
    Mg2LodCut Select(std::span<const Mg2LodEye>,float errorPixels,
        std::span<const uint32_t> previous={})const;
private:
    void Bake(const SceneData&,std::span<const Mg2ForestPart>);
};
}
