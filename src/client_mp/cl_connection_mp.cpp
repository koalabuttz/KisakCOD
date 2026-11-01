// Connection resend state machine extracted without changing wire/state policy.
#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif
#include "client_mp.h"
#include "cl_auth.h"
#include "cl_connection_mp.h"
#include <cgame_mp/cg_local_mp.h>
#include <client/client.h>
#include <universal/assertive.h>
#include <qcommon/msg_mp.h>
#include <win32/win_local.h>
#include <win32/win_storage.h>

namespace {
struct PendingChallenge {
    bool sent;
    netadr_t endpoint;
    int attempt;
    int sendTime;
};
PendingChallenge pendingChallenges[STATIC_MAX_LOCAL_CLIENTS];
}

void CL_ResetConnectionHandshake(int localClientNum)
{
    iassert(localClientNum >= 0 && localClientNum < STATIC_MAX_LOCAL_CLIENTS);
    pendingChallenges[localClientNum] = {};
}

bool CL_ChallengeResponseAllowed(int localClientNum, netadr_t from)
{
    iassert(localClientNum >= 0 && localClientNum < STATIC_MAX_LOCAL_CLIENTS);
    const auto &pending = pendingChallenges[localClientNum];
    const auto *connection = CL_GetLocalClientConnection(localClientNum);
    return pending.sent && CL_GetLocalClientConnectionState(localClientNum) == CA_CONNECTING
        && connection->connectPacketCount >= pending.attempt
        && connection->connectTime >= pending.sendTime
        && NET_CompareAdr(connection->serverAddress, pending.endpoint)
        && NET_CompareAdr(from, pending.endpoint);
}

uint8_t msgBuffer[2048];
void __cdecl CL_CheckForResend(int localClientNum)
{
    const char *v2; // eax
    char *v3; // eax
    const char *v4; // eax
    const char *v5; // eax
    const char *v6; // eax
    int32_t v7; // [esp+0h] [ebp-1188h]
    uint8_t dst[1244]; // [esp+50h] [ebp-1138h] BYREF
    connstate_t connectionState; // [esp+52Ch] [ebp-C5Ch]
    char dest[1028]; // [esp+530h] [ebp-C58h] BYREF
    int32_t pktlen; // [esp+934h] [ebp-854h] BYREF
    uint8_t src[1028]; // [esp+938h] [ebp-850h] BYREF
    uint32_t count; // [esp+D3Ch] [ebp-44Ch]
    msg_t buf; // [esp+D40h] [ebp-448h] BYREF
    int32_t length; // [esp+D68h] [ebp-420h]
    void *data; // [esp+D6Ch] [ebp-41Ch]
    clientConnection_t *clc; // [esp+D70h] [ebp-418h]
    int32_t c; // [esp+D74h] [ebp-414h]
    char pkt[1036]; // [esp+D78h] [ebp-410h] BYREF

    char challengeRequest[1536];

    iassert(localClientNum == 0);
    connectionState = clientUIActives[0].connectionState;
    if (clientUIActives[0].connectionState == CA_CONNECTING
        || connectionState == CA_CHALLENGING
        || connectionState == CA_SENDINGSTATS)
    {
        clc = CL_GetLocalClientConnection(localClientNum);
        if (connectionState == CA_SENDINGSTATS)
        {
            if (cls.realtime - clc->lastPacketSentTime < 100)
                return;
        }
        else if (cls.realtime - clc->connectTime < 3000)
        {
            return;
        }
        if (!clc->demoplaying)
        {
            clc->connectTime = cls.realtime;
            ++clc->connectPacketCount;
            switch (connectionState)
            {
            case CA_CONNECTING:
                if (net_lanauthorize->current.enabled || !Sys_IsLANAddress(clc->serverAddress))
                    CL_RequestAuthorization(localClientNum);
                strcpy(pkt, "getchallenge");
                pktlen = &pkt[strlen(pkt) + 1] - &pkt[1];
                //PbClientConnecting(1, pkt, &pktlen);
                //CL_BuildMd5StrFromCDKey(md5Str);
                //v2 = va("getchallenge 0 \"%s\"", md5Str);

                {
                    CL_ResetConnectionHandshake(localClientNum);
                    ClientAuthResult auth = CL_GetPlatformChallenge(challengeRequest, sizeof(challengeRequest));
                    if (auth != ClientAuthResult::ready)
                    {
                        Com_PrintError(CON_CHANNEL_CLIENT, "Client authentication %s; challenge not sent\n",
                            CL_AuthResultName(auth));
                        break;
                    }
                    if (NET_OutOfBandPrint((netsrc_t)localClientNum, clc->serverAddress, challengeRequest))
                    {
                        pendingChallenges[localClientNum] = { true, clc->serverAddress,
                            clc->connectPacketCount, clc->connectTime };
                    }
                }
                break;
            case CA_CHALLENGING:
                v3 = Dvar_InfoString(localClientNum, 2);
                I_strncpyz(dest, v3, 1024);
                v4 = va("%i", 1);
                Info_SetValueForKey(dest, "protocol", v4);
                v5 = va("%i", clc->challenge);
                Info_SetValueForKey(dest, "challenge", v5);
                if (!clc->qport)
                    MyAssertHandler(".\\client_mp\\cl_main_mp.cpp", 2980, 0, "%s", "clc->qport != 0");
                v6 = va("%i", clc->qport);
                Info_SetValueForKey(dest, "qport", v6);
                qmemcpy(src, "connect \"", 9);
                count = &dest[strlen(dest) + 1] - &dest[1];
                memcpy(&src[9], dest, count);
                src[count + 9] = 34;
                src[count + 10] = 0;
                pktlen = count + 10;
                memcpy(pkt, src, count + 10);
                //PbClientConnecting(2, pkt, &pktlen);
                NET_OutOfBandData((netsrc_t)localClientNum, clc->serverAddress, src, count + 10);
                dvar_modifiedFlags &= ~2u;
                break;
            case CA_SENDINGSTATS:
                MSG_Init(&buf, msgBuffer, 2048);
                MSG_WriteString(&buf, "stats");
                c = CL_HighestPriorityStatPacket(clc);
                if (c < 0)
                {
                    // KISAK: nothing eligible to send this frame. Should be unreachable (the statResponse
                    // handler never leaves the mask at 0 in this state, and the 100ms resend gate guarantees
                    // an older stamp), but index -1 read 1240 bytes before the stat buffer and wrote
                    // statPacketSendTime[-1], so skip the frame rather than trust it.
                    break;
                }
                if (c > 6)
                    MyAssertHandler(
                        ".\\client_mp\\cl_main_mp.cpp",
                        3017,
                        0,
                        "%s\n\t(packetToSend) = %i",
                        "(packetToSend >= 0 && packetToSend < 7)",
                        c);
                if (LiveStorage_DoWeHaveStats())
                {
                    data = (char*)LiveStorage_GetStatBuffer() + 1240 * c;
                }
                else
                {
                    if (onlinegame->current.enabled)
                        MyAssertHandler(".\\client_mp\\cl_main_mp.cpp", 3029, 0, "%s", "!onlinegame->current.enabled");
                    memset(dst, 0, 1240u);
                    data = dst;
                }
                MSG_WriteShort(&buf, clc->qport);
                MSG_WriteByte(&buf, c);
                if (0x2000 - 1240 * c > 1240)
                    v7 = 1240;
                else
                    v7 = 0x2000 - 1240 * c;
                length = v7;
                MSG_WriteData(&buf, (unsigned char*)data, v7);
                clc->statPacketSendTime[c] = cls.realtime;
                clc->lastPacketSentTime = cls.realtime;
                NET_OutOfBandData((netsrc_t)localClientNum, clc->serverAddress, buf.data, buf.cursize);
                break;
            default:
                Com_Error(ERR_FATAL, "CL_CheckForResend: bad connstate");
                break;
            }
        }
    }
}

int32_t __cdecl CL_HighestPriorityStatPacket(clientConnection_t *clc)
{
    int32_t packet; // [esp+0h] [ebp-Ch]
    int32_t oldestPacketTime; // [esp+4h] [ebp-8h]
    int32_t oldestPacket; // [esp+8h] [ebp-4h]

    oldestPacketTime = cls.realtime;
    oldestPacket = -1;
    for (packet = 0; packet < 7; ++packet)
    {
        if (((1 << packet) & clc->statPacketsToSend) != 0)
        {
            if (!clc->statPacketSendTime[packet])
                return packet;
            if (clc->statPacketSendTime[packet] < oldestPacketTime)
            {
                oldestPacketTime = clc->statPacketSendTime[packet];
                oldestPacket = packet;
            }
        }
    }
    if (oldestPacket < 0)
        MyAssertHandler(".\\client_mp\\cl_main_mp.cpp", 2863, 0, "%s", "oldestPacket >= 0");
    return oldestPacket;
}
