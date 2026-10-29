#!/usr/bin/env python3
"""Check MP vehicle metadata audio and optional entity allocation.

Extracts the production allocator, vehicle sound ownership/update routines,
map cleanup and remote-client sound-blend playback. Authored entity handles,
configstrings and a recording sound backend replace the engine/device boundary.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(source, name):
    start=re.search(r'^(?:static )?(?:gentity_s\s*\*|void|bool)\s*(?:__cdecl\s+)?'+re.escape(name)+r'\([^;]*?\)\s*\{',source,re.M)
    if not start:raise ValueError(f'Missing production function {name}')
    depth=1
    for token in re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27|[{}]',source[start.end():],re.S):
        if token.group()=='{':depth+=1
        elif token.group()=='}':
            depth-=1
            if not depth:return source[start.start():start.end()+token.end()].replace('__cdecl','')
    raise ValueError(f'Unterminated production function {name}')


PREAMBLE=r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef NDEBUG
#error Regression checks require assertions
#endif
#define __int16 short
#define iassert(x) assert(x)
constexpr int MAX_GENTITIES=1024,ENTITYNUM_WORLD=1022,ENTITYNUM_NONE=1023;
constexpr int ERR_DROP=1,ET_SOUND_BLEND=7,ET_VEHICLE=14,TR_STATIONARY=0,TR_INTERPOLATE=1,VEHICLE_RIDESLOT_DRIVER=0;
constexpr float EQUAL_EPSILON=0.001f;
constexpr int VEH_LOW_IDLE_SND=0,VEH_HIGH_IDLE_SND=1,VEH_LOW_ENGINE_SND=2,VEH_HIGH_ENGINE_SND=3,NUM_VEHICLE_SNDS=6;
constexpr int VEH_MOVESTATE_STOP=0,VEH_MANUAL_OFF=0,VEH_TURRET_STOPPED=0,CS_SOUNDALIASES=1000,SASYS_CGAME=1;
struct gentity_s;
struct EntHandle;
std::set<EntHandle*> handleRefs;
struct EntHandle {
 gentity_s *value=nullptr;
 bool isDefined()const{return value!=nullptr;}
 gentity_s*ent()const{assert(value);return value;}
 void setEnt(gentity_s*e){handleRefs.erase(this);value=e;if(e)handleRefs.insert(this);}
};
struct LerpEntityState {
 int eFlags{};struct{int trType{};float trBase[3]{};}pos,apos;
 union U{struct{float lerp;}soundBlend;U(){soundBlend={0};}}u;
};
struct entityState_s {int number{},eType{},eventParms[4]{},loopSound{},time2{};LerpEntityState lerp;};
struct playerState_s{int placeholder;};
struct gclient_s{playerState_s ps;};
struct VehicleRideSlot_t{int entNum=ENTITYNUM_NONE;};
struct VehicleTags{VehicleRideSlot_t riderSlots[3];};
struct scr_vehicle_s{
 struct{float origin[3]{},angles[3]{};}phys;
 int infoIdx{},entNum=ENTITYNUM_NONE,flags{},playEngineSound{},turretHitNum{},moveState{},waitNode{},drawOnCompass{},manualMode{};
 float speed{},engineSndLerp{},idleSndLerp{},waitSpeed{},manualSpeed{},manualAccel{},manualDecel{},manualTime{},turningAbility{};
 float joltDir[2]{},joltTime{},joltWave{},joltSpeed{},joltDecel{},jitter[15]{};
 struct{int fireTime{},fireBarrel{},turretState{};}turret;
 uint16_t lookAtText0{},lookAtText1{};
 EntHandle idleSndEnt,engineSndEnt,lookAtEnt;
};
struct gentity_s {
 entityState_s s;struct {bool inuse=false;int contents{},svFlags{};float currentOrigin[3]{},currentAngles[3]{};EntHandle ownerNum;}r;
 scr_vehicle_s *scr_vehicle{};gentity_s*nextFree{};uint16_t classname{};int eventTime{},freeAfterEvent{},health=100,nextthink{},takedamage{},active{};
};
gentity_s g_entities[MAX_GENTITIES];gclient_s clients[2];scr_vehicle_s vehicles[8];
struct level_s{
 int time=1000,num_entities=100,frametime=50;gentity_s*firstFreeEnt{},*lastFreeEnt{},*gentities=g_entities;gclient_s*clients=::clients;
 EntHandle droppedWeaponCue[32];
}level;
struct vehicle_info_t{uint8_t sndIndices[NUM_VEHICLE_SNDS]{};float engineSndSpeed=100,suspensionTravel=1;};
vehicle_info_t s_vehicleInfos[8];
struct {uint16_t noclass=1,tag_origin=3;}scr_const;
int allocated,frees,links,locates,printed,dropErrors,tagLookups;
bool tagAvailable=false,failLookup=false,shouldPlay=true,verifyOwnerCleared=false;
std::vector<int>freedIds;
struct snd_alias_t{std::string name;};
std::map<int,std::string> configStrings;
std::map<std::string,snd_alias_t> aliases;
struct SndEntHandle {int number;explicit SndEntHandle(int n):number(n){}};
struct Played{std::string low,high;float lerp;int entity;float origin[3];};
std::vector<Played>played;
struct centity_s{entityState_s nextState;LerpEntityState currentState;struct{float origin[3]{};}pose;};
struct{float frameInterpolation=1;}cgame;
auto*CG_GetLocalClientGlobals(int){return &cgame;}
bool CG_ShouldPlaySoundOnLocalClient(){return shouldPlay;}
const char*CL_GetConfigString(int,int n){return configStrings[n].c_str();}
snd_alias_t*CL_PickSoundAlias(const char*n){if(failLookup)return nullptr;auto it=aliases.find(n);return it==aliases.end()?nullptr:&it->second;}
void SND_PlayBlendedSoundAliases(snd_alias_t*a,snd_alias_t*b,float f,double volume,SndEntHandle handle,const float*origin,int,int system){
 assert(volume==1&&system==SASYS_CGAME&&f>=0&&f<=1);Played p{a->name,b->name,f,handle.number,{}};std::copy(origin,origin+3,p.origin);played.push_back(p);
}
void MyAssertHandler(const char*,int,int,const char*,...){throw std::runtime_error("engine invariant");}
void Com_Error(int kind,const char*message,...){assert(kind==ERR_DROP&&std::string(message)=="G_Spawn: no free entities");++dropErrors;throw std::runtime_error("no free entities");}
void Scr_SetString(uint16_t*p,uint16_t n){*p=n;}
void Scr_SetStringFromCharString(uint16_t*p,const char*s){assert(std::string(s)=="sound_blend");*p=2;}
void G_PrintEntities(){++printed;}
void SV_LocateGameData(gentity_s*base,int count,int entitySize,playerState_s*ps,int clientSize){assert(base==g_entities&&count==level.num_entities&&entitySize==sizeof(gentity_s)&&ps==&clients[0].ps&&clientSize==sizeof(gclient_s));++locates;}
void SV_LinkEntity(gentity_s*e){assert(e->r.inuse);++links;}
void G_SetOrigin(gentity_s*e,const float*p){std::copy(p,p+3,e->r.currentOrigin);std::copy(p,p+3,e->s.lerp.pos.trBase);e->s.lerp.pos.trType=TR_STATIONARY;}
int G_DObjGetWorldTagPos(gentity_s*e,uint32_t tag,float*out){assert(tag==scr_const.tag_origin);++tagLookups;if(!tagAvailable)return 0;for(int i=0;i<3;++i)out[i]=e->r.currentOrigin[i]+10;return 1;}
void VEH_InitPhysics(gentity_s*){}
void Com_Memset(uint32_t*p,int value,int count){assert(count==60);std::memset(p,value,size_t(count));}
void VEH_SetPosition(gentity_s*e,const float*p,const float*){G_SetOrigin(e,p);}
void VEH_UpdateSounds(gentity_s*e){e->s.loopSound=0;}
struct{struct{bool enabled=false;}current;}debugServer;
const auto*vehDebugServer=&debugServer;
int s_phys;
void VEH_BackupPosition(gentity_s*){}
VehicleTags*RideTagFirst(gentity_s*){return nullptr;}
VehicleTags*RideTagNext(gentity_s*,VehicleRideSlot_t*){return nullptr;}
void G_EntUnlink(gentity_s*){}
void VEH_UpdateClients(gentity_s*){}
void UpdateSimulation(gentity_s*){}
void VEH_TouchEntities_0(gentity_s*,float){}
void VEH_DebugBox(const float*,float,float,float,float){}
void VEH_UpdateWeapon(gentity_s*){}
void UpdateTurret(gentity_s*){}
void VEH_UpdateBody(gentity_s*,float){}
void VEH_UpdateSteering(gentity_s*){}
void VEH_UpdateMaterialTime(gentity_s*,float){}
void InitFirstThink(gentity_s*){throw std::runtime_error("unexpected initialization branch");}

void G_VehFreeEntity(gentity_s*);
void G_FreeEntity(gentity_s*e){
 assert(e->r.inuse);++frees;freedIds.push_back(e->s.number);
 if(verifyOwnerCleared&&e->s.eType==ET_SOUND_BLEND){
  auto*vehicle=e->r.ownerNum.ent();
  assert(!vehicle->scr_vehicle->idleSndEnt.isDefined()||vehicle->scr_vehicle->idleSndEnt.ent()!=e);
  assert(!vehicle->scr_vehicle->engineSndEnt.isDefined()||vehicle->scr_vehicle->engineSndEnt.ent()!=e);
 }
 if(e->scr_vehicle)G_VehFreeEntity(e);
 auto refs=handleRefs;for(auto*h:refs)if(h->value==e)h->setEnt(nullptr);
 e->r.ownerNum.setEnt(nullptr);int number=e->s.number;*e=gentity_s{};e->s.number=number;e->eventTime=level.time;
 if(level.lastFreeEnt)level.lastFreeEnt->nextFree=e;else level.firstFreeEnt=e;
 level.lastFreeEnt=e;
}
bool G_MaySpawnEntity(gentity_s*);
gentity_s*G_TrySpawn();
'''

TESTS=r'''
gentity_s*vehicle(int i=0){return &g_entities[80+i];}
void reset(){
 auto refs=handleRefs;for(auto*h:refs)h->setEnt(nullptr);assert(handleRefs.empty());
 std::fill(std::begin(g_entities),std::end(g_entities),gentity_s{});
 std::fill(std::begin(vehicles),std::end(vehicles),scr_vehicle_s{});
 std::fill(std::begin(s_vehicleInfos),std::end(s_vehicleInfos),vehicle_info_t{});
 level=level_s{};for(int i=0;i<MAX_GENTITIES;++i)g_entities[i].s.number=i;
 auto*v=vehicle();v->r.inuse=true;v->s.eType=ET_VEHICLE;v->scr_vehicle=&vehicles[0];vehicles[0].entNum=80;vehicles[0].playEngineSound=1;
 for(int i=0;i<4;++i)s_vehicleInfos[0].sndIndices[i]=uint8_t(11+i);
 configStrings.clear();aliases.clear();played.clear();freedIds.clear();
 for(int i=0;i<4;++i){std::string name="authored_vehicle_"+std::to_string(i);configStrings[CS_SOUNDALIASES+11+i]=name;aliases[name]={name};}
 allocated=frees=links=locates=printed=dropErrors=tagLookups=0;tagAvailable=failLookup=verifyOwnerCleared=false;shouldPlay=true;cgame.frameInterpolation=1;
}
void renderBlend(gentity_s*s,float previous=-1,float interpolation=1){
 centity_s cent;cent.nextState=s->s;cent.currentState=s->s.lerp;
 if(previous>=0)cent.currentState.u.soundBlend.lerp=previous;
 std::copy(s->r.currentOrigin,s->r.currentOrigin+3,cent.pose.origin);cgame.frameInterpolation=interpolation;
 CG_SoundBlend(0,&cent);
}
void assertNoSounds(){assert(!vehicles[0].idleSndEnt.isDefined()&&!vehicles[0].engineSndEnt.isDefined());}
void testMetadataAliasesReachRemoteClient(){
 vehicle()->scr_vehicle->speed=25;G_VehUpdateEngineSounds(vehicle());assert(links==2&&locates==2);
 renderBlend(vehicles[0].idleSndEnt.ent());renderBlend(vehicles[0].engineSndEnt.ent());assert(played.size()==2);
 assert(played[0].low=="authored_vehicle_0"&&played[0].high=="authored_vehicle_1"&&played[0].lerp==0.75f);
 assert(played[1].low=="authored_vehicle_2"&&played[1].high=="authored_vehicle_3"&&played[1].lerp==0.25f);
 assert(played[0].entity!=80&&played[0].entity!=played[1].entity);
}
void testMetadataSpeedControlsBlend(){vehicles[0].speed=25;s_vehicleInfos[0].engineSndSpeed=50;G_VehUpdateEngineSounds(vehicle());assert(vehicles[0].engineSndLerp==0.5f);s_vehicleInfos[0].engineSndSpeed=100;G_VehUpdateEngineSounds(vehicle());assert(vehicles[0].engineSndLerp==0.25f);assert(locates==2);}
void testEmptyMetadataDoesNotInventDefaults(){for(auto&i:s_vehicleInfos[0].sndIndices)i=0;G_VehUpdateEngineSounds(vehicle());assertNoSounds();assert(locates==0&&links==0);}
void testIncompletePairsRemainSilent(){s_vehicleInfos[0].sndIndices[1]=0;s_vehicleInfos[0].sndIndices[2]=0;G_VehUpdateEngineSounds(vehicle());assertNoSounds();assert(locates==0);}
void testOnlyEnginePairAllocatesOne(){s_vehicleInfos[0].sndIndices[0]=s_vehicleInfos[0].sndIndices[1]=0;G_VehUpdateEngineSounds(vehicle());assert(!vehicles[0].idleSndEnt.isDefined()&&vehicles[0].engineSndEnt.isDefined());assert(locates==1);}
void testOnlyIdlePairAllocatesOne(){s_vehicleInfos[0].sndIndices[2]=s_vehicleInfos[0].sndIndices[3]=0;G_VehUpdateEngineSounds(vehicle());assert(vehicles[0].idleSndEnt.isDefined()&&!vehicles[0].engineSndEnt.isDefined());assert(locates==1);}
void testRepeatedUpdatesReuseEntities(){G_VehUpdateEngineSounds(vehicle());auto*idle=vehicles[0].idleSndEnt.ent();for(int i=0;i<1000;++i)G_VehUpdateEngineSounds(vehicle());assert(locates==2&&level.num_entities==102&&frees==0&&vehicles[0].idleSndEnt.ent()==idle);}
void testTagOriginFollowsVehicle(){tagAvailable=true;vehicle()->r.currentOrigin[0]=20;G_VehUpdateEngineSounds(vehicle());renderBlend(vehicles[0].engineSndEnt.ent());assert(played.back().origin[0]==30);vehicle()->r.currentOrigin[0]=70;G_VehUpdateEngineSounds(vehicle());renderBlend(vehicles[0].engineSndEnt.ent());assert(played.back().origin[0]==80);assert(vehicles[0].engineSndEnt.ent()->s.lerp.pos.trType==TR_INTERPOLATE);}
void testMissingTagFallsBackToVehicleOrigin(){vehicle()->r.currentOrigin[0]=42;G_VehUpdateEngineSounds(vehicle());renderBlend(vehicles[0].engineSndEnt.ent());assert(played.back().origin[0]==42);}
void testZeroNegativeAndNanSpeedSafe(){for(float speed:{0.f,-2.f,std::numeric_limits<float>::quiet_NaN()}){vehicles[0].speed=speed;G_VehUpdateEngineSounds(vehicle());assert(vehicles[0].engineSndLerp==0&&vehicles[0].idleSndLerp==1);}}
void testZeroNegativeAndNanThresholdSafe(){vehicles[0].speed=10;for(float threshold:{0.f,-2.f,std::numeric_limits<float>::quiet_NaN()}){s_vehicleInfos[0].engineSndSpeed=threshold;G_VehUpdateEngineSounds(vehicle());assert(vehicles[0].engineSndLerp==0);}}
void testOverspeedClamps(){vehicles[0].speed=10000;G_VehUpdateEngineSounds(vehicle());assert(vehicles[0].engineSndLerp==1&&vehicles[0].idleSndLerp==0);}
void testDisabledEngineFreesAndRestarts(){G_VehUpdateEngineSounds(vehicle());vehicles[0].playEngineSound=0;G_VehUpdateEngineSounds(vehicle());assertNoSounds();assert(frees==2);vehicles[0].playEngineSound=1;G_VehUpdateEngineSounds(vehicle());assert(vehicles[0].idleSndEnt.isDefined()&&vehicles[0].engineSndEnt.isDefined());}
void testDeadVehicleFreesAndStaysSilent(){G_VehUpdateEngineSounds(vehicle());vehicle()->health=0;G_VehUpdateEngineSounds(vehicle());assertNoSounds();int n=locates;G_VehUpdateEngineSounds(vehicle());assert(frees==2&&locates==n);}
void testVehicleRemovalOwnsSoundCleanup(){G_VehUpdateEngineSounds(vehicle());verifyOwnerCleared=true;G_VehFreeEntity(vehicle());assertNoSounds();assert(frees==2&&vehicle()->scr_vehicle==nullptr);}
void testRepeatedSoundCleanupIsSafe(){G_VehUpdateEngineSounds(vehicle());G_VehFreeEngineSounds(&vehicles[0]);G_VehFreeEngineSounds(&vehicles[0]);assertNoSounds();assert(frees==2);}
void testSoundEntityFreedBeforeVehicle(){G_VehUpdateEngineSounds(vehicle());G_FreeEntity(vehicles[0].idleSndEnt.ent());assert(!vehicles[0].idleSndEnt.isDefined());G_VehFreeEntity(vehicle());assert(frees==2&&vehicle()->scr_vehicle==nullptr);}
void testMapShutdownParentBeforeSounds(){G_VehUpdateEngineSounds(vehicle());G_FreeEntities();assert(level.num_entities==0&&level.firstFreeEnt==nullptr&&level.lastFreeEnt==nullptr);assertNoSounds();assert(frees==3&&handleRefs.empty());}
void testMapShutdownSoundsBeforeParent(){g_entities[20].eventTime=0;g_entities[21].eventTime=0;g_entities[20].nextFree=&g_entities[21];level.firstFreeEnt=&g_entities[20];level.lastFreeEnt=&g_entities[21];G_VehUpdateEngineSounds(vehicle());assert(vehicles[0].idleSndEnt.ent()->s.number==20);G_FreeEntities();assertNoSounds();assert(frees==3&&handleRefs.empty());}
void testFullAllocatorIsNonfatalForAudio(){level.num_entities=ENTITYNUM_WORLD;G_VehUpdateEngineSounds(vehicle());assertNoSounds();assert(frees==0&&printed==0&&dropErrors==0&&locates==0);}
void testSecondAllocationFailureRollsBack(){level.num_entities=ENTITYNUM_WORLD-1;G_VehUpdateEngineSounds(vehicle());assertNoSounds();assert(frees==1&&printed==0&&dropErrors==0);assert(!g_entities[ENTITYNUM_WORLD-1].r.inuse&&level.firstFreeEnt==&g_entities[ENTITYNUM_WORLD-1]);}
void testAllocationRecoversAfterCapacityReturns(){level.num_entities=ENTITYNUM_WORLD-1;G_VehUpdateEngineSounds(vehicle());g_entities[20].eventTime=0;g_entities[20].nextFree=level.firstFreeEnt;level.firstFreeEnt=&g_entities[20];G_VehUpdateEngineSounds(vehicle());assert(vehicles[0].idleSndEnt.isDefined()&&vehicles[0].engineSndEnt.isDefined());assert(dropErrors==0);}
void testExistingPairSurvivesMissingCapacity(){s_vehicleInfos[0].sndIndices[2]=s_vehicleInfos[0].sndIndices[3]=0;G_VehUpdateEngineSounds(vehicle());auto*idle=vehicles[0].idleSndEnt.ent();level.num_entities=ENTITYNUM_WORLD;s_vehicleInfos[0].sndIndices[2]=13;s_vehicleInfos[0].sndIndices[3]=14;vehicle()->r.currentOrigin[0]=90;G_VehUpdateEngineSounds(vehicle());assert(vehicles[0].idleSndEnt.ent()==idle&&!vehicles[0].engineSndEnt.isDefined());assert(idle->r.currentOrigin[0]==90&&frees==0);}
void testMetadataRemovingPairFreesIt(){G_VehUpdateEngineSounds(vehicle());s_vehicleInfos[0].sndIndices[0]=0;G_VehUpdateEngineSounds(vehicle());assert(!vehicles[0].idleSndEnt.isDefined()&&vehicles[0].engineSndEnt.isDefined()&&frees==1);}
void testMultipleVehiclesRemainIndependent(){auto*v=vehicle(1);v->r.inuse=true;v->s.eType=ET_VEHICLE;v->scr_vehicle=&vehicles[1];vehicles[1].entNum=81;vehicles[1].playEngineSound=1;vehicles[1].infoIdx=1;s_vehicleInfos[1]=s_vehicleInfos[0];s_vehicleInfos[1].sndIndices[2]=50;s_vehicleInfos[1].sndIndices[3]=51;G_VehUpdateEngineSounds(vehicle());G_VehUpdateEngineSounds(v);assert(vehicles[0].engineSndEnt.ent()!=vehicles[1].engineSndEnt.ent());assert(vehicles[1].engineSndEnt.ent()->s.eventParms[0]==50);G_VehFreeEntity(v);assert(vehicles[0].engineSndEnt.isDefined()&&frees==2);}
void testSpawnDefaultEnablesEngine(){vehicles[0].playEngineSound=0;InitEntityVehicleVars(vehicle(),&vehicles[0],0);assert(vehicles[0].playEngineSound==1&&vehicles[0].engineSndLerp==0&&vehicles[0].idleSndLerp==0);}
void testTrySpawnUsesExistingReusePolicy(){g_entities[20].eventTime=level.time-499;level.firstFreeEnt=level.lastFreeEnt=&g_entities[20];auto*e=G_TrySpawn();assert(e==&g_entities[100]&&level.firstFreeEnt==&g_entities[20]);level.time+=1;e=G_TrySpawn();assert(e==&g_entities[20]&&level.firstFreeEnt==nullptr&&level.lastFreeEnt==nullptr);}
void testTrySpawnReusesRecentEntityAtLimit(){g_entities[20].eventTime=level.time;level.firstFreeEnt=level.lastFreeEnt=&g_entities[20];level.num_entities=ENTITYNUM_WORLD;auto*e=G_TrySpawn();assert(e==&g_entities[20]&&e->r.inuse&&dropErrors==0);}
void testTrySpawnFailureDoesNotMutate(){level.num_entities=ENTITYNUM_WORLD;assert(G_TrySpawn()==nullptr);assert(level.num_entities==ENTITYNUM_WORLD&&level.firstFreeEnt==nullptr&&level.lastFreeEnt==nullptr&&printed==0&&dropErrors==0);}
void testRequiredSpawnStillDropsOnExhaustion(){level.num_entities=ENTITYNUM_WORLD;bool caught=false;try{G_Spawn();}catch(const std::runtime_error&){caught=true;}assert(caught&&printed==1&&dropErrors==1);}
void testRequiredSpawnStillSucceeds(){auto*e=G_Spawn();assert(e==&g_entities[100]&&e->r.inuse&&level.num_entities==101&&locates==1&&dropErrors==0);}
void testRemoteClientInterpolation(){vehicles[0].speed=75;G_VehUpdateEngineSounds(vehicle());renderBlend(vehicles[0].engineSndEnt.ent(),0.25f,0.5f);assert(played.size()==1&&played[0].lerp==0.5f);}
void testRemoteMissingAliasOrDisabledSound(){G_VehUpdateEngineSounds(vehicle());failLookup=true;renderBlend(vehicles[0].engineSndEnt.ent());assert(played.empty());failLookup=false;shouldPlay=false;renderBlend(vehicles[0].engineSndEnt.ent());assert(played.empty());}
void testVehicleThinkPublishesMetadataSounds(){vehicles[0].flags=8;G_VehEntHandler_Think(vehicle());assert(vehicles[0].idleSndEnt.isDefined()&&vehicles[0].engineSndEnt.isDefined());assert(vehicle()->nextthink==level.time+50);renderBlend(vehicles[0].engineSndEnt.ent());assert(played.size()==1&&played[0].low=="authored_vehicle_2");}
void testUnsignedAliasIndicesPreserved(){const uint8_t indices[4]={128,200,254,255};for(int i=0;i<4;++i){s_vehicleInfos[0].sndIndices[i]=indices[i];std::string name="high_alias_"+std::to_string(i);configStrings[CS_SOUNDALIASES+indices[i]]=name;aliases[name]={name};}G_VehUpdateEngineSounds(vehicle());renderBlend(vehicles[0].idleSndEnt.ent());renderBlend(vehicles[0].engineSndEnt.ent());assert(played.size()==2&&played[0].low=="high_alias_0"&&played[1].high=="high_alias_3");}
int main(int argc,char**argv){
 const std::map<std::string,std::function<void()>>cases={
#define CASE(n) {#n,test##n}
 CASE(MetadataAliasesReachRemoteClient),CASE(MetadataSpeedControlsBlend),CASE(EmptyMetadataDoesNotInventDefaults),CASE(IncompletePairsRemainSilent),
 CASE(OnlyEnginePairAllocatesOne),CASE(OnlyIdlePairAllocatesOne),CASE(RepeatedUpdatesReuseEntities),CASE(TagOriginFollowsVehicle),CASE(MissingTagFallsBackToVehicleOrigin),
 CASE(ZeroNegativeAndNanSpeedSafe),CASE(ZeroNegativeAndNanThresholdSafe),CASE(OverspeedClamps),CASE(DisabledEngineFreesAndRestarts),CASE(DeadVehicleFreesAndStaysSilent),
 CASE(VehicleRemovalOwnsSoundCleanup),CASE(RepeatedSoundCleanupIsSafe),CASE(SoundEntityFreedBeforeVehicle),CASE(MapShutdownParentBeforeSounds),CASE(MapShutdownSoundsBeforeParent),
 CASE(FullAllocatorIsNonfatalForAudio),CASE(SecondAllocationFailureRollsBack),CASE(AllocationRecoversAfterCapacityReturns),CASE(ExistingPairSurvivesMissingCapacity),
 CASE(MetadataRemovingPairFreesIt),CASE(MultipleVehiclesRemainIndependent),CASE(SpawnDefaultEnablesEngine),CASE(TrySpawnUsesExistingReusePolicy),CASE(TrySpawnReusesRecentEntityAtLimit),
 CASE(TrySpawnFailureDoesNotMutate),CASE(RequiredSpawnStillDropsOnExhaustion),CASE(RequiredSpawnStillSucceeds),CASE(RemoteClientInterpolation),CASE(RemoteMissingAliasOrDisabledSound),CASE(VehicleThinkPublishesMetadataSounds),CASE(UnsignedAliasIndicesPreserved)
#undef CASE
 };
 if(argc==1){for(auto&c:cases)std::puts(c.first.c_str());return 0;}
 try{reset();cases.at(argv[1])();std::printf("PASS %s\n",argv[1]);return 0;}
 catch(const std::exception&e){std::fprintf(stderr,"FAIL %s: %s\n",argv[1],e.what());return 1;}
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[2])
    args=parser.parse_args()
    groups={
        'src/game_mp/g_utils_mp.cpp':['G_InitGentity','G_TrySpawn','G_Spawn','G_MaySpawnEntity'],
        'src/game_mp/g_vehicles_mp.cpp':['G_VehFreeSoundBlend','G_VehFreeEngineSounds','G_VehInitSoundBlend','G_VehEnsureEngineSounds','G_VehUpdateSoundBlend','G_VehUpdateEngineSounds','G_VehFreeEntity','InitEntityVehicleVars','G_VehEntHandler_Think'],
        'src/game_mp/g_main_mp.cpp':['G_FreeEntities'],
        'src/cgame_mp/cg_ents_mp.cpp':['CG_SoundBlend'],
    }
    bodies=[]
    for path,names in groups.items():
        source=(args.source_root/path).read_text()
        for name in names:bodies.append(extract(source,name))
    client=(args.source_root/'src/cgame_mp/cg_vehicles_mp.cpp').read_text()
    if re.search(r'CL_PickSoundAlias\("hummer_(?:engine|idle)_',client):
        raise SystemExit('Hardcoded client engine/idle playback would duplicate metadata sound entities')
    with tempfile.TemporaryDirectory(prefix='kisak-vehicle-audio-')as directory:
        root=Path(directory);source=root/'audio.cpp';binary=root/'audio'
        source.write_text(PREAMBLE+'\n'.join(bodies)+TESTS)
        command=shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-variable','-fsanitize=undefined','-fno-sanitize-recover=all',str(source),'-o',str(binary)]
        subprocess.run(command,check=True)
        names=subprocess.check_output([str(binary)],text=True).splitlines();failures=[]
        for name in names:
            result=subprocess.run([str(binary),name],check=False)
            if result.returncode:failures.append(name)
        if failures:raise SystemExit(f'{len(failures)} of {len(names)} MP vehicle audio cases failed: '+', '.join(failures))
        print(f'{len(names)} MP vehicle metadata audio/allocation cases passed')

if __name__=='__main__':main()
