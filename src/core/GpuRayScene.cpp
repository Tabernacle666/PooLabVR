#include "GpuRayScene.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace lab {
namespace {
uint32_t Bits(float value) { return std::bit_cast<uint32_t>(value); }
}

GpuRayScene::Key GpuRayScene::MakeKey(const GpuInstance& i) {
    Key key{};
    if (i.flags & GpuRayNoTracingFlag) {
        key.bits[13] = GpuRayNoTracingFlag;
        return key;
    }
    key.bits[0]=Bits(i.positionRadius.x);key.bits[1]=Bits(i.positionRadius.y);key.bits[2]=Bits(i.positionRadius.z);
    key.bits[3]=Bits(i.rotation.x);key.bits[4]=Bits(i.rotation.y);key.bits[5]=Bits(i.rotation.z);key.bits[6]=Bits(i.rotation.w);
    key.bits[7]=Bits(i.scale.x);key.bits[8]=Bits(i.scale.y);key.bits[9]=Bits(i.scale.z);
    key.bits[10]=i.mesh;key.bits[11]=i.material;
    // Only flags consumed by RayBuildMain belong to the ray descriptor revision.
    key.bits[12]=i.flags & (1u|2u|4u|GpuRayNoTracingFlag);
    key.bits[13]=i.id;
    key.bits[15]=0x52545931u; // "RTY1": prevents an enabled all-zero key matching NoRT.
    return key;
}

void GpuRayScene::MergeRange(std::vector<GpuRayDirtyRange>& ranges, GpuRayDirtyRange range) {
    if (!range.count) return;
    uint64_t first=range.first,end=first+range.count;
    auto it=ranges.begin();
    while(it!=ranges.end()&&uint64_t(it->first)+it->count<first)++it;
    while(it!=ranges.end()&&uint64_t(it->first)<=end){first=std::min<uint64_t>(first,it->first);end=std::max<uint64_t>(end,uint64_t(it->first)+it->count);it=ranges.erase(it);}
    ranges.insert(it,{uint32_t(first),uint32_t(end-first)});
}
void GpuRayScene::MarkRange(uint32_t first,uint32_t count){for(auto& frame:frames_)MergeRange(frame.cpuDirty,{first,count});}
void GpuRayScene::ClipRanges(uint32_t count){
    for(auto& frame:frames_){auto& ranges=frame.cpuDirty;for(auto it=ranges.begin();it!=ranges.end();){if(it->first>=count){it=ranges.erase(it);continue;}uint64_t end=std::min<uint64_t>(uint64_t(it->first)+it->count,count);it->count=uint32_t(end-it->first);++it;}}
}

void GpuRayScene::Reset(){staticCount_=0;staticGeneration_=0;keys_.clear();for(auto& frame:frames_)frame={};}
void GpuRayScene::ReplaceStatic(std::span<const GpuInstance> instances){
    staticCount_=uint32_t(instances.size());keys_.resize(instances.size());for(uint32_t i=0;i<instances.size();++i)keys_[i]=MakeKey(instances[i]);
    uint64_t hash=1469598103934665603ull;for(uint32_t i=0;i<staticCount_;++i)for(uint32_t word:keys_[i].bits){for(uint32_t b=0;b<4;++b){hash^=uint8_t(word>>(b*8));hash*=1099511628211ull;}}staticGeneration_=hash?hash:1ull;
    for(auto& frame:frames_){frame.cpuDirty.clear();if(!instances.empty())frame.cpuDirty.push_back({0,uint32_t(instances.size())});frame.cpuTopologyDirty=true;frame.publications.clear();frame.stagedPublications.clear();frame.publicationDirty.clear();frame.publicationTopologyDirty=true;}
}
void GpuRayScene::UpdateDynamic(uint32_t staticCount,std::span<const GpuInstance> instances){
    if(staticCount!=staticCount_)return;
    uint32_t oldCount=uint32_t(keys_.size()),newCount=staticCount_+uint32_t(instances.size());
    if(oldCount!=newCount)for(auto& frame:frames_)frame.cpuTopologyDirty=true;
    if(newCount<oldCount){keys_.resize(newCount);ClipRanges(newCount);}else if(newCount>oldCount)keys_.resize(newCount);
    uint32_t runFirst=0,runCount=0;auto flush=[&]{if(runCount){MarkRange(runFirst,runCount);runCount=0;}};
    for(uint32_t i=0;i<instances.size();++i){uint32_t index=staticCount_+i;Key next=MakeKey(instances[i]);bool fresh=index>=oldCount;if(!fresh&&keys_[index]==next){flush();continue;}keys_[index]=next;if(!runCount)runFirst=index;++runCount;}
    flush();
}
void GpuRayScene::InvalidateFrame(uint32_t slot){
    if(slot>=FrameSlots)return;
    auto& frame=frames_[slot];frame.cpuDirty.clear();if(!keys_.empty())frame.cpuDirty.push_back({0,uint32_t(keys_.size())});frame.cpuTopologyDirty=true;frame.publications.clear();frame.stagedPublications.clear();frame.publicationDirty.clear();frame.publicationTopologyDirty=true;
}
void GpuRayScene::StagePublications(uint32_t slot,std::span<const GpuRayPublicationState> publications){
    if(slot>=FrameSlots)return;
    auto& frame=frames_[slot];frame.stagedPublications.assign(publications.begin(),publications.end());frame.publicationDirty.clear();
    if(frame.publications.size()!=publications.size())frame.publicationTopologyDirty=true;
    for(uint32_t i=0;i<publications.size();++i){bool changed=i>=frame.publications.size()||frame.publications[i].generation!=publications[i].generation||frame.publications[i].base!=publications[i].base||frame.publications[i].capacity!=publications[i].capacity;if(changed)frame.publicationDirty.push_back(i);if(i>=frame.publications.size()||frame.publications[i].base!=publications[i].base||frame.publications[i].capacity!=publications[i].capacity)frame.publicationTopologyDirty=true;}
}
void GpuRayScene::MarkGeometryDirty(uint32_t slot){if(slot<FrameSlots)frames_[slot].geometryDirty=true;}
std::span<const GpuRayDirtyRange> GpuRayScene::PendingCpu(uint32_t slot)const{return slot<FrameSlots?std::span<const GpuRayDirtyRange>(frames_[slot].cpuDirty):std::span<const GpuRayDirtyRange>{};}
std::span<const uint32_t> GpuRayScene::PendingPublications(uint32_t slot)const{return slot<FrameSlots?std::span<const uint32_t>(frames_[slot].publicationDirty):std::span<const uint32_t>{};}
bool GpuRayScene::NeedsTlas(uint32_t slot)const{if(slot>=FrameSlots)return true;const auto& f=frames_[slot];return f.cpuTopologyDirty||f.publicationTopologyDirty||f.geometryDirty||!f.cpuDirty.empty()||!f.publicationDirty.empty();}
void GpuRayScene::Commit(uint32_t slot){if(slot>=FrameSlots)return;auto& f=frames_[slot];f.cpuDirty.clear();f.cpuTopologyDirty=false;f.publications=f.stagedPublications;f.publicationDirty.clear();f.publicationTopologyDirty=false;f.geometryDirty=false;}
}
