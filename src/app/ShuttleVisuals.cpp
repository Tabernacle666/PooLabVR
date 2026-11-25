#include "AppSession.h"
#include "PhysicalMaterials.h"
#include "ShuttleVisuals.h"
#include "ShuttlePilot.h"
#include "VrInputMapping.h"

#include <charconv>
#include <iterator>
#include <string_view>

namespace OASIS {
namespace {
constexpr uint32_t FirstId=0x50000000u;
constexpr uint32_t FirstFlameId=FirstId+0x10000u;
constexpr uint32_t FirstWakeId=FirstId+0x20000u;

XMMATRIX At(XMFLOAT3 target,FXMMATRIX base) {
    auto model=base;
    model.r[3]=XMVectorSet(target.x,target.y,target.z,1.f);
    return model;
}

XMMATRIX WakeBillboard(XMFLOAT3 position,XMFLOAT3 direction,const Pose& viewer,float width,float length) {
    const auto normal=Unit(Sub(position,viewer.p));
    auto up=Sub(direction,Scale(normal,Dot(direction,normal)));
    if(Length(up)<.04f){
        const auto viewUp=TransformDirection({0,1,0},Matrix(viewer));
        up=Sub(viewUp,Scale(normal,Dot(viewUp,normal)));
        if(Length(up)<.04f){
            const auto axis=std::abs(normal.x)<.8f?XMFLOAT3{1,0,0}:XMFLOAT3{0,0,1};
            up=Sub(axis,Scale(normal,Dot(axis,normal)));
        }
    }
    up=Unit(up);
    XMFLOAT3 side;
    XMStoreFloat3(&side,XMVector3Cross(XMLoadFloat3(&normal),XMLoadFloat3(&up)));
    return XMMatrixSet(normal.x,normal.y,normal.z,0,up.x*length,up.y*length,up.z*length,0,
        side.x*width,side.y*width,side.z*width,0,position.x,position.y,position.z,1);
}

XMMATRIX HandModel(XMFLOAT3 palm,XMFLOAT3 angles,FXMMATRIX root) {
    // Keep the palm position and draw the handle upright at its calibrated rest pose.
    constexpr XMFLOAT3 center{-.004f,.193f,0.f};
    return XMMatrixTranslation(-center.x,-center.y,-center.z)*
           XMMatrixRotationY(angles.z)*XMMatrixRotationZ(-angles.x)*
           XMMatrixRotationX(angles.y)*At(palm,root);
}

RenderObject MakeSpeedDust(SceneData& scene) {
    constexpr uint32_t Count=384,GroupSize=21;
    auto& mesh=scene.meshes;
    const uint32_t firstVertex=uint32_t(mesh.vertices.size()),firstIndex=uint32_t(mesh.indices.size());
    const uint32_t firstMeshlet=uint32_t(mesh.meshlets.size());
    uint32_t seed=0xe87856dbu;
    auto random=[&]{
        seed=1664525u*seed+1013904223u;
        return float(seed>>8u)*(1.f/16777216.f);
    };
    constexpr std::array<XMFLOAT2,3> corners{{{-.5f,-1.f},{-.5f,1.f},{.5f,0.f}}};
    for(uint32_t start=0;start<Count;start+=GroupSize){
        const uint32_t size=std::min(GroupSize,Count-start);
        mesh.meshlets.push_back({uint32_t(mesh.remap.size()),uint32_t(mesh.triangles.size()),size*3u,size});
        for(uint32_t particle=0;particle<size;++particle){
            const XMFLOAT3 point{40.f*(random()-.5f),18.f*(random()-.5f),18.f*(random()-.5f)};
            const float gate=random();
            for(const auto& corner:corners){
                const uint32_t index=uint32_t(mesh.vertices.size());
                mesh.vertices.push_back({point,{0,1,0},corner,{gate,0.f}});
                mesh.indices.push_back(index);
                mesh.remap.push_back(index);
            }
            mesh.triangles.push_back({particle*3u,particle*3u+1u,particle*3u+2u});
        }
    }
    RenderObject object;
    object.mesh=uint32_t(mesh.ranges.size());
    object.material=uint32_t(scene.materials.size());
    object.flags=DynamicObject|NoCollision|NoRayTracing|NoShadow|ShuttleSpeedDust;
    mesh.ranges.push_back({firstMeshlet,uint32_t(mesh.meshlets.size())-firstMeshlet,
        firstVertex,Count*3u,firstIndex,Count*3u,{0,0,0,30.f}});
    mesh.deformations.emplace_back();mesh.motions.emplace_back();mesh.skin.resize(mesh.vertices.size());
    if(!scene.triangleSolids.empty())scene.triangleSolids.resize(mesh.indices.size()/3u,Invalid);
    AssetMaterial material{};
    material.colorRoughness={0,0,0,1};
    material.emissionMetallic={.68f,.94f,1.30f,0};
    material.alphaMode=2u;material.alpha=.62f;
    material.doubleSided=material.unlit=1u;
    ApplyPhysicalMaterial(material,"medium.aerosol");
    scene.materials.push_back(material);
    scene.materialNames.emplace_back("Navette / poussiere fine");
    return object;
}

std::array<RenderObject,2> MakeFlame(SceneData& scene) {
    constexpr uint32_t sides=8;
    auto& m=scene.meshes;
    const uint32_t vertex=uint32_t(m.vertices.size()),index=uint32_t(m.indices.size()),
        meshlet=uint32_t(m.meshlets.size()),remap=uint32_t(m.remap.size()),triangles=uint32_t(m.triangles.size());
    constexpr std::array<XMFLOAT2,3> rings{{{0.f,.47f},{-.29f,1.f},{-.73f,.31f}}};
    for(const auto ring:rings)for(uint32_t i=0;i<sides;++i){
        const float angle=XM_2PI*float(i)/float(sides);
        const XMFLOAT3 normal{0.f,std::cos(angle),std::sin(angle)};
        m.vertices.push_back({{ring.x,ring.y*normal.y,ring.y*normal.z},normal,{-ring.x,.5f}});
    }
    const uint32_t tip=uint32_t(rings.size())*sides;
    m.vertices.push_back({{-1.f,0.f,0.f},{-1.f,0.f,0.f},{1.f,.5f}});
    for(uint32_t i=0;i<=tip;++i)m.remap.push_back(vertex+i);
    auto triangle=[&](uint32_t a,uint32_t b,uint32_t c){
        m.triangles.push_back({a,b,c});
        m.indices.insert(m.indices.end(),{vertex+a,vertex+b,vertex+c});
    };
    for(uint32_t i=0;i<sides;++i){
        const uint32_t next=(i+1u)%sides;
        for(uint32_t ring=0;ring+1u<rings.size();++ring){
            const uint32_t curr=ring*sides,a=curr+i,b=curr+next;
            triangle(a,b,b+sides);
            triangle(a,b+sides,a+sides);
        }
        triangle(tip-sides+i,tip-sides+next,tip);
    }
    const auto count=uint32_t(m.triangles.size())-triangles;
    const uint32_t mesh=uint32_t(m.ranges.size());
    m.meshlets.push_back({remap,triangles,tip+1u,count});
    m.ranges.push_back({meshlet,1u,vertex,tip+1u,index,count*3u,{-.5f,0.f,0.f,1.2f}});
    m.deformations.emplace_back();m.motions.emplace_back();
    m.skin.resize(m.vertices.size());
    if(!scene.triangleSolids.empty())scene.triangleSolids.resize(m.indices.size()/3u,Invalid);

    TextureData gradient{};
    gradient.width=64;gradient.height=gradient.mips=1;
    gradient.format=DXGI_FORMAT_R8G8B8A8_UNORM;gradient.offsets={0};
    constexpr std::array<XMFLOAT3,5> stops{{{.42f,.66f,1.f},{1.f,.93f,.71f},
        {1.f,.46f,.096f},{.44f,.062f,.008f},{.16f,.014f,.002f}}};
    for(uint32_t i=0;i<gradient.width;++i){
        const float t=float(i)*float(stops.size()-1u)/float(gradient.width-1u);
        const size_t segment=std::min(size_t(t),stops.size()-2u);
        const auto& a=stops[segment];const auto& b=stops[segment+1u];
        for(float channel:{std::lerp(a.x,b.x,t-float(segment)),
            std::lerp(a.y,b.y,t-float(segment)),std::lerp(a.z,b.z,t-float(segment))})
            gradient.bytes.push_back(std::byte(uint8_t(std::lround(channel*255.f))));
        gradient.bytes.push_back(std::byte{255});
    }
    const uint32_t texture=uint32_t(scene.textures.size());
    scene.textures.push_back(std::move(gradient));
    const uint32_t sampler=uint32_t(scene.samplers.size());
    scene.samplers.push_back({9729,9729,33071,33071});
    constexpr std::array<XMFLOAT4,2> colors{{{1.8f,.84f,.35f,.27f},{5.f,4.f,3.8f,1.f}}};
    std::array<RenderObject,2> result{};
    for(size_t layer=0;layer<result.size();++layer){
        auto& object=result[layer];
        const auto color=colors[layer];
        object.mesh=mesh;
        object.material=uint32_t(scene.materials.size());
        object.flags=DynamicObject|NoCollision|NoRayTracing|NoShadow;
        AssetMaterial material{};
        material.colorRoughness={0.f,0.f,0.f,1.f};
        material.emissionMetallic={color.x,color.y,color.z,0.f};
        material.alpha=color.w;
        // A thin, opaque core supplies depth through the canopy against open space.
        material.alphaMode=layer==0u?2u:0u;
        material.emissive=texture;
        material.samplers[3]=sampler;
        material.doubleSided=material.unlit=1u;
        ApplyPhysicalMaterial(material,"medium.plasma");
        scene.materials.push_back(material);
        scene.materialNames.emplace_back("Navette / flamme");
    }
    return result;
}
}

void ShuttleAssets::BuildWakeMaterials(SceneData& scene) {
    constexpr uint32_t Size=32;
    const uint32_t sampler=uint32_t(scene.samplers.size());
    scene.samplers.push_back({9729,9729,33071,33071});
    const uint32_t firstTexture=uint32_t(scene.textures.size());
    for(uint32_t kind=0;kind<2u;++kind){
        TextureData texture{};
        texture.width=texture.height=Size;texture.mips=1;
        texture.format=DXGI_FORMAT_R8G8B8A8_UNORM;texture.offsets={0};
        texture.bytes.reserve(Size*Size*4u);
        for(uint32_t y=0;y<Size;++y)for(uint32_t x=0;x<Size;++x){
            const float u=2.f*(float(x)+.5f)/float(Size)-1.f;
            const float v=2.f*(float(y)+.5f)/float(Size)-1.f;
            const float radius=std::hypot(u,v);
            const float turbulence=.69f+.20f*std::cos(8.f*u+3.f*v)+.11f*std::sin(5.f*u-9.f*v);
            const float falloff=std::max(0.f,1.f-radius);
            const float alpha=falloff*falloff*(kind==0u?turbulence:1.f);
            texture.bytes.insert(texture.bytes.end(),{std::byte{255},std::byte{255},std::byte{255},
                std::byte(uint8_t(std::lround(alpha*255.f)))});
        }
        scene.textures.push_back(std::move(texture));
    }
    constexpr std::array<XMFLOAT4,3> colors{{{.49f,.54f,.65f,0.f},{.63f,1.27f,1.62f,0.f},
                                           {1.44f,1.65f,2.2f,0.f}}};
    for(size_t kind=0;kind<wakeMaterials_.size();++kind)for(size_t level=0;level<WakeLevels;++level){
        AssetMaterial material{};
        material.colorRoughness={0,0,0,1};material.emissionMetallic=colors[kind];
        material.alphaMode=2u;material.alpha=(.1f+.82f*float(level)/float(WakeLevels-1u));
        material.albedo=firstTexture+uint32_t(kind!=0u);material.samplers[0]=sampler;
        material.doubleSided=material.unlit=1u;
        wakeMaterials_[kind][level]=uint32_t(scene.materials.size());
        ApplyPhysicalMaterial(material,"medium.aerosol");
        scene.materials.push_back(material);
        scene.materialNames.emplace_back("Navette / particules");
    }
}

ShuttleAssets::ShuttleAssets(SceneData& scene) {
    auto catalog=LoadSceneGLTF(AssetPath(L"navette_organes.glb"),true);
    for(size_t i=0;i<catalog.materialNames.size();++i){
        const auto& name=catalog.materialNames[i];
        if(name=="OASIS.Verriere")catalog.materials[i].unlit=3u;
        if(name=="OASIS.VerreCRT")catalog.materials[i].unlit=4u;
    }
    if(!catalog.animations.empty()||!catalog.bones.empty()||!catalog.meshes.morphs.empty())throw AssetImportError("Shuttle requires rigid geometry");
    std::vector<uint32_t> moduleIds;
    for(const auto& object:catalog.objects){
        const auto& name=catalog.meshNodeNames[object.mesh];
        constexpr std::string_view prefix="Organe_";
        if(!name.starts_with(prefix)){
            moduleIds.push_back(Invalid);scene.assetDegraded=true;
            const auto line="asset_unavailable source=navette_organes.glb node="+name+" reason=unknown_module\r\n";DiagnosticLine(line.c_str());continue;
        }
        uint32_t module{};
        const auto result=std::from_chars(name.data()+prefix.size(),name.data()+name.size(),module);
        if(result.ec!=std::errc{}||result.ptr!=name.data()+name.size()||module>=modules_.size()){
            moduleIds.push_back(Invalid);scene.assetDegraded=true;
            const auto line="asset_unavailable source=navette_organes.glb node="+name+" reason=invalid_module\r\n";DiagnosticLine(line.c_str());continue;
        }
        moduleIds.push_back(module);
    }
    const auto [mesh,material]=AppendRigidAsset(scene,catalog);
    size_t partIndex=0;
    for(auto object:catalog.objects){
        const auto module=moduleIds[partIndex++];
        if(module==Invalid)continue;
        object.mesh+=mesh;
        object.material+=material;
        object.flags=DynamicObject|NoCollision|NoRayTracing;
        modules_[module].push_back(object);
    }
    // Individual missing visual modules do not invalidate the rest of the craft.
    for(size_t i=0;i<modules_.size();++i)if(modules_[i].empty()){
        scene.assetDegraded=true;
        const auto line="asset_unavailable source=navette_organes.glb module="+std::to_string(i)+"\r\n";DiagnosticLine(line.c_str());
    }

    // Vaisseau2 keeps the entire proven pilot/propulsion organ chain. Only the
    // original hull is exchanged for the dimensional Z31 shell.
    const auto z31Path=AssetPath(L"vaisseau2_z31.glb");
    if(std::filesystem::exists(z31Path))try{
        auto z31=LoadSceneGLTF(z31Path,true);
        if(!z31.animations.empty()||!z31.bones.empty()||!z31.meshes.morphs.empty())throw AssetImportError("Z31 requires rigid geometry");
        const auto [firstMesh,firstMaterial]=AppendRigidAsset(scene,z31);
        std::vector<uint32_t> poweredMaterials(z31.materials.size(),Invalid);
        for(size_t material=0;material<z31.materialNames.size();++material){
            const auto& name=z31.materialNames[material];
            if(name!="Z31.Headlamp"&&name!="Z31.Taillamp")continue;
            auto illuminated=scene.materials[firstMaterial+uint32_t(material)];
            if(name=="Z31.Headlamp")illuminated.emissionMetallic={4.0f,4.2f,4.8f,illuminated.emissionMetallic.w};
            else illuminated.emissionMetallic={3.0f,.10f,.04f,illuminated.emissionMetallic.w};
            poweredMaterials[material]=uint32_t(scene.materials.size());
            scene.materials.push_back(illuminated);
        }
        for(auto object:z31.objects){
            const auto name=std::string_view(z31.meshNodeNames[object.mesh]);
            if(name=="Z31_Glass"||name.starts_with("Z31_TTopGlass")){
                const auto& range=scene.meshes.ranges[firstMesh+object.mesh];
                for(uint32_t index=0;index+2u<range.indexCount;index+=3u){
                    ShuttleWake::Triangle triangle;
                    for(uint32_t v=0;v<3u;++v)
                        triangle[v]=scene.meshes.vertices[scene.meshes.indices[range.firstIndex+index+v]].position;
                    z31Canopy_.push_back(triangle);
                }
            }
            const auto powered=object.material<poweredMaterials.size()?poweredMaterials[object.material]:Invalid;
            const bool translucent=z31.materials[object.material].alphaMode==2u;
            object.mesh+=firstMesh;
            object.material+=firstMaterial;
            object.flags=DynamicObject|NoCollision|NoRayTracing|
                (translucent?(AlphaBlendObject|NoShadow):0u);
            z31Hull_.push_back({object,powered});
        }
    }catch(const AssetImportError& error){
        scene.assetDegraded=true;
        const auto line=std::string("asset_unavailable source=vaisseau2_z31.glb reason=")+error.what()+"\r\n";DiagnosticLine(line.c_str());
    }

    auto& m=scene.meshes;
    for(const auto& object:modules_[2]){
        const auto& range=m.ranges[object.mesh];
        for(uint32_t k=0;k+2u<range.indexCount;k+=3u){
            ShuttleWake::Triangle triangle;
            for(uint32_t j=0;j<3u;++j)triangle[j]=m.vertices[m.indices[range.firstIndex+k+j]].position;
            canopy_.push_back(triangle);
        }
    }

    // The NAV marks and the wake share a quad with a UV for soft particle edges.
    const uint32_t v=uint32_t(m.vertices.size()),index=uint32_t(m.indices.size()),
        remap=uint32_t(m.remap.size()),triangle=uint32_t(m.triangles.size()),
        meshlet=uint32_t(m.meshlets.size());
    for(const auto& p:std::array<XMFLOAT3,4>{{{0,-1,-1},{0,-1,1},{0,1,1},{0,1,-1}}})
        m.vertices.push_back({p,{-1,0,0},{(p.z+1.f)*.5f,(1.f-p.y)*.5f}});
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
        ApplyPhysicalMaterial(ink,"electronic.emissive_indicator");
        scene.materials.push_back(ink);scene.materialNames.emplace_back("Navette / carte");
    }
    flame_=MakeFlame(scene);
    BuildWakeMaterials(scene);
    speedDust_=MakeSpeedDust(scene);
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
        XMMatrixTranslation(ShuttleLayout::Parking.x,ShuttleLayout::Parking.y,ShuttleLayout::Parking.z)*oasisToCosmos;
    uint32_t id=FirstId;
    auto append=[&](RenderObject object,FXMMATRIX transform,bool crt=false,uint32_t page=0u,uint32_t stableId=Invalid){
        XMVECTOR scale,rotation,position;
        if(!XMMatrixDecompose(&scale,&rotation,&position,transform))return;
        XMStoreFloat3(&object.scale,scale);
        XMStoreFloat3(&object.current.p,position);
        XMStoreFloat4(&object.current.q,rotation);
        object.previous=object.current;
        object.flags|=SolarWorldVisual;
        object.id=stableId==Invalid?id++:stableId;
        if(crt){
            object.flags|=ShuttleCrt|NoShadow;
            object.cockpitDisplay=occupied?pilot.DisplayData(page):0u;
        }
        scene.objects.push_back(object);
    };
    if(pilot.Craft()==2u&&!z31Hull_.empty()){
        for(const auto& part:z31Hull_){
            auto object=part.object;
            if(pilot.AvionicsOn()&&part.poweredMaterial!=Invalid)object.material=part.poweredMaterial;
            append(object,root);
        }
    }else{
        for(const auto module:ShuttleLayout::HullModules)
            for(auto object:modules_[module])append(object,root);
    }

    const auto& hands=input.State(actions.handPose).pose;
    std::array<XMFLOAT4X4,2> handles{};
    if(occupied)for(uint32_t hand=0;hand<2;++hand)if(hands[hand].tracked)
        XMStoreFloat4x4(&handles[hand],HandModel(hands[hand].pose.p,pilot.HandleAngles(hand),root));

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
                {
                    const auto& load=pilot.Loads()[slot];
                    model=XMMatrixRotationY(-float(load.gimbalRad))*model;
                    const float thrust=float(std::hypot(load.forceN.x,load.forceN.y)/
                        Vaisseaux::Lunaire::Propulsion.reactor.curve.back().forceN);
                    if(thrust>.001f){
                        const float size=std::sqrt(thrust);
                        const auto jet=XMMatrixTranslation(ShuttleLayout::JetExitX,0.f,0.f)*model;
                        append(flame_[0],XMMatrixScaling(1.34f*size,.116f*size,.116f*size)*jet,
                            false,0u,FirstFlameId+2u*slot);
                        append(flame_[1],XMMatrixScaling(.88f*size,.045f*size,.045f*size)*jet,
                            false,0u,FirstFlameId+2u*slot+1u);
                    }
                    break;
                }
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
    if(mapVisible&&pilot.AvionicsOn()){
        const auto panel=XMLoadFloat4x4(&nav);
        constexpr double orbitRadius=SpectacleRadius*2.;
        auto dot=[&](Vaisseaux::MapDot p,float size,uint32_t color){
            const float x=182.f+float(std::clamp(p.x/orbitRadius,-1.,1.))*53.f;
            const float y=82.f-float(std::clamp(p.y/orbitRadius,-1.,1.))*36.f;
            const float lateral=(.5f-x/256.f)*.251f;
            const float vertical=(.5f-y/128.f)*.153f;
            auto object=mapDot_;object.material=mapMaterials_[color];
            append(object,XMMatrixScaling(1.f,size,size)*XMMatrixTranslation(-.079f,vertical,lateral)*panel);
        };
        dot(pilot.Map().dots[0],.0021f,1u);
        dot(pilot.Map().craft,.0020f,2u);
    }
    if(occupied&&input.insideSphere==Invalid){
        const auto velocity=pilot.LocalVelocity();
        const float speed=Length(velocity);
        const float density=pilot.Mix(ShuttlePilot::Mixer::DustDensity)*std::min(1.f,std::sqrt(speed/30.f));
        if(speed>1.f&&density>.015f){
            const auto forward=Unit(TransformDirection({1,0,0},root));
            const auto up=std::abs(forward.y)>.94f?XMFLOAT3{0,0,1}:XMFLOAT3{0,1,0};
            const auto center=Add(input.head.p,Scale(forward,21.5f));
            const auto orientation=XMMatrixInverse(nullptr,XMMatrixLookToLH(
                XMLoadFloat3(&center),XMLoadFloat3(&forward),XMLoadFloat3(&up)));
            auto dust=speedDust_;
            const auto phase=uint32_t(pilot.DustTravel()*(4096.f/40.f))&4095u;
            const auto length=uint32_t(std::round(std::min(speed/65.f,1.f)*63.f));
            const auto coverage=uint32_t(std::round(density*63.f));
            dust.speedDust=phase|(length<<12u)|(coverage<<18u);
            append(dust,XMMatrixRotationY(XM_PIDIV2*-1.f)*orientation,false,0u,FirstWakeId+256u);
        }
    }
    if(occupied){
        const auto frameToWorld=input.insideSphere==Invalid?XMMatrixIdentity():
            SolarBodyToCosmos(input.insideSphere,SolarBody(input.insideSphere,input.solarTime));
        const float metres=input.sceneUnitsPerMeter;
        uint32_t particleId=FirstWakeId;
        for(const auto& p:pilot.Wake().Particles()){
            const uint32_t stableId=particleId++;
            if(p.age>=p.life)continue;
            using Kind=ShuttleWake::Kind;
            const bool dust=p.kind==Kind::Dust;
            const bool smoke=p.kind==Kind::Smoke;
            const bool hit=dust&&p.age>=p.arrival;
            const auto space=dust?root:frameToWorld;
            const float age=std::min(p.age,dust?p.arrival:p.life);
            const auto pos=TransformPoint(Add(p.origin,Scale(p.velocity,age)),space);
            const float distance=Length(Sub(pos,input.head.p));
            if(distance>100.f*metres||distance<.20f*metres)continue;
            const float progress=p.age/p.life;
            const float fade=hit?1.f-(p.age-p.arrival)/(p.life-p.arrival):
                dust?std::min(1.f,p.age*35.f):std::min(1.f,p.age*(smoke?22.f:50.f))*(1.f-progress);
            const float glow=fade*p.strength;
            if(glow<.055f)continue;
            const auto level=size_t(std::lround(glow*float(WakeLevels-1u)));
            const float width=smoke?(.22f+.72f*progress):hit?(p.glass?.047f:.029f):
                dust?.018f:.024f+.029f*progress;
            const float length=smoke?width*1.13f:hit?width:dust?.20f:.13f+.19f*progress;
            auto object=mapDot_;
            object.material=wakeMaterials_[size_t(p.kind)][level];
            const auto direction=Unit(TransformDirection(p.axis,space));
            append(object,WakeBillboard(pos,direction,input.head,width*metres,length*metres),
                false,0u,stableId);
        }
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
