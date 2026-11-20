#include "Assets.h"
#include "LunarBattle.h"
#include <array>

namespace OASIS {
namespace {
enum class Material {
    Regolith,Crater,Underside,Wall,Panel,Metal,Track,Armor,Visor,Cyan,Amber,Tracer,Count
};
using enum Material;

struct MaterialProfile {
    const char* name;
    XMFLOAT4 colorRoughness;
    XMFLOAT4 emissionMetallic{};
};
constexpr std::array<MaterialProfile,size_t(Count)> Materials{{
    {"Lunar regolith",{.36f,.35f,.34f,.95f}},
    {"Lunar crater floor",{.089f,.083f,.085f,.98f}},
    {"Lunar underside",{.20f,.20f,.215f,.96f}},
    {"Lunar barrier concrete",{.26f,.28f,.30f,.91f}},
    {"Lunar barrier facing",{.57f,.57f,.53f,.84f}},
    {"Blackened lunar alloy",{.11f,.13f,.16f,.55f},{0,0,0,.58f}},
    {"Lunar track rubber",{.045f,.046f,.052f,.95f}},
    {"Lunar suit shell",{.63f,.64f,.63f,.67f}},
    {"Lunar helmet visor",{.036f,.09f,.14f,.56f},{0,0,0,.38f}},
    {"Cyan company markings",{.029f,.52f,.82f,.69f},{.006f,.06f,.10f,.24f}},
    {"Amber company markings",{.95f,.34f,.047f,.69f},{.10f,.025f,.001f,.24f}},
    {"Lunar tracer",{.98f,.56f,.18f,.76f},{3.1f,1.27f,.24f,0.f}}
}};

using Triangle=std::array<XMFLOAT3,3>;

XMFLOAT3 Cross(XMFLOAT3 a,XMFLOAT3 b){
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}

class LunarMesh {
    std::array<std::vector<Triangle>,Materials.size()> surfaces_;
    XMFLOAT3 origin_{};
    float sinYaw_{},cosYaw_{1.f};

    XMFLOAT3 Position(XMFLOAT3 point)const {
        return Add(origin_,{cosYaw_*point.x+sinYaw_*point.z,point.y,
            cosYaw_*point.z-sinYaw_*point.x});
    }
public:
    void Place(XMFLOAT3 origin,float yaw=0.f){
        origin_=origin;
        sinYaw_=std::sin(yaw);
        cosYaw_=std::cos(yaw);
    }
    void Face(Material material,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c){
        surfaces_[size_t(material)].push_back({Position(a),Position(b),Position(c)});
    }
    void Quad(Material material,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,XMFLOAT3 d){
        Face(material,a,b,c);
        Face(material,a,c,d);
    }
    void Block(Material material,XMFLOAT3 lo,XMFLOAT3 hi,float corner){
        const auto [x,y,z]=lo;
        const auto [u,v,w]=hi;
        const float k=std::min(corner,.45f*std::min(u-x,w-z));
        const std::array<XMFLOAT2,8> ring{{
            {x+k,z},{u-k,z},{u,z+k},{u,w-k},{u-k,w},{x+k,w},{x,w-k},{x,z+k}
        }};
        const XMFLOAT3 bottom{(x+u)*.5f,y,(z+w)*.5f},top{bottom.x,v,bottom.z};
        for(size_t i=0;i<ring.size();++i){
            const auto a=ring[i],b=ring[(i+1u)%ring.size()];
            Quad(material,{a.x,y,a.y},{a.x,v,a.y},{b.x,v,b.y},{b.x,y,b.y});
            Face(material,bottom,{a.x,y,a.y},{b.x,y,b.y});
            Face(material,top,{b.x,v,b.y},{a.x,v,a.y});
        }
    }
    void Tube(Material material,XMFLOAT3 a,XMFLOAT3 b,float radius,unsigned sides){
        const auto axis=Unit(Sub(b,a));
        const auto up=std::abs(axis.y)<.9f?XMFLOAT3{0.f,1.f,0.f}:XMFLOAT3{0.f,0.f,1.f};
        const auto u=Scale(Unit(Cross(up,axis)),radius);
        const auto v=Cross(axis,u);
        auto offset=[&](unsigned i){
            const float angle=XM_2PI*float(i%sides)/float(sides);
            return Add(Scale(u,std::cos(angle)),Scale(v,std::sin(angle)));
        };
        for(unsigned i=0;i<sides;++i){
            const auto front=offset(i),next=offset(i+1u);
            const auto a0=Add(a,front),a1=Add(a,next),b0=Add(b,front),b1=Add(b,next);
            Quad(material,a0,a1,b1,b0);
            Face(material,a,a1,a0);
            Face(material,b,b0,b1);
        }
    }
    void Append(SceneData& scene,const Pose& rest={},uint32_t visualPose=0u)const {
        auto& mesh=scene.meshes;
        for(size_t i=0;i<surfaces_.size();++i){
            if(surfaces_[i].empty())continue;
            const auto& profile=Materials[i];
            AssetMaterial material{};
            material.colorRoughness=profile.colorRoughness;
            material.emissionMetallic=profile.emissionMetallic;
            RenderObject object{};
            object.current=rest;
            object.current.p=Add(SpectacleCenters[SolarMoon],rest.p);
            object.previous=object.current;
            object.mesh=uint32_t(mesh.ranges.size());
            object.material=uint32_t(scene.materials.size());
            object.id=uint32_t(scene.objects.size());
            object.flags=NoCollision|NoRayTracing|NoShadow;
            object.visualPose=visualPose;
            MeshRange range{};
            range.firstIndex=uint32_t(mesh.indices.size());
            range.firstVertex=uint32_t(mesh.vertices.size());
            for(const auto& triangle:surfaces_[i]){
                const auto normal=Unit(Cross(Sub(triangle[1],triangle[0]),Sub(triangle[2],triangle[0])));
                for(const auto point:triangle){
                    mesh.indices.push_back(uint32_t(mesh.vertices.size()));
                    mesh.vertices.push_back({point,normal,{}});
                }
            }
            range.indexCount=uint32_t(mesh.indices.size())-range.firstIndex;
            range.vertexCount=uint32_t(mesh.vertices.size())-range.firstVertex;
            scene.materials.push_back(material);
            scene.materialNames.emplace_back(profile.name);
            mesh.ranges.push_back(range);
            mesh.deformations.emplace_back();
            mesh.motions.emplace_back();
            scene.objects.push_back(object);
        }
    }
};

struct LunarCrater {float x,z,radius,depth;};
constexpr std::array Craters{
    LunarCrater{-.41f,-.58f,.24f,.14f},LunarCrater{.44f,.60f,.27f,.16f},
    LunarCrater{.48f,-.55f,.18f,.10f},LunarCrater{-.35f,.62f,.19f,.10f},
    LunarCrater{.87f,.13f,.16f,.08f}
};

float CraterDistance(XMFLOAT3 point,const LunarCrater& crater){
    return std::hypot(point.x-crater.x,point.z-crater.z)/crater.radius;
}

XMFLOAT3 Ground(XMFLOAT3 point){
    float relief=.032f*std::sin(12.f*point.x+5.f*point.z)+.022f*std::cos(18.f*point.z-4.f*point.x);
    for(const auto& crater:Craters){
        const float d=CraterDistance(point,crater);
        const float bowl=std::max(0.f,1.f-d*d);
        const float rim=std::max(0.f,1.f-std::abs(d-.94f)/.20f);
        relief+=.029f*rim-crater.depth*bowl*bowl;
    }
    const float edge=std::max(std::abs(point.x)/.80f,std::abs(point.z)/.50f);
    const float blend=std::clamp((edge-.96f)/.18f,0.f,1.f);
    point.y=LunarGroundHeight+relief*blend*blend*(3.f-2.f*blend);
    return point;
}

void AddIsland(LunarMesh& mesh){
    constexpr size_t sides=48,rings=19;
    std::array<std::array<XMFLOAT3,sides>,rings> top{};
    std::array<XMFLOAT3,sides> bevel{},bottom{};
    for(size_t side=0;side<sides;++side){
        const float angle=XM_2PI*float(side)/float(sides);
        const float outline=.94f+.065f*std::cos(5.f*angle)+.04f*std::sin(9.f*angle);
        const XMFLOAT3 edge{1.11f*outline*std::cos(angle),0.f,.88f*outline*std::sin(angle)};
        for(size_t ring=0;ring<rings;++ring)top[ring][side]=Ground(Scale(edge,float(ring+1u)/rings));
        bevel[side]=Scale(edge,.82f);
        bevel[side].y=-.24f+.06f*std::sin(7.f*angle);
        bottom[side]=Scale(edge,.40f);
        bottom[side].y=-.51f+.067f*std::cos(4.f*angle+1.f);
    }
    auto upper=[&](XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c){
        const auto center=Scale(Add(Add(a,b),c),1.f/3.f);
        const bool dark=std::any_of(Craters.begin(),Craters.end(),[&](const auto& crater){
            return CraterDistance(center,crater)<.66f;
        });
        mesh.Face(dark?Crater:Regolith,a,b,c);
    };
    for(size_t side=0;side<sides;++side){
        const size_t next=(side+1u)%sides;
        upper(Ground({}),top[0][next],top[0][side]);
        for(size_t ring=1;ring<rings;++ring){
            const auto& inner=top[ring-1u];
            const auto& outer=top[ring];
            upper(inner[side],outer[next],outer[side]);
            upper(inner[side],inner[next],outer[next]);
        }
        mesh.Quad(Underside,bevel[side],top.back()[side],top.back()[next],bevel[next]);
        mesh.Quad(Underside,bottom[side],bevel[side],bevel[next],bottom[next]);
        mesh.Face(Underside,{0.f,-.58f,0.f},bottom[side],bottom[next]);
    }
}

void AddTankHull(LunarMesh& mesh,Material paint){
    mesh.Block(Metal,{-.069f,.021f,-.097f},{.069f,.067f,.099f},.019f);
    mesh.Block(paint,{-.061f,.055f,-.082f},{.061f,.079f,.085f},.022f);
    mesh.Tube(Metal,{0.f,.075f,-.009f},{0.f,.091f,-.009f},.041f,16u);
    for(float side:{-1.f,1.f}){
        const float x=side*.066f;
        mesh.Block(Track,{x-.019f,.003f,-.099f},{x+.019f,.055f,.102f},.012f);
        mesh.Block(Metal,{x-.019f,.050f,-.086f},{x+.019f,.062f,.083f},.008f);
        for(int wheel=-2;wheel<=2;++wheel){
            const float z=float(wheel)*.035f;
            const XMFLOAT3 hub{x+side*.018f,.028f,z};
            mesh.Tube(Metal,hub,Add(hub,{side*.004f,0.f,0.f}),.0195f,12u);
            mesh.Tube(Armor,Add(hub,{side*.003f,0.f,0.f}),Add(hub,{side*.006f,0.f,0.f}),.007f,8u);
        }
        for(int cleat=-5;cleat<=5;++cleat){
            const float z=float(cleat)*.016f;
            mesh.Block(Metal,{x-.020f,.051f,z-.003f},{x+.020f,.056f,z+.003f},.001f);
        }
        mesh.Block(Armor,{x-.007f,.058f,.080f},{x+.007f,.068f,.087f},.002f);
    }
}

void AddTankTurret(LunarMesh& mesh,Material paint){
    mesh.Place(Scale(LunarTurretPivot,-1.f));
    mesh.Block(paint,{-.039f,.087f,-.051f},{.039f,.121f,.030f},.012f);
    mesh.Block(Metal,{-.022f,.093f,.022f},{.022f,.114f,.053f},.005f);
    mesh.Tube(Metal,{0.f,.107f,.039f},{0.f,.107f,.141f},.009f,12u);
    mesh.Tube(Armor,{0.f,.107f,.127f},{0.f,.107f,.148f},.012f,12u);
    mesh.Block(Visor,{-.023f,.108f,-.006f},{-.010f,.124f,.013f},.002f);
    mesh.Tube(Metal,{.032f,.116f,-.041f},{.032f,.167f,-.048f},.002f,6u);
}

void AddInfantryLeg(LunarMesh& mesh,float x){
    mesh.Place({0.f,-LunarHipHeight,0.f});
    const float z=x<0.f?.007f:-.008f;
    mesh.Block(Track,{x-.008f,.001f,z-.011f},{x+.008f,.014f,z+.016f},.004f);
    mesh.Tube(Armor,{x,.011f,z},{x,.063f,0.f},.009f,8u);
    mesh.Block(Metal,{x-.008f,.031f,z+.005f},{x+.008f,.045f,z+.013f},.003f);
}

void AddInfantryBody(LunarMesh& mesh,Material paint){
    mesh.Place({0.f,-LunarHipHeight,0.f});
    mesh.Block(Metal,{-.016f,.051f,-.010f},{.016f,.077f,.010f},.005f);
    mesh.Block(Armor,{-.020f,.065f,-.013f},{.020f,.105f,.016f},.007f);
    mesh.Block(paint,{-.018f,.076f,.012f},{.018f,.101f,.021f},.004f);
    mesh.Block(Metal,{-.014f,.068f,-.022f},{.014f,.100f,-.009f},.004f);
    mesh.Block(Armor,{-.016f,.104f,-.014f},{.016f,.139f,.014f},.007f);
    mesh.Block(Visor,{-.013f,.113f,.011f},{.013f,.130f,.020f},.005f);
    mesh.Tube(Armor,{-.023f,.092f,0.f},{-.027f,.075f,.018f},.008f,8u);
    mesh.Tube(Armor,{.023f,.093f,0.f},{.027f,.073f,.016f},.008f,8u);
    mesh.Tube(paint,{-.024f,.097f,-.007f},{-.024f,.097f,.008f},.009f,8u);
    mesh.Tube(paint,{.024f,.097f,-.007f},{.024f,.097f,.008f},.009f,8u);
    mesh.Block(Metal,{.020f,.070f,.012f},{.031f,.082f,.043f},.003f);
    mesh.Tube(Track,{.025f,.077f,.030f},{.025f,.077f,.067f},.004f,8u);
}

void AddWall(LunarMesh& mesh){
    for(int panel=-2;panel<=2;++panel){
        const float z=float(panel)*.205f;
        mesh.Place(Ground({0.f,0.f,z}));
        mesh.Block(Wall,{-.060f,-.010f,-.100f},{.060f,.035f,.100f},.012f);
        mesh.Block(Wall,{-.037f,.024f,-.098f},{.037f,.196f,.098f},.008f);
        for(float side:{-1.f,1.f}){
            const float x=side*.039f;
            mesh.Block(Panel,{x-.005f,.052f,-.077f},{x+.005f,.174f,.077f},.004f);
        }
        mesh.Block(Metal,{-.043f,.185f,-.102f},{.043f,.211f,.102f},.008f);
    }
    for(float side:{-1.f,1.f}){
        mesh.Place(Ground({0.f,0.f,.527f*side}));
        mesh.Block(Wall,{-.072f,-.01f,-.035f},{.072f,.235f,.035f},.016f);
        mesh.Block(Amber,{-.056f,.218f,-.028f},{.056f,.243f,.028f},.009f);
    }
}

void AddFlag(LunarMesh& mesh,float side,Material paint){
    mesh.Place(Ground({side*.754f,0.f,.018f}),-side*XM_PIDIV2);
    mesh.Block(Wall,{-.044f,-.006f,-.044f},{.044f,.021f,.044f},.012f);
    mesh.Tube(Armor,{0.f,.015f,0.f},{0.f,.294f,0.f},.005f,8u);
    mesh.Block(paint,{.002f,.194f,-.005f},{.105f,.276f,.005f},.004f);
    mesh.Block(Armor,{.025f,.228f,-.006f},{.082f,.245f,.006f},.003f);
}

void AppendPart(SceneData& scene,const LunarMesh& source,uint32_t count,uint32_t firstUnit,
                uint32_t part,XMFLOAT3 pivot){
    const size_t first=scene.objects.size();
    size_t end=first;
    for(uint32_t i=0;i<count;++i){
        const uint32_t unit=firstUnit+i,slot=unit*LunarPartsPerUnit+part+1u;
        const auto formation=LunarFormation(unit);
        Pose rest{formation.position};
        XMStoreFloat4(&rest.q,XMQuaternionRotationRollPitchYaw(0.f,formation.yaw,0.f));
        rest.p=Transform(pivot,rest);
        if(i==0u){
            source.Append(scene,rest,slot);
            end=scene.objects.size();
            continue;
        }
        rest.p=Add(rest.p,SpectacleCenters[SolarMoon]);
        for(size_t index=first;index<end;++index){
            auto object=scene.objects[index];
            object.previous=object.current=rest;
            object.id=uint32_t(scene.objects.size());
            object.visualPose=slot;
            scene.objects.push_back(object);
        }
    }
}
}

void AppendLunarBattlefield(SceneData& scene){
    LunarMesh mesh;
    AddIsland(mesh);
    AddWall(mesh);
    AddFlag(mesh,-1.f,Cyan);
    AddFlag(mesh,1.f,Amber);
    mesh.Append(scene);

    for(uint32_t company=0;company<2u;++company){
        const auto paint=company==0u?Cyan:Amber;
        const uint32_t infantry=company*LunarCompanySize,tanks=infantry+LunarInfantryCount;
        LunarMesh body,leftLeg,rightLeg,hull,turret;
        AddInfantryBody(body,paint);
        AddInfantryLeg(leftLeg,-.012f);
        AddInfantryLeg(rightLeg,.012f);
        AddTankHull(hull,paint);
        AddTankTurret(turret,paint);
        AppendPart(scene,body,LunarInfantryCount,infantry,0u,{0.f,LunarHipHeight,0.f});
        AppendPart(scene,leftLeg,LunarInfantryCount,infantry,1u,{0.f,LunarHipHeight,0.f});
        AppendPart(scene,rightLeg,LunarInfantryCount,infantry,2u,{0.f,LunarHipHeight,0.f});
        AppendPart(scene,hull,LunarTankCount,tanks,0u,{});
        AppendPart(scene,turret,LunarTankCount,tanks,1u,LunarTurretPivot);
    }
    LunarMesh tracer;
    tracer.Tube(Tracer,{0.f,0.f,0.f},{0.f,0.f,1.f},.0017f,5u);
    tracer.Append(scene,{},LunarActorPoseCount+1u);
    const auto emitter=scene.objects.back();
    for(uint32_t i=1u;i<LunarTracerCount;++i){
        auto instance=emitter;
        instance.id=uint32_t(scene.objects.size());
        instance.visualPose+=i;
        scene.objects.push_back(instance);
    }
}
}
