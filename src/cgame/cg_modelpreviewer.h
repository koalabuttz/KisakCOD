#pragma once

#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include <xanim/dobj.h>
#include <xanim/xanim.h>
#include "cg_local.h"
#include <gfx_d3d/r_scene.h>

enum ButtonNames : __int32
{
    BTN_MODESWITCH = 0x0,
    BTN_DROPMDL = 0x1,
    BTN_WALKABOUT_ENTER = 0x2,
    BTN_WALKABOUT_EXIT = 0x3,
    BTN_FREE_DROPFRONT = 0x4,
    BTN_FREE_DROPPOS = 0x5,
    BTN_FREE_TOGGLEMOVESPEED = 0x6,
    BTN_FREE_TOGGLERAGDOLL = 0x7,
    BTN_FREE_DOWN = 0x8,
    BTN_FREE_UP = 0x9,
    BTN_FREE_UPDOWN = 0xA,
    BTN_FOCUS_TOGGLEMOV = 0xB,
    BTN_FOCUS_TOGGLEROT = 0xC,
    BTN_FOCUS_TOGGLEFOCALMOVE = 0xD,
    BTN_FOCUS_DEFAULT_CLONEMODEL = 0xE,
    BTN_FOCUS_DEFAULT_CLEARCLONES = 0xF,
    BTN_FOCUS_DEFAULT_ZOOM = 0x10,
    BTN_FOCUS_DEFAULT_ORBIT = 0x11,
    BTN_FOCUS_MMOV_2D = 0x12,
    BTN_FOCUS_MMOV_UPDOWN = 0x13,
    BTN_FOCUS_MROT_TOGGLECAM = 0x14,
    BTN_FOCUS_MROT_PITCHROLL = 0x15,
    BTN_FOCUS_MROT_YAW = 0x16,
    BTN_FOCUS_MROT_RESET = 0x17,
    BTN_FOCUS_FOCALMOVE_2D = 0x18,
    BTN_FOCUS_FOCALMOVE_UPDOWN = 0x19,
    BTN_FOCUS_FOCALMOVE_RESET = 0x1A,
    TOTAL_BUTTONNAMES = 0x1B,
};

enum MdlPrvFreeSpeed : __int32
{
    FREESPEED_NORMAL = 0x0,
    FREESPEED_SLOW = 0x1,
    FREESPEED_FAST = 0x2,
};

enum ModPrvUiModePC : __int32
{
    SELECTION_MODE = 0x0,
    MOVE_MODE = 0x1,
    ROTATE_MODE = 0x2,
    SCALE_MODE = 0x3,
};

enum MdlPrvUiModeGamepad : __int32
{
    MDLPRVMODE_FOCUSED = 0x0,
    MDLPRVMODE_FREE = 0x1,
    MDLPRVMODE_WALKABOUT = 0x2,
};

struct MdlPrvClone
{
    GfxSceneEntity ent;
    DObj_s *obj;
    cpose_t pose;
    char objBuf[100];
};

enum MdlPrvFocusedMode : __int32
{
    FOCUSEDMODE_CAMERA = 0x0,
    FOCUSEDMODE_MODELROTATE = 0x1,
    FOCUSEDMODE_MODELMOVE = 0x2,
    FOCUSEDMODE_FOCALMOVE = 0x3,
};

enum MdlPrvMRotCamMode : __int32
{
    MROTCAMMODE_STATIC = 0x0,
    MROTCAMMODE_TRAVEL = 0x1,
};

struct MdlPrvBtnTimes
{
    int mode;
    int mdlRotMode;
    int camUp;
    int camDown;
    int dropToFloor;
    int clone;
    int clearClones;
    int walkabout;
    int freeSpeed;
    int ragdollSpeed;
};

struct ModelPreviewer_System // 4-aligned on i386 as retail; holds pointers, so 8 on x64
{
    int modelCount;
    const char **modelNames;
    int animCount;
    const char **animNames;
    bool cachedAllModels;
    bool startedCaching;
    int cachedModelCount;
    int lastLoadModel;
    int lastMatReplace;
    ModPrvUiModePC uiModePC;
    float gamePadRStickDeflect;
    MdlPrvUiModeGamepad uiModeGPad;
    MdlPrvFocusedMode focusedMode;
    MdlPrvMRotCamMode modelRotCamMode;
    MdlPrvBtnTimes buttonTimes;
    bool walkaboutActive;
};

struct ModelPreviewer_Viewer
{
    float vertical;
    float horizontal;
    float centerRadius;
    float zNear;
    float zNearChangeLimit;
    MdlPrvFreeSpeed freeModeSpeed;
    float freeModeOrigin[3];
    float freeModeAngles[3];
};

struct ModelPreviewer_Material
{
    int handleCount;
    Material **handleArray;
    Material **surfMatHandles;
    const char *nameTable[66];
    int replaceIndex;
    Material *prevReplaced;
    int selectSliderIndex;
    int replaceSliderIndex;
};

struct ModelPreviewer_Light
{
    int setupCount;
    const char *nameTable[18];
    SunLightParseParams parsedSunLight[16];
    SunLightParseParams tweakableSunLight;
};

struct ModelPreviewer_Anim
{
    int fromCurrentIndex;
    int toCurrentIndex;
    bool isAnimPlaying;
    const dvar_s *mruNames[4];
    const char *mruNameTable[5];
    bool isToAnimPlaying;
    float stepCounter;
    bool isFromLooped;
    bool isToLooped;
    __int16 fromSliderID;
    __int16 toSliderID;
    __int16 fromMRUSliderID;
    __int16 toMRUSliderID;
    float deltaYaw;
};

struct ModelPreviewer_Model
{
    bool inited;
    int currentIndex;
    GfxSceneEntity currentEntity;
    cpose_t pose;
    DObj_s *currentObj;
    char objBuf[100];
    float initialOrigin[3];
    float initialYaw;
    int lodDist[4];
    int surfaceCount;
    __int16 boneInfoSliderID;
    __int16 loadSliderID;
    const dvar_s *mruNames[4];
    const char *mruNameTable[5];
    const char *boneNameTable[133];
    MdlPrvClone clones[100];
    int cloneNextIdx;
    int ragdoll;
    int ragdollDef;
};

struct ModelPreviewer
{
    bool inited;
    ModelPreviewer_System system;
    ModelPreviewer_Viewer viewer;
    ModelPreviewer_Model model;
    ModelPreviewer_Anim anim;
    ModelPreviewer_Material mat;
    ModelPreviewer_Light light;
};

void CG_ModPrvUpdateMru(const dvar_s **mruDvars, const char **stringTable, const dvar_s *dvar);
void CG_ModPrvPushMruEntry(const char *entry, const dvar_s **mruDvars, const char **stringTable, const dvar_s *dvar);
void CG_ModPrvRemoveMruEntry(const dvar_s **mruDvars, const char **stringTable, const dvar_s *dvar);
void CG_ModPrvRegisterDvars();
void CG_ModPrvSetEntityAxis(float *angles, float *quat);
void MdlPrvGetBounds(float *mins, float *maxs, float *center);
void CG_ModPrvFrameModel();
void CG_ModPrvResetOrientation_f();
void SetViewerActive(bool active);
void CG_ModPrvUnregisterCmds();
void *CG_ModPrvAlloc(int size);
void CG_ModPrvFree(void *allocated, int size);
void CG_ModPrvResetGlobals();
bool CG_ModPrvCompareString(const char *string1, const char *string2);
void CG_ModPrvGetAssetName(const XModel *header, unsigned int *data);
void CG_ModPrvUnloadModel();
void CG_ModPrvShutdown();
void CG_ModPrvDrawViewAxis(const float *centerPos);
void CG_ModPrvOriginUpdate();
void CG_ModPrvRotateUpdate();
void CG_ModPrvModelResetRotation();
void CG_ModPrvModelResetRotationXY();
int CG_ModPrvGetNumTotalBones(DObj_s *dobj);
int CG_ModPrvGetNumSurfaces(DObj_s *obj, int lod);
const char *CG_ModPrvModelGetBoneName(DObj_s *dobj, int modelIndex, int boneIndex);
void CG_ModPrvDrawBones();
void CG_ModPrvLoadAnimations(const char *animationFilename);
void CG_ModPrvApplyAnimationBlend(double deltaTime);
void CG_ModPrvApplyDelta(double deltaTime);
int CG_ModPrvLoopAnimation();
void CG_ModPrvAnimRecentAccept(const dvar_s *dvar, int *currentIndex);
void CG_ModPrvLoadAnimAccept(const dvar_s *dvar, int *currentIndex);
XAnimTree_s *CG_ModPrvAnimBlendWeightUpdate();
void CG_ModPrvMatReplaceAccepted();
void CG_ModPrvMatReplaceUpdate();
void CG_ModPrvLightSetupModified();
bool CG_ModPrvAnyLightValuesChanged();
void CG_ModPrvLightValuesUpdate();
void TRACK_cg_modelpreviewer();
void CG_ModelPreviewerPauseAnim();
void CG_ModelPreviewerStepAnim(float deltaTime);
//int MdlPrvPrint(double x, double y, const char *txt, const char *a4, const float *a5, int a6, Font_s *a7);
//int MdlPrvPrintColor(
//    double x,
//    double y,
//    const char *txt,
//    const float *color,
//    const float *a5,
//    int a6,
//    Font_s *a7);
int MdlPrvPrintHelpLine(ButtonNames idx, float vPos);
void DrawDistFromModel();
void MdlPrvDrawOverlayGamepad();
void CG_ModelPreviewerDrawInfo();
void CG_ModelPreviewerRotateCamera(double dx, double dy);
void CG_ModelPreviewerZoomCamera(double dx, double dy);
void MdlPrvModelOriginSet(float *origin);
void MdlPrvModelOriginOffset(double dx, double dy, double dz);
void MdlPrvSpin_(unsigned int yprIdx, double deg);
void MdlPrvSpinYaw(double deg);
void MdlPrvSpinPitch(double deg);
void MdlPrvSpinRoll(double deg);
void MdlPrvSpinYawOffset(double deg);
void MdlPrvSpinClearPitchRoll();
void MdlPrvMoveModelUpDown(double dist);
void MdlPrvMoveModel2D(const cg_s *cgGlob, float away, float left);
void MdlPrvMoveFocusUpDown(double dist);
void MdlPrvMoveFocus2D(const cg_s *cgGlob, double away, double left);
void MdlPrvMoveFocusReset();
void MdlPrvFreeMove(const cg_s *cgGlob, double dx, double dy);
void MdlPrvFreeMoveVertical(const cg_s *cgGlob, double dz);
void MdlPrvFreeRot(double yaw, double pitch);
void MdlPrvFreePlaceModel(float *pos);
void MdlPrvFreePlaceModelInFrontCamera(const cg_s *cgGlob);
void MdlPrvModeToggle();
void MdlPrvRotModeToggle();
void MdlPrvDropToFloor();
void MdlPrvCloneClear(MdlPrvClone *clone);
void MdlPrvCloneClearAll();
void MdlPrvCloneModel(const cg_s *cgGlob);
void MdlPrvFreeSpeedToggle();
void MdlPrvRagdollToggle();
void MdlPrvControlsGamepad(int localClientNum, double forward, double side, double pitch, double yaw);
void CG_ModelPreviewerHandleGamepadEvents(int localClientNum, double forward, double side, double pitch, double yaw);
void CG_ModelPreviewerHandleKeyEvents(int localClientNum, int key, int down, unsigned int time);
void MdlPrvUpdateViewFocused(float *viewOrigin, float (*viewAxis)[3], float *viewAngles, float *zNear);
void MdlPrvUpdateViewFree(float *viewOrigin, float (*viewAxis)[3], float *viewAngles, float *zNear);
void CG_ModelPreviewerUpdateView(float *viewOrigin, float (*viewAxis)[3], float *viewAngles, float *zNear);
bool CG_ModelPreviewerNeedsVieworgInterpSkipped(int localClientNum);
void CG_AddModelPreviewerModel(int frametime);
void CG_ModelPreviewerDestroyDevGui();
void CG_ModelPreviewerBuildInfoStr(char *buffer, int bufferSize);
void CG_ModelPreviewerBuildViewPosStr(char *buffer, int bufferSize);
void CG_ModPrvSaveDObjs();
void CG_ModPrvLoadDObjs();
void CG_ModPrvExit_f();
void CG_ModPrvRegisterCmds();
void CG_ModPrvModelGetBoneNameList();
void CG_ModPrvLoadModel(const cg_s *cgameGlob, const char *modelFilename);
void CG_ModPrvModelRecentAccepted(const cg_s *cgameGlob);
void CG_ModelPreviewerFrame(const cg_s *cgameGlob);
void CG_ModPrvEnumerateModels_FastFile();
void CG_ModPrvEnumerateModels();
void CG_ModPrvEnumerateAnimations_FastFile();
void CG_ModPrvEnumerateAnimations();
void CG_ModelPreviewerEnumerateAssets();
void CG_ModPrvStartup(int localClientNum);
void CG_ModelPreviewerCreateDevGui(int localClientNum);
