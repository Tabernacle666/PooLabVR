#include "ShuttleVisuals.h"
#include "ShuttlePilot.h"
#include "VrInputMapping.h"

#include <charconv>
#include <iterator>
#include <string_view>

namespace OASIS {
namespace {
constexpr uint32_t FirstId=0x50000000u;
constexpr XMFLOAT3 Parking{2.10f,.020f+.925f*SpectacleInteriorUnitsPerMeter,4.42f};

uint32_t AppendRigidMeshes(SceneData& scene,SceneData& source) {
    auto& dst=scene.meshes;
    auto& src=source.meshes;
    const uint32_t mesh=uint32_t(dst.ranges.size()),vertex=uint32_t(dst.vertices.size());
    const uint32_t indices=uint32_t(dst.indices.size()),meshlet=uint32_t(dst.meshlets.size());
    const uint32_t remap=uint32_t(dst.remap.size()),triangle=uint32_t(dst.triangles.size());
    if(!source.animations.empty()||!source.bones.empty()||!src.morphs.empty())
        Fail("Shuttle catalogue must contain rigid geometry");
    for(auto& range:src.ranges){
        range.firstVertex+=vertex;range.firstIndex+=indices;range.firstMeshlet+=meshlet;
    }
    for(auto& item:src.meshlets){item.vertexOffset+=remap;item.triangleOffset+=triangle;}
    for(auto& index:src.indices)index+=vertex;
    for(auto& index:src.remap)index+=vertex;
    auto append=[](auto& dest,auto& data){dest.insert(dest.end(),data.begin(),data.end());};
    append(dst.vertices,src.vertices);append(dst.indices,src.indices);
    append(dst.remap,src.remap);append(dst.triangles,src.triangles);
    append(dst.meshlets,src.meshlets);append(dst.ranges,src.ranges);
    append(dst.deformations,src.deformations);append(dst.motions,src.motions);
    dst.skin.resize(dst.vertices.size());
    if(!scene.triangleSolids.empty())scene.triangleSolids.resize(dst.indices.size()/3u,Invalid);
    return mesh;
}

uint32_t AppendMaterials(SceneData& scene,SceneData& source) {
    const uint32_t material=uint32_t(scene.materials.size()),texture=uint32_t(scene.textures.size());
    const uint32_t sampler=uint32_t(scene.samplers.size());
    for(auto& item:source.materials){
        for(auto* handle:{&item.albedo,&item.normal,&item.metalRough,&item.emissive,&item.occlusion})
            if(*handle!=Invalid)*handle+=texture;
        for(auto& handle:item.samplers)if(handle!=Invalid)handle+=sampler;
        scene.materials.push_back(item);
    }
    scene.materialNames.insert(scene.materialNames.end(),source.materialNames.begin(),source.materialNames.end());
    scene.samplers.insert(scene.samplers.end(),source.samplers.begin(),source.samplers.end());
    scene.textures.insert(scene.textures.end(),std::make_move_iterator(source.textures.begin()),
                          std::make_move_iterator(source.textures.end()));
    return material;
}

XMMATRIX At(XMFLOAT3 target,FXMMATRIX base) {
    auto model=base;
    model.r[3]=XMVectorSet(target.x,target.y,target.z,1.f);
    return model;
}

XMMATRIX HandModel(const Pose& palm,float units) {
    // The imported handle's Y is its long axis, and the OpenXR grip Z is thumb-up.
    constexpr XMFLOAT3 center{-.004f,.193f,0.f};
    const auto toGrip=XMMatrixSet(0,-1,0,0, 0,0,1,0, -1,0,0,0, 0,0,0,1);
    return XMMatrixTranslation(-center.x,-center.y,-center.z)*toGrip*
           XMMatrixScaling(units,units,units)*Matrix(palm);
}
}

ShuttleAssets::ShuttleAssets(SceneData& scene) {
    auto catalog=LoadSceneGLTF(AssetPath(L"navette_organes.glb"),true);
    const auto mesh=AppendRigidMeshes(scene,catalog),material=AppendMaterials(scene,catalog);
    for(auto object:catalog.objects){
        const auto& name=catalog.meshNodeNames[object.mesh];
        constexpr std::string_view prefix="Organe_";
        if(!name.starts_with(prefix))Fail("Shuttle catalogue module name");
        uint32_t module{};
        const auto result=std::from_chars(name.data()+prefix.size(),name.data()+name.size(),module);
        if(result.ec!=std::errc{}||result.ptr!=name.data()+name.size()||module>=modules_.size())
            Fail("Shuttle catalogue module index");
        object.mesh+=mesh;
        object.material+=material;
        object.flags=DynamicObject|NoCollision|NoRayTracing;
        modules_[module].push_back(object);
    }
    for(const auto& module:modules_)if(module.empty())Fail("Shuttle catalogue module missing");

    // The NAV display's marks are simple shared quads inside the glass.
    auto& m=scene.meshes;
    const uint32_t v=uint32_t(m.vertices.size()),index=uint32_t(m.indices.size()),
        remap=uint32_t(m.remap.size()),triangle=uint32_t(m.triangles.size()),
        meshlet=uint32_t(m.meshlets.size());
    for(const auto& p:std::array<XMFLOAT3,4>{{{0,-1,-1},{0,-1,1},{0,1,1},{0,1,-1}}})
        m.vertices.push_back({p,{-1,0,0},{}});
    m.indices.insert(m.indices.end(),{v,v+1u,v+2u,v,v+2u,v+3u});
    m.remap.insert(m.remap.end(),{v,v+1u,v+2u,v+3u});
    m.triangles.insert(m.triangles.end(),{{0,1,2},{0,2,3}});
    m.meshlets.push_back({remap,triangle,4,2});
    mapDot_.mesh=uint32_t(m.ranges.size());
    m.ranges.push_back({meshlet,1,v,4,index,6,{0,0,0,1.415f}});
    m.deformations.emplace_back();m.motions.emplace_back();m.skin.resize(m.vertices.size());
    if(!scene.triangleSolids.empty())scene.triangleSolids.resize(m.indices.size()/3u,Invalid);
    mapDot_.flags=DynamicObject|NoCollision|NoRayTracing|NoShadow;
    constexpr std::array<XMFLOAT4,3> colors{{{.27f,.77f,.29f,0},{1.f,.58f,.18f,0},{.24f,.78f,1.f,0}}};
    for(uint32_t i=0;i<colors.size();++i){
        mapMaterials_[i]=uint32_t(scene.materials.size());
        AssetMaterial ink{};
        ink.colorRoughness={0,0,0,.8f};ink.emissionMetallic=colors[i];
        ink.doubleSided=ink.unlit=1;
        scene.materials.push_back(ink);scene.materialNames.emplace_back("Navette / carte");
    }
}

void ShuttleAssets::Append(RenderScene& scene,const FrameInput& input,const ShuttlePilot& pilot,
                           const VrInputActions& actions) const {
    if(input.solarTime<0.)return;
    const bool occupied=pilot.Active();
    const auto oasis=SolarBody(0u,input.solarTime);
    const auto oasisToCosmos=SolarBodyToCosmos(0u,oasis);
    // The parked instance is relevant on OASIS and on the final approach to it.
    if(!occupied&&input.insideSphere!=0u&&
       (input.insideSphere!=Invalid||Length(Sub(input.head.p,oasis.p))>SolarBodyRadius(0u)+.95f))return;
    const float u=SpectacleInteriorUnitsPerMeter;
    const auto root=occupied?pilot.ModelToWorld():XMMatrixScaling(u,u,u)*XMMatrixRotationY(-XM_PIDIV2)*
        XMMatrixTranslation(Parking.x,Parking.y,Parking.z)*oasisToCosmos;
    uint32_t id=FirstId;
    auto append=[&](RenderObject object,FXMMATRIX transform,bool crt=false,uint32_t page=0u){
        XMVECTOR scale,rotation,position;
        if(!XMMatrixDecompose(&scale,&rotation,&position,transform))return;
        XMStoreFloat3(&object.scale,scale);
        XMStoreFloat3(&object.current.p,position);
        XMStoreFloat4(&object.current.q,rotation);
        object.previous=object.current;
        object.id=id++;
        if(crt){
            object.flags|=ShuttleCrt|NoShadow;
            object.visualPose=occupied?pilot.DisplayData(page):0u;
        }
        scene.objects.push_back(object);
    };
    for(const auto module:ShuttleLayout::HullModules)
        for(auto object:modules_[module])append(object,root);

    const auto& hands=input.State(actions.handPose).pose;
    std::array<XMFLOAT4X4,2> handles{};
    if(occupied)for(uint32_t hand=0;hand<2;++hand)if(hands[hand].tracked)
        XMStoreFloat4x4(&handles[hand],HandModel(hands[hand].pose,input.sceneUnitsPerMeter));

    std::array<XMFLOAT4X4,ShuttleLayout::Parts.size()> transforms;
    XMFLOAT4X4 nav{};
    bool mapVisible=false;
    for(uint32_t i=0;i<ShuttleLayout::Parts.size();++i){
        const auto& part=ShuttleLayout::Parts[i];
        const auto parent=part.parent==ShuttleLayout::None?root:XMLoadFloat4x4(&transforms[part.parent]);
        auto model=XMMatrixScaling(part.scale.x,part.scale.y,part.scale.z)*Matrix(part.local)*parent;
        if(occupied){
            using ShuttleLayout::Motion;
            const auto slot=part.slot;
            switch(part.motion){
                case Motion::Nozzle:
                    model=XMMatrixRotationY(-float(pilot.Loads()[slot].gimbalRad))*model;break;
                case Motion::Rotor:
                    model=XMMatrixRotationY(-float(pilot.Loads()[slot].rotorPhaseRad))*model;break;
                case Motion::Blade:{
                    const auto sign=Vaisseaux::Lunaire::Mounts[slot].direction;
                    model=XMMatrixRotationX(-float(pilot.Loads()[slot].pitchRad)*float(sign))*model;break;
                }
                case Motion::Pedestal:
                    if(hands[slot].tracked)
                        model=At(FromMatrix(XMLoadFloat4x4(&handles[slot])).p,model);
                    break;
                case Motion::Grip:
                    if(hands[slot].tracked)model=XMLoadFloat4x4(&handles[slot]);
                    break;
                case Motion::Brake:
                    model=XMMatrixRotationX((slot==0u?1.f:-1.f)*pilot.Brake(slot)*.85f)*model;break;
                case Motion::Panel:
                    if(slot==0u){XMStoreFloat4x4(&nav,model);mapVisible=true;}break;
                default:break;
            }
        }
        XMStoreFloat4x4(&transforms[i],model);
        if(part.module!=ShuttleLayout::None)for(auto object:modules_[part.module])
            append(object,model,part.motion==ShuttleLayout::Motion::Crt,part.slot);
    }
    if(mapVisible){
        const auto panel=XMLoadFloat4x4(&nav);
        constexpr double orbitRadius=FirstSolarOrbit+double(SolarPlanetCount-1)*SolarOrbitSpacing;
        auto dot=[&](Vaisseaux::MapDot p,float size,uint32_t color){
            const float x=182.f+float(std::clamp(p.x/orbitRadius,-1.,1.))*53.f;
            const float y=82.f-float(std::clamp(p.y/orbitRadius,-1.,1.))*36.f;
            const float lateral=(.5f-x/256.f)*.251f;
            const float vertical=(.5f-y/128.f)*.153f;
            auto object=mapDot_;object.material=mapMaterials_[color];
            append(object,XMMatrixScaling(1.f,size,size)*XMMatrixTranslation(-.079f,vertical,lateral)*panel);
        };
        dot({},.0018f,1u);
        for(uint32_t body=0;body<SolarBodyCount;++body)
            dot(pilot.Map().dots[body],body==pilot.Map().activeBody?.0021f:.0013f,
                body==pilot.Map().activeBody?1u:0u);
        dot(pilot.Map().craft,.0020f,2u);
    }
    scene.count=uint32_t(scene.objects.size());
}

RenderContributions ShuttleAssets::Contribution::Bind(){
    auto result=base;
    result.shuttleCrt=true;
    result.context=this;
    result.appendContext=[](void* context,RenderScene& scene,const FrameInput& input,double time){
        const auto& c=*static_cast<Contribution*>(context);
        if(c.base.appendContext)c.base.appendContext(c.base.context,scene,input,time);
        c.assets.Append(scene,input,c.pilot,StandardVrActions());
    };
    if(base.lightContext)result.lightContext=[](void* context,SceneLight& light,double time){
        const auto& c=*static_cast<Contribution*>(context);
        c.base.lightContext(c.base.context,light,time);
    };
    if(base.residentTiles)result.residentTiles=[](void* context){
        const auto& c=*static_cast<Contribution*>(context);
        return c.base.residentTiles(c.base.context);
    };
    return result;
}
}
