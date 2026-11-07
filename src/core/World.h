#pragma once
#include "Core.h"
#include "Interaction.h"
#include <type_traits>
#include <tuple>
namespace lab {
class EntityRegistry {
    std::vector<uint32_t> generation_,free_;
    std::vector<uint8_t> alive_;
public:
    Handle Create() {
        uint32_t i;
        if(free_.empty()){
            if(generation_.size()>=Invalid)Fail("Entity address space exhausted");
            i=uint32_t(generation_.size());generation_.push_back(1);alive_.push_back(1);
        }else{i=free_.back();free_.pop_back();alive_[i]=1;}
        return {(uint64_t(generation_[i])<<32)|i};
    }
    bool Alive(Handle e)const{return e&&e.index()<alive_.size()&&alive_[e.index()]&&generation_[e.index()]==e.generation();}
    void Destroy(Handle e){if(!Alive(e))return;auto i=e.index();alive_[i]=0;if(generation_[i]!=UINT32_MAX){++generation_[i];free_.push_back(i);}}
};
template<class T> class ComponentPool {
    std::vector<uint32_t> sparse_;
    std::vector<Handle> entities_;
    std::vector<T> values_;
public:
    T* Get(Handle e){if(!e||e.index()>=sparse_.size())return nullptr;uint32_t i=sparse_[e.index()];return i<values_.size()&&entities_[i]==e?&values_[i]:nullptr;}
    const T* Get(Handle e)const{return const_cast<ComponentPool*>(this)->Get(e);}
    T& Set(Handle e,const T& value={}){
        if(auto* old=Get(e)){*old=value;return *old;}
        if(!e)Fail("component identity");
        if(e.index()>=sparse_.size())sparse_.resize(size_t(e.index())+1,Invalid);
        if(sparse_[e.index()]!=Invalid)Fail("component generation mismatch");
        sparse_[e.index()]=uint32_t(values_.size());entities_.push_back(e);values_.push_back(value);return values_.back();
    }
    void Remove(Handle e){if(!Get(e))return;uint32_t i=sparse_[e.index()],last=uint32_t(values_.size()-1);if(i!=last){entities_[i]=entities_[last];values_[i]=std::move(values_[last]);sparse_[entities_[i].index()]=i;}entities_.pop_back();values_.pop_back();sparse_[e.index()]=Invalid;}
    uint32_t Count()const{return uint32_t(values_.size());}
    Handle Entity(uint32_t i)const{return entities_[i];}
    T& Value(uint32_t i){return values_[i];}
    const T& Value(uint32_t i)const{return values_[i];}
};
struct RigidBodyComponent { uint32_t slot{}; };
class World {
    struct ExternalPool {const void* type;void* pool;void (*remove)(void*,Handle);};
    std::vector<ExternalPool> external_;
    template<class T>inline static uint8_t poolType_{};
public:
    World()=default;
    World(const World&)=delete;
    World& operator=(const World&)=delete;
    template<class T>void Register(ComponentPool<T>& pool){
        for(const auto& entry:external_)if(entry.type==&poolType_<T>)Fail("Component pool already registered");
        external_.push_back({&poolType_<T>,&pool,[](void* value,Handle entity){static_cast<ComponentPool<T>*>(value)->Remove(entity);}});
    }
    template<class T>void Unregister(ComponentPool<T>& pool){
        std::erase_if(external_,[&](const auto& entry){return entry.type==&poolType_<T>&&entry.pool==&pool;});
    }
    EntityRegistry entities;
    ComponentPool<RigidBodyComponent> rigidBodies;
    ComponentPool<GripComponent> grips;
    template<class T>auto& Pool() {
        if constexpr(std::is_same_v<T,RigidBodyComponent>)return rigidBodies;
        else if constexpr(std::is_same_v<T,GripComponent>)return grips;
        else {
            for(const auto& entry:external_)if(entry.type==&poolType_<T>)return *static_cast<ComponentPool<T>*>(entry.pool);
            Fail("Component pool not registered");
        }
    }
    template<class First,class... Rest,class Function>void Each(Function&& function) {
        auto& first=Pool<First>();
        for(uint32_t i=0;i<first.Count();i++){auto e=first.Entity(i);if(!entities.Alive(e))continue;auto others=std::tuple{Pool<Rest>().Get(e)...};std::apply([&](auto*... rest){if((...&&bool(rest)))function(e,first.Value(i),*rest...);},others);}
    }
    void Destroy(Handle e) { rigidBodies.Remove(e); grips.Remove(e); for(const auto& entry:external_)entry.remove(entry.pool,e); entities.Destroy(e); }
};


class WorldServices {
    struct Entry { const void* type{}; void* service{}; };
    std::vector<Entry> entries_;
    template<class T> inline static uint8_t serviceType_{};
public:
    template<class T> void Register(T& service) {
        for(const auto& entry:entries_)if(entry.type==&serviceType_<T>)Fail("World service already registered");
        entries_.push_back({&serviceType_<T>,&service});
    }
    template<class T> void Unregister(T& service) {
        std::erase_if(entries_,[&](const Entry& entry){return entry.type==&serviceType_<T>&&entry.service==&service;});
    }
    template<class T> T* Find() const noexcept {
        for(const auto& entry:entries_)if(entry.type==&serviceType_<T>)return static_cast<T*>(entry.service);
        return nullptr;
    }
    template<class T> T& Require() const {
        if(auto* service=Find<T>())return *service;
        Fail("Required world service is not registered");
    }
};

class WorldLayer {
public:
    virtual ~WorldLayer()=default;
    virtual void Load(WorldServices&)=0;
    virtual void Unload(WorldServices&)=0;
    virtual void Reset(WorldServices& services){Unload(services);Load(services);}
};

class WorldLifecycle {
    WorldServices services_;
    std::vector<WorldLayer*> persistent_;
    WorldLayer* active_{};
    uint64_t revision_{};
    bool Persistent(const WorldLayer& layer)const noexcept {
        return std::find(persistent_.begin(),persistent_.end(),&layer)!=persistent_.end();
    }
public:
    WorldLifecycle()=default;
    WorldLifecycle(const WorldLifecycle&)=delete;
    WorldLifecycle& operator=(const WorldLifecycle&)=delete;
    WorldServices& Services() noexcept {return services_;}
    const WorldServices& Services() const noexcept {return services_;}
    uint64_t Revision() const noexcept {return revision_;}
    WorldLayer* Active() const noexcept {return active_;}
    bool IsPersistent(const WorldLayer& layer) const noexcept {return Persistent(layer);}
    bool IsLoaded(const WorldLayer& layer) const noexcept {return active_==&layer||Persistent(layer);}
    void LoadPersistent(WorldLayer& layer) {
        if(active_==&layer)Fail("World layer cannot be active and persistent simultaneously");
        if(Persistent(layer))return;
        layer.Load(services_);persistent_.push_back(&layer);++revision_;
    }
    void UnloadPersistent(WorldLayer& layer) {
        auto it=std::find(persistent_.begin(),persistent_.end(),&layer);if(it==persistent_.end())return;
        layer.Unload(services_);persistent_.erase(it);++revision_;
    }
    void ResetPersistent(WorldLayer& layer) {
        if(!Persistent(layer))Fail("Persistent world layer is not loaded");
        layer.Reset(services_);++revision_;
    }
    void ReplaceActive(WorldLayer& layer) {
        if(Persistent(layer))Fail("World layer cannot be active and persistent simultaneously");
        if(active_==&layer){layer.Reset(services_);++revision_;return;}
        if(active_)active_->Unload(services_);
        active_=nullptr;
        layer.Load(services_);active_=&layer;++revision_;
    }
    void ResetActive() {
        if(!active_)Fail("No active world layer to reset");
        active_->Reset(services_);++revision_;
    }
    void UnloadActive() {
        if(!active_)return;
        auto* layer=active_;active_=nullptr;layer->Unload(services_);++revision_;
    }
    void UnloadAll() {
        if(active_){auto* layer=active_;active_=nullptr;layer->Unload(services_);++revision_;}
        while(!persistent_.empty()){auto* layer=persistent_.back();persistent_.pop_back();layer->Unload(services_);++revision_;}
    }
};
}
