#include "ClientLayers.h"
namespace lab {
void ClientLayers::Set(RenderContributions persistent,RenderContributions active){persistent_=persistent;active_=active;materials_.clear();materials_.insert(materials_.end(),persistent.streamMaterials.begin(),persistent.streamMaterials.end());materials_.insert(materials_.end(),active.streamMaterials.begin(),active.streamMaterials.end());}
void ClientLayers::Append(void* context,RenderScene& scene,const FrameInput& input,double time){auto& self=*static_cast<ClientLayers*>(context);auto append=[&](RenderContributions& c){if(c.appendContext)c.appendContext(c.context,scene,input,time);};append(self.persistent_);append(self.active_);}
void ClientLayers::Light(void* context,SceneLight& light,double time){auto& self=*static_cast<ClientLayers*>(context);auto apply=[&](RenderContributions& c){if(c.lightContext)c.lightContext(c.context,light,time);};apply(self.persistent_);apply(self.active_);}
RenderContributions ClientLayers::Contributions()const{RenderContributions out;out.streamMaterials=materials_;out.context=const_cast<ClientLayers*>(this);out.appendContext=Append;out.lightContext=Light;return out;}
}
