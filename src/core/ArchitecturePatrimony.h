#pragma once
#include "TerritorialPatrimony.h"
#include <map>
#include <set>
#include <unordered_map>

namespace OASIS {
// CPU-only semantic ownership. No GPU ABI changes and no per-vegetation
// object storage. WorldRegistry still owns the real object and its transform.
enum class ArchitectureKind : uint8_t { Structure, Facade, Decor, Worktop, Mobile };
struct ArchitectureAttachment {
    uint64_t building{},room{},component{},source{};
    int32_t floor{};
    uint32_t ordinal{};
    ArchitectureKind kind{ArchitectureKind::Structure};
    bool operator==(const ArchitectureAttachment&)const=default;
};

class ArchitecturePatrimony {
public:
    using Key=TerritorialPatrimony::Key;
    struct Entry {
        ArchitectureAttachment attachment;
        Key leaf;
    };
    struct Node {
        std::set<uint64_t> routes;
        std::set<Key> children;
        uint64_t revision{},descendants{};
    };
private:
    std::unordered_map<uint64_t,Entry> entries_;
    std::map<Key,Node> nodes_;
    std::set<Key> changed_;
    uint64_t revision_{};
    static Key Leaf(const ArchitectureAttachment& a,uint32_t flags){
        Key k{};
        k.building=a.building;k.floor=a.floor;k.room=a.room;
        // Separate envelope, interior and moving parts; optical/shadow
        // contracts cannot be mixed in one replacement cut. Keep flags
        // exactly as authored, including unit-box and collision semantics.
        const uint32_t domain=a.kind==ArchitectureKind::Facade?1u:
            a.kind==ArchitectureKind::Mobile?2u:0u;
        k.partition=(flags&0x3fffffffu)|(domain<<30u);
        if(domain==1u)k.room=0;
        return k;
    }
    static Key Parent(Key k){return TerritorialPatrimony::DomainParent(TerritorialDomain::Architecture,k);}
    void Touch(const Key& k,Node& n){
        n.revision=++revision_;changed_.insert(k);
    }
    void Add(uint64_t route,const Key& leaf){
        auto k=leaf;Key child{};
        for(;;){
            auto& node=nodes_[k];
            if(!k.level){
                if(!node.routes.insert(route).second)Fail("Duplicate architecture route");
            }else node.children.insert(child);
            ++node.descendants;Touch(k,node);
            if(k.level+1u==TerritorialPatrimony::DomainLevels(TerritorialDomain::Architecture))break;
            child=k;k=Parent(k);
        }
    }
    void Remove(uint64_t route,const Key& leaf){
        auto k=leaf;Key child{};bool childEmpty=false;
        for(;;){
            auto it=nodes_.find(k);
            if(it==nodes_.end()||!it->second.descendants)Fail("Missing architecture owner");
            auto& node=it->second;
            if(!k.level){if(!node.routes.erase(route))Fail("Missing architecture route");}
            else if(childEmpty)node.children.erase(child);
            --node.descendants;Touch(k,node);
            childEmpty=node.descendants==0;
            if(childEmpty)nodes_.erase(it);
            if(k.level+1u==TerritorialPatrimony::DomainLevels(TerritorialDomain::Architecture))break;
            child=k;k=Parent(k);
        }
    }
public:
    void Set(uint64_t route,const ArchitectureAttachment& a,uint32_t flags,bool geometryChanged=true){
        if(!a.building){Erase(route);return;}
        const auto key=Leaf(a,flags);
        if(auto it=entries_.find(route);it!=entries_.end()){
            if(it->second.leaf==key){
                if(!geometryChanged&&it->second.attachment==a)return;
                it->second.attachment=a;
                for(auto k=key;;k=Parent(k)){
                    Touch(k,nodes_.at(k));
                    if(k.level+1u==TerritorialPatrimony::DomainLevels(TerritorialDomain::Architecture))break;
                }
                return;
            }
            Remove(route,it->second.leaf);
            it->second={a,key};
        }else entries_.emplace(route,Entry{a,key});
        Add(route,key);
    }
    void Erase(uint64_t route){
        const auto it=entries_.find(route);if(it==entries_.end())return;
        Remove(route,it->second.leaf);entries_.erase(it);
    }
    const auto& Entries()const noexcept{return entries_;}
    const auto& Nodes()const noexcept{return nodes_;}
    uint64_t Revision()const noexcept{return revision_;}
    std::set<Key> TakeChanges(){std::set<Key> result;result.swap(changed_);return result;}
    const auto& Changes()const noexcept{return changed_;}

    // A composition references original meshes, materials and poses; parents
    // do not own another copy of their descendants. Only a requested cut is
    // flattened into a worker-owned input, never during each camera frame.
    struct Placement {
        uint64_t route{},revision{};
        ArchitectureAttachment attachment;
        uint32_t mesh{},material{},flags{};
        Pose pose{};
        XMFLOAT3 scale{};
    };
    struct Recipe {
        Key key{};
        uint64_t revision{};
        std::vector<Placement> placements;
        std::vector<uint64_t> excluded;
    };
    Recipe Capture(Key key,const GpuSceneCache& cache)const{
        Recipe result;result.key=key;
        const auto root=nodes_.find(key);if(root==nodes_.end())return result;
        result.revision=root->second.revision;
        const auto collect=[&](auto&& self,const Node& node)->void{
            for(auto route:node.routes){
                uint32_t index=0;const auto* tile=cache.FindResidentOwner(route,index);
                if(!tile)Fail("Architecture recipe references missing resident route");
                const auto& o=tile->objects.at(index);
                const auto& a=entries_.at(route).attachment;
                if(!o.enabled||a.kind==ArchitectureKind::Mobile||
                   (o.flags&(DynamicObject|DeformedObject|AlphaBlendObject))){
                    result.excluded.push_back(route);continue;
                }
                result.placements.push_back({route,o.revision,a,o.mesh,o.material,o.flags,
                    {{o.position.x,o.position.y,o.position.z},{o.rotation.x,o.rotation.y,o.rotation.z,o.rotation.w}},
                    {o.scale.x,o.scale.y,o.scale.z}});
            }
            for(const auto& child:node.children)self(self,nodes_.at(child));
        };
        collect(collect,root->second);
        if(result.placements.size()+result.excluded.size()!=root->second.descendants)
            Fail("Architecture recipe descendant count mismatch");
        return result;
    }
};
}
