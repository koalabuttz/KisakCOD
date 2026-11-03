#pragma once


#ifndef KISAK_SP
#error This file is Single-Player only
#endif

struct vehicle_node_t;
struct vehicle_pathpos_t;

void VP_AddDebugLine(float *start, float *end, int forceDraw);
void VP_SetScriptVariable(const char *key, const char *value, vehicle_node_t *node);
void VP_ParseField(const char *key, const char *value, vehicle_node_t *node);
void VP_ParseFields(vehicle_node_t *node);
void VP_ClearNode(vehicle_node_t *node);
void VP_CopyNode(const vehicle_node_t *src, vehicle_node_t *dst);
int VP_GetNodeIndex(unsigned short name, float *origin);
float VP_CalcNodeSpeed(short nodeIdx);
float VP_CalcNodeLookAhead(short nodeIdx);
void VP_CalcNodeAngles(short nodeIdx, float *angles);
float VP_GetSpeed(const vehicle_pathpos_t *vpp);
float VP_GetLookAhead(const vehicle_pathpos_t *vpp);
float VP_GetSlide(const vehicle_pathpos_t *vpp);
void VP_GetAngles(const vehicle_pathpos_t *vpp, float *angles);
void VP_GetLookAheadXYZ(const vehicle_pathpos_t *vpp, float *lookXYZ);
int VP_UpdatePathPos(vehicle_pathpos_t *vpp, const float *dir, short nodeTest);
void VP_BeginSwitchNode(const vehicle_pathpos_t *vpp);
void VP_EndSwitchNode(const vehicle_pathpos_t *vpp);
void G_InitVehiclePaths();
void G_FreeVehiclePaths();
void G_FreeVehiclePathsScriptInfo();
void G_SetupVehiclePaths();
void G_VehInitPathPos(vehicle_pathpos_t *vpp);
void G_VehFreePathPos(vehicle_pathpos_t *vpp);
void G_VehSetUpPathPos(vehicle_pathpos_t *vpp, short nodeIdx);
int G_VehUpdatePathPos(vehicle_pathpos_t *vpp, short testNode);
void G_VehSetSwitchNode(vehicle_pathpos_t *vpp, short srcNodeIdx, short dstNodeIdx);
void TRACK_g_vehicle_path();
void SP_info_vehicle_node(int rotated);
int GScr_GetVehicleNodeIndex(int index);
void GScr_AddFieldsForVehicleNode();
void GScr_GetVehicleNodeField(unsigned int entnum, unsigned int offset);
void GScr_GetVehicleNode();
void GScr_GetVehicleNodeArray();
void GScr_GetAllVehicleNodes();
void VP_DrawPath(const vehicle_pathpos_t *vpp);
void G_DrawVehiclePaths();
