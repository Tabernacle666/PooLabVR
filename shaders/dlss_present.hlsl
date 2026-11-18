#define OASIS_DLSS_FRAME
#include "present.hlsl"

Present TonemapDlss(Out i){
    return Tonemap(i);
}
