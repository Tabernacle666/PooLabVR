#include "UrbanScene.h"
#include "ClientAssets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <numbers>

namespace lab::urban {
namespace {
constexpr std::array<const wchar_t*,uint32_t(SurfaceMaterial::Steam)> Names{
    L"asphalt",L"asphalt_patch",L"wet_asphalt",L"yellow_line",L"concrete",L"brick",
    L"mortar",L"cast_iron",L"rust",L"lamp_enamel",L"bark",L"pine_bark",
    L"winter_leaf",L"conifer",L"grass",L"forest_litter",L"soil",
    L"snow",L"slush",L"water",L"overcast_sky"
};
constexpr uint32_t Flag=NoCollision;
float Ground(Point p) { return static_cast<float>(GroundHeight(p)); }
void AddObject(SceneData& scene,const Library& l,MeshId mesh,SurfaceMaterial mat,
               Point point,float height,float yaw,float size,
               uint64_t identity,uint32_t flags=Flag) {
    RenderObject object{};
    object.current.p={static_cast<float>(point.x),height,static_cast<float>(point.z)};
    XMStoreFloat4(&object.current.q,XMQuaternionRotationRollPitchYaw(0,yaw,0));
    object.previous=object.current;
    object.scale={size,size,size};
    object.mesh=l.meshBase+uint32_t(mesh);
    object.material=l.materialBase+uint32_t(mat);
    object.id=uint32_t(Hash(identity));
    object.flags=flags;
    scene.objects.push_back(object);
}
float Rotation(Axis axis) {
    return axis==Axis::North?std::numbers::pi_v<float>*.5f:0.f;
}
double Uniform(uint64_t key) { return double(Hash(key)&0xffffffu)/double(0xffffffu); }
} // namespace

Library InstallLibrary(SceneData& scene) {
    Library l{uint32_t(scene.meshes.ranges.size()),uint32_t(scene.materials.size()),false};
    SceneData geometry;
    geometry.meshes=LoadGLTF(AssetPath(L"assets/urban/urban.gltf"));
    if(geometry.meshes.ranges.size()!=uint32_t(MeshId::Count))
        Fail("Urban geometry library mesh order/count mismatch");
    AppendClientScene(scene,geometry);
    for(uint32_t i=0;i<Names.size();++i) {
        AssetMaterial m{};
        m.colorRoughness={1,1,1,1};
        m.emissionMetallic={};
        // A distinct material flag enables cold skylight on the exterior
        // without changing the original dark-room material response.
        m.unlit=2;
        m.doubleSided=(i==uint32_t(SurfaceMaterial::WinterLeaf)||
                       i==uint32_t(SurfaceMaterial::Grass)||
                       i==uint32_t(SurfaceMaterial::Conifer))?1u:0u;
        // The ORM map supplies about 0.48 roughness; multiply by 0.55
        // to land near 0.26, beneath the quality-3 ray-reflection gate.
        if(i==uint32_t(SurfaceMaterial::WetAsphalt))m.colorRoughness.w=.55f;
        if(i==uint32_t(SurfaceMaterial::LampEnamel))m.emissionMetallic.w=.63f;
        if(i==uint32_t(SurfaceMaterial::CastIron))m.emissionMetallic.w=.75f;
        if(i==uint32_t(SurfaceMaterial::OvercastSky)) {
            m.unlit=1;m.doubleSided=1;
        }
        if(i==uint32_t(SurfaceMaterial::Water)){
            m.colorRoughness.w=.07f;
            m.alpha=.76f;m.alphaMode=2;
        }
        const std::wstring prefix=std::wstring(L"assets/urban/textures/")+Names[i];
        auto addTexture=[&](const wchar_t* suffix,TextureUse use) {
            const uint32_t index=uint32_t(scene.textures.size());
            const auto path=AssetPath((prefix+suffix+L".png").c_str());
            scene.textures.push_back(LoadTexture(path,use));
            return index;
        };
        m.albedo=addTexture(L"_basecolor",TextureUse::Color);
        m.normal=addTexture(L"_normal",TextureUse::Normal);
        m.metalRough=addTexture(L"_orm",TextureUse::Linear);
        // The separate _height.png is retained for the visual_v1 POM path.
        scene.materials.push_back(m);
    }
    AssetMaterial steam{};
    steam.colorRoughness={.74f,.82f,.86f,.89f};
    steam.alphaMode=2;
    steam.alpha=.12f;
    steam.doubleSided=1;
    steam.unlit=1;
    scene.materials.push_back(steam);
    AssetMaterial glow{};
    glow.colorRoughness={.94f,.72f,.27f,.24f};
    glow.emissionMetallic={3.2f,1.10f,.16f,0};
    glow.unlit=1;
    scene.materials.push_back(glow);
    l.loaded=true;
    return l;
}
void AppendTile(SceneData& scene,const Library& l,Tile tile) {
    if(!l.loaded)Fail("Urban tile appended before library installation");
    const auto features=GenerateTile(tile);
    const auto at=[&](Point p){return Ground(p);};
    uint64_t salt=0;
    for(const auto& f:features) {
        const float heading=Rotation(f.axis);
        const float y=at(f.position);
        switch(f.kind) {
        case Kind::Intersection:
            AddObject(scene,l,MeshId::Intersection12,SurfaceMaterial::Asphalt,
                      f.position,y,0,1,f.id);
            break;
        case Kind::RoadEast:case Kind::RoadNorth:
            AddObject(scene,l,MeshId::Road108,SurfaceMaterial::Asphalt,
                      f.position,y,heading,1,f.id);
            AddObject(scene,l,MeshId::YellowStripe,SurfaceMaterial::YellowLine,
                      f.position,y+.024f,heading,1,f.id^0x174ull);
            break;
        case Kind::Sidewalk:
            AddObject(scene,l,MeshId::Sidewalk108,SurfaceMaterial::Concrete,
                      f.position,y+.08f,heading,1,f.id);
            // The curb is a separate profile with distinct shadow geometry.
            AddObject(scene,l,MeshId::Curb108,SurfaceMaterial::Concrete,
                      f.position,y+.08f,heading,1,f.id^0x942ull);
            break;
        case Kind::Drain:
            AddObject(scene,l,MeshId::OpenDrain,SurfaceMaterial::CastIron,
                      f.position,y+.055f,heading,1,f.id);
            break;
        case Kind::Lamp: {
            // Arm points toward the carriageway; poles are on the sidewalk.
            const float yaw=f.axis==Axis::East?
                (f.side>0?std::numbers::pi_v<float>*.5f:
                          -std::numbers::pi_v<float>*.5f):
                (f.side>0?std::numbers::pi_v<float>:0.f);
            AddObject(scene,l,MeshId::LampPole,SurfaceMaterial::LampEnamel,
                      f.position,y,yaw,1,f.id);
            AddObject(scene,l,MeshId::LampDiffuser,SurfaceMaterial::LampGlow,
                      f.position,y,yaw,1,f.id^0x7a5ull,NoCollision|NoShadow);
            if(scene.lights.size()<MaxLights) {
                SceneLight light{};
                // The point source belongs under the 1.5-m luminaire arm,
                // not on the pole axis. Both axes are oriented by yaw.
                light.position={float(f.position.x)+std::cos(yaw)*1.50f,
                                y+6.26f,float(f.position.z)-std::sin(yaw)*1.50f};
                light.radius=25;light.power=132.0f;
                light.color={1.f,.61f,.22f};
                light.direction={0,-.7f,0};
                light.innerCos=.75f;light.outerCos=.14f;
                light.flags=1;light.id=uint32_t(f.id);
                scene.lights.push_back(light);
            }
            break;
        }
        case Kind::Wall: {
            // Alternate walled lots and open winter forest. Every parcel
            // was enclosed previously, so trees on the opposite side of
            // the street could not be seen from pedestrian height.
            if(((tile.x^tile.z)&1ll)!=0)break;
            AddObject(scene,l,MeshId::BrickWall,SurfaceMaterial::Brick,
                      f.position,y,heading,1,f.id);
            // Street-facing vines: the old +.23 offset placed the foliage
            // behind the south/west wall and concealed it in photographs.
            const float faceOffset=float(f.side)*.285f;
            const float vineYaw=f.axis==Axis::East?
                (f.side>0?0.f:std::numbers::pi_v<float>):
                (f.side>0?std::numbers::pi_v<float>*.5f:
                          -std::numbers::pi_v<float>*.5f);
            for(unsigned k=0;k<9;++k) {
                const float offset=float(k)*10.f-40.f;
                Point p=f.axis==Axis::East?
                    Point{f.position.x+offset,f.position.z+faceOffset}:
                    Point{f.position.x+faceOffset,f.position.z+offset};
                AddObject(scene,l,MeshId::Vines,SurfaceMaterial::Bark,
                          p,at(p),vineYaw,1,f.id^uint64_t(k+1)*1831ull);
                AddObject(scene,l,MeshId::VineLeaves,SurfaceMaterial::WinterLeaf,
                          p,at(p),vineYaw,1,f.id^uint64_t(k+1)*3463ull,
                          NoCollision|NoShadow);
            }
            break;
        }
        case Kind::Deciduous:
            AddObject(scene,l,MeshId::DeciduousTrunk,SurfaceMaterial::Bark,
                      f.position,y,0,float(f.height/8.5),f.id);
            // Winter deciduous crowns are predominantly bare branches.
            // The old 170 unattached red quads read as airborne squares.
            break;
        case Kind::Conifer:
            AddObject(scene,l,MeshId::Conifer,SurfaceMaterial::Conifer,
                      f.position,y,0,float(f.height/8.5),f.id);
            break;
        }
    }
    const auto origin=Origin(tile);
    const Point parcel{origin.x+BlockMeters*.5,origin.z+BlockMeters*.5};
    AddObject(scene,l,MeshId::BlockTerrain,SurfaceMaterial::ForestLitter,
              parcel,at(parcel),0,1,Identity(Kind::Conifer,tile,899u));
    // Stable scatter inside the NE block, never on the road/sidewalk.
    for(uint32_t i=0;i<55;++i) {
        const uint64_t id=Identity(Kind::Conifer,tile,100+i);
        const Point p{origin.x+17+Uniform(id^917)*86,
                      origin.z+17+Uniform(id^1941)*86};
        const float ground=at(p);
        const bool snow=(i%3)!=0;
        const float scale=snow?
            (.78f+float(Uniform(id^0x881))*.66f):
            (.7f+float(Uniform(id^0x881))*1.3f);
        const float yaw=float(Uniform(id^0x6d32u))*std::numbers::pi_v<float>*2.f;
        AddObject(scene,l,snow?MeshId::SnowPatch:MeshId::Grass,
                  snow?SurfaceMaterial::Snow:SurfaceMaterial::Grass,
                  p,ground+.012f,yaw,scale,id);
    }
    for(uint32_t i=0;i<6;++i) {
        // Irregular shallow asphalt puddles where grade is low.
        const uint64_t id=Identity(Kind::Drain,tile,100+i);
        const double x=origin.x+14+Uniform(id^9)*92;
        const double z=origin.z+(i&1?2.5:-2.5);
        Point p{x,z};
        AddObject(scene,l,MeshId::Puddle,SurfaceMaterial::Water,
                  p,at(p)+.024f,0,.8f+float(Uniform(id^21)),id,
                  NoCollision|NoShadow|AlphaBlendObject);
    }
    (void)salt;
}
} // namespace lab::urban
