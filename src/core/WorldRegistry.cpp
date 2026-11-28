#include "WorldRegistry.h"
#include "Graphics.h"
#include "OasisMetric.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_set>

namespace OASIS {
namespace {
// Logical spatial cells remain 10m. Expansion canopy groups those cells
// into stable ownership pages; actual GPU storage and PTLAS partitions are
// still allocated independently.
constexpr double CellSize=TerritorialTileMeters;
static int64_t CellCoordinate(double position) {
    if(!std::isfinite(position)||std::abs(position)>1.0e7)
        Fail("World object position outside supported coordinate range");
    return static_cast<int64_t>(std::floor(position/CellSize));
}
static GpuTileKey Key(int64_t cx,int64_t cz,int32_t layer) {
    return {cx,cz,layer};
}
static GpuFloat4 Position(const XMFLOAT3& p) {
    return {p.x,p.y,p.z,1.f};
}
static GpuFloat4 Rotation(const XMFLOAT4& q) {
    return {q.x,q.y,q.z,q.w};
}
static bool SameArchitectureGeometry(const GpuResidentTileObject& a,const GpuResidentTileObject& b){
    return a.mesh==b.mesh&&a.material==b.material&&a.flags==b.flags&&a.enabled==b.enabled&&
        std::memcmp(&a.position,&b.position,sizeof(GpuFloat4))==0&&
        std::memcmp(&a.rotation,&b.rotation,sizeof(GpuFloat4))==0&&
        std::memcmp(&a.scale,&b.scale,sizeof(GpuFloat4))==0;
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
    const auto x=CellCoordinate(o.current.p.x),z=CellCoordinate(o.current.p.z);
    if(o.tileLayer==ExpansionCanopyTileLayer)return ExpansionCanopyPageKey(x,z);
    return Key(x,z,o.tileLayer);
}
GpuResidentTileObject WorldRegistry::Record(const WorldObject& o,uint64_t id,uint32_t renderId) {
    if(!Valid(o))Fail("World object has invalid pose, scale or bounds");
    const int64_t cx=CellCoordinate(o.current.p.x),cz=CellCoordinate(o.current.p.z);
    const float maxScale=std::max({std::abs(o.scale.x),std::abs(o.scale.y),std::abs(o.scale.z)});
    const float radius=float(CellSize*.7071067811865476)+maxScale*o.boundRadius;
    if(!std::isfinite(radius))Fail("World object cell radius overflow");
    GpuResidentTileObject value{};
    value.identity=id;value.renderId=renderId;
    value.mesh=o.mesh;value.material=o.material;value.flags=o.flags;
    value.position=Position(o.current.p);value.previousPosition=Position(o.previous.p);
    value.rotation=Rotation(o.current.q);value.previousRotation=Rotation(o.previous.q);
    value.scale={o.scale.x,o.scale.y,o.scale.z,0.f};
    value.cell=0;
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
void WorldRegistry::RefreshPatrimony(){
    vegetationPatrimony_.RefreshInvalid(cache_);
    forestPatrimony_.RefreshInvalid(cache_);
}
bool WorldRegistry::MutationIdentityAllowed(uint64_t id)const noexcept{
    return id&&((id&(1ull<<63))||reservedMutationIdentities_.contains(id));
}
uint64_t WorldRegistry::ForestAdmissionEpoch()const noexcept{
    return forestAdmission_?forestAdmission_->ForestAdmissionEpoch():0u;
}
std::vector<uint64_t> WorldRegistry::ReserveMutationIdentities(size_t count){
    if(count>(1ull<<63)-nextIdentity_)Fail("World mutation identity range exhausted");
    std::vector<uint64_t> ids;ids.reserve(count);
    for(size_t i=0;i<count;++i){const auto id=nextIdentity_++;reservedMutationIdentities_.insert(id);ids.push_back(id);}
    return ids;
}
void WorldRegistry::FinishForestMutation(uint64_t id){
    if(forestAdmission_)forestAdmission_->ReleaseForestMutation(id);
    reservedMutationIdentities_.erase(id);
}
void WorldRegistry::CancelMutation(std::span<const uint64_t> ids){
    for(const auto id:ids)FinishForestMutation(id);
}

uint64_t WorldRegistry::Create(const WorldObject& object) {
    auto ids=CreateBatch(std::span<const WorldObject>(&object,1));
    return ids.front();
}
std::vector<uint64_t> WorldRegistry::CreateBatch(std::span<const WorldObject> objects,std::span<const uint64_t> stableIdentities) {
    return ApplyBatch({}, {}, objects,stableIdentities);
}
void WorldRegistry::CreateCanopyBatch(std::span<const WorldObject> objects,
        std::span<const uint32_t> assemblies,std::span<const uint64_t> stableIdentities){
    if(objects.size()%2u||objects.size()/2u!=assemblies.size())
        Fail("Canopy assembly must own two resident routes");
    canopyPlants_.reserve(canopyPlants_.size()+assemblies.size());
    if(!stableIdentities.empty()){
        if(stableIdentities.size()!=objects.size())Fail("Canopy identity count mismatch");
        for(size_t i=0;i<stableIdentities.size();i+=2)
            if(stableIdentities[i+1]!=stableIdentities[i]+1u)Fail("Canopy route identities must be consecutive");
    }
    const auto ids=CreateBatch(objects,stableIdentities);
    for(size_t i=0;i<assemblies.size();++i){
        const auto root=ids[2*i];
        canopyPlants_.emplace(root,CanopyPlant{root,{root,ids[2*i+1]},assemblies[i]});
    }
}
bool WorldRegistry::TryCreateCanopyBatch(std::span<const WorldObject> objects,
        std::span<const uint32_t> assemblies,std::span<const uint64_t> ids){
    if(objects.size()%2u||objects.size()/2u!=assemblies.size()||ids.size()!=objects.size())
        Fail("Canopy assembly mutation identity count");
    for(size_t i=0;i<ids.size();i+=2)
        if(ids[i+1]!=ids[i]+1u)Fail("Canopy route identities must be consecutive");
    auto committed=TryApplyBatch({}, {}, objects,ids);
    if(!committed)return false;
    for(size_t i=0;i<assemblies.size();++i){
        const auto root=(*committed)[2*i];
        canopyPlants_.emplace(root,CanopyPlant{root,{root,(*committed)[2*i+1]},assemblies[i]});
    }
    return true;
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
    if(!TryApplyBatch(changes,{},{}))return false;
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
std::optional<std::vector<uint64_t>> WorldRegistry::TryApplyBatch(std::span<const Change> changes,
        std::span<const uint64_t> removed,std::span<const WorldObject> objects,
        std::span<const uint64_t> ids){
    if(!ids.empty()&&ids.size()!=objects.size())Fail("Territorial identity count mismatch");
    if(!forestAdmission_)return ApplyBatch(changes,removed,objects,ids);
    // Validate the transaction before registering any future route.
    std::unordered_set<uint64_t> touched;
    for(const auto id:removed)touched.insert(id);
    for(const auto& change:changes){
        if(!owners_.contains(change.identity)||!touched.insert(change.identity).second)
            Fail("Invalid world mutation identity");
        (void)Record(change.object,change.identity,RenderId(change.identity));
    }
    for(size_t i=0;i<objects.size();++i){
        const auto id=ids.empty()?nextIdentity_+i:ids[i];
        if(!ids.empty()&&(!MutationIdentityAllowed(id)||owners_.contains(id)||!touched.insert(id).second))
            Fail("Invalid world mutation insertion identity");
        (void)Record(objects[i],id,0u);
        if(ids.empty()&&!forestAdmission_->ForestRouteReady(objects[i],id))
            Fail("Deferred MG2 insertion requires reserved or stable identities");
    }
    bool ready=true;
    for(const auto& change:changes)
        if(!forestAdmission_->StageForestMutation(change.object,change.identity))ready=false;
    for(size_t i=0;i<objects.size();++i){
        const auto id=ids.empty()?nextIdentity_+i:ids[i];
        if(!forestAdmission_->StageForestMutation(objects[i],id))ready=false;
    }
    if(!ready)return std::nullopt;
    return ApplyBatch(changes,removed,objects,ids);
}
std::vector<uint64_t> WorldRegistry::ApplyBatch(std::span<const Change> changes,
        std::span<const uint64_t> removed,std::span<const WorldObject> objects,
        std::span<const uint64_t> stableIdentities) {
    if(!stableIdentities.empty()&&stableIdentities.size()!=objects.size())
        Fail("Territorial identity count mismatch");
    if(objects.size()>uint64_t(0x80000000u-nextRenderId_)||
       (stableIdentities.empty()&&objects.size()>(1ull<<63)-nextIdentity_))
        Fail("World registry identity range exhausted");

    struct TileEdits {
        std::unordered_set<uint64_t> removed;
        std::unordered_map<uint64_t,GpuResidentTileObject> patched;
        std::vector<GpuResidentTileObject> added;
    };
    std::map<GpuTileKey,TileEdits> edits;
    std::vector<std::pair<uint64_t,GpuTileKey>> moved;
    std::vector<uint64_t> crossing;
    std::unordered_set<uint64_t> architectureGeometryChanges;
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
        if(change.object.architecture.building||architecturePatrimony_.Entries().contains(change.identity)){
            const auto& tile=cache_.ResidentTiles().at(owner->second);
            if(!SameArchitectureGeometry(tile.objects.at(tile.byIdentity.at(change.identity)),record))
                architectureGeometryChanges.insert(change.identity);
        }
        if(forestAdmission_&&!forestAdmission_->ForestRouteReady(change.object,change.identity))
            Fail("MG2 mutation must be prepared with TryApplyBatch");
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
    std::unordered_set<uint64_t> batchIdentities;
    if(!stableIdentities.empty())batchIdentities.reserve(objects.size());
    for(size_t i=0;i<objects.size();++i){
        const auto& object=objects[i];
        const auto id=stableIdentities.empty()?identity++:stableIdentities[i];
        if(!stableIdentities.empty()&&(!MutationIdentityAllowed(id)||owners_.contains(id)||
           !batchIdentities.insert(id).second))Fail("Duplicate or invalid territorial identity");
        const auto key=TileOf(object);
        const auto record=Record(object,id,renderId);
        if(forestAdmission_&&!forestAdmission_->ForestRouteReady(object,id))
            Fail("MG2 insertion must be prepared with TryApplyBatch");
        edits[key].added.push_back(record);
        identities.push_back(id);
        moved.emplace_back(id,key);
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
    for(uint64_t id:removed){vegetationPatrimony_.Invalidate(id);forestPatrimony_.Invalidate(id);architecturePatrimony_.Erase(id);owners_.erase(id);ForgetCanopyRoute(id);}
    for(const auto& change:changes){
        vegetationPatrimony_.InvalidateAt(change.identity,change.object.mesh,change.object.material,change.object.current.p);
        forestPatrimony_.InvalidateAt(change.identity,change.object.mesh,change.object.material,change.object.current.p);
    }
    for(const auto& [id,key]:moved)owners_.insert_or_assign(id,key);
    for(const auto& change:changes)
        if(change.object.architecture.building||architecturePatrimony_.Entries().contains(change.identity))
            architecturePatrimony_.Set(change.identity,change.object.architecture,change.object.flags,
                architectureGeometryChanges.contains(change.identity));
    for(size_t i=0;i<objects.size();++i)if(objects[i].architecture.building)
        architecturePatrimony_.Set(identities[i],objects[i].architecture,objects[i].flags);
    nextIdentity_=identity;
    nextRenderId_=renderId;
    RefreshPatrimony();
    for(const auto id:removed)FinishForestMutation(id);
    for(const auto& change:changes)FinishForestMutation(change.identity);
    for(const auto id:identities)FinishForestMutation(id);
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
    const bool architectureGeometryChanged=!SameArchitectureGeometry(old,updated);
    if(forestAdmission_&&!forestAdmission_->StageForestMutation(object,id))return false;
    const auto refresh=[&]{
        vegetationPatrimony_.InvalidateAt(id,object.mesh,object.material,object.current.p);
        forestPatrimony_.InvalidateAt(id,object.mesh,object.material,object.current.p);
        RefreshPatrimony();
    };
    if(oldKey==newKey){
        const bool changed=cache_.PatchTileObject(oldKey,id,updated);
        if(changed)refresh();
        if(object.architecture.building||architecturePatrimony_.Entries().contains(id))
            architecturePatrimony_.Set(id,object.architecture,object.flags,architectureGeometryChanged);
        FinishForestMutation(id);
        return changed;
    }
    auto source=CopyTile(oldKey);
    const uint32_t index=source.byIdentity.at(id);
    source.objects.erase(source.objects.begin()+index);
    if(oldTile.byIdentity.size()==1)cache_.RemoveTile(oldKey);
    else cache_.UpsertTile(std::move(source));
    auto target=CopyTile(newKey);
    target.objects.push_back(updated);
    cache_.UpsertTile(std::move(target));
    owner->second=newKey;
    if(object.architecture.building||architecturePatrimony_.Entries().contains(id))
        architecturePatrimony_.Set(id,object.architecture,object.flags);
    refresh();
    FinishForestMutation(id);
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
        for(uint64_t id:idsInTile){owners_.erase(id);if(forgetCanopy){vegetationPatrimony_.Invalidate(id);forestPatrimony_.Invalidate(id);architecturePatrimony_.Erase(id);ForgetCanopyRoute(id);}}
    }
    if(forgetCanopy){
        RefreshPatrimony();
        for(const auto id:ids)FinishForestMutation(id);
    }
    return count;
}
RenderContributions WorldRegistry::Contributions()noexcept {
    RenderContributions result{};
    result.context=this;
    result.residentTiles=[](void* ptr)->const GpuSceneCache*{
        return &static_cast<WorldRegistry*>(ptr)->cache_;
    };
    result.territorialPatrimony=[](void* ptr,bool forest)->TerritorialPatrimony*{
        auto& world=*static_cast<WorldRegistry*>(ptr);
        return forest?&world.forestPatrimony_:&world.vegetationPatrimony_;
    };
    result.architecturePatrimony=[](void* ptr)->ArchitecturePatrimony*{
        return &static_cast<WorldRegistry*>(ptr)->architecturePatrimony_;
    };
    return result;
}
}
