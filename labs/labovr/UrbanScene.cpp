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
// Material relief scales refer to metres per texture tile. Keep these
// in lockstep with UrbanTextures.py and the UVs in UrbanMeshes.py.
constexpr std::array<std::array<float,3>,uint32_t(SurfaceMaterial::Steam)> PhysicalSurface{{
    {{3.f,3.f,.012f}},{{3.f,3.f,.016f}},{{3.f,3.f,.003f}},
    {{1.5f,1.5f,.001f}},{{1.5f,1.5f,.006f}},
    {{1.76f,1.04f,.014f}},{{1.76f,1.04f,.006f}},
    {{.8f,.8f,.003f}},{{.8f,.8f,.004f}},{{.8f,.8f,.001f}},
    {{.65f,1.1f,.012f}},{{.65f,1.1f,.014f}},
    {{.2f,.2f,.001f}},{{.6f,.6f,.002f}},{{.8f,.8f,.007f}},
    {{4.f,4.f,.016f}},{{3.f,3.f,.014f}},{{1.5f,1.5f,.012f}},
    {{1.5f,1.5f,.005f}},{{1.5f,1.5f,.0004f}},{{1.f,1.f,0.f}}
}};
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

SpatialIndex IndexTile(Tile tile,std::span<const RenderObject> objects,const MeshData& meshes) {
    SpatialIndex result{};
    const Point origin=Origin(tile);
    const float pitch=float(BlockMeters)/float(SpatialIndex::Side);
    for(uint32_t z=0;z<SpatialIndex::Side;++z)
        for(uint32_t x=0;x<SpatialIndex::Side;++x) {
            auto& cell=result.cells[z*SpatialIndex::Side+x];
            cell.x=float(origin.x)+(float(x)+.5f)*pitch;
            cell.z=float(origin.z)+(float(z)+.5f)*pitch;
        }
    result.objectCell.reserve(objects.size());
    for(const auto& object:objects) {
        if(object.mesh>=meshes.ranges.size())
            Fail("Spatial index references a missing mesh");
        const auto sectorCoordinate=[&](double position,double offset) {
            return uint32_t(std::clamp(int(std::floor((position-offset)/pitch)),
                                       0,int(SpatialIndex::Side)-1));
        };
        const uint32_t x=sectorCoordinate(object.current.p.x,origin.x);
        const uint32_t z=sectorCoordinate(object.current.p.z,origin.z);
        const uint32_t sector=z*SpatialIndex::Side+x;
        result.objectCell.push_back(uint8_t(sector));
        auto& cell=result.cells[sector];
        ++cell.objectCount;
        const auto& bounds=meshes.ranges[object.mesh].bounds;
        const float maxScale=std::max({std::abs(object.scale.x),
                                       std::abs(object.scale.y),
                                       std::abs(object.scale.z)});
        // Snow/puddles change scale with the weather after indexing.
        // Enlarge their precomputed bounds rather than requiring a
        // per-frame spatial rebuild or risking a false rejection.
        const float localReach=std::sqrt(bounds.x*bounds.x+
                           bounds.y*bounds.y+bounds.z*bounds.z)+bounds.w;
        const float reach=localReach*maxScale*2.5f+2.f;
        const float dx=object.current.p.x-cell.x;
        const float dz=object.current.p.z-cell.z;
        cell.radius=std::max(cell.radius,std::sqrt(dx*dx+dz*dz)+reach);
    }
    return result;
}
bool SectorCanContribute(const SpatialIndex::Cell& sector,Point viewer) {
    if(!sector.objectCount)return false;
    // FarZ comes from the same XR projection as the raster renderer.
    // The additional 48 m retains distant shadow casters and reflected
    // geometry beyond the visible projection. There is no world shrink.
    constexpr float effectEnvelope=48.f;
    const double dx=double(sector.x)-viewer.x;
    const double dz=double(sector.z)-viewer.z;
    const double range=double(FarZ)+effectEnvelope+sector.radius;
    return dx*dx+dz*dz<=range*range;
}

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
        const auto& physical=PhysicalSurface[i];
        m.heightUvScale={physical[2]/physical[0],physical[2]/physical[1]};
        // Low-frequency, world-anchored variation complements each material's
        // tightly tiled aggregate/grain; it is not another repeated UV map.
        m.macroVariation=(i==uint32_t(SurfaceMaterial::OvercastSky)||
                          i==uint32_t(SurfaceMaterial::Water)||
                          i==uint32_t(SurfaceMaterial::LampEnamel))?0.f:.09f;
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
        // Physically scaled height is packed into ORM alpha and read by
        // PixelMain; the standalone _height.png remains for visual_v1.
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
void AppendTile(SceneData& scene,const Library& l,Tile tile,
                std::vector<uint64_t>* identities) {
    if(!l.loaded)Fail("Urban tile appended before library installation");
    // Collect IDs at emission, not by reverse-hashing a lossy render ID.
    auto emit=[&](SceneData& target,const Library& library,MeshId mesh,
                  SurfaceMaterial material,Point position,float height,float yaw,
                  float size,uint64_t identity,uint32_t flags=Flag){
        AddObject(target,library,mesh,material,position,height,yaw,size,identity,flags);
        if(identities)identities->push_back(identity);
    };
    const auto features=GenerateTile(tile);
    const auto at=[&](Point p){return Ground(p);};
    const auto origin=Origin(tile);
    const Point parcel{origin.x+BlockMeters*.5,origin.z+BlockMeters*.5};
    // Use the foundation cutout only for the laboratory's own tile.
    const MeshId groundMesh=(tile.x==0&&tile.z==-1)?
        MeshId::ContinuousGroundLab:MeshId::ContinuousGround;
    emit(scene,l,groundMesh,SurfaceMaterial::Grass,
              parcel,at(parcel),0,1,Identity(Kind::Intersection,tile,991u));
    uint64_t salt=0;
    for(const auto& f:features) {
        const float heading=Rotation(f.axis);
        const float y=at(f.position);
        switch(f.kind) {
        case Kind::Intersection:
            emit(scene,l,MeshId::Intersection12,SurfaceMaterial::Asphalt,
                      f.position,y,0,1,f.id);
            break;
        case Kind::RoadEast:case Kind::RoadNorth: {
            const bool north=f.kind==Kind::RoadNorth;
            emit(scene,l,north?MeshId::RoadNorth108:MeshId::Road108,
                      SurfaceMaterial::Asphalt,f.position,y,heading,1,f.id);
            emit(scene,l,north?MeshId::RoadRepairNorth:MeshId::RoadRepairEast,
                      SurfaceMaterial::AsphaltPatch,f.position,y,heading,1,
                      f.id^0x6a3ull);
            emit(scene,l,north?MeshId::YellowNorthStripe:MeshId::YellowStripe,
                      SurfaceMaterial::YellowLine,f.position,y+.024f,heading,1,
                      f.id^0x174ull);
            break;
        }
        case Kind::Sidewalk: {
            const MeshId slab=f.axis==Axis::East?
                (f.side>0?MeshId::Sidewalk108:MeshId::SidewalkEastNegative):
                (f.side>0?MeshId::SidewalkNorthPositive:MeshId::SidewalkNorthNegative);
            const MeshId edge=f.axis==Axis::East?
                (f.side>0?MeshId::Curb108:MeshId::CurbEastNegative):
                (f.side>0?MeshId::CurbNorthPositive:MeshId::CurbNorthNegative);
            emit(scene,l,slab,SurfaceMaterial::Concrete,
                      f.position,y+.08f,heading,1,f.id);
            // Move curbstones from the sidewalk centre to the road edge.
            Point curb=f.position;
            if(f.axis==Axis::East)curb.z-=double(f.side)*.93;
            else curb.x-=double(f.side)*.93;
            emit(scene,l,edge,SurfaceMaterial::Concrete,
                      curb,at(curb)+.08f,heading,1,f.id^0x942ull);
            break;
        }
        case Kind::Drain:
            emit(scene,l,MeshId::OpenDrain,SurfaceMaterial::CastIron,
                      f.position,y+.055f,heading,1,f.id);
            break;
        case Kind::Lamp: {
            // Arm points toward the carriageway; poles are on the sidewalk.
            const float yaw=f.axis==Axis::East?
                (f.side>0?std::numbers::pi_v<float>*.5f:
                          -std::numbers::pi_v<float>*.5f):
                (f.side>0?std::numbers::pi_v<float>:0.f);
            emit(scene,l,MeshId::LampPole,SurfaceMaterial::LampEnamel,
                      f.position,y,yaw,1,f.id);
            emit(scene,l,MeshId::LampDiffuser,SurfaceMaterial::LampGlow,
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
            emit(scene,l,MeshId::BrickWall,SurfaceMaterial::Brick,
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
                emit(scene,l,MeshId::Vines,SurfaceMaterial::Bark,
                          p,at(p),vineYaw,1,f.id^uint64_t(k+1)*1831ull);
                emit(scene,l,MeshId::VineLeaves,SurfaceMaterial::WinterLeaf,
                          p,at(p),vineYaw,1,f.id^uint64_t(k+1)*3463ull,
                          NoCollision|NoShadow);
            }
            break;
        }
        case Kind::Deciduous:
            emit(scene,l,MeshId::DeciduousTrunk,SurfaceMaterial::Bark,
                      f.position,y,0,float(f.height/8.5),f.id);
            // Winter deciduous crowns are predominantly bare branches.
            // The old 170 unattached red quads read as airborne squares.
            break;
        case Kind::Conifer:
            emit(scene,l,MeshId::Conifer,SurfaceMaterial::Conifer,
                      f.position,y,0,float(f.height/8.5),f.id);
            break;
        }
    }
    emit(scene,l,MeshId::BlockTerrain,SurfaceMaterial::ForestLitter,
              parcel,at(parcel),0,1,Identity(Kind::Conifer,tile,899u));
    // Small real grass clumps on the 8..10-m green verge, both axes.
    for(uint32_t k=0;k<48;++k) {
        const bool east=k<24u;
        const int side=(k&1u)?-1:1;
        const uint64_t id=Identity(Kind::Deciduous,tile,200u+k);
        const double distance=7.5+double(k%12u)*9.2+Uniform(id^0x5c1u)*2.0;
        const double verge=8.3+Uniform(id^0x8c2u)*1.25;
        const Point p=east?Point{origin.x+distance,origin.z+side*verge}:
                           Point{origin.x+side*verge,origin.z+distance};
        emit(scene,l,MeshId::Grass,SurfaceMaterial::Grass,
                  p,at(p)+.015f,float(Uniform(id^0x923u)*6.283185307179586),
                  .7f+float(Uniform(id^0xf37u))*.55f,id,NoCollision|NoShadow);
    }
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
        emit(scene,l,snow?MeshId::SnowPatch:MeshId::Grass,
                  snow?SurfaceMaterial::Snow:SurfaceMaterial::Grass,
                  p,ground+.012f,yaw,scale,id);
    }
    for(uint32_t i=0;i<6;++i) {
        // Irregular shallow asphalt puddles where grade is low.
        const uint64_t id=Identity(Kind::Drain,tile,100+i);
        const double x=origin.x+14+Uniform(id^9)*92;
        const double z=origin.z+(i&1?2.5:-2.5);
        Point p{x,z};
        emit(scene,l,MeshId::Puddle,SurfaceMaterial::Water,
                  p,at(p)+.024f,0,.8f+float(Uniform(id^21)),id,
                  NoCollision|NoShadow|AlphaBlendObject);
    }
    (void)salt;
}
GpuResidentTile DescribeResidentTile(Tile tile,std::span<const RenderObject> objects,
                                    std::span<const uint64_t> identities,
                                    const SpatialIndex& spatial){
    if(objects.size()!=identities.size()||objects.size()!=spatial.objectCell.size())
        Fail("Urban resident metadata does not match authored objects");
    static_assert(SpatialIndex::Count==GpuResidentTile::Cells);
    GpuResidentTile record{};
    record.key={tile.x,tile.z};
    record.objects.reserve(objects.size());
    for(uint32_t i=0;i<objects.size();++i){
        const auto& source=objects[i];
        GpuResidentTileObject object{};
        object.identity=identities[i];
        object.renderId=source.id;
        object.mesh=source.mesh;object.material=source.material;object.flags=source.flags;
        object.position={source.current.p.x,source.current.p.y,source.current.p.z,0};
        object.previousPosition={source.previous.p.x,source.previous.p.y,source.previous.p.z,0};
        object.rotation={source.current.q.x,source.current.q.y,source.current.q.z,source.current.q.w};
        object.previousRotation={source.previous.q.x,source.previous.q.y,source.previous.q.z,source.previous.q.w};
        object.scale={source.scale.x,source.scale.y,source.scale.z,0};
        object.cell=spatial.objectCell[i];
        record.objects.push_back(object);
    }
    return record;
}
} // namespace lab::urban
