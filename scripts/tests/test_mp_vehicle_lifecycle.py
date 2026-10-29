#!/usr/bin/env python3
"""Exercise production MP vehicle occupancy, use, exit sweeps and steering.

The engine functions are extracted unchanged. Authored doubles provide entity
handles, tag allocation, collision responses and rendering-independent input.
No game assets or native Windows SDK are required.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(source, name):
    start = re.search(r"^(?:static )?(?:void|bool|char|int32_t)\s+(?:__cdecl\s+)?" + re.escape(name) + r"\([^;]*?\)\s*\{", source, re.M)
    if not start:
        raise ValueError(f"Missing production function {name}")
    depth = 1
    for token in re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27|[{}]', source[start.end():], re.S):
        if token.group() == "{": depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start.start():start.end() + token.end()].replace("__cdecl", "")
    raise ValueError(f"Unterminated production function {name}")


PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <set>
#include <string>
#include <map>
#include <functional>
#include <vector>
#ifdef NDEBUG
#error Assertions must remain enabled
#endif
using byte = unsigned char;
#define __int16 short
using mat3x3 = float[3][3];
#define iassert(x) assert(x)
constexpr int ENTITYNUM_NONE=1023, ENTITYNUM_WORLD=1022, MAX_GENTITIES=1024, VEHICLE_RIDESLOTS_COUNT=3;
constexpr int VEHICLE_RIDESLOT_DRIVER=0, VEHICLE_RIDESLOT_PASSENGER=1, VEHICLE_RIDESLOT_GUNNER=2;
constexpr int VEH_WHEELS_4=0,VEH_TANK=1;
constexpr int PMF_VEHICLE_ATTACHED=0x100000, PMF_MANTLE=4, PMF_SPRINTING=8;
constexpr int PM_NORMAL=0, PM_NORMAL_LINKED=1, PM_DEAD=2, PM_DEAD_LINKED=3;
constexpr int ET_VEHICLE=12, TR_INTERPOLATE=1, CONTENTS_USE=0x200000, MASK_WEAPONCLIP=3;
constexpr int ENT_HANDLER_VEHICLE=1, MASK_PLAYERSOLID=0x2810011, TEAM_FREE=0, TEAM_AXIS=1, TEAM_ALLIES=2, TEAM_SPECTATOR=3;
constexpr int CON_CHANNEL_SYSTEM=1, ERR_DROP=1, CON_DISCONNECTED=0;
constexpr int HINT_ACTIVATE=2, WEAPON_HINT_OFFSET=4, FL_CURSOR_HINT=0x10000;
constexpr int WEAPON_OFFHAND_INIT=20, WEAPON_OFFHAND_END=30;
constexpr int CON_CHANNEL_SERVER=2;
constexpr bool alwaysfails=false;
const float vec3_origin[3]={};
struct gentity_s;
struct Handle {
 gentity_s *value=nullptr;
 bool isDefined() const { return value!=nullptr; }
 gentity_s *ent() const { assert(value); return value; }
 int entnum() const;
 void setEnt(gentity_s *e) { value=e; }
};
struct usercmd_s { int serverTime{}, weapon{}, offHandIndex{}, angles[3]{}, buttons{}; };
struct playerState_s {
 unsigned pm_flags{}, weapFlags{}, eFlags{}; int pm_type{}, viewlocked_entNum=ENTITYNUM_NONE;
 float viewangles[3]{}, delta_angles[3]{}, origin[3]{};
 int cursorHint{}, cursorHintString=-1, cursorHintEntIndex=ENTITYNUM_NONE, weaponstate{};
};
struct clientState_s {
 int team=TEAM_ALLIES, modelindex{}, attachModelIndex[6]{}, attachTagIndex[6]{};
 int attachedVehEntNum=ENTITYNUM_NONE, attachedVehSlotIndex{};
};
struct gclient_s {
 playerState_s ps; struct { clientState_s cs; int connected=1; usercmd_s cmd; } sess;
 Handle useHoldEntity; int useHoldTime{};
};
struct VehicleRideSlot_t { uint32_t tagName{}; int boneIdx=-1, entNum=ENTITYNUM_NONE; };
struct vehicle_physic_t { float maxs[3]{}, angles[3]{},mins[3]{},origin[3]{},prevOrigin[3]{},prevAngles[3]{},wheelZPos[4]{}; };
struct scr_vehicle_s {
 int flags{}, team{}, infoIdx{}, entNum=ENTITYNUM_NONE; float speed{};
 struct { VehicleRideSlot_t riderSlots[3];int wheel[4]{-1,-1,-1,-1}; } boneIndex;
 vehicle_physic_t phys;
};
struct tagInfo_s {
 gentity_s *parent{}, *next{}; uint16_t name{}; float axis[4][3]{};
};
struct Shared {
 bool inuse=true; Handle ownerNum;
 float currentOrigin[3]{}, currentAngles[3]{}, mins[3]{-15,-15,0}, maxs[3]{15,15,70};
 float absmin[3]{-40,-30,0}, absmax[3]{40,30,40};
 int contents=CONTENTS_USE, svFlags{};bool bmodel=false;
};
struct EntityState {
 int number{}, clientNum{}, eType=ET_VEHICLE, time2{},loopSound{}, weapon{}, weaponModel{}, model{};
 struct { int eFlags{}; struct {int trType{};} pos,apos;
  struct { struct {float bodyPitch{},bodyRoll{},steerYaw{},gunPitch{},gunYaw{}; int materialTime{},teamAndOwnerIndex{};} vehicle; }u;
 }lerp;
};
struct gentity_s {
 EntityState s; Shared r; gclient_s *client{}; scr_vehicle_s *scr_vehicle{};
 tagInfo_s *tagInfo{}; gentity_s *tagChildren{};
 uint16_t classname=72; int health=100,flags{},active{},model{},spawnflags{},handler{},nextthink{},clipmask{},takedamage{};
 int attachModelNames[6]{},attachTagNames[6]{};
};
int Handle::entnum() const { return ent()->s.number; }
gentity_s g_entities[MAX_GENTITIES];
gclient_s clients[8];
scr_vehicle_s s_vehicles[8];
struct { int time=100; } level;
struct clientInfo_t { bool infoValid=true; char model[64]{},attachModelNames[6][64]{},attachTagNames[6][64]{}; };
struct { clientInfo_t clientinfo[64]; } level_bgs;
struct { uint16_t script_vehicle=71, script_vehicle_collmap=73,tag_driver=10,tag_passenger=11; } scr_const;
struct vehicle_info_t { char turretWeapon[64]{};int type=VEH_WHEELS_4; };
vehicle_info_t s_vehicleInfos[8];
struct trace_t { bool startsolid{},allsolid{}; float fraction=1;int hitType{},hitId{};float normal[3]{}; };
struct IgnoreEntParams { int baseEntity{},parentEntity{},ignoreParent{},ignoreSiblings=1;bool ignoreSelf=true,ignoreChildren=true; };
struct WeaponDef { const char *dropHintString=""; int dropHintStringIndex=13; } weapon;
std::set<tagInfo_s*> tags;
int teleports, linked, collisionSetups, traceSweeps, traceChecks;
int blockCandidate=-1; bool blockAll, noGround, lowCeiling, endpointSolid, startSolid, scriptActive=true;
float teleported[3]{}; gentity_s *lastLinkPlayer{}, *lastLinkVehicle{};
float AngleNormalize360(float a) { a=std::fmod(a,360.0f); return a<0?a+360:a; }
float AngleNormalize180(float a) { a=AngleNormalize360(a); return a>180?a-360:a; }
float I_fabs(float a) { return std::fabs(a); }
void MyAssertHandler(const char*,int,int,const char*,...) { throw std::runtime_error("Engine invariant failed"); }
void Com_Error(int,const char*,...) { throw std::runtime_error("Engine error"); }
void Com_PrintWarning(int,const char*,...) {}
const char *va(const char*,...) { return "unused"; }
void Vec3Sub(const float*a,const float*b,float*c) { for(int i=0;i<3;++i)c[i]=a[i]-b[i]; }
float Vec3Length(const float*a) { return std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); }
void Vec3Lerp(const float*a,const float*b,float f,float*c) { for(int i=0;i<3;++i)c[i]=a[i]+(b[i]-a[i])*f; }
void updateBounds(gentity_s*);
void G_SetOrigin(gentity_s*e,const float*v) { std::memcpy(e->r.currentOrigin,v,12); updateBounds(e); }
void G_SetAngle(gentity_s*e,const float*v) { std::memcpy(e->r.currentAngles,v,12); }
void SetClientViewAngle(gentity_s*e,const float*v) { std::memcpy(e->client->ps.viewangles,v,12); }
void TeleportPlayer(gentity_s*e,const float*v,const float*a) { ++teleports; std::memcpy(teleported,v,12);G_SetOrigin(e,v);SetClientViewAngle(e,a); }
void Scr_SetString(uint16_t*p,uint16_t v) { *p=v; }
void MT_Free(byte*p,unsigned length) { assert(length==sizeof(tagInfo_s));auto*t=reinterpret_cast<tagInfo_s*>(p);assert(tags.erase(t));delete t; }
void SV_LinkEntity(gentity_s*e) { ++linked;updateBounds(e); }
void SV_DObjGetBounds(gentity_s*,float*,float*) {}
int G_GetWeaponIndexForName(const char*) { return 1; }
void SetupCollisionMap(gentity_s*) { ++collisionSetups; }
void VEH_GetWheelOrigin(gentity_s*,int i,float*out) { out[0]=i==0?-20:20;out[1]=out[2]=0; }
void VEH_GroundPlant(gentity_s*e,int gravity,float frame) { assert(gravity==0&&frame==0.05f);e->scr_vehicle->phys.origin[2]=7; }
void VEH_SetPosition(gentity_s*e,const float*origin,const float*angles) { G_SetOrigin(e,origin);G_SetAngle(e,angles); }
void VEH_TouchEntities_0(gentity_s*,float frame) { assert(frame==0.05f); }

int G_ModelName(int index) { return index; }
const char* SL_ConvertToString(int) { return "model"; }
void I_strncpyz(char*o,const char*i,unsigned n) { std::snprintf(o,n,"%s",i); }
int G_TagIndex(char*) { return 1; }
void G_DObjGetWorldBoneIndexPos(gentity_s*,int bone,float*o) { o[0]=float(bone)*20; o[1]=0;o[2]=40; }
void G_DObjGetWorldBoneIndexMatrix(gentity_s*e,int bone,float(*m)[3]) {
 std::memset(m,0,sizeof(float)*12);for(int i=0;i<3;++i)m[i][i]=1;
 G_DObjGetWorldBoneIndexPos(e,bone,m[3]);
}
void AxisToAngles(const mat3x3&,float*a) { std::memset(a,0,12); }
int G_EntLinkToWithOffset(gentity_s*player,gentity_s*parent,unsigned tag,const float*offset,const float*) {
 assert(!player->tagInfo); auto*t=new tagInfo_s{};tags.insert(t);
 t->parent=parent;t->next=parent->tagChildren;t->name=uint16_t(tag);
 player->tagInfo=t;parent->tagChildren=player;lastLinkPlayer=player;lastLinkVehicle=parent;
 for(int i=0;i<3;++i) { player->r.currentOrigin[i]+=offset[i]; } updateBounds(player);return 1;
}
#define __int64 int64_t
#define HIDWORD(x) reinterpret_cast<uint32_t*>(&(x))[1]
#define LODWORD(x) reinterpret_cast<uint32_t*>(&(x))[0]
#define LOWORD(x) uint16_t(x)
#define IS_NAN(x) std::isnan(x)
#define PROF_SCOPED(x) ((void)0)
constexpr int TRACE_HITTYPE_NONE=0, TRACE_HITTYPE_ENTITY=1;
struct TraceExtents { float start[3]{},end[3]{},invDelta[3]{}; };
struct moveclip_t {
 float mins[3]{},maxs[3]{},outerSize[3]{}; TraceExtents extents;
 int passEntityNum=ENTITYNUM_NONE,passOwnerNum=-1,contentmask{};
};
struct pointtrace_t {
 int contentmask{};TraceExtents extents;const IgnoreEntParams*ignoreEntParams{};
 int bLocational{};uint8_t*priorityMap{};
};
struct svEntity_s { int linkcontents{}; };
struct { svEntity_s svEntities[MAX_GENTITIES]; }sv;
float ceilingHeight=85;
std::vector<int> clippedEntities;
void Vec3Add(const float*a,const float*b,float*c) { for(int i=0;i<3;++i)c[i]=a[i]+b[i]; }
void Vec3Scale(const float*a,float f,float*c) { for(int i=0;i<3;++i)c[i]=a[i]*f; }
gentity_s*SV_GentityNum(int n) { assert(n>=0&&n<MAX_GENTITIES);return &g_entities[n]; }
void CM_CalcTraceExtents(TraceExtents*) {}
void updateBounds(gentity_s*e) { for(int i=0;i<3;++i){e->r.absmin[i]=e->r.currentOrigin[i]+e->r.mins[i];e->r.absmax[i]=e->r.currentOrigin[i]+e->r.maxs[i];} }
void CM_BoxTrace(trace_t*t,const float*start,const float*end,const float*mins,const float*maxs,int,int mask) {
 assert(mask==MASK_PLAYERSOLID);*t={};
 bool point=std::equal(start,start+3,end);
 bool sweep=!point && std::equal(start,start+3,g_entities[0].r.currentOrigin);
 if(sweep) {
  ++traceSweeps;
  if(startSolid)t->startsolid=true;
  if(blockAll||traceSweeps-1==blockCandidate)t->fraction=0.5f;
 } else { ++traceChecks; if(endpointSolid)t->startsolid=true; }
 float feet=start[2]+mins[2],endFeet=end[2]+mins[2];
 if(feet<0){t->startsolid=true;t->allsolid=endFeet<0;t->fraction=0;}
 else if(endFeet<0&&!noGround){t->fraction=std::min(t->fraction,feet/(feet-endFeet));t->normal[2]=1;}
 if(lowCeiling){
  float head=start[2]+maxs[2],endHead=end[2]+maxs[2];
  if(head>ceilingHeight){t->startsolid=true;t->allsolid=endHead>ceilingHeight;t->fraction=0;}
  else if(endHead>ceilingHeight)t->fraction=std::min(t->fraction,(ceilingHeight-head)/(endHead-head));
 }
}
int CM_AreaEntities(const float*mins,const float*maxs,int*out,int maxcount,int mask) {
 int count=0;
 for(int n=0;n<MAX_GENTITIES;++n){
  auto&e=g_entities[n];if(!e.r.inuse||!(e.r.contents&mask))continue;
  bool hit=true;for(int i=0;i<3;++i)if(maxs[i]<e.r.absmin[i]||mins[i]>e.r.absmax[i])hit=false;
  if(hit){assert(count<maxcount);out[count++]=n;}
 }
 return count;
}
uint32_t SV_ClipHandleForEntity(const gentity_s*e) { return unsigned(e->s.number); }
int CM_TraceBox(const TraceExtents*,const float*,const float*,float) { return 0; }
void CM_TransformedBoxTrace(trace_t*t,const float*start,const float*end,const float*mins,const float*maxs,int64_t model,const float*,const float*) {
 int n=int(uint64_t(model)&0xffffffff);assert(n>=0&&n<MAX_GENTITIES);clippedEntities.push_back(n);
 const auto&e=g_entities[n];float enter=0,leave=1;int normalAxis=-1;float normalSign=0;
 bool inside=true,endInside=true;
 for(int axis=0;axis<3;++axis){
  float low=e.r.absmin[axis]-maxs[axis], high=e.r.absmax[axis]-mins[axis];
  inside&=start[axis]>low&&start[axis]<high;endInside&=end[axis]>low&&end[axis]<high;
  float d=end[axis]-start[axis];
  if(d==0){if(start[axis]<=low||start[axis]>=high)return;continue;}
  float a=(low-start[axis])/d,b=(high-start[axis])/d;
  float near=std::min(a,b),far=std::max(a,b);
  if(near>enter){enter=near;normalAxis=axis;normalSign=d>0?-1:1;}
  leave=std::min(leave,far);if(enter>leave)return;
 }
 if(inside){t->startsolid=true;t->allsolid=endInside;t->fraction=0;return;}
 if(enter<=t->fraction&&enter>=0&&leave>=0&&enter<=1){
  t->fraction=enter;if(normalAxis>=0)t->normal[normalAxis]=normalSign;
 }
}
void SV_ClipMoveToEntity(const moveclip_t*,svEntity_s*,trace_t*);
void CM_ClipMoveToEntities(moveclip_t*c,trace_t*t) {
 for(int n=0;n<MAX_GENTITIES;++n)if(g_entities[n].r.inuse&&(g_entities[n].r.contents&c->contentmask))SV_ClipMoveToEntity(c,&sv.svEntities[n],t);
}
void CM_PointTraceStaticModels(trace_t*,const float*,const float*,int) { throw std::runtime_error("unexpected point trace"); }
void CM_PointTraceToEntities(pointtrace_t*,trace_t*) { throw std::runtime_error("unexpected point trace"); }
WeaponDef *BG_GetWeaponDef(int) { return &weapon; }
bool Scr_IsSystemActive() { return scriptActive; }
struct clientActive_t {
 int cgameUserCmdWeapon=7,cgameUserCmdOffHandIndex=3,serverTime=100;
 struct { int serverTime=100;playerState_s ps; }snap;
 float viewangles[3]{},cgameKickAngles[3]{};unsigned cgameExtraButtons{};
 float vehicleViewYaw{},vehicleViewPitch{};
} input;
struct dvar_s {struct {float value;bool enabled=false;} current;};
dvar_s minHeight{{-15}},maxHeight{{50}};
const dvar_s *vehDriverViewHeightMin=&minHeight,*vehDriverViewHeightMax=&maxHeight;
bool driving=true;
clientActive_t*CL_GetLocalClientGlobals(int) { return &input; }
bool CG_VehLocalClientDriving(int) { return driving; }
#define DEG2RAD(a) ((a)*3.14159265358979323846/180.0)
struct cg_s {
 playerState_s predictedPlayerState;
 struct { float vieworg[3]{},viewaxis[3][3]{}; } refdef;
 float refdefViewAngles[3]{};
} cgame;
const float TEMP_OFFSET[3]={0,0,55},colorBlue[4]={0,0,1,1};
dvar_s viewDist{{300}},viewFocusRange{{50}},debugClient{{0}};
const dvar_s *vehDriverViewDist=&viewDist,*vehDriverViewFocusRange=&viewFocusRange,*vehDebugClient=&debugClient;
float recordedFocus[3]{};
cg_s*CG_GetLocalClientGlobals(int) { return &cgame; }
void CG_DebugStar(const float*,const float*,int) {}
float Vec3Normalize(float*v) { float len=Vec3Length(v);assert(len>0);for(int i=0;i<3;++i)v[i]/=len;return len; }
void vectoangles(const float*v,float*a) { a[0]=float(std::atan2(-v[2],std::hypot(v[0],v[1]))*180.0/3.14159265358979323846);a[1]=float(std::atan2(v[1],v[0])*180.0/3.14159265358979323846);a[2]=0; }
void AnglesToAxis(const float*,float(*a)[3]) { for(int i=0;i<3;++i)for(int j=0;j<3;++j)a[i][j]=float(i==j); }
void ThirdPersonViewTrace(cg_s*,const float*focus,const float*end,int,float*out) { std::copy(focus,focus+3,recordedFocus);std::copy(end,end+3,out); }
void CG_CalcFov(int) {}
void G_VehUnlinkPlayer(gentity_s*,gentity_s*);
void VehicleClearRideSlotForPlayer(gentity_s*,int32_t);
char VehicleHasSeatFree(gentity_s*);
void LinkPlayerToVehicle(gentity_s*,gentity_s*);
void G_EntUnlink(gentity_s*);
'''

TESTS = r'''
void reset() {
 for(auto*t:tags) { delete t; } tags.clear();
 std::fill(std::begin(g_entities),std::end(g_entities),gentity_s{});
 std::fill(std::begin(clients),std::end(clients),gclient_s{});
 std::fill(std::begin(s_vehicles),std::end(s_vehicles),scr_vehicle_s{});
 for(int i=0;i<MAX_GENTITIES;++i){g_entities[i].s.number=i;g_entities[i].r.inuse=false;g_entities[i].r.contents=0;}
 for(int i=0;i<8;++i){g_entities[i].client=&clients[i];g_entities[i].s.clientNum=i;g_entities[i].r.contents=0x2000000;g_entities[i].r.inuse=true;g_entities[i].r.currentOrigin[0]=1000+float(i)*200;updateBounds(&g_entities[i]);}
 for(auto&ci:level_bgs.clientinfo)ci=clientInfo_t{};
 auto&v=g_entities[100];v.scr_vehicle=&s_vehicles[0];v.scr_vehicle->entNum=100;
 v.r.inuse=true;v.r.contents=0x800000|CONTENTS_USE;v.r.absmin[0]=-40;v.r.absmin[1]=-30;v.r.absmin[2]=0;v.r.absmax[0]=40;v.r.absmax[1]=30;v.r.absmax[2]=40;
 v.scr_vehicle->flags=8; v.classname=72; // actual script_vehicle_mp, not script_vehicle.
 v.scr_vehicle->phys.maxs[2]=40;
 for(int i=0;i<3;++i){auto&r=v.scr_vehicle->boneIndex.riderSlots[i];r.boneIdx=i;r.tagName=10+i;}
 teleports=linked=collisionSetups=traceSweeps=traceChecks=0;
 blockCandidate=-1;blockAll=noGround=lowCeiling=endpointSolid=startSolid=false;
 scriptActive=driving=true;input=clientActive_t{};weapon=WeaponDef{};minHeight.current.value=-15;maxHeight.current.value=50;ceilingHeight=85;clippedEntities.clear();cgame=cg_s{};s_vehicleInfos[0].type=VEH_WHEELS_4;
}
gentity_s* vehicle() { return &g_entities[100]; }
gentity_s* player(int i=0) { return &g_entities[i]; }
void attach(int seat,int client=0) {
 auto*p=player(client);p->r.currentOrigin[0]=seat*20.0f;p->r.currentOrigin[2]=40;
 LinkPlayerToVehicle(vehicle(),p);
 assert(p->r.ownerNum.ent()==vehicle());assert(p->tagInfo);
 assert(vehicle()->scr_vehicle->boneIndex.riderSlots[seat].entNum==client);
}
void assertDetached(int client=0) {
 auto*p=player(client);assert(!p->tagInfo);assert(!p->r.ownerNum.isDefined());
 assert(!(p->client->ps.pm_flags&PMF_VEHICLE_ATTACHED));
 assert(!(p->client->ps.weapFlags&0x80));assert(p->client->ps.viewlocked_entNum==ENTITYNUM_NONE);
}
void testAttachEachSeat() {
 for(int seat=0;seat<3;++seat){reset();attach(seat);assert(vehicle()->scr_vehicle->flags&1);
  assert(player()->client->ps.weapFlags==unsigned(seat==1?0:0x80));}
}
void testSpawnedMapClass() { G_VehiclesSetupSpawnedEnts();assert(collisionSetups==1);assert(vehicle()->classname!=scr_const.script_vehicle); }
void testUsableSpawnInitialization() { auto*v=vehicle();v->spawnflags=1;InitEntityVars(v,v->scr_vehicle,0);assert(v->r.contents&CONTENTS_USE);assert(v->handler==ENT_HANDLER_VEHICLE);assert(linked==1); }
void testUnlinkEachSeat() { for(int s=0;s<3;++s){reset();attach(s);G_EntUnlink(player());assertDetached();assert(!vehicle()->tagChildren);assert(!(vehicle()->scr_vehicle->flags&1));assert(tags.empty());} }
void testDisconnectSeatReuse() { attach(0);G_EntUnlink(player());player()->client=nullptr;assert(G_VehUsable(vehicle(),player(1)));player(1)->r.currentOrigin[0]=0;player(1)->r.currentOrigin[2]=40;LinkPlayerToVehicle(vehicle(),player(1));assert(vehicle()->scr_vehicle->boneIndex.riderSlots[0].entNum==1); }
void testOtherRidersKeepActive() { attach(0,0);attach(1,1);attach(2,2);G_EntUnlink(player(1));assert(vehicle()->scr_vehicle->flags&1);assert(vehicle()->scr_vehicle->boneIndex.riderSlots[0].entNum==0);assert(vehicle()->scr_vehicle->boneIndex.riderSlots[2].entNum==2);G_EntUnlink(player(2));assert(vehicle()->scr_vehicle->flags&1);G_EntUnlink(player());assert(!(vehicle()->scr_vehicle->flags&1)); }
void testFreeVehicleChildren() { attach(0,0);attach(1,1);attach(2,2);while(vehicle()->tagChildren)G_EntUnlink(vehicle()->tagChildren);for(int i=0;i<3;++i)assertDetached(i);assert(tags.empty()); }
void testUnrelatedLinkedChild() { G_EntLinkToWithOffset(player(),vehicle(),0,vec3_origin,vec3_origin);G_EntUnlink(player());assert(teleports==0);assert(!player()->tagInfo); }
void testStandardLinkedEntity() { auto*parent=&g_entities[101];G_EntLinkToWithOffset(player(),parent,0,vec3_origin,vec3_origin);player()->client->ps.pm_type=PM_NORMAL_LINKED;G_EntUnlink(player());assert(player()->client->ps.pm_type==PM_NORMAL);assert(teleports==0); }
void testExitHintRoutesUse() { attach(0);Player_SetVehicleDropHint(player());assert(player()->client->ps.cursorHint==HINT_ACTIVATE);assert(player()->client->ps.cursorHintString==-1);assert(player()->client->ps.cursorHintEntIndex==100);assert(Player_ActivateCmd(player()));assert(player()->client->useHoldEntity.ent()==vehicle());G_VehEntHandler_Use(vehicle(),player(),player());assertDetached(); }
void testWeaponExitHint() { attach(2);vehicle()->s.weapon=7;weapon.dropHintString="drop";Player_SetVehicleDropHint(player());assert(player()->client->ps.cursorHint==7+WEAPON_HINT_OFFSET);assert(player()->client->ps.cursorHintString==13); }
void testClientInfoAttached() { attach(1);G_UpdateClientInfo(player());assert(clients[0].sess.cs.attachedVehEntNum==100);assert(clients[0].sess.cs.attachedVehSlotIndex==1);G_EntUnlink(player());G_UpdateClientInfo(player());assert(clients[0].sess.cs.attachedVehEntNum==ENTITYNUM_NONE); }
void testReentrantUnlink() { attach(0);G_EntUnlink(player());int n=teleports;G_EntUnlink(player());assert(teleports==n);assert(tags.empty()); }
void testBlockedExitRetainsSeat() { attach(0);blockAll=true;G_VehEntHandler_Use(vehicle(),player(),player());assert(player()->tagInfo);assert(vehicle()->scr_vehicle->boneIndex.riderSlots[0].entNum==0);assert(teleports==0); }
void testBlockedForcedUnlink() { attach(0);blockAll=true;G_EntUnlink(player());assertDetached();assert(teleports==0);assert(tags.empty()); }
void testExitTracesFullHull() { attach(0);G_VehEntHandler_Use(vehicle(),player(),player());assertDetached();assert(traceSweeps>=2 && traceChecks>=4);assert(teleports==1);assert(teleported[0]>vehicle()->r.absmax[0]);assert(std::fabs(teleported[2])<0.001f); }
void testExitTriesAnotherSide() { attach(0);blockCandidate=0;float out[3]{};assert(G_VehFindExitPosition(vehicle(),player(),out));assert(traceSweeps==2);assert(out[0]<vehicle()->r.absmin[0]); }
void testLowCeilingExitNoTeleport() { attach(0);lowCeiling=true;ceilingHeight=60;G_VehEntHandler_Use(vehicle(),player(),player());assert(player()->tagInfo);assert(teleports==0); }
void testEndpointOverlapRejected() { attach(0);endpointSolid=true;float out[3]{};assert(!G_VehFindExitPosition(vehicle(),player(),out));assert(traceSweeps==5);assert(traceChecks==5); }
void testNoWorldGroundUsesRoof() { attach(0);noGround=true;G_VehEntHandler_Use(vehicle(),player(),player());assertDetached();assert(teleports==1);assert(teleported[2]>=vehicle()->r.absmax[2]); }
void testStartSolidRejected() { attach(0);startSolid=true;float out[3]{};assert(!G_VehFindExitPosition(vehicle(),player(),out)); }
void testDifferentVehicleUseNoUnlink() { attach(0);G_VehEntHandler_Use(&g_entities[101],player(),player());assert(player()->tagInfo);assert(teleports==0); }
void testEnemyCannotShare() { attach(0);clients[1].sess.cs.team=TEAM_AXIS;assert(!G_VehUsable(vehicle(),player(1)));G_VehEntHandler_Use(vehicle(),player(1),player(1));assert(!player(1)->tagInfo); }
void testTeamRestrictedVehicle() { vehicle()->scr_vehicle->team=TEAM_AXIS;assert(!G_VehUsable(vehicle(),player()));clients[0].sess.cs.team=TEAM_AXIS;assert(G_VehUsable(vehicle(),player())); }
void testFriendlyCanShare() { attach(0);assert(G_VehUsable(vehicle(),player(1))); }
void testFreeForAllCanShare() { clients[0].sess.cs.team=TEAM_FREE;clients[1].sess.cs.team=TEAM_FREE;attach(0);assert(G_VehUsable(vehicle(),player(1))); }
void testSpectatorCannotEnter() { clients[0].sess.cs.team=TEAM_SPECTATOR;assert(!G_VehUsable(vehicle(),player())); }
void testDeadCannotEnter() { player()->health=0;G_VehEntHandler_Use(vehicle(),player(),player());assert(!player()->tagInfo); }
void testMovingCannotEnter() { vehicle()->scr_vehicle->speed=2;G_VehEntHandler_Use(vehicle(),player(),player());assert(!player()->tagInfo); }
void testFullOrMissingSeat() { for(auto&s:vehicle()->scr_vehicle->boneIndex.riderSlots)s.boneIdx=-1;assert(!G_VehUsable(vehicle(),player())); }
void testInvalidRiderNotDereferenced() { vehicle()->scr_vehicle->boneIndex.riderSlots[0].entNum=MAX_GENTITIES;assert(!G_VehUsable(vehicle(),player())); }
void testNoVehicleCannotEnter() { vehicle()->scr_vehicle=nullptr;assert(!G_VehUsable(vehicle(),player())); }
void testInputYawAndPitch() { input.viewangles[0]=15;input.viewangles[1]=120;usercmd_s cmd;CL_FinishMove(0,&cmd);assert(input.vehicleViewYaw==30);assert(input.vehicleViewPitch==15);assert(cmd.angles[1]==uint16_t(int(120*182.0444488525391)));assert(cmd.angles[0]==uint16_t(int(15*182.0444488525391))); }
void testInputWorldDelta() { input.viewangles[0]=-10;input.viewangles[1]=20;input.snap.ps.delta_angles[0]=15;input.snap.ps.delta_angles[1]=70;usercmd_s cmd;CL_FinishMove(0,&cmd);assert(input.vehicleViewYaw==0);assert(input.vehicleViewPitch==5);assert(cmd.angles[1]==uint16_t(int(20*182.0444488525391))); }
void testInputWrap() { input.viewangles[0]=370;input.viewangles[1]=-370;usercmd_s cmd;CL_FinishMove(0,&cmd);assert(input.vehicleViewYaw==260);assert(input.vehicleViewPitch==10); }
void testNonDriverInputUnchanged() { driving=false;input.viewangles[2]=10;input.vehicleViewYaw=13;input.cgameExtraButtons=4;usercmd_s cmd;CL_FinishMove(0,&cmd);assert(input.vehicleViewYaw==13);assert(cmd.angles[2]!=0);assert(cmd.buttons&4);assert(input.cgameExtraButtons==0); }
void testSteeringStraightAndTurns() { auto*p=player();vehicle_physic_t phys;phys.angles[1]=90;p->client->ps.delta_angles[1]=70;for(int yaw:{10,20,30}){p->client->sess.cmd.angles[1]=uint16_t(int(yaw*65536.0/360.0));int turn=static_cast<signed char>(VEH_PlayerRotation(p,&phys));if(yaw==20)assert(std::abs(turn)<=1);else if(yaw==10)assert(turn>0);else assert(turn<0);} }
void testSteeringWrapAndClamp() { auto*p=player();vehicle_physic_t phys;phys.angles[1]=359;p->client->sess.cmd.angles[1]=uint16_t(int(1*65536.0/360.0));assert(static_cast<signed char>(VEH_PlayerRotation(p,&phys))<0);p->client->sess.cmd.angles[1]=uint16_t(int(90*65536.0/360.0));assert(static_cast<signed char>(VEH_PlayerRotation(p,&phys))==-127); }
void testTraceParentFlagAndLegacyPath() {
 attach(0);IgnoreEntParams ignore;SV_SetupIgnoreEntParams(&ignore,0);ignore.ignoreParent=true;
 float end[3]={57,0,5};trace_t oldResult,newResult;
 SV_Trace(&oldResult,player()->r.currentOrigin,player()->r.mins,player()->r.maxs,end,&ignore,MASK_PLAYERSOLID,0,nullptr,0);
 assert(oldResult.startsolid); // The legacy hull path intentionally stays unchanged.
 clippedEntities.clear();
 SV_TraceCapsuleWithIgnore(&newResult,player()->r.currentOrigin,player()->r.mins,player()->r.maxs,end,&ignore,MASK_PLAYERSOLID);
 assert(!newResult.startsolid&&newResult.fraction==1);
 assert(std::find(clippedEntities.begin(),clippedEntities.end(),100)==clippedEntities.end());
}
void testTraceEndpointIncludesOtherOccupant() {
 attach(0,0);attach(1,1);vehicle()->r.contents=0;
 IgnoreEntParams ignore;SV_SetupIgnoreEntParams(&ignore,0);ignore.ignoreSiblings=false;
 trace_t result,oldResult;const float*where=player(1)->r.currentOrigin;
 SV_Trace(&oldResult,where,player()->r.mins,player()->r.maxs,where,&ignore,MASK_PLAYERSOLID,0,nullptr,0);
 assert(!oldResult.startsolid);
 SV_TraceCapsuleWithIgnore(&result,where,player()->r.mins,player()->r.maxs,where,&ignore,MASK_PLAYERSOLID);
 assert(result.startsolid);
 assert(std::find(clippedEntities.begin(),clippedEntities.end(),1)!=clippedEntities.end());
 ignore.ignoreSiblings=true;
 SV_TraceCapsuleWithIgnore(&result,where,player()->r.mins,player()->r.maxs,where,&ignore,MASK_PLAYERSOLID);
 assert(!result.startsolid);
}
void testTraceSelfFlag() {
 attach(0);vehicle()->r.contents=0;
 IgnoreEntParams ignore;SV_SetupIgnoreEntParams(&ignore,0);ignore.ignoreSelf=false;
 trace_t result;const float*where=player()->r.currentOrigin;
 SV_TraceCapsuleWithIgnore(&result,where,player()->r.mins,player()->r.maxs,where,&ignore,MASK_PLAYERSOLID);
 assert(result.startsolid);
 ignore.ignoreSelf=true;
 SV_TraceCapsuleWithIgnore(&result,where,player()->r.mins,player()->r.maxs,where,&ignore,MASK_PLAYERSOLID);
 assert(!result.startsolid);
}
void testTraceChildrenFlag() {
 attach(0);auto&child=g_entities[101];child.r.inuse=true;child.r.contents=0x2000000;
 child.r.ownerNum.setEnt(player());child.r.currentOrigin[0]=200;child.r.currentOrigin[2]=5;updateBounds(&child);
 IgnoreEntParams ignore;SV_SetupIgnoreEntParams(&ignore,0);trace_t result;
 SV_TraceCapsuleWithIgnore(&result,child.r.currentOrigin,player()->r.mins,player()->r.maxs,child.r.currentOrigin,&ignore,MASK_PLAYERSOLID);
 assert(!result.startsolid);
 ignore.ignoreChildren=false;
 SV_TraceCapsuleWithIgnore(&result,child.r.currentOrigin,player()->r.mins,player()->r.maxs,child.r.currentOrigin,&ignore,MASK_PLAYERSOLID);
 assert(result.startsolid);
}
void testTraceWorldHitPreservesNormal() {
 IgnoreEntParams ignore{};ignore.baseEntity=ENTITYNUM_NONE;ignore.parentEntity=-1;
 float start[3]={500,0,100},end[3]={500,0,-20};trace_t result;
 SV_TraceCapsuleWithIgnore(&result,start,player()->r.mins,player()->r.maxs,end,&ignore,MASK_PLAYERSOLID);
 assert(std::fabs(result.fraction-100.0f/120.0f)<0.0001f);
 assert(result.hitType==TRACE_HITTYPE_ENTITY&&result.hitId==ENTITYNUM_WORLD&&result.normal[2]==1);
}
void testTraceEntityHitBeforeWorld() {
 IgnoreEntParams ignore{};ignore.baseEntity=ENTITYNUM_NONE;ignore.parentEntity=-1;
 float start[3]={0,0,150},end[3]={0,0,-100};trace_t result;
 SV_TraceCapsuleWithIgnore(&result,start,player()->r.mins,player()->r.maxs,end,&ignore,MASK_PLAYERSOLID);
 assert(std::fabs(result.fraction-110.0f/250.0f)<0.0001f);
 assert(result.hitType==TRACE_HITTYPE_ENTITY&&result.hitId==100&&result.normal[2]==1);
}
void testTraceWorldStartSolid() {
 IgnoreEntParams ignore{};ignore.baseEntity=ENTITYNUM_NONE;ignore.parentEntity=-1;
 float start[3]={500,0,-5},end[3]={500,0,-1};trace_t result;
 SV_TraceCapsuleWithIgnore(&result,start,player()->r.mins,player()->r.maxs,end,&ignore,MASK_PLAYERSOLID);
 assert(result.startsolid&&result.allsolid&&result.fraction==0&&result.hitId==ENTITYNUM_WORLD);
 assert(clippedEntities.empty());
}
void testTraceStationaryParentOverlap() {
 attach(0);IgnoreEntParams ignore;SV_SetupIgnoreEntParams(&ignore,0);ignore.ignoreParent=false;
 trace_t result;const float*where=player()->r.currentOrigin;
 SV_TraceCapsuleWithIgnore(&result,where,player()->r.mins,player()->r.maxs,where,&ignore,MASK_PLAYERSOLID);
 assert(result.startsolid);
}
void testInputPitchLimits() {
 usercmd_s cmd;input.viewangles[0]=85;CL_FinishMove(0,&cmd);assert(input.vehicleViewPitch==50);
 input.viewangles[0]=-85;CL_FinishMove(0,&cmd);assert(input.vehicleViewPitch==-15);
}
void testInputPitchInvertedAndEqualLimits() {
 usercmd_s cmd;minHeight.current.value=30;maxHeight.current.value=-10;input.viewangles[0]=70;
 CL_FinishMove(0,&cmd);assert(input.vehicleViewPitch==30);
 minHeight.current.value=maxHeight.current.value=0;CL_FinishMove(0,&cmd);assert(input.vehicleViewPitch==0);
}
void testCameraFocusWithinRange() {
 input.vehicleViewPitch=0;CalcViewValuesVehicleDriver(0);assert(recordedFocus[2]==105);
 input.vehicleViewPitch=50;CalcViewValuesVehicleDriver(0);assert(recordedFocus[2]==55);
 input.vehicleViewPitch=80;CalcViewValuesVehicleDriver(0);assert(recordedFocus[2]==55);
}
void testCameraZeroOrNegativeMaximum() {
 maxHeight.current.value=0;input.vehicleViewPitch=0;CalcViewValuesVehicleDriver(0);assert(recordedFocus[2]==55);
 maxHeight.current.value=-10;input.vehicleViewPitch=-30;CalcViewValuesVehicleDriver(0);assert(recordedFocus[2]==55);
 for(float v:cgame.refdef.vieworg)assert(std::isfinite(v));
}
void testFirstThinkFourWheelInitialization() {
 auto*v=vehicle();v->scr_vehicle->flags=0;InitFirstThink(v);
 assert(v->scr_vehicle->flags&8);assert(v->scr_vehicle->phys.mins[0]==-30);
 assert(v->scr_vehicle->phys.maxs[0]==30&&v->scr_vehicle->phys.maxs[2]==60);
 assert(v->health==99999&&v->handler==ENT_HANDLER_VEHICLE&&v->nextthink==level.time+50);
}
void testFirstThinkWheelHeightTags() {
 auto&veh=*vehicle()->scr_vehicle;veh.boneIndex.wheel[0]=1;veh.boneIndex.wheel[2]=2;
 veh.phys.wheelZPos[1]=-4;InitFirstThink(vehicle());
 assert(veh.phys.wheelZPos[0]==40&&veh.phys.wheelZPos[2]==40&&veh.phys.wheelZPos[1]==-4);
}
void testFirstThinkBacksUpPosition() {
 auto&phys=vehicle()->scr_vehicle->phys;phys.origin[0]=10;phys.origin[1]=20;phys.angles[1]=90;
 InitFirstThink(vehicle());assert(phys.prevOrigin[0]==10&&phys.prevOrigin[1]==20&&phys.prevOrigin[2]==7);
 assert(phys.prevAngles[1]==90&&vehicle()->r.currentAngles[1]==90);
}
void testFirstThinkHelicopterPreservesBounds() {
 s_vehicleInfos[0].type=5;auto&phys=vehicle()->scr_vehicle->phys;
 phys.mins[0]=-12;phys.maxs[0]=12;InitFirstThink(vehicle());assert(phys.mins[0]==-12&&phys.maxs[0]==12);
}
void testFirstThinkUnsupportedTankStillRejected() {
 s_vehicleInfos[0].type=VEH_TANK;bool rejected=false;
 try { InitFirstThink(vehicle()); } catch(const std::runtime_error&) { rejected=true; }
 assert(rejected);
}
int main(int argc,char**argv) {
 const std::map<std::string,std::function<void()>> cases={
#define CASE(name) {#name,test##name}
 CASE(AttachEachSeat),CASE(SpawnedMapClass),CASE(UsableSpawnInitialization),CASE(UnlinkEachSeat),
 CASE(DisconnectSeatReuse),CASE(OtherRidersKeepActive),CASE(FreeVehicleChildren),CASE(UnrelatedLinkedChild),
 CASE(StandardLinkedEntity),CASE(ExitHintRoutesUse),CASE(WeaponExitHint),CASE(ClientInfoAttached),CASE(ReentrantUnlink),
 CASE(BlockedExitRetainsSeat),CASE(BlockedForcedUnlink),CASE(ExitTracesFullHull),CASE(ExitTriesAnotherSide),
 CASE(LowCeilingExitNoTeleport),CASE(EndpointOverlapRejected),CASE(NoWorldGroundUsesRoof),CASE(StartSolidRejected),
 CASE(DifferentVehicleUseNoUnlink),CASE(EnemyCannotShare),CASE(TeamRestrictedVehicle),CASE(FriendlyCanShare),
 CASE(FreeForAllCanShare),CASE(SpectatorCannotEnter),CASE(DeadCannotEnter),CASE(MovingCannotEnter),CASE(FullOrMissingSeat),
 CASE(InvalidRiderNotDereferenced),CASE(NoVehicleCannotEnter),CASE(InputYawAndPitch),CASE(InputWorldDelta),
 CASE(InputWrap),CASE(NonDriverInputUnchanged),CASE(SteeringStraightAndTurns),CASE(SteeringWrapAndClamp),
 CASE(TraceParentFlagAndLegacyPath),CASE(TraceEndpointIncludesOtherOccupant),CASE(TraceSelfFlag),CASE(TraceChildrenFlag),
 CASE(TraceWorldHitPreservesNormal),CASE(TraceEntityHitBeforeWorld),CASE(TraceWorldStartSolid),CASE(TraceStationaryParentOverlap),
 CASE(InputPitchLimits),CASE(InputPitchInvertedAndEqualLimits),CASE(CameraFocusWithinRange),CASE(CameraZeroOrNegativeMaximum),
 CASE(FirstThinkFourWheelInitialization),CASE(FirstThinkWheelHeightTags),CASE(FirstThinkBacksUpPosition),
 CASE(FirstThinkHelicopterPreservesBounds),CASE(FirstThinkUnsupportedTankStillRejected)
#undef CASE
 };
 if(argc==1){for(auto&c:cases)std::puts(c.first.c_str());return 0;}
 try { reset();cases.at(argv[1])();for(auto*t:tags) { delete t; } tags.clear();std::printf("PASS %s\n",argv[1]);return 0; }
 catch(const std::exception&e){std::fprintf(stderr,"FAIL %s: %s\n",argv[1],e.what());return 1;}
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[2])
    args=parser.parse_args()
    groups={
        'src/server/sv_world.cpp':['SV_SetupIgnoreEntParams','SV_ClipMoveToEntity','SV_Trace','SV_TraceCapsuleWithIgnore'],
        'src/game_mp/g_vehicles_mp.cpp': ['G_VehPlayerRideSlot','G_VehFindExitPosition','G_VehUnlinkPlayer','VehicleClearRideSlotForPlayer','G_VehiclesSetupSpawnedEnts','InitEntityVars','G_VehUsable','VehicleHasSeatFree','G_VehEntHandler_Use','LinkPlayerToVehicle','VEH_PlayerRotation','InitFirstThink'],
        'src/game_mp/g_utils_mp.cpp':['G_EntUnlink'],
        'src/game_mp/g_active_mp.cpp':['G_UpdateClientInfo'],
        'src/game_mp/player_use_mp.cpp':['Player_SetVehicleDropHint','Player_ActivateCmd'],
        'src/cgame_mp/cg_vehicles_mp.cpp':['CG_VehSphereCoordsToPos'],
        'src/cgame_mp/cg_view_mp.cpp':['CalcViewValuesVehicleDriver'],
        'src/client_mp/cl_input.cpp':['CL_FinishMove'],
    }
    bodies=[]
    for path,names in groups.items():
        source=(args.source_root/path).read_text()
        for name in names:
            if name == 'SV_TraceCapsuleWithIgnore' and not re.search(r'^void __cdecl SV_TraceCapsuleWithIgnore\(',source,re.M):
                bodies.append('void SV_TraceCapsuleWithIgnore(trace_t*r,const float*s,const float*mi,const float*ma,const float*e,const IgnoreEntParams*i,int c) { SV_Trace(r,s,mi,ma,e,i,c,0,nullptr,0); }')
            elif name == 'G_VehFindExitPosition' and not re.search(r'^static bool G_VehFindExitPosition\(', source, re.M):
                # Allow the original source to compile for a negative control.
                # It has no safe-exit query; the original unlink/use bodies are
                # still extracted, and the exit assertions must fail.
                bodies.append('static bool G_VehFindExitPosition(gentity_s*, gentity_s*, float*) { return false; }')
            else:
                bodies.append(extract(source,name))
    with tempfile.TemporaryDirectory(prefix='kisak-vehicle-') as directory:
        root=Path(directory); source=root/'vehicle.cpp';binary=root/'vehicle'
        source.write_text(PREAMBLE+'\n'.join(bodies)+TESTS)
        command=shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-O1','-g','-Wall','-Wextra','-Wno-unused-variable','-Wno-unused-parameter','-Wno-parentheses','-fsanitize=undefined','-fno-sanitize-recover=all',str(source),'-o',str(binary)]
        subprocess.run(command,check=True)
        names=subprocess.check_output([str(binary)],text=True).splitlines()
        failures=[]
        for name in names:
            result=subprocess.run([str(binary),name],check=False)
            if result.returncode:failures.append(name)
        if failures: raise SystemExit(f'{len(failures)} of {len(names)} MP vehicle cases failed: '+', '.join(failures))
        print(f'{len(names)} MP vehicle lifecycle/input cases passed')

if __name__=='__main__':main()
