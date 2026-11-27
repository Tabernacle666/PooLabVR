// Included inside namespace OASIS, after the existing botanical cook helpers.
// ARB-5.2: independent species-local mesh products; no campus bake dependency.
namespace {
using UFamily=Understory::Family;
using ULeafShape=Understory::Leaf;
using UAccent=Understory::Accent;
XMFLOAT2 UColor(unsigned color){
    return {(float(color%4u)+.5f)/4.f,(float(color/4u)+.5f)/4.f};
}
void UFace(Triangles& g,XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c,unsigned color){
    if(Length(Cross(Sub(b,a),Sub(c,a)))<1.e-9f)return;
    const auto first=g.vertices.size();
    FloorTriangle(g,a,b,c);
    for(size_t i=first;i<g.vertices.size();++i)g.vertices[i].uv=UColor(color);
}
void UBlade(Triangles& g,XMFLOAT3 base,XMFLOAT3 tip,XMFLOAT3 side,float width,
            float fold,ULeafShape shape,unsigned color){
    const auto delta=Sub(tip,base);
    if(Length(delta)<1.e-5f||width<1.e-5f)return;
    const auto axis=Unit(delta);
    side=Transport(side,axis);
    auto up=Unit(Cross(side,axis));
    if(up.y<0.f)up=Scale(up,-1.f);
    const auto mid=Add(Lerp(base,tip,.48f),Scale(up,fold));
    const auto point=[&](float t,float w){return Add(Lerp(base,tip,t),Scale(side,width*w));};
    std::array<XMFLOAT3,12> outline{};
    size_t n=0;
    outline[n++]=base;
    if(shape==ULeafShape::Lobed){
        outline[n++]=point(.18f,-.8f);outline[n++]=point(.32f,-.48f);
        outline[n++]=point(.49f,-1.f);outline[n++]=point(.66f,-.43f);
        outline[n++]=point(.80f,-.63f);outline[n++]=tip;
        outline[n++]=point(.80f,.63f);outline[n++]=point(.66f,.43f);
        outline[n++]=point(.49f,1.f);outline[n++]=point(.32f,.48f);outline[n++]=point(.18f,.8f);
    }else if(shape==ULeafShape::Heart){
        outline[n++]=point(.10f,-.70f);outline[n++]=point(.28f,-1.f);
        outline[n++]=point(.60f,-.69f);outline[n++]=tip;
        outline[n++]=point(.60f,.69f);outline[n++]=point(.28f,1.f);outline[n++]=point(.10f,.70f);
    }else if(shape==ULeafShape::Oval){
        outline[n++]=point(.24f,-.72f);outline[n++]=point(.53f,-1.f);
        outline[n++]=point(.80f,-.58f);outline[n++]=tip;
        outline[n++]=point(.80f,.58f);outline[n++]=point(.53f,1.f);outline[n++]=point(.24f,.72f);
    }else if(shape==ULeafShape::Needle){
        outline[n++]=point(.38f,-1.f);outline[n++]=tip;outline[n++]=point(.38f,1.f);
    }else{
        outline[n++]=point(.32f,-.86f);outline[n++]=point(.58f,-1.f);
        outline[n++]=tip;outline[n++]=point(.58f,1.f);outline[n++]=point(.32f,.86f);
    }
    // Indexed leaf surface with a raised midrib; no alpha cards. The smooth
    // face normals interpolate across the physical folded silhouette.
    const auto first=uint32_t(g.vertices.size());
    const auto uv=UColor(color);
    g.vertices.push_back({mid,up,uv,{}});
    for(size_t i=0;i<n;++i){
        const auto edge=outline[i];
        const auto normal=Unit(Add(up,Scale(Unit(Sub(mid,edge)),.18f)));
        g.vertices.push_back({edge,normal,uv,{}});
    }
    const bool flip=Dot(Cross(Sub(outline[0],mid),Sub(outline[1],mid)),up)<0.f;
    for(uint32_t i=0;i<uint32_t(n);++i){
        const uint32_t a=first+1u+i,b=first+1u+(i+1u)%uint32_t(n);
        if(flip)g.indices.insert(g.indices.end(),{first,b,a});
        else g.indices.insert(g.indices.end(),{first,a,b});
    }
}
void UStem(Triangles& g,XMFLOAT3 a,XMFLOAT3 b,float r0,float r1,unsigned color=7u){
    const auto d=Sub(b,a);if(Length(d)<1.e-5f)return;
    const auto axis=Unit(d),side=Perpendicular(axis),other=Cross(axis,side);
    constexpr unsigned sides=4u;
    const auto first=uint32_t(g.vertices.size());
    const auto uv=UColor(color);
    for(unsigned end=0;end<2u;++end)for(unsigned i=0;i<sides;++i){
        const float angle=Tau*float(i)/float(sides);
        const auto n=Add(Scale(side,std::cos(angle)),Scale(other,std::sin(angle)));
        g.vertices.push_back({Add(end?b:a,Scale(n,end?r1:r0)),n,uv,{}});
    }
    for(unsigned i=0;i<sides;++i){
        const unsigned j=(i+1u)%sides;
        g.indices.insert(g.indices.end(),{first+i,first+j,first+sides+j,first+i,first+sides+j,first+sides+i});
    }
}
void ULeaf(Triangles& g,const Understory::Species& s,XMFLOAT3 base,XMFLOAT3 tip,
           XMFLOAT3 side,float width,unsigned shade){
    if(s.leaf!=ULeafShape::Compound){
        UBlade(g,base,tip,side,width,width*.20f,s.leaf,shade);return;
    }
    const auto axis=Unit(Sub(tip,base));side=Transport(side,axis);
    const float len=Length(Sub(tip,base));
    UStem(g,base,tip,.0025f,.001f,shade);
    for(unsigned k=0;k<2u;++k){
        const auto p=Lerp(base,tip,.25f+.27f*k);
        for(float sign:{-1.f,1.f}){
            const auto q=Add(Add(p,Scale(side,sign*len*(.40f-.09f*k))),Scale(axis,len*.23f));
            UBlade(g,p,q,axis,width*.75f,width*.12f,ULeafShape::Oval,shade);
        }
    }
    UBlade(g,Lerp(base,tip,.68f),tip,side,width*.70f,width*.12f,ULeafShape::Oval,shade);
}
unsigned UAccentColor(UAccent a){
    switch(a){
    case UAccent::Yellow:return 8;case UAccent::White:return 9;
    case UAccent::Pink:return 10;case UAccent::RedFruit:return 11;
    case UAccent::BlueFruit:return 12;case UAccent::Purple:return 13;
    case UAccent::Orange:return 14;case UAccent::Copper:case UAccent::Seed:return 15;
    default:return 7;
    }
}
void UBerry(Triangles& g,XMFLOAT3 p,float r,unsigned color){
    const XMFLOAT3 top=Add(p,{0,r,0}),bottom=Add(p,{0,-r,0});
    for(unsigned i=0;i<6u;++i){
        const float a=Tau*i/6.f,b=Tau*(i+1u)/6.f;
        const auto q=Add(p,{r*std::cos(a),0,r*std::sin(a)});
        const auto w=Add(p,{r*std::cos(b),0,r*std::sin(b)});
        UFace(g,top,w,q,color);UFace(g,bottom,q,w,color);
    }
}
void UFlower(Triangles& g,XMFLOAT3 p,float r,unsigned shade){
    for(unsigned i=0;i<5u;++i){
        const float a=Tau*i/5.f;
        const XMFLOAT3 radial{std::cos(a),0,std::sin(a)},side{-radial.z,0,radial.x};
        const auto tip=Add(Add(p,Scale(radial,r)),{0,r*.22f,0});
        UBlade(g,p,tip,side,r*.40f,r*.12f,ULeafShape::Oval,shade);
    }
    UBerry(g,Add(p,{0,r*.12f,0}),r*.18f,8);
}
void UBlossom(Triangles& g,const Understory::Species& s,XMFLOAT3 p,float h,uint32_t seed){
    if(s.accent==UAccent::None)return;
    const auto color=UAccentColor(s.accent);
    if(s.accent==UAccent::RedFruit||s.accent==UAccent::BlueFruit){
        for(unsigned k=0;k<3u;++k){
            const float a=Tau*(float(k)/3.f+Variation(seed,k)*.1f);
            const auto q=Add(p,{.018f*std::cos(a),-.018f-.012f*k,.018f*std::sin(a)});
            UStem(g,p,q,.0015f,.001f);UBerry(g,q,s.accent==UAccent::BlueFruit?.007f:.012f,color);
        }
    }else if(s.accent==UAccent::Seed||s.accent==UAccent::Copper){
        const float l=h*(s.accent==UAccent::Copper?.21f:.13f);
        for(unsigned k=0;k<7u;++k){
            const float a=2.399963f*k;
            const auto q=Add(p,{0,l*k/7.f,0});
            UBlade(g,q,Add(q,{.024f*std::cos(a),.035f,.024f*std::sin(a)}),
                {-std::sin(a),0,std::cos(a)},.009f,.002f,ULeafShape::Lance,color);
        }
    }else{
        const float radius=s.accent==UAccent::White?.014f:s.accent==UAccent::Yellow?.017f:.027f;
        const unsigned flowers=s.accent==UAccent::Orange?1u:3u;
        for(unsigned k=0;k<flowers;++k){
            const float a=Tau*k/float(flowers);
            const auto q=Add(p,{.033f*std::cos(a),.014f*k,.033f*std::sin(a)});
            UStem(g,p,q,.002f,.001f);UFlower(g,q,radius,color);
        }
    }
}
void UWoodPlant(Triangles& g,const Understory::Species& s,XMFLOAT3 root,float h,uint32_t seed,bool sapling){
    const unsigned branches=s.branches+(sapling?1u:3u)+unsigned(Variation(seed,102)*3.f);
    const float spread=h*s.spread*.78f;
    const float rotation=Tau*Variation(seed,99);
    const bool cane=s.accent==UAccent::RedFruit;
    const auto center=Add(root,{(Variation(seed,91)-.5f)*h*.09f,h*.36f,0});
    const auto top=Add(center,{(Variation(seed,92)-.5f)*h*.12f,h*.52f,(Variation(seed,93)-.5f)*h*.11f});
    if(sapling){
        UStem(g,root,center,h*.026f,h*.018f);UStem(g,center,top,h*.018f,h*.004f);
    }
    for(unsigned b=0;b<branches;++b){
        const float a=rotation+2.399963f*b+.35f*Variation(seed,b,1);
        const XMFLOAT3 radial{std::cos(a),0,std::sin(a)},side{-radial.z,0,radial.x};
        const float bh=h*(.45f+.48f*Variation(seed,b,2));
        const auto start=sapling?Lerp(center,top,.08f+.70f*float(b)/float(branches)):root;
        const auto bend=sapling?Add(start,Add(Scale(radial,spread*.40f),{0,h*.055f,0})):
            Add(root,Add(Scale(radial,spread*.43f),{0,bh*.47f,0}));
        const auto end=sapling?Add(bend,Add(Scale(radial,spread*.47f),{0,h*.08f,0})):
            Add(root,Add(Scale(radial,spread*(.70f+.30f*Variation(seed,b,3))),{0,bh*(cane?.83f:1.f),0}));
        UStem(g,start,bend,h*.013f,h*.007f);UStem(g,bend,end,h*.007f,h*.002f);
        for(unsigned k=0;k<s.leafPairs;++k){
            const float t=.27f+.68f*(k+.5f)/float(s.leafPairs);
            const auto p=t<.5f?Lerp(start,bend,t*2.f):Lerp(bend,end,(t-.5f)*2.f);
            const float leafLength=s.leafLength*(.87f+.40f*Variation(seed,b,k));
            for(float sign:{-1.f,1.f}){
                if(cane&&((k%2u==0u)!=(sign>0)))continue;
                const auto q=Add(Add(p,Scale(side,sign*leafLength*.72f)),
                    Add(Scale(radial,leafLength*.58f),{0,leafLength*.19f,0}));
                ULeaf(g,s,p,q,radial,s.leafWidth*(1.04f+.28f*Variation(seed,k,b,8)),s.green);
                if(!sapling&&k>0u&&Variation(seed,b,k,31)>.28f){
                    const auto twig=Add(p,Add(Scale(radial,leafLength*.36f),Scale(side,sign*leafLength*.33f)));
                    UStem(g,p,twig,.002f,.001f);
                    const auto leafTip=Add(twig,Add(Scale(radial,leafLength*.44f),{0,leafLength*.45f,0}));
                    ULeaf(g,s,twig,leafTip,side,s.leafWidth*.82f,s.green);
                }
            }
        }
        if(!sapling&&Variation(seed,b,19)>.42f)UBlossom(g,s,end,h,seed^b);
    }
    if(sapling)ULeaf(g,s,top,Add(top,{.035f,.04f,0}),{0,0,1},s.leafWidth*.75f,s.green);
}
void UFern(Triangles& g,const Understory::Species& s,XMFLOAT3 root,float h,uint32_t seed){
    const float spread=h*s.spread*.88f;
    const unsigned fronds=s.branches+2u+unsigned(Variation(seed,65)*3.f);
    for(unsigned b=0;b<fronds;++b){
        const float a=Tau*(float(b)/fronds+.10f*Variation(seed,b,2));
        const XMFLOAT3 radial{std::cos(a),0,std::sin(a)},side{-radial.z,0,radial.x};
        const float height=h*(.73f+.23f*Variation(seed,b,3));
        const bool vase=s.branches>=8u;
        const auto frond=[&](float t){
            return Add(Add(root,Scale(radial,spread*(vase?t*t:t))),
                {0,height*(vase?(.96f*t+.12f*std::sin(Pi*t)):(.87f*std::sin(Pi*.78f*t)+.15f*t)),0});
        };
        auto prev=root;
        for(unsigned k=0;k<s.leafPairs;++k){
            const float t=.12f+.82f*float(k)/float(s.leafPairs);
            const auto p=frond(t);
            UStem(g,prev,p,.003f*(1.f-t)+.001f,.002f*(1.f-t)+.0007f,s.green);
            prev=p;
            const float reach=s.leafLength*(.78f+.40f*h)*std::pow(std::sin(Pi*t),.65f);
            for(float sign:{-1.f,1.f}){
                const auto q=Add(Add(p,Scale(side,sign*reach)),Add(Scale(radial,reach*.36f),{0,-reach*.13f,0}));
                const auto shape=s.leaf==ULeafShape::Compound?ULeafShape::Lobed:ULeafShape::Lance;
                const float width=std::max(.008f,s.leafWidth*1.38f*std::sin(Pi*t));
                if(s.leaf==ULeafShape::Compound){
                    // A real bipinnate pinna: secondary leaflets follow a curved axis.
                    UStem(g,p,q,.0018f,.0007f,s.green);
                    for(unsigned n=0;n<3u;++n){
                        const float f=.18f+.24f*n;
                        const auto at=Lerp(p,q,f);
                        const float w=width*(1.f-.18f*n);
                        for(float d:{-1.f,1.f}){
                            const auto tip=Add(Add(at,Scale(radial,d*w)),Scale(Unit(Sub(q,p)),w*.65f));
                            UBlade(g,at,tip,side,w*.41f,w*.10f,ULeafShape::Lance,s.green);
                        }
                    }
                    UBlade(g,Lerp(p,q,.76f),q,radial,width*.45f,width*.08f,ULeafShape::Lance,s.green);
                }else UBlade(g,p,q,radial,width,.007f,shape,s.green);
            }
        }
        const auto end=frond(1.f);
        UStem(g,prev,end,.002f,.0008f,s.green);
        UBlade(g,prev,end,side,.012f,.003f,ULeafShape::Lance,s.green);
    }
    if(s.accent==UAccent::Copper){
        const auto p=Add(root,{.02f,h*.72f,0});
        UStem(g,root,p,.008f,.004f);UBlossom(g,s,p,h,seed);
    }
}
void UHerb(Triangles& g,const Understory::Species& s,XMFLOAT3 root,float h,uint32_t seed){
    for(unsigned b=0;b<s.branches;++b){
        const float a=Tau*(float(b)/float(s.branches)+Variation(seed,b,2)*.19f);
        const XMFLOAT3 radial{std::cos(a),0,std::sin(a)},side{-radial.z,0,radial.x};
        const float bh=h*(.72f+.25f*Variation(seed,b,3));
        const float spread=bh*s.spread*.48f;
        const auto stem=[&](float t){
            const float arc=s.name[0]=='P'?std::sin(t*Pi*.65f):t*t;
            return Add(root,Add(Scale(radial,spread*arc),{0,bh*t,0}));
        };
        auto prev=root;
        for(unsigned k=0;k<s.leafPairs;++k){
            const float t=.16f+.68f*(k+.5f)/float(s.leafPairs);
            const auto p=stem(t);
            UStem(g,prev,p,.005f*(1.f-t)+.002f,.003f*(1.f-t)+.001f);
            prev=p;
            const float len=s.leafLength*(.95f+.35f*Variation(seed,b,k))*(1.f-.22f*t);
            const float sign=(k&1u)?-1.f:1.f;
            const auto q=Add(Add(p,Scale(side,sign*len*.70f)),Add(Scale(radial,len*.65f),{0,len*.22f,0}));
            ULeaf(g,s,p,q,radial,s.leafWidth*(1.18f-.18f*t),s.green);
            if(s.name[0]=='P'&&k%2u==0u&&Variation(seed,b,k,21)>.35f)
                UBlossom(g,s,Add(p,{0,-.026f,0}),h,seed^k);
        }
        const auto end=stem(1.f);UStem(g,prev,end,.002f,.001f);
        if(Variation(seed,b,19)>.22f)UBlossom(g,s,end,h,seed^b);
    }
}
void UGrass(Triangles& g,const Understory::Species& s,XMFLOAT3 root,float h,uint32_t seed){
    const unsigned blades=s.branches+7u;
    for(unsigned b=0;b<blades;++b){
        const float a=Tau*(float(b)/blades+Variation(seed,b)*.10f);
        const XMFLOAT3 radial{std::cos(a),0,std::sin(a)},side{-radial.z,0,radial.x};
        const float bh=h*(.58f+.29f*Variation(seed,b,2));
        const float spread=h*s.spread*(.27f+.27f*Variation(seed,b,3));
        const auto p=Add(root,Add(Scale(radial,spread*.25f),{0,bh*.64f,0}));
        const auto q=Add(root,Add(Scale(radial,spread),{0,bh*.72f,0}));
        UBlade(g,root,p,side,s.leafWidth,.002f,ULeafShape::Lance,s.green);
        UBlade(g,p,q,side,s.leafWidth*.85f,.003f,ULeafShape::Lance,s.green);
        if(b%3u==0u){
            const auto end=Add(p,{.04f,h*.22f,0});
            UStem(g,p,end,.003f,.001f);UBlossom(g,s,end,h,seed^b);
        }
    }
}
void UConifer(Triangles& g,const Understory::Species& s,XMFLOAT3 root,float h,uint32_t seed){
    const bool spruce=s.name[0]=='P';
    const auto bend=Add(root,{(Variation(seed,8)-.5f)*h*.06f,h*.54f,0});
    const auto top=Add(bend,{.015f,h*.46f,0});
    UStem(g,root,bend,h*.017f,h*.010f);UStem(g,bend,top,h*.010f,.001f);
    for(unsigned tier=0;tier<s.leafPairs;++tier){
        const float t=.16f+.75f*float(tier)/float(s.leafPairs);
        const auto base=t<.54f?Lerp(root,bend,t/.54f):Lerp(bend,top,(t-.54f)/.46f);
        for(unsigned b=0;b<s.branches;++b){
            const float a=Tau*b/float(s.branches)+tier*1.6f+.25f*Variation(seed,b,tier);
            const XMFLOAT3 radial{std::cos(a),0,std::sin(a)},side{-radial.z,0,radial.x};
            const float length=h*s.spread*.58f*(1.f-t)*(.75f+.24f*Variation(seed,tier,b));
            const auto tip=Add(Add(base,Scale(radial,length)),{0,spruce?-.025f:.025f,0});
            UStem(g,base,tip,.005f*(1.f-t)+.001f,.001f);
            for(unsigned k=0;k<5u;++k){
                const auto p=Lerp(base,tip,.22f+.16f*k);
                for(float sign:{-1.f,1.f}){
                    const float len=s.leafLength*(.60f+.36f*Variation(seed,b,k,tier));
                    const auto q=Add(Add(p,Scale(side,sign*len)),Add(Scale(radial,len*.45f),{0,spruce?len*.36f:len*.13f,0}));
                    UBlade(g,p,q,radial,s.leafWidth,.001f,ULeafShape::Needle,s.green);
                }
            }
        }
    }
}
Triangles UMakePatch(unsigned model){
    const unsigned species=model/Understory::Variants,variant=model%Understory::Variants;
    const auto& s=Understory::SpeciesList[species];
    const uint32_t seed=0x55355242u^(model+1u)*0x9e3779b9u;
    Triangles g;
    const auto count=Understory::Individuals(species);
    constexpr std::array<float,Understory::Variants> maturity{.10f,.24f,.38f,.52f,.65f,.77f,.88f,.98f};
    for(unsigned i=0;i<count;++i){
        const uint32_t individual=seed^(i+1u)*0x85ebca6bu;
        const float angle=2.399963f*float(i)+.65f*Variation(individual,3);
        const float radius=.74f*std::sqrt((float(i)+.5f)/float(count))*(.84f+.22f*Variation(individual,4));
        const XMFLOAT3 root{radius*std::cos(angle),0,radius*std::sin(angle)};
        const float h=std::lerp(s.minHeight,s.maxHeight,std::clamp(maturity[variant]+(Variation(individual,5)-.5f)*.38f,0.f,1.f));
        auto shape=s;
        shape.spread*=.72f+.56f*Variation(individual,117);
        shape.leafLength*=.70f+.60f*Variation(individual,118);
        shape.leafWidth*=.70f+.60f*Variation(individual,119);
        const auto begin=g.vertices.size();
        switch(s.family){
        case UFamily::Shrub: UWoodPlant(g,shape,root,h,individual,false);break;
        case UFamily::Fern: UFern(g,shape,root,h,individual);break;
        case UFamily::Herb: UHerb(g,shape,root,h,individual);break;
        case UFamily::Grass: UGrass(g,shape,root,h,individual);break;
        case UFamily::Sapling:
            if(s.leaf==ULeafShape::Needle)UConifer(g,shape,root,h,individual);
            else UWoodPlant(g,shape,root,h,individual,true);
            break;
        }
        float highest=0.f;
        for(size_t k=begin;k<g.vertices.size();++k)highest=std::max(highest,g.vertices[k].position.y);
        const float vertical=h/std::max(.01f,highest);
        for(size_t k=begin;k<g.vertices.size();++k){
            auto& v=g.vertices[k];
            v.position.y*=vertical;
            v.normal=Unit({v.normal.x,v.normal.y/vertical,v.normal.z});
        }
    }
    return g;
}
TextureData UPalette(){
    constexpr unsigned N=64;
    constexpr std::array<XMFLOAT3,16> colors{{
        {.13f,.34f,.071f},{.22f,.43f,.095f},{.085f,.25f,.053f},{.11f,.30f,.10f},
        {.25f,.40f,.083f},{.15f,.36f,.13f},{.31f,.43f,.11f},{.13f,.081f,.037f},
        {.78f,.52f,.028f},{.82f,.80f,.65f},{.57f,.11f,.30f},{.57f,.035f,.022f},
        {.055f,.095f,.27f},{.30f,.13f,.50f},{.81f,.25f,.037f},{.36f,.21f,.072f}
    }};
    TextureData t{};t.width=N;t.height=N;t.mips=1u;t.offsets.push_back(0);
    t.format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    t.bytes.resize(N*N*4u);
    const auto byte=[](float c){
        const float s=c<=.0031308f?12.92f*c:1.055f*std::pow(c,1.f/2.4f)-.055f;
        return std::byte(uint8_t(std::clamp(s,0.f,1.f)*255.f+.5f));
    };
    for(unsigned y=0;y<N;++y)for(unsigned x=0;x<N;++x){
        const auto c=colors[(y/(N/4u))*4u+x/(N/4u)];
        const size_t at=(size_t(y)*N+x)*4u;
        t.bytes[at]=byte(c.x);t.bytes[at+1]=byte(c.y);t.bytes[at+2]=byte(c.z);t.bytes[at+3]=std::byte{255};
    }
    return t;
}
}
std::array<WorldObject,Understory::ModelCount> BuildUnderstorySources(SceneData& scene,
        const std::array<bool,Understory::ModelCount>& used){
    std::array<WorldObject,Understory::ModelCount> result{};
    BotanicalBake::Stats stats,atlasStats;
    const auto start=GetTickCount64();
    const auto paletteKey=BotanicalBake::Key(L"botanical-understory-atlases","palette-v2");
    TextureData palette;
    if(!BotanicalBake::Load(paletteKey,palette,atlasStats,[](const auto& t){
        return BotanicalBake::Valid(t)&&t.width==64u&&t.height==64u&&t.mips==1u;
    })){
        palette=UPalette();BotanicalBake::Save(paletteKey,palette,atlasStats);
    }
    const auto map=uint32_t(scene.textures.size());scene.textures.push_back(std::move(palette));
    SamplerData sample{};sample.wrapS=sample.wrapT=33071u;
    const auto sampler=uint32_t(scene.samplers.size());scene.samplers.push_back(sample);
    std::array<uint32_t,3> materials{Invalid,Invalid,Invalid};
    uint64_t triangles=0;
    size_t forms=0;
    for(unsigned model=0;model<result.size();++model){
        if(!used[model])continue;
        const auto species=model/Understory::Variants;
        const auto& definition=Understory::SpeciesList[species];
        const auto kind=definition.leaf==ULeafShape::Needle?2u:definition.family==UFamily::Grass?1u:0u;
        if(materials[kind]==Invalid){
            constexpr std::array<const char*,3> names{"Understory broadleaf","Understory grass","Understory conifer"};
            constexpr std::array<const char*,3> ids{"plant.leaf_living","plant.grass","plant.needle_living"};
            const auto material=AppendMaterial(scene,names[kind],ids[kind],{1,1,1},.84f);
            auto& m=scene.materials[material];m.doubleSided=1u;m.albedo=map;m.samplers[0]=sampler;
            materials[kind]=material;
        }
        const auto key=BotanicalBake::Key(L"botanical-understory-geometry",
            "understory-v3/"+std::string(definition.name)+"/"+std::to_string(model%Understory::Variants)+"/"+std::to_string(BotanicalBake::MeshRevision)+"/"+
            std::to_string(sizeof(Vertex))+"/"+std::to_string(sizeof(MeshRange))+"/"+std::to_string(sizeof(Meshlet)));
        MeshData mesh;bool hit=false;
        if(!key.empty()){
            DiskBake::Reader io(key);
            hit=BotanicalBake::MeshFields(io,mesh)&&io.Finish()&&mesh.ranges.size()==1u&&
                !mesh.vertices.empty()&&!mesh.indices.empty()&&!mesh.meshlets.empty()&&
                mesh.ranges.front().vertexCount==mesh.vertices.size()&&mesh.ranges.front().indexCount==mesh.indices.size();
        }
        if(hit){++stats.hits;stats.readBytes+=BotanicalBake::Size(key);}
        else{
            ++stats.misses;mesh=CookMesh(UMakePatch(model));
            if(!key.empty()){
                DiskBake::Writer io(key);
                if(BotanicalBake::MeshFields(io,mesh)&&io.Finish()){++stats.writes;stats.writtenBytes+=BotanicalBake::Size(key);}
            }
        }
        triangles+=mesh.indices.size()/3u;
        auto object=AppendGeometry(scene,mesh,materials[kind]);
        const auto& bounds=scene.meshes.ranges[object.mesh].bounds;
        object.boundRadius=Length({bounds.x,bounds.y,bounds.z})+bounds.w;
        result[model]=object;++forms;
        MarkDiagnosticStartupProgress();
    }
    scene.meshes.skin.resize(scene.meshes.vertices.size());
    if(!scene.triangleSolids.empty())scene.triangleSolids.resize(scene.meshes.indices.size()/3u,Invalid);
    char line[512];std::snprintf(line,sizeof(line),
        "arb5_3_assets forms=%zu unique_triangles=%llu mesh_hits=%llu mesh_writes=%llu atlas_hits=%llu atlas_writes=%llu read_bytes=%llu written_bytes=%llu total_ms=%llu\r\n",
        forms,(unsigned long long)triangles,(unsigned long long)stats.hits,(unsigned long long)stats.writes,
        (unsigned long long)atlasStats.hits,(unsigned long long)atlasStats.writes,
        (unsigned long long)(stats.readBytes+atlasStats.readBytes),(unsigned long long)(stats.writtenBytes+atlasStats.writtenBytes),
        (unsigned long long)(GetTickCount64()-start));
    DiagnosticLine(line);
    return result;
}
