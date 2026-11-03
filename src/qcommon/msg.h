#pragma once

#ifndef KISAK_SP
#error This File is SinglePlayer Only
#endif

// LWSS: these are aislop generated, may be inaccurate.
#define BUTTON_ATTACK         (1 <<  0)  // +attack
#define BUTTON_SPRINT         (1 <<  1)  // set in CL_KeyMove from kb[KEY_SPRINT=27]
#define BUTTON_MELEE          (1 <<  2)  // +melee
#define BUTTON_USE            (1 <<  3)  // +activate
#define BUTTON_RELOAD         (1 <<  4)  // +reload
#define BUTTON_USE_RELOAD     (1 <<  5)  // +usereload (combined-bind, distinct from USE/RELOAD)
#define BUTTON_LEAN_LEFT      (1 <<  6)  // +leanleft
#define BUTTON_LEAN_RIGHT     (1 <<  7)  // +leanright
#define BUTTON_PRONE          (1 <<  8)  // +prone (held)
#define BUTTON_CROUCH         (1 <<  9)  // crouch stance (set by CL_AddCurrentStanceToCmd)
#define BUTTON_JUMP           (1 << 10)  // +moveup / +gostand
#define BUTTON_ADS            (1 << 11)  // +speed in COD (aim-down-sights)
#define BUTTON_TEMP_STANCE    (1 << 12)  // holding stance key (vs toggled stance)
#define BUTTON_BREATH         (1 << 13)  // +breath (hold-breath while scoped)
#define BUTTON_FRAG           (1 << 14)  // +frag
#define BUTTON_SMOKE          (1 << 15)  // +smoke
#define BUTTON_LOC_CONFIRM    (1 << 16)  // artillery/location confirm
#define BUTTON_LOC_CANCEL     (1 << 17)  // artillery/location cancel
#define BUTTON_NIGHTVISION    (1 << 18)  // +nightvision
#define BUTTON_THROW          (1 << 19)  // +throw (frag throwback)
#define BUTTON_LOC_SELECTING  (1 << 20)  // location-selection UI active
#define BUTTON_BIT_COUNT 21

struct msg_t
{
	int overflowed;
	int readOnly;
	unsigned __int8 *data;
	unsigned __int8 *splitData;
	int maxsize;
	int cursize;
	int splitSize;
	int readcount;
	int bit;
	int lastEntityRef;
};

struct netField_t
{
	const char *name;
	int offset;
	int bits;
};

struct usercmd_s
{
    int serverTime;
    int buttons;
    int angles[3];
    unsigned __int8 weapon;
    unsigned __int8 offHandIndex;
    char forwardmove;
    char rightmove;
    char upmove;
    char pitchmove;
    char yawmove;
    float gunPitch;
    float gunYaw;
    float gunXOfs;
    float gunYOfs;
    float gunZOfs;
    float meleeChargeYaw;
    unsigned __int8 meleeChargeDist;
    char selectedLocation[2];
};

struct hudelem_s;
struct playerState_s;

void TRACK_msg();
void MSG_Init(msg_t *buf, unsigned __int8 *data, int length);
void MSG_BeginReading(msg_t *msg);
void MSG_Truncate(msg_t *msg);
void MSG_WriteBit0(msg_t *msg);
void MSG_WriteBit1(msg_t *msg);
void MSG_WriteBits(msg_t *msg, int value, unsigned int bits);
int MSG_ReadBits(msg_t *msg, unsigned int bits);
int MSG_ReadBit(msg_t *msg);
void MSG_WriteByte(msg_t *msg, unsigned __int8 c);
void MSG_WriteData(msg_t *buf, unsigned char *data, int length);
void MSG_WriteShort(msg_t *msg, __int16 c);
void MSG_WriteLong(msg_t *msg, int c);
void MSG_WriteFloat(msg_t *sb, double f);
void MSG_WriteString(msg_t *sb, char *s);
void MSG_WriteAngle(msg_t *sb, double f);
void MSG_WriteAngle16(msg_t *sb, double f);
void MSG_WriteInt64(msg_t *msg, unsigned __int64 c);
int MSG_ReadByte(msg_t *msg);
int MSG_ReadShort(msg_t *msg);
int MSG_ReadLong(msg_t *msg);
float MSG_ReadFloat(msg_t *msg);
int MSG_ReadString(msg_t *msg, char *buffer, int bufsize);
float MSG_ReadAngle16(msg_t *msg);
void MSG_ReadData(msg_t *msg, unsigned char *data, int len);
int MSG_ReadInt64(msg_t *msg);
void MSG_WriteDelta(msg_t *msg, int oldV, int newV, unsigned int bits);
int MSG_ReadDelta(msg_t *msg, int oldV, unsigned int bits);
void MSG_WriteDeltaFloat(msg_t *msg, double oldV, double newV);
float MSG_ReadDeltaFloat(msg_t *msg, double oldV);
void MSG_WriteDeltaUsercmd(msg_t *msg, const usercmd_s *from, const usercmd_s *to);
void MSG_ReadDeltaUsercmd(msg_t *msg, const usercmd_s *from, usercmd_s *to);
void MSG_WriteDeltaField(msg_t *msg, unsigned __int8 *to, const netField_t *field);
void MSG_ReadDeltaField(msg_t *msg, unsigned __int8 *to, const netField_t *field, int print);
void MSG_WriteDeltaHudElems(msg_t *msg, hudelem_s *to, int count);
void MSG_ReadDeltaHudElems(msg_t *msg, hudelem_s *to, unsigned int count);
void MSG_WriteDeltaPlayerstate(msg_t *msg, playerState_s *to);
void MSG_ReadDeltaPlayerstate(msg_t *msg, playerState_s *to);
int MSG_GetByte(msg_t *msg, int where);
void MSG_GetBytes(msg_t *msg, int where, unsigned __int8 *dest, int len);
