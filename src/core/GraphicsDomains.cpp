#include "GraphicsInternal.h"

namespace lab {
        Graphics::Impl::MeshSolidDomain::MeshSolidDomain(Impl& value):owner(value){bindings.reserve(16);}

        GpuWorldDomain Graphics::Impl::MeshSolidDomain::Identity()const noexcept {return GpuDomainId::MeshSolid;}

        const char* Graphics::Impl::MeshSolidDomain::Name()const noexcept {return "MeshSolid";}

        void Graphics::Impl::MeshSolidDomain::BeginFrame(uint64_t){bindings.clear();total=owner.active?owner.active->cpuCount:0;}

        bool Graphics::Impl::MeshSolidDomain::ValidatePublication(const GpuPublication& p)const {
            if(!GpuDomain::ValidatePublication(p)||!p.consumerFence||!p.consumerValue)return false;
            auto* instances=p.Find(GpuMeshPublicationAbi::Instances);auto* metadata=p.Find(GpuMeshPublicationAbi::Metadata);auto* indirect=p.Find(GpuMeshPublicationAbi::IndirectArgs);
            for(const auto& resource:p.resources)if((resource.binding>=GpuMeshPublicationAbi::LightBase&&resource.binding<GpuMeshPublicationAbi::LightBase+MaxLights)){uint32_t index=resource.binding-GpuMeshPublicationAbi::LightBase;if(index>=owner.active->lightCount||resource.bytes!=sizeof(Light)||!(owner.active->lightData[index].parameters.z==5.f))return false;}
            return instances&&metadata&&indirect&&instances->type==GpuPublishedResourceType::Buffer&&metadata->type==GpuPublishedResourceType::Buffer&&indirect->type==GpuPublishedResourceType::Buffer&&instances->bytes%sizeof(GpuInstance)==0&&metadata->bytes==GpuMeshPublicationAbi::MetadataBytes&&indirect->bytes==GpuMeshPublicationAbi::IndirectBytes;
        }

        void Graphics::Impl::MeshSolidDomain::BindPublication(const GpuPublication& p){
            auto* instances=p.Find(GpuMeshPublicationAbi::Instances);uint64_t capacity=instances->bytes/sizeof(GpuInstance);if(capacity>UINT32_MAX||total+capacity>=Invalid)Fail("MeshSolid publication address space exhausted");bindings.push_back({p,uint32_t(total),uint32_t(capacity),uint32_t(bindings.size())});total+=capacity;
        }

        uint32_t Graphics::Impl::MeshSolidDomain::TotalInstances()const{if(total>=Invalid)Fail("MeshSolid instance total overflow");return uint32_t(total);}

        const std::vector<Graphics::Impl::MeshSolidDomain::Binding>& Graphics::Impl::MeshSolidDomain::Bindings()const{return bindings;}

        void Graphics::Impl::MeshSolidDomain::RecordCopies(void* context,ID3D12GraphicsCommandList7* list){
            auto& self=*static_cast<MeshSolidDomain*>(context);auto& f=*self.owner.active;
            for(const auto& bound:self.bindings){auto& p=bound.publication;auto* instances=p.Find(GpuMeshPublicationAbi::Instances);auto* metadata=p.Find(GpuMeshPublicationAbi::Metadata);auto* indirect=p.Find(GpuMeshPublicationAbi::IndirectArgs);list->CopyBufferRegion(f.instances.Get(),uint64_t(bound.base)*sizeof(GpuInstance),instances->resource.Get(),instances->offset,instances->bytes);uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes;list->CopyBufferRegion(f.streamControl.Get(),controlBase,metadata->resource.Get(),metadata->offset,metadata->bytes);list->CopyBufferRegion(f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::MetadataBytes,indirect->resource.Get(),indirect->offset,indirect->bytes);for(const auto& r:p.resources)if((r.binding>=GpuMeshPublicationAbi::LightBase&&r.binding<GpuMeshPublicationAbi::LightBase+MaxLights))list->CopyBufferRegion(f.lights.Get(),uint64_t(r.binding-GpuMeshPublicationAbi::LightBase)*sizeof(Light),r.resource.Get(),r.offset,r.bytes);}
        }

        void Graphics::Impl::MeshSolidDomain::BuildPasses(GpuPassBuilder& builder,const GpuFrameContext&){
            if(bindings.empty())return;auto& f=*owner.active;
            builder.BeginPass("MeshSolid.Publications",GpuPassQueue::Copy,{this,&RecordCopies});
            builder.Buffer(f.instances.Get(),GpuPassResourceAccess::ReadWrite,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            builder.Buffer(f.streamControl.Get(),GpuPassResourceAccess::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            bool lightWrite=false;for(const auto& b:bindings)for(const auto& r:b.publication.resources)lightWrite|=(r.binding>=GpuMeshPublicationAbi::LightBase&&r.binding<GpuMeshPublicationAbi::LightBase+MaxLights);
            // Upload establishes the CPU light slots; this dependent copy replaces
            // the reserved dynamic slots with the same GPU pose used by meshes.
            if(lightWrite)builder.Buffer(f.lights.Get(),GpuPassResourceAccess::ReadWrite,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            std::vector<ID3D12Resource*> declared;
            for(const auto& b:bindings)for(const auto& r:b.publication.resources){
                if(std::find(declared.begin(),declared.end(),r.resource.Get())!=declared.end())continue;
                declared.push_back(r.resource.Get());builder.Buffer(r.resource.Get(),GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);
            }
            builder.EndPass();
        }

        Graphics::Impl::DomainPassBuilder::DomainPassBuilder(Impl& o):owner(o){}

        void Graphics::Impl::DomainPassBuilder::SetDomain(GpuWorldDomain id){if(open)Fail("GPU domain changed with pass open");domainId=id;}

        GraphQueue Graphics::Impl::DomainPassBuilder::Queue(GpuPassQueue q){return q==GpuPassQueue::Copy?GraphQueue::Copy:q==GpuPassQueue::Compute?GraphQueue::Compute:GraphQueue::Direct;}

        GraphMode Graphics::Impl::DomainPassBuilder::Mode(GpuPassResourceAccess a){return a==GpuPassResourceAccess::Read?GraphMode::Read:a==GpuPassResourceAccess::Write?GraphMode::Write:GraphMode::ReadWrite;}

        uint32_t Graphics::Impl::DomainPassBuilder::NameHash(const char* name){uint32_t h=2166136261u;for(;*name;name++){h^=uint8_t(*name);h*=16777619u;}return h;}

        void Graphics::Impl::DomainPassBuilder::ProducerWaits(ID3D12Resource* resource){for(const auto& bound:owner.publications)for(const auto& published:bound.publication.resources)if(published.resource.Get()==resource){owner.frameGraph.ExternalWait(node,bound.publication.producerFence.Get(),bound.publication.producerValue);break;}}

        void Graphics::Impl::DomainPassBuilder::BeginPass(const char* name,GpuPassQueue queue,GpuPassCallback cb){if(open)Fail("GPU domain pass already open");if(!name||!*name||strlen(name)>=64)Fail("Invalid GPU domain pass name");auto q=Queue(queue);
            // EndPass raises the priority of compute that does not directly
            // wait on an external producer. Resource dependencies still hold
            // depth, lighting and other consumers until their inputs are ready.
            constexpr uint64_t AsyncComputeKey=55;
            uint64_t key=q==GraphQueue::Compute?AsyncComputeKey:
                (1ull<<63)|(uint64_t(domainId)<<32)|NameHash(name);
            node=owner.frameGraph.AddPass(name,q,key,[cb](const CompiledNode&,ID3D12GraphicsCommandList7* list){if(cb)cb.record(cb.context,list);});open=true;}

        void Graphics::Impl::DomainPassBuilder::Buffer(ID3D12Resource* resource,GpuPassResourceAccess access,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS barrierAccess,uint32_t contentVersion){if(!open)Fail("GPU domain buffer outside pass");owner.managedMemory.Pin(resource);ProducerWaits(resource);owner.frameGraph.Buffer(node,resource,Mode(access),sync,barrierAccess,sync,barrierAccess,contentVersion);}

        void Graphics::Impl::DomainPassBuilder::Texture(ID3D12Resource* resource,GpuPassResourceAccess access,D3D12_BARRIER_LAYOUT initialLayout,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS barrierAccess,GpuTextureSubresources range,uint32_t contentVersion){if(!open)Fail("GPU domain texture outside pass");owner.managedMemory.Pin(resource);ProducerWaits(resource);owner.frameGraph.Texture(node,owner.CanonicalTexture(resource),resource,initialLayout,Mode(access),layout,sync,barrierAccess,range,contentVersion);}

        void Graphics::Impl::DomainPassBuilder::EndPass(){if(!open)Fail("GPU domain pass not open");auto& pass=owner.frameGraph.Node(node);if(pass.queue==GraphQueue::Compute)owner.frameGraph.SetStableKey(node,DomainComputePriority(!pass.waits.empty()));open=false;node=UINT32_MAX;}

    TextureResource* Graphics::Impl::CanonicalTexture(ID3D12Resource* resource){
        if(!resource)return nullptr;for(auto* texture:{&hdr,&reflections,&depth,&hdrResolved,&reflectionsResolved,&hierarchy,&shadow})if(texture->resource.Get()==resource)return texture;for(auto& texture:history)if(texture.resource.Get()==resource)return &texture;for(auto& texture:historyDepth)if(texture.resource.Get()==resource)return &texture;return nullptr;
    }

    GpuDomain* Graphics::Impl::FindDomain(GpuWorldDomain id)const{for(auto* domain:domainRegistry)if(domain->Identity()==id)return domain;return nullptr;}

    void Graphics::Impl::RegisterDomain(std::shared_ptr<GpuDomain> domain){if(!domain||domain->Identity()==GpuDomainId::Invalid)Fail("Invalid GPU domain registration");if(FindDomain(domain->Identity()))Fail("Duplicate GPU domain registration");auto* raw=domain.get();externalDomains.push_back(std::move(domain));std::stable_sort(externalDomains.begin(),externalDomains.end(),[](const auto& a,const auto& b){return a->PresentationOrder()<b->PresentationOrder();});domainRegistry.push_back(raw);if(domainServicesReady)raw->Initialize(*this);LogEvent(0,"gpu.domain_registered","domain=%u name=%s",uint32_t(raw->Identity()),raw->Name());}

    bool Graphics::Impl::UnregisterDomain(GpuWorldDomain id){
        if(id==GpuDomainId::Invalid||id==meshDomain->Identity()||active)return false;
        auto it=std::find_if(externalDomains.begin(),externalDomains.end(),[&](const std::shared_ptr<GpuDomain>& d){return d&&d->Identity()==id;});
        if(it==externalDomains.end())return false;auto* raw=it->get();
        if(std::find_if(streams.begin(),streams.end(),[&](GpuStream* stream){return stream&&stream->Domain()==id;})!=streams.end())return false;
        publications.clear();publicationCatalog.Clear();std::erase(domainRegistry,raw);externalDomains.erase(it);
        LogEvent(0,"gpu.domain_unregistered","domain=%u",uint32_t(id));return true;
    }

    void Graphics::Impl::DetachStream(GpuStream& stream){if(active)Fail("GPU stream cannot detach while a frame is being prepared");WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);std::erase(streams,&stream);publications.clear();}

    void Graphics::Impl::ReserveStreams(){
        if(!reservedStreams.empty())Fail("GPU stream reservation from previous frame remains");
        for(auto* stream:streams){
            if(!stream)continue;
            auto publication=stream->ReserveForFrame();
            if(!publication)continue;
            if(publication.domain!=stream->Domain()||!publication.producerFence||!publication.producerValue||!publication.consumerFence||!publication.consumerValue)
                Fail("Reserved GPU publication missing its domain or fences");
            reservedStreams.push_back({stream,std::move(publication)});
        }
    }

    void Graphics::Impl::CancelReservedStreams(){
        for(auto& reserved:reservedStreams){
            if(!reserved.stream)continue;
            auto& p=reserved.publication;
            // Return an unused slot after its producer, without a CPU wait.
            Hr(copy.queue->Wait(p.producerFence.Get(),p.producerValue),"cancelled publication producer wait");
            Hr(copy.queue->Signal(p.consumerFence.Get(),p.consumerValue),"cancelled publication consumer signal");
            reserved.stream->Release(p);reserved.stream=nullptr;
        }
        reservedStreams.clear();
    }
}
