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
    return ApplyBatch({}, {}, objects);
}
void WorldRegistry::CreateCanopyBatch(std::span<const WorldObject> objects,
        std::span<const uint32_t> assemblies){
    if(objects.size()%2u||objects.size()/2u!=assemblies.size())
        Fail("Canopy assembly must own two resident routes");
    canopyPlants_.reserve(canopyPlants_.size()+assemblies.size());
    const auto ids=CreateBatch(objects);
    for(size_t i=0;i<assemblies.size();++i){
        const auto root=ids[2*i];
        canopyPlants_.emplace(root,CanopyPlant{root,{root,ids[2*i+1]},assemblies[i]});
    }
}
const CanopyPlant* WorldRegistry::FindCanopyPlant(uint64_t identity)const noexcept{
    const auto it=canopyPlants_.find(identity);
    return it==canopyPlants_.end()?nullptr:&it->second;
}
void WorldRegistry::ForgetCanopyRoute(uint64_t identity){
    if(canopyPlants_.erase(identity))return;
    if(identity){
        const auto it=canopyPlants_.find(identity-1u);
        if(it!=canopyPlants_.end()&&it->second.routes[1]==identity)canopyPlants_.erase(it);
    }
}
bool WorldRegistry::UpdateCanopyPlant(uint64_t identity,uint32_t assembly,
        const std::array<WorldObject,2>& objects,bool morphology,bool transform,bool state){
    const auto it=canopyPlants_.find(identity);
    if(it==canopyPlants_.end())return false;
    auto& plant=it->second;
    morphology=morphology||plant.assembly!=assembly;
    if((morphology&&plant.morphologyRevision==UINT64_MAX)||
       (transform&&plant.transformRevision==UINT64_MAX)||
       (state&&plant.stateRevision==UINT64_MAX))Fail("Canopy plant revision exhausted");
    const std::array<Change,2> changes{{{plant.routes[0],objects[0]},{plant.routes[1],objects[1]}}};
    ApplyBatch(changes,{},{});
    plant.assembly=assembly;
    if(morphology)++plant.morphologyRevision;
    if(transform)++plant.transformRevision;
    if(state)++plant.stateRevision;
    return true;
}
bool WorldRegistry::EraseCanopyPlant(uint64_t identity){
    const auto it=canopyPlants_.find(identity);
    if(it==canopyPlants_.end())return false;
    const auto routes=it->second.routes;
    return EraseBatch(routes)!=0;
}
std::vector<uint64_t> WorldRegistry::ApplyBatch(std::span<const Change> changes,
        std::span<const uint64_t> removed,std::span<const WorldObject> objects) {
    if(objects.size()>uint64_t(0x80000000u-nextRenderId_)||
       objects.size()>UINT64_MAX-nextIdentity_)
        Fail("World registry identity range exhausted");

    struct TileEdits {
        std::unordered_set<uint64_t> removed;
        std::unordered_map<uint64_t,GpuResidentTileObject> patched;
        std::vector<GpuResidentTileObject> added;
    };
    std::map<GpuTileKey,TileEdits> edits;
    std::vector<std::pair<uint64_t,GpuTileKey>> moved;
    std::vector<uint64_t> crossing;
    for(uint64_t id:removed){
        if(auto owner=owners_.find(id);owner!=owners_.end())
            edits[owner->second].removed.insert(id);
    }
    for(const auto& change:changes){
        const auto owner=owners_.find(change.identity);
        if(owner==owners_.end())Fail("World batch references an unknown object");
        auto& source=edits[owner->second];
        if(source.removed.contains(change.identity))
            Fail("World batch tries to update a removed object");
        const auto renderId=RenderId(change.identity);
        const auto record=Record(change.object,change.identity,renderId);
        const auto target=TileOf(change.object);
        if(target==owner->second){
            source.patched.insert_or_assign(change.identity,record);
        }else{
            source.removed.insert(change.identity);
            edits[target].added.push_back(record);
            moved.emplace_back(change.identity,target);
            crossing.push_back(change.identity);
        }
    }

    std::vector<uint64_t> identities;
    identities.reserve(objects.size());
    auto identity=nextIdentity_;
    auto renderId=nextRenderId_;
    for(const auto& object:objects){
        const auto key=TileOf(object);
        edits[key].added.push_back(Record(object,identity,renderId));
        identities.push_back(identity);
        moved.emplace_back(identity,key);
        ++identity;
        ++renderId;
    }

    // Remove cross-tile identities before inserting at the destination. The
    // cache indexes their ownership globally, independent of tile order.
    EraseBatchImpl(crossing,false);
    for(auto& [key,edit]:edits){
        if(edit.removed.empty()&&edit.added.empty()){
            for(const auto& [id,record]:edit.patched)cache_.PatchTileObject(key,id,record);
            continue;
        }
        auto tile=CopyTile(key);
        std::erase_if(tile.objects,[&](GpuResidentTileObject& record){
            if(edit.removed.contains(record.identity))return true;
            if(auto patch=edit.patched.find(record.identity);patch!=edit.patched.end())
                record=patch->second;
            return false;
        });
        tile.objects.insert(tile.objects.end(),edit.added.begin(),edit.added.end());
        if(tile.objects.empty())cache_.RemoveTile(key);
        else cache_.UpsertTile(std::move(tile));
    }
    for(uint64_t id:removed){owners_.erase(id);ForgetCanopyRoute(id);}
    for(const auto& [id,key]:moved)owners_.insert_or_assign(id,key);
    nextIdentity_=identity;
    nextRenderId_=renderId;
    return identities;
}
uint32_t WorldRegistry::RenderId(uint64_t identity)const{
    const auto found=owners_.find(identity);
    if(found==owners_.end())return Invalid;
    const auto& tile=cache_.ResidentTiles().at(found->second);
    return tile.objects.at(tile.byIdentity.at(identity)).renderId;
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
    return EraseBatchImpl(ids,true);
}
uint64_t WorldRegistry::EraseBatchImpl(std::span<const uint64_t> ids,bool forgetCanopy) {
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
        for(uint64_t id:idsInTile){owners_.erase(id);if(forgetCanopy)ForgetCanopyRoute(id);}
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
