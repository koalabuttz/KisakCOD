#ifndef KISAK_SP
#error This file is for SinglePlayer only
#endif

#include "client.h"
#include <qcommon/msg.h>

//
// cl_input
//
typedef struct {
	int			down[2];		// key nums holding it down
	unsigned	downtime;		// msec timestamp
	unsigned	msec;			// msec down this frame if both a down and up happened
	qboolean	active;			// current state
	qboolean	wasPressed;		// set when down, not cleared when up
} kbutton_t;

void TRACK_cl_input();
bool IN_IsTempProneKeyActive();
int IN_IsTempStanceKeyActive();
void IN_MLookDown();
void IN_KeyDown(kbutton_t *b);
void IN_KeyUp(kbutton_t *b);
// local variable allocation has failed, the output may be wrong!
float CL_KeyState(kbutton_t *key);
void CL_SetStance(int localClientNum, StanceState stance);
void CL_ToggleStance(StanceState preferredStance);
void IN_UpDown();
void IN_UpUp();
void IN_DownDown();
void IN_DownUp();
void IN_LeftDown();
void IN_LeftUp();
void IN_RightDown();
void IN_RightUp();
void IN_ForwardDown();
void IN_ForwardUp();
void IN_BackDown();
void IN_BackUp();
void IN_LookupDown();
void IN_LookupUp();
void IN_LookdownDown();
void IN_LookdownUp();
void IN_MoveleftDown();
void IN_MoveleftUp();
void IN_MoverightDown();
void IN_MoverightUp();
void IN_SpeedDown();
void IN_SpeedUp();
void IN_StrafeDown();
void IN_StrafeUp();
void IN_Attack_Down();
void IN_Attack_Up();
void IN_Breath_Down();
void IN_Breath_Up();
void IN_MeleeBreath_Down();
void IN_MeleeBreath_Up();
void IN_Frag_Down();
void IN_Frag_Up();
void IN_Smoke_Down();
void IN_Smoke_Up();
void IN_BreathSprint_Down();
void IN_BreathSprint_Up();
void IN_Melee_Down();
void IN_Melee_Up();
void IN_Activate_Down();
void IN_Activate_Up();
void IN_Reload_Down();
void IN_Reload_Up();
void IN_UseReload_Down();
void IN_UseReload_Up();
void IN_LeanLeft_Down();
void IN_LeanLeft_Up();
void IN_LeanRight_Down();
void IN_LeanRight_Up();
void IN_Prone_Down();
void IN_Prone_Up();
void IN_Stance_Down();
void IN_Stance_Up();
void IN_CenterView();
void IN_RemoteKeyboard();
void IN_RemoteMouseMove();
void IN_ToggleADS();
void IN_LeaveADS();
void IN_Throw_Down();
void IN_Throw_Up();
void IN_ToggleADS_Throw_Down();
void IN_ToggleADS_Throw_Up();
void IN_Speed_Throw_Down();
void IN_Speed_Throw_Up();
void IN_LowerStance();
void IN_RaiseStance();
void IN_ToggleCrouch();
void IN_ToggleProne();
void IN_GoProne();
void IN_GoCrouch();
void IN_GoStandDown();
void IN_GoStandUp();
void IN_SprintDown();
void IN_SprintUp();
void IN_NightVisionDown();
void IN_NightVisionUp();
void CL_AdjustAngles();
void CL_StanceButtonUpdate();
void CL_AddCurrentStanceToCmd(usercmd_s *cmd);
void CL_KeyMove(usercmd_s *cmd);
int CL_AllowInput();
void CL_GamepadMove(usercmd_s *cmd);
void CL_GetMouseMovement(clientActive_t *cl, float *mx, float *my);
void CL_MouseMove(usercmd_s *cmd);
void CL_UpdateCmdButton(int *cmdButtons, int kbButton, int buttonFlag);
void CL_CmdButtons(usercmd_s *cmd);
void CL_SetUsercmdButtonsWeapons(int buttons, int weapon, int offhand);
void CL_FinishMove(usercmd_s *cmd);
int CG_HandleLocationSelectionInput(int localClientNum, usercmd_s *cmd);
void CL_CreateCmd(usercmd_s *result);
void CL_CreateNewCommands();
void CL_WritePacket();
void CL_ClearClientCommandPacket();
void CL_ClearClientThinkPacket();
void PausedModelPreviewerGamepad();
void CL_Input(int localClientNum);
void CL_ShutdownInput();
void CL_ClearKeys(int localClientNum);
void IN_MLookUp();
void CL_InitInput();

// LWSS ADD - Mouse from PC (MP)
void CL_ShowSystemCursor(bool show);
int CL_MouseEvent(int x, int y, int dx, int dy);
// LWSS END

extern const dvar_t *cl_stanceHoldTime;
extern const dvar_t *cl_analog_attack_threshold;
extern const dvar_t *cl_yawspeed;
extern const dvar_t *cl_upspeed;
extern const dvar_t *cl_sidespeed;
extern const dvar_t *cl_pitchspeed;
extern const dvar_t *cl_forwardspeed;
extern const dvar_t *cl_anglespeedkey;