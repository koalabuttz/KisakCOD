// Integration at the transport boundary: production cgame setters -> command
// finishing -> packet writer -> netchan encoding -> server decoding/readers.
// No authentication, socket, renderer, prediction simulation or live server.
#include <universal/q_shared.h>
#include <universal/assertive.h>
#include <universal/com_memory.h>
#include <client_mp/client_mp.h>
#include <server_mp/server_mp.h>
#include <qcommon/msg_mp.h>
#include <qcommon/huffman.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <stdexcept>

huffman_t msgHuff;
uint32_t Sys_Milliseconds() { return 0; } // Huffman initialization profiling only.
void _copyDWord(uint32_t *dest, uint32_t value, uint32_t count) { std::fill_n(dest,count,value); }
clientActive_t clients[STATIC_MAX_LOCAL_CLIENTS];
clientUIActive_t clientUIActives[STATIC_MAX_LOCAL_CLIENTS];
clientStatic_t cls;
static clientConnection_t connection;
clientConnection_t *CL_GetLocalClientConnection(int n) { iassert(n == 0); return &connection; }
static dvar_t zeroDvar{};
const dvar_t *cl_packetdup=&zeroDvar, *cl_showSend=&zeroDvar, *cl_nodelta=&zeroDvar;
const dvar_t *cl_debugMessageKey=&zeroDvar, *com_statmon=&zeroDvar;
const dvar_t *vehDriverViewHeightMin=&zeroDvar, *vehDriverViewHeightMax=&zeroDvar;
bool CG_VehLocalClientDriving(int) { return false; }
void Com_Printf(int, const char *, ...) {}
void Com_PrintError(int, const char *, ...) {}
void Com_DPrintf(int, const char *, ...) {}
void StatMon_Warning(int, int, const char *) {}
void Com_Error(errorParm_t, const char *fmt, ...) { throw std::runtime_error(fmt); }
void MyAssertHandler(const char *file, int line, int, const char *fmt, ...)
{
    std::fprintf(stderr,"assert %s:%d: %s\n",file,line,fmt); std::abort();
}
// Checked temporary allocation double. Only CL_WritePacket's 2048-byte arena
// is supported; nested/unbalanced allocations fail instead of hiding behavior.
alignas(16) static uint8_t temporary[2048];
static bool allocated;
LargeLocal::LargeLocal(int requested) : startPos(0), size(requested)
{ iassert(!allocated && requested == sizeof(temporary)); allocated=true; }
LargeLocal::~LargeLocal() { iassert(allocated); allocated=false; }
uint8_t *LargeLocal::GetBuf() { iassert(allocated); return temporary; }
static std::vector<uint8_t> wire;
static int transmissions;
bool Netchan_Transmit(netchan_t *chan, int length, char *data)
{
    iassert(chan == &connection.netchan && length >= 9 && length <= 2048);
    wire.assign(data,data+length); ++transmissions; ++chan->outgoingSequence; return true;
}
bool Netchan_TransmitNextFragment(netchan_t *) { std::abort(); }
const char *NET_AdrToString(netadr_t) { return "test-transport"; }
static int checks, failures;
static void check(bool condition,const char *name)
{ ++checks; std::printf("%s: %s\n",condition?"PASS":"FAIL",name); if(!condition)++failures; }
static void reset()
{
    std::memset(clients,0,sizeof(clients)); std::memset(&connection,0,sizeof(connection));
    std::memset(&cls,0,sizeof(cls)); wire.clear();transmissions=0;
    clientUIActives[0].connectionState=CA_ACTIVE;
    connection.netchan.outgoingSequence=1; connection.challenge=0x46;
    connection.serverMessageSequence=32; connection.serverCommandSequence=128;
    connection.checksumFeed=0x123456;
    std::strcpy(connection.serverCommands[0],"command at ring zero");
    clients[0].serverId=7;clients[0].snap.valid=true;clients[0].snap.messageNum=32;
}
static usercmd_s make_command(int time)
{
    auto &cl=clients[0];cl.serverTime=time;cl.snap.serverTime=time-100;
    float origin[3]={float(time),2,3},velocity[3]={4,5,6},angles[3]={0,90,0},kick[3]={0,0,0};
    CL_SetUserCmdOrigin(0,origin,velocity,angles,12,3);
    CL_SetUserCmdWeapons(0,7,3);CL_SetUserCmdAimValues(0,kick);
    std::memcpy(cl.viewangles,angles,sizeof(angles));
    usercmd_s cmd{};cmd.forwardmove=127;cmd.rightmove=-127;
    CL_SetExtraButtons(0,BUTTON_ATTACK);CL_FinishMove(0,&cmd);
    cl.cmds[++cl.cmdNumber & 127]=cmd;
    return cmd;
}
struct Decoded { int serverId, messageAck, commandAck, opcode, count;usercmd_s cmd{};float origin[3];int time;std::vector<int> commandTimes;bool clean; };
static Decoded decode()
{
    Decoded result{};auto data=wire;msg_t header{};MSG_InitReadOnly(&header,data.data(),data.size());
    result.serverId=MSG_ReadByte(&header);result.messageAck=MSG_ReadLong(&header);result.commandAck=MSG_ReadLong(&header);
    auto server=std::make_unique<client_t>();
    server->challenge=connection.challenge;server->serverId=result.serverId;server->messageAcknowledge=result.messageAck;
    server->reliableAcknowledge=server->reliableSequence=result.commandAck;
    std::strcpy(server->reliableCommandInfo[result.commandAck&127].cmd,"command at ring zero");
    SV_Netchan_Decode(server.get(),data.data()+9,data.size()-9);
    uint8_t plain[2048];int length=MSG_ReadBitsCompress(data.data()+9,plain,data.size()-9,sizeof(plain));
    iassert(length>=0);msg_t msg{};MSG_InitReadOnly(&msg,plain,length);
    result.opcode=MSG_ReadBits(&msg,3);result.count=MSG_ReadByte(&msg);
    usercmd_s previous{};MSG_SetDefaultUserCmd(&clients[0].snap.ps,&previous);
    int key=connection.checksumFeed ^ result.messageAck ^ Com_HashKey("command at ring zero",32);
    for(int i=0;i<result.count;++i){MSG_ReadDeltaUsercmdKey(&msg,key,&previous,&result.cmd);previous=result.cmd;result.commandTimes.push_back(result.cmd.serverTime);}
    for(int i=0;i<3;++i){int bits=MSG_ReadLong(&msg);std::memcpy(&result.origin[i],&bits,4);}
    result.time=MSG_ReadLong(&msg);result.clean=MSG_ReadBits(&msg,3)==3 && !msg.overflowed;
    return result;
}
static bool lookup(int time,float expected)
{
    float o[3]={-1,-1,-1},v[3],a[3];int bob,dir;
    return CL_GetPredictedOriginForServerTime(&clients[0],time,o,v,a,&bob,&dir)
        && o[0]==expected && o[1]==2 && v[2]==6 && a[1]==90 && bob==12 && dir==3;
}
int main()
{
    reset();auto cmd=make_command(1000);cls.realtime=2000;
    CL_WritePacket(0);auto decoded=decode();
    check(decoded.clean && decoded.serverId==7 && decoded.messageAck==32 && decoded.commandAck==128,"production packet header/codec/compression boundary");
    check(decoded.opcode==0 && decoded.count==1 && decoded.cmd.serverTime==cmd.serverTime && decoded.cmd.buttons==BUTTON_ATTACK && decoded.cmd.angles[1]==16384 && decoded.cmd.weapon==7 && decoded.cmd.offHandIndex==3 && decoded.cmd.forwardmove==127 && decoded.cmd.rightmove==-127,"finished user command survives production packet and server readers");
    check(decoded.origin[0]==1000 && decoded.origin[1]==2 && decoded.time==1000 && lookup(1000,1000),"published cgame prediction travels into packet and archive");
    check(clients[0].cgameExtraButtons==0 && clients[0].outPackets[1].p_cmdNumber==1 && clients[0].outPackets[1].p_serverTime==1000 && clients[0].outPackets[1].p_realtime==2000 && connection.lastPacketSentTime==2000 && !allocated,"packet bookkeeping and temporary ownership are balanced");
    usercmd_s recalled{};check(CL_GetCurrentCmdNumber(0)==1 && CL_GetUserCmd(0,1,&recalled) && recalled.serverTime==1000,"prediction consumer retrieves finished command from history");
    reset();bool all=true;
    for(int frame=0;frame<300;++frame){int time=1000+frame*20;make_command(time);cls.realtime=time;CL_WritePacket(0);auto d=decode();all=all&&d.clean&&d.count==1&&d.cmd.serverTime==time&&d.time==time;}
    check(all && transmissions==300 && clients[0].clientArchiveIndex==44,"300 successive packets cross command, packet and prediction ring wraps");
    check(lookup(6980,6980) && lookup(6979,6960),"archive lookup returns exact and most recent preceding prediction");
    float o[3]={-1,-1,-1},v[3]={-1,-1,-1},a[3]={-1,-1,-1};int bob=-1,dir=-1;
    check(!CL_GetPredictedOriginForServerTime(&clients[0],1000,o,v,a,&bob,&dir) && o[0]==-1 && bob==-1,"overwritten prediction history is rejected without overwriting outputs");
    check(!CL_GetUserCmd(0,172,&recalled) && CL_GetUserCmd(0,173,&recalled) && recalled.serverTime==4440,"user-command history retention uses the exact 128-command boundary");
    bool rejected=false;try { CL_GetUserCmd(0,301,&recalled); }catch(const std::runtime_error&){rejected=true;}
    check(rejected,"future user command follows production error path");
    int archive=clients[0].clientArchiveIndex;CL_WritePacket(0);
    check(clients[0].clientArchiveIndex==archive,"empty command batch does not create duplicate prediction archive entries");
    connection.demoplaying=true;int before=transmissions;CL_WritePacket(0);
    check(transmissions==before && !allocated,"demo playback suppresses packet emission and releases temporary storage");
    reset();make_command(1000);clients[0].snap.valid=false;CL_WritePacket(0);decoded=decode();
    check(decoded.clean && decoded.opcode==1 && decoded.count==1,"invalid snapshot selects non-delta movement packet");
    reset();for(int i=0;i<40;++i)make_command(1000+i*20);CL_WritePacket(0);decoded=decode();
    std::vector<int> expectedTimes;for(int i=8;i<40;++i)expectedTimes.push_back(1000+i*20);
    check(decoded.commandTimes==expectedTimes && decoded.clean && decoded.count==32 && decoded.cmd.serverTime==1780 && decoded.commandTimes.front()==1160 && lookup(1780,1780),"backlogged command emission caps batch at 32 and archives newest prediction");
    archive=clients[0].clientArchiveIndex;make_command(1780);CL_WritePacket(0);decoded=decode();
    check(decoded.clean && decoded.count==1 && clients[0].clientArchiveIndex==archive,"repeated server time emits command without duplicating prediction archive");
    std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}
