#include "../../src/core/FrameGraphDependencies.h"
#include <algorithm>
#include <cassert>
#include <iostream>

int main(){
    using namespace lab;
    FrameGraphDependencies graph;
    int instances{},animation{},bounds{},depth{},snow{};
    const auto upload=graph.AddPass("Upload",GraphQueue::Copy,10);
    const auto deform=graph.AddPass("Deform",GraphQueue::Compute,15);
    const auto publication=graph.AddPass("MeshSolid.Publications",GraphQueue::Copy,100);
    const auto cull=graph.AddPass("DeformCull",GraphQueue::Compute,30);
    const auto depthPass=graph.AddPass("Depth",GraphQueue::Direct,40);
    const auto fog=graph.AddPass("Atmosphere.Advection",GraphQueue::Compute,55);
    const auto fogIntegrate=graph.AddPass("Atmosphere.Integrate",GraphQueue::Compute,55);
    const auto clear=graph.AddPass("Snow.Clear",GraphQueue::Compute,55);
    const auto splat=graph.AddPass("Snow.Splat",GraphQueue::Compute,55);
    assert(graph.SetStableKey(fog,DomainComputePriority(false)));
    assert(graph.SetStableKey(fogIntegrate,DomainComputePriority(false)));
    assert(graph.SetStableKey(clear,DomainComputePriority(false)));
    assert(graph.SetStableKey(splat,DomainComputePriority(true)));
    assert(graph.Buffer(upload,&instances,GraphMode::Write));
    assert(graph.Buffer(upload,&animation,GraphMode::Write));
    assert(graph.Buffer(deform,&animation,GraphMode::Read));
    assert(graph.Buffer(deform,&bounds,GraphMode::Write));
    assert(graph.Buffer(publication,&instances,GraphMode::ReadWrite));
    assert(graph.Buffer(cull,&instances,GraphMode::Read));
    assert(graph.Buffer(cull,&bounds,GraphMode::Read));
    assert(graph.Buffer(depthPass,&instances,GraphMode::Read));
    assert(graph.Dependency(cull,depthPass));
    assert(graph.Buffer(depthPass,&depth,GraphMode::Write));
    assert(graph.Buffer(fogIntegrate,&depth,GraphMode::Read));
    assert(graph.Buffer(clear,&snow,GraphMode::Write));
    assert(graph.Buffer(splat,&depth,GraphMode::Read));
    assert(graph.Buffer(splat,&snow,GraphMode::ReadWrite));
    assert(graph.Resolve());
    const auto& order=graph.ResolvedOrder();
    const auto before=[&](uint32_t a,uint32_t b){
        return std::find(order.begin(),order.end(),a)<std::find(order.begin(),order.end(),b);
    };
    assert(before(upload,deform)&&before(deform,publication));
    assert(before(fog,publication)&&before(clear,publication));
    assert(before(publication,cull)&&before(cull,depthPass));
    assert(before(depthPass,fogIntegrate));
    assert(before(depthPass,splat)&&before(clear,splat));
    std::cout<<"independent GPU work precedes publication waits: PASS\n";
}
