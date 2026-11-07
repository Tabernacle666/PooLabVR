#pragma once
#include "LaboVRWorld.h"
#define LAB_TERMINAL_DOMAIN_V1 0x5445524du
#define LAB_TERMINAL_MAX_LINES_V1 32u
#define LAB_TERMINAL_MAX_CLIENTS_V1 8u
#define LAB_TERMINAL_PHOSPHOR_CRT_V1 1u
/* UTF-8 is not decoded by this 5x7 atlas: text contains ASCII bytes.
   Centers/extents are meters in the activated world. curveDepth bends toward
   -Z about curveCenterX. Contours are concatenated by contourIndex/count. */
typedef struct LabTerminalLineV1 {
    LabVec3V1 center;
    float halfWidth,halfHeight,curveDepth,curveCenterX,curveHalfWidth;
    LabU32 flags,hovered,contourIndex,contourCount;
    char text[32];
} LabTerminalLineV1;
typedef struct LabTerminalFrameV1 {
    LabU32 structSize,lineCount;
    float timeSeconds,reserved;
    LabTerminalLineV1 lines[LAB_TERMINAL_MAX_LINES_V1];
} LabTerminalFrameV1;

/* V2 is an additive, owner-scoped retained publication. Providers declare
   real nodes/capabilities and route returned control IDs to their services.
   Arrays are copied during PublishTerminalV2; IDs must survive reorderings.
   Coordinates are in panel-local meters, pose is world-space. +Z points
   through the panel from its reading side. No solid collision is implied. */
#define LAB_TERMINAL_MAX_ELEMENTS_V2 384u
#define LAB_TERMINAL_MAX_NODES_V2 256u
#define LAB_TERMINAL_TEXT_BYTES_V2 96u
#define LAB_TERMINAL_CAP_TRANSFORM_V2 1u
#define LAB_TERMINAL_CAP_ACTION_V2 2u
#define LAB_TERMINAL_CAP_PARAMETERS_V2 4u
#define LAB_TERMINAL_NODE_OPEN_V2 1u
#define LAB_TERMINAL_NODE_SELECTED_V2 2u
#define LAB_TERMINAL_INTERACTIVE_V2 1u
#define LAB_TERMINAL_DISABLED_V2 2u
#define LAB_TERMINAL_HOVERED_V2 4u
#define LAB_TERMINAL_SELECTED_V2 8u
#define LAB_TERMINAL_BOTH_FACES_V2 16u
#define LAB_TERMINAL_PANEL_V2 0u
#define LAB_TERMINAL_CONTROL_V2 1u
#define LAB_TERMINAL_BRANCH_V2 2u
#define LAB_TERMINAL_TAB_V2 3u
#define LAB_TERMINAL_LABEL_V2 4u
#define LAB_TERMINAL_THEME_GREEN_V2 0u
#define LAB_TERMINAL_THEME_AMBER_V2 1u
typedef struct LabTerminalNodeV2 {
    LabU64 id,parentId,entityId,componentId;
    LabU32 capabilities,flags;
    char label[LAB_TERMINAL_TEXT_BYTES_V2];
} LabTerminalNodeV2;
typedef struct LabTerminalElementV2 {
    LabU64 id,nodeId;
    LabU32 kind,flags;
    float x,y,halfWidth,halfHeight,pixelMeters,backgroundAlpha;
    char text[LAB_TERMINAL_TEXT_BYTES_V2];
} LabTerminalElementV2;
typedef struct LabTerminalFrameV2 {
    LabU32 structSize,nodeCount,elementCount,theme;
    LabPoseV1 pose;
    float timeSeconds,reserved;
    const LabTerminalNodeV2* nodes;
    const LabTerminalElementV2* elements;
} LabTerminalFrameV2;
/* TOUCH uses origin as a world-space finger point and touchRadius as the
   tolerance normal to the surface. RAY uses a normalized direction and
   maxDistance. Both return the very same published element rectangles.
   BOTH_FACES is for an escape/face control: it remains reachable from behind. */
#define LAB_TERMINAL_HIT_RAY_V2 0u
#define LAB_TERMINAL_HIT_TOUCH_V2 1u
typedef struct LabTerminalHitQueryV2 {
    LabU32 structSize,mode;
    LabVec3V1 origin,direction;
    float maxDistance,touchRadius;
} LabTerminalHitQueryV2;
typedef struct LabTerminalHitV2 {
    LabU32 structSize,hit;
    LabU64 elementId,nodeId;
    float x,y,distance,reserved;
} LabTerminalHitV2;
