#include <qcommon/cmd_services.h>
// Production packet readers/tokenizer/resend/receive/timeout code. Host services
// are doubles; successful tickets are an isolated fixture, never an executable mode.
#include <client_mp/client_mp.h>
#include <client_mp/cl_auth.h>
#include <client_mp/cl_connection_mp.h>
#include <qcommon/cmd.h>
#include <qcommon/msg_mp.h>
#include <qcommon/huffman.h>
#include <universal/assertive.h>
#include <universal/com_files.h>
#include <win32/win_storage.h>
#include <stringed/stringed_hooks.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

clientUIActive_t clientUIActives[STATIC_MAX_LOCAL_CLIENTS];
clientStatic_t cls;
clientActive_t clients[STATIC_MAX_LOCAL_CLIENTS];
static clientConnection_t connection;
clientConnection_t *CL_GetLocalClientConnection(int n) { iassert(n == 0); return &connection; }
extern CmdArgsPrivate cmd_argsPrivate;
static dvar_t zeroDvar{}, stringDvar{}, connectionTimeout{}, activeTimeout{}, attempts{};
const dvar_t *net_lanauthorize=&zeroDvar, *onlinegame=&zeroDvar, *showpackets=&zeroDvar;
const dvar_t *cl_updateavailable=&zeroDvar, *fs_gameDirVar=&stringDvar;
const dvar_t *cl_paused=&zeroDvar, *sv_paused=&zeroDvar;
const dvar_t *cl_timeout=&activeTimeout, *cl_connectTimeout=&connectionTimeout, *cl_connectionAttempts=&attempts;
int32_t dvar_modifiedFlags, autoupdateChecked, autoupdateStarted;
int g_qport=42;
huffman_t msgHuff;
static bool fixtureAuthReady;
static bool transportSucceeds=true;
static std::vector<std::string> sent;
static std::vector<std::vector<uint8_t>> sentData;
static int masterResponseBytes;
ClientAuthResult CL_GetPlatformChallenge(char *out, size_t cap) {
    if (!fixtureAuthReady) { if (cap) out[0]=0; return ClientAuthResult::unavailable; }
    std::snprintf(out,cap,"getchallenge 0 \"SYNTHETIC-TEST-ONLY\" \"1\"");
    return ClientAuthResult::ready;
}
const char *CL_AuthResultName(ClientAuthResult r) { return r==ClientAuthResult::ready?"ready":"unavailable"; }
void MyAssertHandler(const char *file,int line,int,const char *fmt,...) {
    std::fprintf(stderr,"assert %s:%d %s\n",file,line,fmt);std::abort();
}
void Com_Error(errorParm_t, const char *fmt,...) { throw std::runtime_error(fmt); }
void Com_Printf(int,const char *,...) {}
void Com_DPrintf(int,const char *,...) {}
void Com_PrintError(int,const char *,...) {}
void Com_PrintMessage(int,const char *,int) {}
uint32_t Sys_Milliseconds() { return cls.realtime; }
void *Sys_GetValue(int key) { static va_info_t info{}; iassert(key==1); return &info; }
bool Sys_IsMainThread() { return true; }
void Sys_EnterCriticalSection(int) { std::abort(); }
void Sys_LeaveCriticalSection(int) { std::abort(); }
void Scr_MonitorCommand(const char *) { std::abort(); }
void _copyDWord(uint32_t *p,uint32_t v,uint32_t n) { std::fill_n(p,n,v); }
bool Sys_IsLANAddress(netadr_t) { return true; }
void CL_RequestAuthorization(int) { std::abort(); }
bool NET_OutOfBandPrint(netsrc_t,netadr_t,const char *text) { sent.emplace_back(text);return transportSucceeds; }
bool NET_OutOfBandData(netsrc_t,netadr_t,const uint8_t *data,int size) { sentData.emplace_back(data,data+size);return transportSucceeds; }
bool NET_CompareBaseAdr(netadr_t a,netadr_t b) { return a.type==b.type && !std::memcmp(a.ip,b.ip,4); }
bool NET_CompareAdr(netadr_t a,netadr_t b) { return NET_CompareBaseAdr(a,b) && a.port==b.port; }
const char *NET_AdrToString(netadr_t) { return "test-peer"; }
void Netchan_Setup(netsrc_t,netchan_t *n,netadr_t from,int,char *,int,char *,int) { n->remoteAddress=from; }
void CL_Netchan_AddOOBProfilePacket(int,int) {}
void CL_VoicePacket(int,msg_t *) { std::abort(); }
void CL_ServerInfoPacket(netadr_t,msg_t *,int) { std::abort(); }
void CL_ServerStatusResponse(netadr_t,msg_t *) { std::abort(); }
void CL_UpdateInfoPacket(netadr_t) { std::abort(); }
void CL_ServersResponsePacket(netadr_t,msg_t *msg) { masterResponseBytes=msg->cursize; }
void CL_SetupForNewServerMap(char *,char *) { std::abort(); }
void UI_CloseAllMenus(int) { std::abort(); }
void CL_Disconnect(int) { clientUIActives[0].connectionState=CA_DISCONNECTED; }
void CL_DisconnectError(char *) { std::abort(); }
char *SEH_LocalizeTextMessage(const char *,const char *,msgLocErrType_t) { std::abort(); }
void LiveStorage_ReadStatsFromDir(char *) { std::abort(); }
bool LiveStorage_DoWeHaveStats() { return false; }
playerStatNetworkData *LiveStorage_GetStatBuffer() { std::abort(); }
char *Dvar_InfoString(int,char) { static char info[]="\\name\\fixture";return info; }

static int checks,failures;
static void check(bool value,const char *name) {
    ++checks;if(!value)++failures;std::printf("%s: %s\n",value?"PASS":"FAIL",name);
}
static netadr_t server() { netadr_t a{};a.type=NA_IP;a.ip[0]=127;a.ip[3]=1;a.port=1234;return a; }
static void reset() {
    std::memset(&connection,0,sizeof(connection));std::memset(&cls,0,sizeof(cls));
    std::memset(clients,0,sizeof(clients));std::memset(clientUIActives,0,sizeof(clientUIActives));
    std::memset(&cmd_args,0,sizeof(cmd_args));cmd_args.nesting=-1;
    std::memset(&cmd_argsPrivate,0,sizeof(cmd_argsPrivate));
    stringDvar.current.string="";connectionTimeout.current.value=1;activeTimeout.current.value=2;attempts.current.integer=2;
    clientUIActives[0].connectionState=CA_CONNECTING;connection.serverAddress=server();connection.qport=5678;dvar_modifiedFlags=2;
    cls.realtime=3000;fixtureAuthReady=false;sent.clear();sentData.clear();transportSucceeds=true;CL_ResetConnectionHandshake(0);
}
static bool receive(const std::string &text,netadr_t from=server(),bool terminated=true) {
    std::vector<uint8_t> data(4,255);data.insert(data.end(),text.begin(),text.end());if(terminated)data.push_back('\n');
    msg_t msg{};MSG_InitReadOnly(&msg,data.data(),data.size());
    return CL_ConnectionlessPacket(0,from,&msg,cls.realtime);
}
int main() {
    reset();CL_CheckForResend(0);
    check(sent.empty() && connection.connectPacketCount==1,"unavailable fixture sends no challenge");
    auto before=connection.serverAddress;
    bool accepted=receive("challengeResponse 123");
    check(!accepted && clientUIActives[0].connectionState==CA_CONNECTING && NET_CompareAdr(before,connection.serverAddress),
        "unsolicited response cannot advance unavailable authentication");
    reset();auto foreign=server();foreign.ip[3]=2;
    receive("challengeResponse 42",foreign);
    check(clientUIActives[0].connectionState==CA_CONNECTING && NET_CompareAdr(connection.serverAddress,server()),
        "foreign challenge cannot replace selected server");
    reset();clientUIActives[0].connectionState=CA_SENDINGSTATS;connection.statPacketsToSend=127;
    receive("statResponse 0",foreign);
    check(clientUIActives[0].connectionState==CA_SENDINGSTATS && connection.statPacketsToSend==127,
        "foreign stat completion cannot connect client");
    reset();fixtureAuthReady=true;CL_CheckForResend(0);
    check(sent.size()==1 && CL_ChallengeResponseAllowed(0,server()), "test-only ready provider creates one pending request");
    foreign=server();++foreign.port;
    check(!receive("challengeResponse 42",foreign) && CL_ChallengeResponseAllowed(0,server()),
        "same IP with different source port cannot consume pending challenge");
    bool malformed=true;
    for(const char *text : {"challengeResponse", "challengeResponse nope", "challengeResponse 12x", "challengeResponse 2147483648"})
        malformed=malformed && !receive(text) && clientUIActives[0].connectionState==CA_CONNECTING;
    check(malformed && CL_ChallengeResponseAllowed(0,server()), "missing malformed and overflowing challenge arguments preserve state");
    check(receive("challengeResponse -123",server(),false) && clientUIActives[0].connectionState==CA_CHALLENGING
        && connection.challenge==-123 && connection.connectPacketCount==0 && connection.connectTime==-99999,
        "valid delimiter-free server packet advances production handshake and retry fields");
    CL_CheckForResend(0);
    check(sentData.size()==1 && std::string(sentData[0].begin(),sentData[0].end())
        == "connect \"\\name\\fixture\\protocol\\1\\challenge\\-123\\qport\\5678\""
        && !(dvar_modifiedFlags&2), "production resend emits unchanged connect userinfo and clears modification flag");
    check(!receive("challengeResponse 99") && connection.challenge==-123, "duplicate challenge cannot replace accepted challenge");
    check(!receive("connectResponse",foreign) && clientUIActives[0].connectionState==CA_CHALLENGING,
        "connect response must use selected endpoint including port");
    check(receive("connectResponse",server(),false) && clientUIActives[0].connectionState==CA_SENDINGSTATS
        && connection.statPacketsToSend==127 && NET_CompareAdr(connection.netchan.remoteAddress,server())
        && connection.lastPacketTime==cls.realtime && connection.lastPacketSentTime==-9999,
        "valid connect response configures stats synchronization and transport endpoint");
    CL_CheckForResend(0);
    msg_t stats{};MSG_InitReadOnly(&stats,sentData.back().data(),sentData.back().size());
    bool statsWire=std::strcmp(MSG_ReadString(&stats),"stats")==0 && MSG_ReadShort(&stats)==5678
        && MSG_ReadByte(&stats)==0 && stats.cursize-stats.readcount==1240;
    for(int i=stats.readcount;i<stats.cursize;++i)statsWire=statsWire && stats.data[i]==0;
    check(statsWire && connection.statPacketSendTime[0]==cls.realtime,
        "production stats resend emits header qport packet index and offline payload");
    size_t packets=sentData.size();cls.realtime+=99;CL_CheckForResend(0);
    check(sentData.size()==packets, "stats retry respects 100ms transport gate");
    ++cls.realtime;CL_CheckForResend(0);
    MSG_InitReadOnly(&stats,sentData.back().data(),sentData.back().size());
    MSG_BeginReading(&stats);
    MSG_ReadString(&stats);MSG_ReadShort(&stats);
    int decodedIndex=MSG_ReadByte(&stats);
    check(sentData.size()==packets+1 && decodedIndex==1,
        "stats retry at boundary selects next unsent packet");
    bool badStats=true;
    for(const char *text : {"statResponse", "statResponse junk", "statResponse 2147483648"})
        badStats=badStats && !receive(text) && connection.statPacketsToSend==127;
    check(badStats && clientUIActives[0].connectionState==CA_SENDINGSTATS,
        "malformed stats acknowledgements do not complete connection");
    check(receive("statResponse 3") && connection.statPacketsToSend==3
        && clientUIActives[0].connectionState==CA_SENDINGSTATS, "valid stats mask retains only requested packets");
    check(receive("statResponse 128") && !connection.statPacketsToSend
        && clientUIActives[0].connectionState==CA_CONNECTED, "existing masked-zero stats completion remains supported");
    receive("requeststats",foreign);
    check(clientUIActives[0].connectionState==CA_CONNECTED, "foreign requeststats cannot restart synchronization");
    receive("requeststats");
    check(clientUIActives[0].connectionState==CA_SENDINGSTATS && connection.statPacketsToSend==127,
        "selected server can restart stats for an established connection");
    reset();receive("requeststats");
    check(clientUIActives[0].connectionState==CA_CONNECTING, "requeststats cannot bypass challenge while connecting");
    check(!receive("loadingnewmap\nmp_crash\nwar") && clientUIActives[0].connectionState==CA_CONNECTING,
        "new-map notification cannot bypass initial authentication");
    fixtureAuthReady=true;transportSucceeds=false;CL_CheckForResend(0);
    check(!receive("challengeResponse 12") && !CL_ChallengeResponseAllowed(0,server()),
        "failed transport send cannot authorize challenge response");
    transportSucceeds=true;cls.realtime=6000;CL_CheckForResend(0);CL_ResetConnectionHandshake(0);
    check(!receive("challengeResponse 12") && clientUIActives[0].connectionState==CA_CONNECTING,
        "cancellation invalidates pending challenge");
    cls.realtime=9000;CL_CheckForResend(0);fixtureAuthReady=false;cls.realtime=12000;CL_CheckForResend(0);
    check(!receive("challengeResponse 12") && !CL_ChallengeResponseAllowed(0,server()),
        "provider loss on retry invalidates previously pending authentication");
    reset();fixtureAuthReady=true;CL_CheckForResend(0);connection.connectPacketCount=0;connection.connectTime=-99999;
    check(!receive("challengeResponse 12"), "fresh session counters cannot reuse earlier pending request");
    reset();bool timeout=false;
    for(int n=1;n<=3;++n){cls.realtime=n*3000;CL_CheckForResend(0);}
    for(int n=0;n<5;++n)CL_CheckTimeout(0);
    check(clients[0].timeoutcount==5 && sent.empty(), "failed authentication exhausts attempts through existing timeout debounce");
    try{CL_CheckTimeout(0);}catch(const std::runtime_error &e){timeout=std::strstr(e.what(),"SERVER_TIMEOUT");}
    check(timeout, "sixth exhausted-attempt frame follows production timeout error path");
    clientUIActives[0].connectionState=CA_DISCONNECTED;CL_CheckTimeout(0);
    check(clients[0].timeoutcount==0, "disconnected lifecycle resets timeout debounce");
    reset();fixtureAuthReady=true;CL_CheckForResend(0);
    check(!receive(std::string("  challengeResponse 12")+std::string(1024,' '))
        && clientUIActives[0].connectionState==CA_CONNECTING, "oversized first line cannot mutate state after truncation");
    // Place every short OOB/PB prefix directly before an inaccessible page.
    // This catches prefix reads past cursize even without sanitizer support.
    long page=sysconf(_SC_PAGESIZE);
    auto *mapping=static_cast<uint8_t *>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    iassert(mapping!=MAP_FAILED && !mprotect(mapping+page,page,PROT_NONE));
    const uint8_t prefix[]={255,255,255,255,'P','B','_'};
    bool bounded=true;
    for(int size=0;size<=7;++size){
        auto *data=mapping+page-size;std::memcpy(data,prefix,size);
        msg_t msg{};MSG_InitReadOnly(&msg,data,size);
        bool result=CL_ConnectionlessPacket(0,server(),&msg,cls.realtime);
        bounded=bounded && (result==(size==7)) && clientUIActives[0].connectionState==CA_CONNECTING;
    }
    munmap(mapping,page*2);
    check(bounded,"short OOB and PB prefixes are bounded at inaccessible page boundary");
    std::string masterResponse="getserversResponse";
    for(int n=0;n<200;++n)masterResponse += std::string("\\")+std::string(6,'\1');
    check(receive(masterResponse,server(),false) && masterResponseBytes==int(masterResponse.size())+4,
        "large delimiter-free binary master response retains production dispatcher routing");
    check(receive(std::string("PB_")+std::string(1500,'x'),server(),false),
        "large legacy PB binary prefix preserves existing ignored-packet handling");
    check(cmd_args.nesting==-1 && !cmd_argsPrivate.totalUsedArgvPool && !cmd_argsPrivate.totalUsedTextPool,
        "packet dispatch releases production tokenizer nesting and pools");
    std::printf("-- %d checks, %d failures --\n",checks,failures);return failures?1:0;
}

// Receive fixture never executes console commands. Retained Cbuf references use
// the explicit service seam; entering any command subsystem is a test failure.
namespace {
class ReceiveCommandServices final : public CmdServices {
public:
    void Monitor(const char*) override { std::abort(); }
    void WaitServer() override { std::abort(); }
    bool InServerFrame() override { std::abort(); }
    int DvarCommand() override { std::abort(); }
    bool ServerRunning() override { std::abort(); }
    bool ServerGameCommand() override { std::abort(); }
    void Forward(int,const char*) override { std::abort(); }
};
}
CmdServices &Cmd_GetServices() { static ReceiveCommandServices services;return services; }
