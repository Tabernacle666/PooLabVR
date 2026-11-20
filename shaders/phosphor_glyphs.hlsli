#ifndef OASIS_PHOSPHOR_GLYPHS
#define OASIS_PHOSPHOR_GLYPHS
uint2 Glyph(uint c){
    switch(c){
    case 48u:return uint2(0x239ace2eu,0x0eu);
    case 49u:return uint2(0x08421184u,0x0eu);
    case 50u:return uint2(0x1041062eu,0x1fu);
    case 51u:return uint2(0x0217043eu,0x1eu);
    case 52u:return uint2(0x05f928c2u,0x02u);
    case 53u:return uint2(0x021f421fu,0x1eu);
    case 54u:return uint2(0x231f420eu,0x0eu);
    case 55u:return uint2(0x1082083fu,0x08u);
    case 56u:return uint2(0x2317462eu,0x0eu);
    case 57u:return uint2(0x0217c62eu,0x0eu);
    case 65u:return uint2(0x231fc62eu,0x11u);
    case 66u:return uint2(0x231f463eu,0x1eu);
    case 67u:return uint2(0x2108420fu,0x0fu);
    case 68u:return uint2(0x2318c63eu,0x1eu);
    case 69u:return uint2(0x210f421fu,0x1fu);
    case 70u:return uint2(0x210f421fu,0x10u);
    case 71u:return uint2(0x231bc20fu,0x0fu);
    case 73u:return uint2(0x0842109fu,0x1fu);
    case 76u:return uint2(0x21084210u,0x1fu);
    case 77u:return uint2(0x231ad771u,0x11u);
    case 78u:return uint2(0x2319d731u,0x11u);
    case 79u:return uint2(0x2318c62eu,0x0eu);
    case 80u:return uint2(0x210f463eu,0x10u);
    case 82u:return uint2(0x254f463eu,0x11u);
    case 83u:return uint2(0x0217420fu,0x1eu);
    case 84u:return uint2(0x0842109fu,0x04u);
    case 85u:return uint2(0x2318c631u,0x0eu);
    case 86u:return uint2(0x1518c631u,0x04u);
    case 37u:return uint2(0x26b21359u,0x0u);
    case 46u:return uint2(0x0c000000u,0x06u);
    case 47u:return uint2(0x10820841u,0x10u);
    case 45u:return uint2(0x000f8000u,0x00u);
    case 72u:return uint2(0x231fc631u,0x11u);
    case 81u:return uint2(0x2558c62eu,0xdu);
    case 87u:return uint2(0x2b5ac631u,0xau);
    case 88u:return uint2(0x22a22a31u,0x11u);
    case 89u:return uint2(0x8422a31u,0x4u);
    default:return uint2(0u,0u);
    }
}

float Ink(uint code,float2 pixel){
    if(any(pixel<0.f)||pixel.x>=5.f||pixel.y>=7.f)return 0.f;
    const uint2 g=Glyph(code);
    const uint2 q=uint2(pixel);
    const uint bits=q.y==6u?g.y:(g.x>>(q.y*5u));
    return float((bits>>(4u-q.x))&1u);
}

float Word(float2 p,float2 origin,uint4 text,float size){
    float2 q=(p-origin)/size;
    if(any(q<0.f)||q.x>=96.f||q.y>=7.f)return 0.f;
    const uint character=uint(q.x)/6u;
    const uint code=(text[character/4u]>>((character&3u)*8u))&255u;
    return Ink(code,float2(q.x-float(character*6u),q.y));
}

#endif
