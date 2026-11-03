#pragma once

#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include "msg_mp.h"

// number of old messages that must be kept on client and
// server for delta comrpession and ping estimation
#define	PACKET_BACKUP	32
#define	PACKET_MASK		(PACKET_BACKUP-1)

#define	MAX_PACKET_USERCMDS		32		// max number of usercmd_t in a packet

#define	PORT_ANY			-1

#define	MAX_RELIABLE_COMMANDS	128			// max string commands buffered for restransmit

#define FAKELATENCY_MAX_PACKETS_HELD 512

enum netadrtype_t {
    NA_BOT,
    NA_BAD,					// an address lookup failed
    NA_LOOPBACK,
    NA_BROADCAST,
    NA_IP,
    NA_IPX,
    NA_BROADCAST_IPX
};

enum netsrc_t : __int32
{                                       // ...
    NS_CLIENT1 = 0x0,
    NS_SERVER = 0x1,
    NS_MAXCLIENTS = 0x1,
    NS_PACKET = 0x2,
};
inline netsrc_t &operator++(netsrc_t &e) {
    e = static_cast<netsrc_t>(static_cast<int>(e) + 1);
    return e;
}
inline netsrc_t &operator++(netsrc_t &e, int i)
{
    ++e;
    return e;
}

struct netadr_t {
    netadrtype_t	type;

    unsigned char  ip[4];
    unsigned short port;
    unsigned char  ipx[10];
};

struct ClientSnapshotData // sizeof=0x44
{
    int snapshotSize[8];
    int compressedSize[8];
    int index;
};

struct netProfilePacket_t // sizeof=0xC
{                                       // ...
    int iTime;
    int iSize;
    int bFragment;
};

struct netProfileStream_t // sizeof=0x2F0
{                                       // ...
    netProfilePacket_t packets[60];
    int iCurrPacket;
    int iBytesPerSecond;
    int iLastBPSCalcTime;
    int iCountedPackets;
    int iCountedFragments;
    int iFragmentPercentage;
    int iLargestPacket;
    int iSmallestPacket;
};

struct netProfileInfo_t // sizeof=0x5E0
{                                       // ...
    netProfileStream_t send;
    netProfileStream_t recieve;         // ...
};

struct netchan_t // sizeof=0x62C
{                                       // ...
    int outgoingSequence;
    netsrc_t sock;
    int dropped;
    int incomingSequence;
    netadr_t remoteAddress;             // ...
    int qport;
    int fragmentSequence;
    int fragmentLength;
    uint8_t* fragmentBuffer;
    int fragmentBufferSize;
    int unsentFragments;
    int unsentFragmentStart;
    int unsentLength;
    uint8_t* unsentBuffer;
    int unsentBufferSize;
    netProfileInfo_t prof;
};

struct fakedLatencyPackets_t // sizeof=0x50
{
    bool outbound;
    bool loopback;
    // padding byte
    // padding byte
    netsrc_t sock;
    netadr_t addr;
    uint32_t length;
    uint8_t *data;
    int startTime;
    msg_t msg;
};

struct loopmsg_t // sizeof=0x580 (originally; data was 1400)
{                                       // ...
    // KISAK: OOB voice batches to the listen-server host (up to ~10 KB) and large status replies exceeded the
    // original 1400-byte slot and silently overflowed into the next slots. Nothing serializes this struct.
    uint8_t data[16384];
    int datalen;
    int port;
};

struct loopback_t // sizeof=0x5808
{                                       // ...
    loopmsg_t msgs[16];
    volatile uint32_t get;
    volatile uint32_t send;
};

struct clientHeader_t // sizeof=0x64C
{                                       // ...
    int state;                          // ...
    int sendAsActive;
    int deltaMessage;
    int rateDelayed;
    netchan_t netchan;                  // ...
    float predictedOrigin[3];
    int predictedOriginServerTime;      // ...
};

//
//qboolean	NET_CompareAdr(netadr_t a, netadr_t b);
//qboolean	NET_CompareBaseAdr(netadr_t a, netadr_t b);
//qboolean	NET_IsLocalAddress(netadr_t adr);
const char* NET_AdrToString(netadr_t a);
//qboolean	NET_StringToAdr(const char* s, netadr_t* a);
//qboolean	NET_GetLoopPacket(netsrc_t sock, netadr_t* net_from, msg_t* net_message);
//

struct netProfileInfo_t;
struct netProfileStream_t;

void NetProf_PrepProfiling(netProfileInfo_t* prof);
void NetProf_AddPacket(netProfileStream_t* pProfStream, int iSize, int bFragment);
void NetProf_NewSendPacket(netchan_t* pChan, int iSize, int bFragment);
void NetProf_NewRecievePacket(netchan_t* pChan, int iSize, int bFragment);
void NetProf_UpdateStatistics(netProfileStream_t* pStream);
void Net_DisplayProfile(int localClientNum);
char FakeLag_DestroyPacket(uint32_t packet);
void FakeLag_SendPacket_Real(uint32_t packet);
void FakeLag_Init();
uint32_t FakeLag_GetFreeSlot();
bool FakeLag_HostingGameOrParty();
uint32_t FakeLag_SendPacket(netsrc_t sock, int length, uint8_t* data, netadr_t to);
uint32_t FakeLag_QueueIncomingPacket(bool loopback, netsrc_t sock, netadr_t* from, msg_t* msg);
void FakeLag_ReceivePackets();
int FakeLag_GetPacket(bool loopback, netsrc_t sock, netadr_t* net_from, msg_t* net_message);
void FakeLag_Frame();
int FakeLag_SendLaggedPackets();
void FakeLag_Shutdown();
void Netchan_Init(__int16 port);
void Net_DumpProfile_f();
void Netchan_Setup(
    netsrc_t sock,
    netchan_t* chan,
    netadr_t adr,
    int qport,
    char* outgoingBuffer,
    int outgoingBufferSize,
    char* incomingBuffer,
    int incomingBufferSize);
bool Netchan_TransmitNextFragment(netchan_t* chan);
bool Netchan_Transmit(netchan_t* chan, int length, char* data);
void BADPACKET(void *buffer, uint32_t len);
int Netchan_Process(netchan_t* chan, msg_t* msg);
int NET_CompareBaseAdrSigned(netadr_t* a, netadr_t* b);
bool NET_CompareBaseAdr(netadr_t a, netadr_t b);
int NET_CompareAdrSigned(netadr_t* a, netadr_t* b);
bool NET_CompareAdr(netadr_t a, netadr_t b);
bool NET_IsLocalAddress(netadr_t adr);
int NET_GetClientPacket(netadr_t* net_from, msg_t* net_message);
int NET_GetServerPacket(netadr_t* net_from, msg_t* net_message);
int NET_GetLoopPacket_Real(netsrc_t sock, netadr_t* net_from, msg_t* net_message);
int NET_GetLoopPacket(netsrc_t sock, netadr_t* net_from, msg_t* net_message);
void NET_SendLoopPacket(netsrc_t sock, uint32_t length, uint8_t* data, netadr_t to);
char NET_SendPacket(netsrc_t sock, int length, uint8_t* data, netadr_t to);
bool NET_OutOfBandPrint(netsrc_t sock, netadr_t adr, const char* data);
bool NET_OutOfBandData(netsrc_t sock, netadr_t adr, const uint8_t* format, int len);
bool NET_OutOfBandVoiceData(netsrc_t sock, netadr_t adr, uint8_t* format, uint32_t len);
int NET_StringToAdr(char* s, netadr_t* a);

extern const dvar_t* showpackets;
extern const dvar_t* fakelag_target;
extern const dvar_t* fakelag_packetloss;
extern const dvar_t* fakelag_currentjitter;
extern const dvar_t* fakelag_jitter;
extern const dvar_t* fakelag_current;
extern const dvar_t* msg_dumpEnts;
extern const dvar_t* net_profile;
extern const dvar_t* net_lanauthorize;
extern const dvar_t* packetDebug;
extern const dvar_t* showdrop;
extern const dvar_t* fakelag_jitterinterval;
extern const dvar_t* net_showprofile;
extern const dvar_t* msg_printEntityNums;
extern const dvar_t* msg_hudelemspew;

extern int g_qport;

void Com_PacketEventLoop(netsrc_t client, msg_t* netmsg);
void Com_DispatchClientPacketEvent(netadr_t adr, msg_t* netmsg);


extern ClientSnapshotData s_clientSnapshotData[64];