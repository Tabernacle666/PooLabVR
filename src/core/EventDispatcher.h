#pragma once
#include "Core.h"
namespace lab {
// Registration happens at startup; dispatch performs no allocation.
template<class Event>class EventDispatcher {
    struct Handler {uint32_t identity;void* context;void (*invoke)(void*,const Event&);};
    std::vector<Handler> handlers_;
public:
    void Register(uint32_t identity,void* context,void (*invoke)(void*,const Event&)){
        for(const auto& handler:handlers_)if(handler.identity==identity)Fail("Event handler already registered");
        handlers_.push_back({identity,context,invoke});
    }
    bool Dispatch(uint32_t identity,const Event& event)const{
        for(const auto& handler:handlers_)if(handler.identity==identity){handler.invoke(handler.context,event);return true;}
        return false;
    }
    void Clear(){handlers_.clear();}
};
}
