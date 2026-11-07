#include "HomePhysics.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <locale>
namespace lab {
namespace {
// Deliberately narrow, versioned JSON schema. Parse into temporary records;
// unknown fields, duplicate IDs, trailing data and incompatible versions fail.
struct Reader {
    const char* p;bool ok{true};
    void Space(){while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')++p;}
    bool Take(char c){Space();if(*p!=c)return false;++p;return true;}
    void Need(char c){ok=Take(c)&&ok;}
    std::string String(){Space();std::string s;if(!Take('"')){ok=false;return s;}while(*p&&*p!='"'){unsigned char c=*p++;if(c<32||c>=127||c=='\\'||s.size()>96){ok=false;return s;}s+=char(c);}Need('"');return s;}
    void Key(const char* key){if(String()!=key)ok=false;Need(':');}
    float Number(){Space();const char* start=p;if(*p=='-')++p;if(*p=='0')++p;else if(*p>='1'&&*p<='9'){do ++p;while(*p>='0'&&*p<='9');}else{ok=false;return 0;}
        if(*p=='.'){++p;if(*p<'0'||*p>'9')ok=false;while(*p>='0'&&*p<='9')++p;}
        if(*p=='e'||*p=='E'){++p;if(*p=='+'||*p=='-')++p;if(*p<'0'||*p>'9')ok=false;while(*p>='0'&&*p<='9')++p;}
        char* end{};float v=std::strtof(start,&end);if(end!=p||!std::isfinite(v))ok=false;return v;
    }
    uint32_t Integer(uint32_t max){float v=Number();if(v<0||v>float(max)||v!=std::floor(v)){ok=false;return 0;}return uint32_t(v);}
    void Floats(float* const* values,size_t n){Need('[');for(size_t i=0;i<n;i++){if(i)Need(',');*values[i]=Number();}Need(']');}
    LabPoseV1 Pose(float limit=64){float a[7]{};float* v[]{a,a+1,a+2,a+3,a+4,a+5,a+6};Floats(v,7);float q=0;for(uint32_t i=3;i<7;i++)q+=a[i]*a[i];if(std::abs(q-1)>.002f||a[0]*a[0]+a[1]*a[1]+a[2]*a[2]>limit*limit)ok=false;return {{a[0],a[1],a[2]},{a[3],a[4],a[5],a[6]}};}
};
std::vector<float*> FlightValues(LabFlightSettingsV1& f){return {&f.nominalSpeed,&f.maxSpeed,&f.responseSeconds,&f.turnIntensity,&f.turnRadius,&f.lightAttraction,&f.comfortDistance,&f.anticipationDistance,&f.wander,&f.vertical,&f.avoidanceFailure,&f.impactSeconds,&f.impactMultiplier,&f.mass,&f.wingArea,&f.liftCoefficient,&f.dragCoefficient,&f.wingFrequency,&f.bodyRadius,&f.detailDistance,&f.nearDistance};}
std::vector<float*> EffectValues(LabPopulationEffectsV1& e){return {&e.sweepRadius,&e.impulse,&e.fleeSeconds,&e.avoidanceRadius,&e.residualAversion,&e.shadowDensity,&e.shadowIntensity};}
std::vector<float*> FogValues(LabFogSettingsV1& f){return {&f.density,&f.height,&f.falloff,&f.scattering,&f.anisotropy,&f.absorption,&f.turbulence,&f.evolution,&f.handResponse,&f.lightIntensity};}
bool Bounds(const std::vector<float*>& v,const float* low,const float* high){for(size_t i=0;i<v.size();i++)if(*v[i]<low[i]||*v[i]>high[i])return false;return true;}
void Array(std::ostream& s,const std::vector<float*>& v){s<<'[';for(size_t i=0;i<v.size();i++){if(i)s<<',';s<<*v[i];}s<<']';}
void WritePose(std::ostream& s,LabPoseV1 p){s<<'['<<p.position.x<<','<<p.position.y<<','<<p.position.z<<','<<p.orientation.x<<','<<p.orientation.y<<','<<p.orientation.z<<','<<p.orientation.w<<']';}
}
bool HomePhysics::WriteProfile(uint32_t scope){
    if(!RefreshEditor())return false;
    if(frozen_){LabPhysicsEditValidationV1 valid{sizeof(valid)};if(!physics_->ValidateEdit(physics_->serviceContext,scene_,&valid)){status_="SAVE BLOCKED: PHYSICAL LAYOUT";return false;}}
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9);out<<"{\"version\":5,\"objects\":[";
    for(size_t i=0;i<entities_.size();i++){if(i)out<<',';out<<"{\"id\":\""<<entities_[i].id<<"\",\"pose\":";WritePose(out,entities_[i].pose);out<<'}';}
    out<<"],\"joints\":[";for(size_t i=0;i<connections_.size();i++){if(i)out<<',';const auto& c=connections_[i];out<<"{\"id\":\""<<c.id<<"\",\"a\":";WritePose(out,c.frameA);out<<",\"b\":";WritePose(out,c.frameB);out<<'}';}
    out<<"],\"count\":"<<populationCount_<<",\"lamp\":"<<(LampEnabled()?1:0)<<",\"flight\":";Array(out,FlightValues(flight_));out<<",\"effects\":";Array(out,EffectValues(effects_));
    out<<",\"fogEnabled\":"<<fog_.enabled<<",\"fogQuality\":"<<fog_.quality<<",\"fog\":";Array(out,FogValues(fog_));
    out<<",\"fan\":["<<(fanEnabled_?1:0)<<','<<fanRpm_<<','<<fanWind_<<"],\"editSpeed\":"<<editSpeed_<<",\"optical\":["<<optical_.enabled<<','<<optical_.quality<<','<<optical_.stabilizationSeconds<<','<<optical_.coverage<<','<<optical_.visualization<<"],\"panel\":{\"pose\":";WritePose(out,panelPose_);out<<",\"theme\":"<<panelTheme_<<",\"face\":"<<panelFace_<<"},\"capture\":["<<captureEye_<<','<<captureInterface_<<"],\"touch\":["<<touchMasks_[0]<<','<<touchMasks_[1]<<"],\"hands\":["<<handSettings_.positionTau<<','<<handSettings_.rotationTau<<','<<handSettings_.maxSpeed<<','<<handSettings_.maxAngularSpeed<<','<<handSettings_.grabRadius<<','<<handSettings_.grabStiffness<<','<<handSettings_.grabDamping<<','<<handSettings_.grabMaxForce<<"],\"aa\":"<<antialiasing_.mode<<",\"transport\":["<<transport_.enabled<<','<<transport_.contrast<<','<<transport_.frequency<<','<<transport_.sourceRate<<','<<wind_.radius<<','<<wind_.length<<','<<wind_.dissipation<<"],\"surfaces\":[";
    for(uint32_t i=0;i<surfaces_.size();i++){if(i)out<<',';const auto& s=surfaces_[i];out<<'['<<s.color.x<<','<<s.color.y<<','<<s.color.z<<','<<s.roughness<<','<<s.normalAmplitude<<','<<s.normalFrequency<<','<<s.junctionWidth<<','<<s.junctionContrast<<']';}out<<"],\"msaa\":"<<multisampling_.samples<<",\"space\":["<<populationSpace_.enabled<<','<<populationSpace_.center.x<<','<<populationSpace_.center.y<<','<<populationSpace_.center.z<<','<<populationSpace_.halfExtent.x<<','<<populationSpace_.halfExtent.y<<','<<populationSpace_.halfExtent.z<<','<<populationSpace_.returnStrength<<"]}\n";
    auto bytes=out.str();LabStorageResultV1 result{sizeof(result)};
    if(!storage_->WriteProfile(storage_->serviceContext,scope,scope==LAB_PROFILE_SYSTEM_V1?"system-default":"user-default",bytes.data(),uint32_t(bytes.size()),&result)){status_=result.message;return false;}
    status_=scope==LAB_PROFILE_SYSTEM_V1?"SYSTEM-DEFAULT SAVED":"USER-DEFAULT SAVED";return true;
}
bool HomePhysics::ReadProfile(uint32_t scope,bool automatic){
    const char* name=scope==LAB_PROFILE_SYSTEM_V1?"system-default":"user-default";LabStorageResultV1 result{sizeof(result)};
    if(!storage_->ReadProfile(storage_->serviceContext,scope,name,nullptr,0,&result)){if(!automatic||result.status!=LAB_STORAGE_MISSING_V1)status_=result.message;return false;}
    if(!result.bytes||result.bytes>1024*1024){status_="INVALID PROFILE SIZE";return false;}
    std::string bytes(result.bytes,'\0');if(!storage_->ReadProfile(storage_->serviceContext,scope,name,bytes.data(),uint32_t(bytes.size()),&result)){status_=result.message;return false;}
    if(bytes.find('\0')!=std::string::npos){status_="INVALID PROFILE DATA";return false;}
    Reader r{bytes.c_str()};r.Need('{');r.Key("version");uint32_t version=r.Integer(5);if(!r.ok||version<1){status_="PROFILE VERSION: ONLY 1 / 2 / 3 / 4 / 5 SUPPORTED";return false;}r.Need(',');r.Key("objects");r.Need('[');
    bool hadObject=false;std::vector<std::string> seen;LabPoseV1 legacyScreen{};bool hadScreen=false;
    std::vector<LabPhysicsEditBodyV1> poses;std::vector<LabPhysicsEditJointV1> anchors;
    while(r.ok&&!r.Take(']')){
        if(hadObject)r.Need(',');hadObject=true;r.Need('{');r.Key("id");auto id=r.String();r.Need(',');r.Key("pose");auto pose=r.Pose();r.Need('}');
        if(std::find(seen.begin(),seen.end(),id)!=seen.end()){r.ok=false;break;}seen.push_back(id);
        if(version==1&&(id=="room-zplus"||id=="crt-casing"||id=="crt-bezel"||id=="crt-screen"||id=="crt-foot-1"||id=="crt-foot-2"||id=="crt-knob-1"||id=="crt-knob-2")){if(id=="crt-screen"){legacyScreen=pose;hadScreen=true;}continue;}
        auto e=std::find_if(entities_.begin(),entities_.end(),[&](const auto& e){return e.id==id;});if(e==entities_.end()){status_="PROFILE UNKNOWN OBJECT: "+id;return false;}
        if(std::any_of(poses.begin(),poses.end(),[&](const auto& p){return p.body==e->body;})){r.ok=false;break;}poses.push_back({e->body,pose});if(poses.size()>entities_.size())r.ok=false;
    }
    if(poses.size()!=entities_.size()){for(const auto& e:entities_)if(std::none_of(poses.begin(),poses.end(),[&](const auto& p){return p.body==e.body;})){
        bool fanPart=e.id=="fan-post-collider"||e.id=="fan-head-collider";
        bool newObject=e.id.rfind("vase-",0)==0;
        if(fanPart||newObject){
            auto anchorId=fanPart?"fan-base":"table-top";
            auto base=std::find_if(entities_.begin(),entities_.end(),[&](const auto& x){return x.id==anchorId;});
            auto saved=base==entities_.end()?poses.end():std::find_if(poses.begin(),poses.end(),[&](const auto& x){return x.body==base->body;});
            if(base==entities_.end()||saved==poses.end()){status_="PROFILE MISSING OBJECT ANCHOR";return false;}
            auto cv=[](LabPoseV1 p){return Pose{{p.position.x,p.position.y,p.position.z},{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w}};};
            auto transformed=FromMatrix(Matrix(cv(e.pose))*XMMatrixInverse(nullptr,Matrix(cv(base->pose)))*Matrix(cv(saved->pose)));
            poses.push_back({e.body,{{transformed.p.x,transformed.p.y,transformed.p.z},{transformed.q.x,transformed.q.y,transformed.q.z,transformed.q.w}}});
        }else{status_="PROFILE MISSING OBJECT: "+e.id;return false;}
    }}r.Need(',');r.Key("joints");r.Need('[');
    while(r.ok&&!r.Take(']')){
        if(!anchors.empty())r.Need(',');r.Need('{');r.Key("id");auto id=r.String();r.Need(',');r.Key("a");auto a=r.Pose(256);r.Need(',');r.Key("b");auto b=r.Pose(256);r.Need('}');
        auto j=std::find_if(connections_.begin(),connections_.end(),[&](const auto& j){return j.id==id;});if(j==connections_.end()){status_="PROFILE UNKNOWN JOINT: "+id;return false;}
        if(std::any_of(anchors.begin(),anchors.end(),[&](const auto& a){return a.joint==j->joint;})){r.ok=false;break;}anchors.push_back({j->joint,a,b});if(anchors.size()>connections_.size())r.ok=false;
    }
    if(anchors.size()!=connections_.size()){for(const auto& c:connections_)if(std::none_of(anchors.begin(),anchors.end(),[&](const auto& a){return a.joint==c.joint;})){
        // Profiles saved before the table became physical have no leg joints.
        // Preserve their saved table layout and derive each fixed joint from it.
        if(c.id.rfind("table-top--table-leg-",0)==0||c.id=="fan-base--fan-post-collider"||c.id=="fan-base--fan-head-collider"||c.id.rfind("vase-base--vase-wall-",0)==0){
            auto a=std::find_if(poses.begin(),poses.end(),[&](const auto& p){return p.body==c.bodyA;});
            auto b=std::find_if(poses.begin(),poses.end(),[&](const auto& p){return p.body==c.bodyB;});
            if(a==poses.end()||b==poses.end()){status_="PROFILE MISSING TABLE BODY";return false;}
            const auto& ap=a->pose;const auto& bp=b->pose;
            Pose pa{{ap.position.x,ap.position.y,ap.position.z},{ap.orientation.x,ap.orientation.y,ap.orientation.z,ap.orientation.w}};
            Pose pb{{bp.position.x,bp.position.y,bp.position.z},{bp.orientation.x,bp.orientation.y,bp.orientation.z,bp.orientation.w}};
            Pose relative=FromMatrix(Matrix(pb)*XMMatrixInverse(nullptr,Matrix(pa)));
            anchors.push_back({c.joint,{{relative.p.x,relative.p.y,relative.p.z},{relative.q.x,relative.q.y,relative.q.z,relative.q.w}},{{0,0,0},{0,0,0,1}}});
        }else{status_="PROFILE MISSING JOINT: "+c.id;return false;}
    }}
    r.Need(',');r.Key("count");uint32_t count=r.Integer(LAB_PHYSICS_POPULATION_MAX_V1);r.Need(',');r.Key("lamp");bool lamp=r.Integer(1)!=0;
    auto flight=flight_;auto effects=effects_;auto fog=fog_;r.Need(',');r.Key("flight");auto fv=FlightValues(flight);r.Floats(fv.data(),fv.size());r.Need(',');r.Key("effects");auto ev=EffectValues(effects);r.Floats(ev.data(),ev.size());
    r.Need(',');r.Key("fogEnabled");fog.enabled=r.Integer(1);r.Need(',');r.Key("fogQuality");fog.quality=r.Integer(2);r.Need(',');r.Key("fog");auto gv=FogValues(fog);r.Floats(gv.data(),gv.size());
    r.Need(',');r.Key("fan");r.Need('[');bool fan=r.Integer(1)!=0;r.Need(',');float rpm=r.Number();r.Need(',');float wind=r.Number();r.Need(']');r.Need(',');r.Key("editSpeed");uint32_t speed=r.Integer(2);
    auto optical=optical_;optical.enabled=1;optical.quality=1;optical.stabilizationSeconds=.02f;optical.coverage=1;optical.visualization=0;
    bool hasTail=r.Take(',');if(hasTail){r.Key("optical");r.Need('[');optical.enabled=r.Integer(1);r.Need(',');optical.quality=r.Integer(2);r.Need(',');optical.stabilizationSeconds=r.Number();r.Need(',');optical.coverage=r.Number();r.Need(',');optical.visualization=r.Integer(2);r.Need(']');}
    auto panel=panelPose_;uint32_t theme=panelTheme_,face=panelFace_;
    if(version>=2){r.Need(',');r.Key("panel");r.Need('{');r.Key("pose");panel=r.Pose();r.Need(',');r.Key("theme");theme=r.Integer(1);r.Need(',');r.Key("face");face=r.Integer(1);r.Need('}');}
    else {panel={{0,1.875f,3.96f},{0,0,0,1}};theme=face=0;if(hadScreen){panel.position.x=legacyScreen.position.x+.85f;panel.position.y=1.875f+legacyScreen.position.y-1.50f;panel.position.z=3.96f+legacyScreen.position.z-.828f;panel.orientation=legacyScreen.orientation;}}
    uint32_t captureEye=0,captureInterface=1;auto masks=touchMasks_;auto hands=handSettings_;
    if(version>=3){r.Need(',');r.Key("capture");r.Need('[');captureEye=r.Integer(1);r.Need(',');captureInterface=r.Integer(1);r.Need(']');
        r.Need(',');r.Key("touch");r.Need('[');masks[0]=r.Integer(31);r.Need(',');masks[1]=r.Integer(31);r.Need(']');
        r.Need(',');r.Key("hands");float* hp[]{&hands.positionTau,&hands.rotationTau,&hands.maxSpeed,&hands.maxAngularSpeed,&hands.grabRadius,&hands.grabStiffness,&hands.grabDamping,&hands.grabMaxForce};r.Floats(hp,8);
    }
    auto aa=antialiasing_;auto transport=transport_;auto jet=wind_;auto surfaces=surfaces_;
    if(version>=4){r.Need(',');r.Key("aa");aa.mode=r.Integer(2);r.Need(',');r.Key("transport");r.Need('[');transport.enabled=r.Integer(1);r.Need(',');transport.contrast=r.Number();r.Need(',');transport.frequency=r.Number();r.Need(',');transport.sourceRate=r.Number();r.Need(',');jet.radius=r.Number();r.Need(',');jet.length=r.Number();r.Need(',');jet.dissipation=r.Number();r.Need(']');
        r.Need(',');r.Key("surfaces");r.Need('[');for(uint32_t i=0;i<surfaces.size();i++){if(i)r.Need(',');auto& s=surfaces[i];float* values[]{&s.color.x,&s.color.y,&s.color.z,&s.roughness,&s.normalAmplitude,&s.normalFrequency,&s.junctionWidth,&s.junctionContrast};r.Floats(values,8);}r.Need(']');
    }
    auto multisampling=multisampling_;auto space=populationSpace_;
    if(version>=5){r.Need(',');r.Key("msaa");multisampling.samples=r.Integer(4);if(multisampling.samples!=1&&multisampling.samples!=4)r.ok=false;
        r.Need(',');r.Key("space");r.Need('[');space.enabled=r.Integer(1);
        float* values[]{&space.center.x,&space.center.y,&space.center.z,&space.halfExtent.x,&space.halfExtent.y,&space.halfExtent.z,&space.returnStrength};for(auto* v:values){r.Need(',');*v=r.Number();}r.Need(']');}
    r.Need('}');r.Space();if(*r.p)r.ok=false;
    const float flo[]{.1f,.1f,.02f,.1f,.02f,0,.02f,.02f,0,0,0,0,1,1e-7f,1e-8f,.00001f,0,.01f,.001f,.01f,.01f};
    const float fhi[]{8,12,2,8,4,8,2,4,4,4,1,120,10,.01f,.01f,8,8,500,.035f,8,8};
    const float elo[]{.02f,0,0,.1f,0,0,0},ehi[]{1,8,120,4,1,1,1};const float glo[]{0,0,0,0,-.8f,0,0,0,0,0},ghi[]{1,4,4,4,.8f,4,1,4,4,4};
    if(!r.ok){status_="PROFILE SCHEMA: MALFORMED OR UNKNOWN FIELD";return false;}
    if(!Bounds(fv,flo,fhi)||!Bounds(ev,elo,ehi)||!Bounds(gv,glo,ghi)||flight.maxSpeed<flight.nominalSpeed||flight.anticipationDistance<flight.comfortDistance||flight.impactSeconds!=effects.fleeSeconds||rpm<0||rpm>1800||wind<0||wind>4||optical.stabilizationSeconds<0||optical.stabilizationSeconds>.08f||optical.coverage<.25f||optical.coverage>2){status_="PROFILE PARAMETERS: RANGE OR COUPLED VALUES INVALID";return false;}
    if(hands.positionTau<0||hands.positionTau>.2f||hands.rotationTau<0||hands.rotationTau>.2f||hands.maxSpeed<=0||hands.maxSpeed>20||hands.maxAngularSpeed<=0||hands.maxAngularSpeed>50||hands.grabRadius<.025f||hands.grabRadius>.25f||hands.grabStiffness<=0||hands.grabStiffness>2000||hands.grabDamping<0||hands.grabDamping>50||hands.grabMaxForce<=0||hands.grabMaxForce>50){status_="PROFILE HAND RESPONSE OUT OF RANGE";return false;}
    if(transport.contrast<0||transport.contrast>1||transport.frequency<.1f||transport.frequency>2||transport.sourceRate<0||transport.sourceRate>8||jet.radius<.05f||jet.radius>4||jet.length<.1f||jet.length>16||jet.dissipation<0||jet.dissipation>8){status_="PROFILE TRANSPORT OUT OF RANGE";return false;}
    for(const auto& s:surfaces){const float v[]{s.color.x,s.color.y,s.color.z,s.roughness,s.normalAmplitude,s.normalFrequency,s.junctionWidth,s.junctionContrast};const float lo[]{0,0,0,.04f,0,.1f,0,0},hi[]{1,1,1,1,.3f,300,.15f,1};for(uint32_t i=0;i<8;i++)if(v[i]<lo[i]||v[i]>hi[i]){status_="PROFILE MATERIAL OUT OF RANGE";return false;}}
    // Core ApplyEdit is transactional: an invalid candidate never reaches
    // PhysX and leaves both the previous authored layout and FREEZE intact.
    if(std::abs(space.center.x)>16||std::abs(space.center.y)>16||std::abs(space.center.z)>16||space.halfExtent.x<.1f||space.halfExtent.y<.1f||space.halfExtent.z<.1f||space.halfExtent.x>16||space.halfExtent.y>16||space.halfExtent.z>16||space.returnStrength<0||space.returnStrength>16){status_="PROFILE FLIGHT VOLUME INVALID";return false;}
    bool wasFrozen=frozen_;if(!wasFrozen&&!SetFrozen(true))return false;
    LabPhysicsEditBatchV1 batch{sizeof(batch),uint32_t(poses.size()),poses.data(),uint32_t(anchors.size()),0,anchors.data()};
    if(!physics_->ApplyEdit(physics_->serviceContext,scene_,&batch)){
        RefreshEditor();
        // ApplyEdit rolls back the candidate. Restore the running state we
        // entered with instead of silently freezing the simulation on load.
        if(!wasFrozen&&!SetFrozen(false))Fail("Home physics_v1: resume after rejected profile failed");
        status_=version==1?"V1 MIGRATION BLOCKED: PHYSICAL LAYOUT / JOINT ANCHORS":"PROFILE BLOCKED: PHYSICAL LAYOUT / JOINT ANCHORS";return false;
    }
    multisampling_=multisampling;populationSpace_=space;if(!physics_->SetPopulationSpace(physics_->serviceContext,scene_,&populationSpace_))Fail("Home physics_v1: profile flight volume rejected");
    antialiasing_=aa;transport_=transport;wind_.radius=jet.radius;wind_.length=jet.length;wind_.dissipation=jet.dissipation;surfaces_=surfaces;PublishRendering();
    captureEye_=captureEye;captureInterface_=captureInterface;touchMasks_=masks;handSettings_=hands;ApplyTouchSources();panelPose_=panel;panelTheme_=theme;panelFace_=face;flight_=flight;effects_=effects;fog_=fog;fanEnabled_=fan;fanRpm_=rpm;fanWind_=wind;editSpeed_=speed;optical_=optical;PublishOptical();
    physics_->SetFlightSettings(physics_->serviceContext,scene_,&flight_);physics_->SetPopulationEffects(physics_->serviceContext,scene_,&effects_);AddPopulation(int32_t(count)-int32_t(populationCount_));SetLampEnabled(lamp);
    auto settings=fog_;settings.enabled=fog_.enabled&&visible_;gpu_->SetFogSettings(gpu_->serviceContext,&settings);RefreshEditor();CacheValidSelections();
    if(!wasFrozen&&!SetFrozen(false))return false;PublishAcoustics();status_=version==1?"V1 MIGRATED: CRT TO PANEL; +Z WALL REMOVED":scope==LAB_PROFILE_SYSTEM_V1?"SYSTEM-DEFAULT LOADED":"USER-DEFAULT LOADED";return true;
}
}
