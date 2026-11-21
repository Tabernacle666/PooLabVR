#include "WorldRegistry.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_set>

namespace OASIS {
namespace {
constexpr double CellSize=20.0;
constexpr int64_t CellsPerSide=6;
static int64_t CellCoordinate(double position) {
    if(!std::isfinite(position)||std::abs(position)>1.0e7)
        Fail("World object position outside supported coordinate range");
    return static_cast<int64_t>(std::floor(position/CellSize));
}
static int64_t FloorDiv(int64_t a,int64_t b) {
    const int64_t q=a/b,r=a%b;
    return r<0?q-1:q;
}
static GpuTileKey Key(int64_t cx,int64_t cz,int32_t layer) {
    return {FloorDiv(cx,CellsPerSide),FloorDiv(cz,CellsPerSide),layer};
}
static uint32_t Cell(int64_t cx,int64_t cz,GpuTileKey key) {
    const int64_t x=cx-key.x*CellsPerSide,z=cz-key.z*CellsPerSide;
    return uint32_t(x+z*CellsPerSide);
}
static GpuFloat4 Position(const XMFLOAT3& p) {
    return {p.x,p.y,p.z,1.f};
}
static GpuFloat4 Rotation(const XMFLOAT4& q) {
    return {q.x,q.y,q.z,q.w};
}
static bool Valid(const WorldObject& o) {
    return std::isfinite(o.current.p.x)&&std::isfinite(o.current.p.y)&&
        std::isfinite(o.current.p.z)&&std::isfinite(o.previous.p.x)&&
        std::isfinite(o.previous.p.y)&&std::isfinite(o.previous.p.z)&&
        std::isfinite(o.scale.x)&&std::isfinite(o.scale.y)&&std::isfinite(o.scale.z)&&
        std::isfinite(o.boundRadius)&&o.boundRadius>0.f&&
        o.scale.x!=0.f&&o.scale.y!=0.f&&o.scale.z!=0.f;
}
}
GpuTileKey WorldRegistry::TileOf(const WorldObject& o) {
    return Key(CellCoordinate(o.current.p.x),CellCoordinate(o.current.p.z),o.tileLayer);
}
GpuResidentTileObject WorldRegistry::Record(const WorldObject& o,uint64_t id,uint32_t renderId) {
    if(!Valid(o))Fail("World object has invalid pose, scale or bounds");
    const int64_t cx=CellCoordinate(o.current.p.x),cz=CellCoordinate(o.current.p.z);
    const GpuTileKey key=Key(cx,cz,o.tileLayer);
    const float maxScale=std::max({std::abs(o.scale.x),std::abs(o.scale.y),std::abs(o.scale.z)});
    const float radius=14.143f+maxScale*o.boundRadius;
    if(!std::isfinite(radius))Fail("World object cell radius overflow");
    GpuResidentTileObject value{};
    value.identity=id;value.renderId=renderId;
    value.mesh=o.mesh;value.material=o.material;value.flags=o.flags;
    value.position=Position(o.current.p);value.previousPosition=Position(o.previous.p);
    value.rotation=Rotation(o.current.q);value.previousRotation=Rotation(o.previous.q);
    value.scale={o.scale.x,o.scale.y,o.scale.z,0.f};
    value.cell=Cell(cx,cz,key);
    value.cellX=float((double(cx)+.5)*CellSize);
    value.cellZ=float((double(cz)+.5)*CellSize);
    value.cellRadius=radius;
    value.farMesh=o.farMesh;value.hideFar=o.hideFar;
    value.enabled=o.enabled?1u:0u;
    return value;
}
GpuResidentTile WorldRegistry::CopyTile(GpuTileKey key)const {
    auto it=cache_.ResidentTiles().find(key);
    if(it!=cache_.ResidentTiles().end())return it->second;
    GpuResidentTile tile{};tile.key=key;return tile;
}
uint64_t WorldRegistry::Create(const WorldObject& object) {
    auto ids=CreateBatch(std::span<const WorldObject>(&object,1));
    return ids.front();
}
std::vector<uint64_t> WorldRegistry::CreateBatch(std::span<const WorldObject> objects) {
    if(objects.empty())return {};

    if(objects.size()>uint64_t(0x80000000u-nextRenderId_)||
       objects.size()>UINT64_MAX-nextIdentity_)
        Fail("World registry identity range exhausted");
    std::map<GpuTileKey,GpuResidentTile> touched;
    std::vector<uint64_t> identities;
    identities.reserve(objects.size());
    for(const auto& object:objects) {
        const uint64_t id=nextIdentity_++;
        const uint32_t renderId=nextRenderId_++;
        auto record=Record(object,id,renderId);
        const GpuTileKey key=TileOf(object);
        auto [it,inserted]=touched.try_emplace(key);
        if(inserted)it->second=CopyTile(key);
        it->second.objects.push_back(record);
        owners_.emplace(id,key);
        identities.push_back(id);
    }
    for(auto& [key,tile]:touched)cache_.UpsertTile(std::move(tile));
    return identities;
}
bool WorldRegistry::Update(uint64_t id,const WorldObject& object) {
    auto owner=owners_.find(id);if(owner==owners_.end())return false;
    const GpuTileKey oldKey=owner->second,newKey=TileOf(object);
    const auto& oldTile=cache_.ResidentTiles().at(oldKey);
    const auto& old=oldTile.objects.at(oldTile.byIdentity.at(id));
    const auto updated=Record(object,id,old.renderId);
    if(oldKey==newKey)
        return cache_.PatchTileObject(oldKey,id,updated);
    auto source=CopyTile(oldKey);
    const uint32_t index=source.byIdentity.at(id);
    source.objects.erase(source.objects.begin()+index);
    if(oldTile.byIdentity.size()==1)cache_.RemoveTile(oldKey);
    else cache_.UpsertTile(std::move(source));
    auto target=CopyTile(newKey);
    target.objects.push_back(updated);
    cache_.UpsertTile(std::move(target));
    owner->second=newKey;
    return true;
}
bool WorldRegistry::Erase(uint64_t id) {
    return EraseBatch(std::span<const uint64_t>(&id,1))!=0;
}
uint64_t WorldRegistry::EraseBatch(std::span<const uint64_t> ids) {
    std::map<GpuTileKey,std::unordered_set<uint64_t>> removed;
    for(uint64_t id:ids) {
        auto it=owners_.find(id);
        if(it!=owners_.end())removed[it->second].insert(id);
    }
    uint64_t count=0;
    for(const auto& [key,idsInTile]:removed) {
        auto tile=CopyTile(key);
        std::erase_if(tile.objects,[&](const GpuResidentTileObject& o){
            return idsInTile.contains(o.identity);
        });
        count+=idsInTile.size();
        if(tile.byIdentity.size()==idsInTile.size())cache_.RemoveTile(key);
        else cache_.UpsertTile(std::move(tile));
        for(uint64_t id:idsInTile)owners_.erase(id);
    }
    return count;
}
RenderContributions WorldRegistry::Contributions()noexcept {
    RenderContributions result{};
    result.context=this;
    result.residentTiles=[](void* ptr)->const GpuSceneCache*{
        return &static_cast<WorldRegistry*>(ptr)->cache_;
    };
    return result;
}
}
