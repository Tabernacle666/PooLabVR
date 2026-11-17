// GPU-only selection of immutable original or the newest validated baked mesh.
// Packed descriptor order: vertices, indices, ranges, triangle solids.
struct CutSourcePublication {
    uint mesh,solid,triangles,ready;
    uint capTriangles,projectileId,flags,renderInstance;
    uint originalMaterial,sourceFirstIndex,sourceIndexCount,additionalImpacts;
    uint requiredTriangles,requiredSegments,requiredPolygons,requiredCapVertices;
};
struct CutSourceChoice {
    uint vertices,indices,ranges,solids,mesh;
    uint count,published,slot;
};
CutSourceChoice SelectCutSource(uint generationDescriptor,uint instanceIndex,
                                uint originalMesh,uint pageCount,
                                uint4 originalDescriptors,uint4 bakedA,uint4 bakedB){
    CutSourceChoice choice;
    choice.vertices=originalDescriptors.x;
    choice.indices=originalDescriptors.y;
    choice.ranges=originalDescriptors.z;
    choice.solids=originalDescriptors.w;
    choice.mesh=originalMesh;
    choice.count=0;
    choice.published=0;
    choice.slot=0;
    if(generationDescriptor==0xffffffffu||instanceIndex>=pageCount)return choice;
    StructuredBuffer<CutSourcePublication> publication=
        ResourceDescriptorHeap[generationDescriptor];
    CutSourcePublication state=publication[instanceIndex];
    if(state.ready==0||state.renderInstance!=instanceIndex)return choice;
    uint4 descriptors=(state.flags&1u)!=0?bakedB:bakedA;
    choice.vertices=descriptors.x;
    choice.indices=descriptors.y;
    choice.ranges=descriptors.z;
    choice.solids=descriptors.w;
    choice.mesh=instanceIndex;
    choice.count=state.triangles;
    choice.published=1;
    choice.slot=state.flags&1u;
    return choice;
}
uint4 SelectCutDestination(uint generationDescriptor,uint instanceIndex,
                           uint pageCount,uint4 bakedA,uint4 bakedB){
    if(instanceIndex>=pageCount)return bakedA;
    StructuredBuffer<CutSourcePublication> publication=
        ResourceDescriptorHeap[generationDescriptor];
    CutSourcePublication state=publication[instanceIndex];
    return state.ready!=0&&((state.flags&1u)==0u)?bakedB:bakedA;
}
