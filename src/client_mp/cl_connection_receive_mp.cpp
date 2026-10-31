// Production connectionless receive and timeout lifecycle, separated from UI startup.
#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif
#include "client_mp.h"
#include "cl_connection_mp.h"
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <cgame_mp/cg_local_mp.h>
#include <client/client.h>
#include <universal/assertive.h>
#include <universal/com_files.h>
#include <qcommon/cmd.h>
#include <qcommon/msg_mp.h>
#include <qcommon/com_fileaccess.h>
#include <win32/win_storage.h>
#include <ui_mp/ui_mp.h>
#include <stringed/stringed_hooks.h>

namespace {
bool ReadHandshakeInteger(const char *text, int32_t *value)
{
    if (!text || !*text) return false;
    char *end;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno == ERANGE || *end || parsed < INT32_MIN || parsed > INT32_MAX) return false;
    *value = static_cast<int32_t>(parsed);
    return true;
}
}

char __cdecl CL_ConnectionlessPacket(int localClientNum, netadr_t from, msg_t *msg, int32_t time)
{
    const char *v5; // eax
    char success; // [esp+3h] [ebp-9h]
    char *s; // [esp+4h] [ebp-8h]

    // UDP connectionless input is contiguous. Validate before the legacy PB
    // prefix probe. Only handshake commands require an untruncated text line;
    // master-server/PB packets may carry large binary payloads after the prefix.
    if (!msg || !msg->data || msg->cursize < 5 || msg->splitSize) return 0;
    MSG_BeginReading(msg);
    if (MSG_ReadLong(msg) != -1) return 0;
    int lineLength = 0;
    while (lineLength < msg->cursize - 4 && msg->data[4 + lineLength]
        && msg->data[4 + lineLength] != '\n') ++lineLength;
    if (!lineLength) return 0;
    CL_Netchan_AddOOBProfilePacket(localClientNum, msg->cursize);
    //if (!strnicmp((const char *)msg->data + 4, "PB_", 3u))
    if (msg->cursize >= 7 && !_strnicmp((const char *)msg->data + 4, "PB_", 3u))
    {
        // LWSS: Remove Punkbuster crap
        //if (msg->data[7] == 83 || msg->data[7] == 50 || msg->data[7] == 73)
        //    PbSvAddEvent(13, -1, msg->cursize - 4, (char *)msg->data + 4);
        //else
        //    PbClAddEvent(13, msg->cursize - 4, (char *)msg->data + 4);
        return 1;
    }
    else
    {
        s = MSG_ReadStringLine(msg);
        if (showpackets->current.integer)
        {
            v5 = NET_AdrToString(from);
            Com_Printf(CON_CHANNEL_SYSTEM, "recv: %s->'%s'\n", v5, s);
        }
        Cmd_TokenizeString(s);
        if (lineLength >= 1024)
        {
            for (const char *command : {"challengeResponse", "connectResponse", "statResponse", "requeststats", "loadingnewmap"})
            {
                if (!I_stricmp(Cmd_Argv(0), command))
                {
                    Cmd_EndTokenizedString();
                    return 0;
                }
            }
        }
        success = CL_DispatchConnectionlessPacket(localClientNum, from, msg, time);
        Cmd_EndTokenizedString();
        return success;
    }
}

char printBuf[2048];
char __cdecl CL_DispatchConnectionlessPacket(int localClientNum, netadr_t from, msg_t *msg, int32_t time)
{
    const char *v5; // eax
    const char *v6; // eax
    const char *v7; // eax
    char *v8; // eax
    const char *v9; // eax
    const char *v10; // eax
    char *StringLine; // eax
    char *v12; // eax
    clientConnection_t *LocalClientConnection; // eax
    netadr_t v14; // [esp-14h] [ebp-80h]
    netadr_t serverAddress; // [esp-14h] [ebp-80h]
    const char *v16; // [esp-4h] [ebp-70h]
    bool v17; // [esp+0h] [ebp-6Ch]
    char *v18; // [esp+4h] [ebp-68h]
    connstate_t connstate; // [esp+Ch] [ebp-60h]
    const char *c; // [esp+10h] [ebp-5Ch]
    int32_t statPacketsNeeded; // [esp+18h] [ebp-54h]
    clientConnection_t *clcc; // [esp+1Ch] [ebp-50h]
    clientConnection_t *clc; // [esp+1Ch] [ebp-50h]
    clientConnection_t *clca; // [esp+1Ch] [ebp-50h]
    clientConnection_t *clcd; // [esp+1Ch] [ebp-50h]
    clientConnection_t *clce; // [esp+1Ch] [ebp-50h]
    clientConnection_t *clcb; // [esp+1Ch] [ebp-50h]
    char *s; // [esp+20h] [ebp-4Ch]
    char *sa; // [esp+20h] [ebp-4Ch]
    char mapname[68]; // [esp+24h] [ebp-48h] BYREF

    c = Cmd_Argv(0);
    if (!I_stricmp(c, "v"))
    {
        CL_VoicePacket(localClientNum, msg);
        return 1;
    }
    if (!I_stricmp(c, "vt"))
        return 1;

    iassert(localClientNum == 0);
    connstate = clientUIActives[0].connectionState;
    if (!I_stricmp(c, "statResponse") || !I_stricmp(c, "requeststats")
        || !I_stricmp(c, "loadingnewmap") || !I_stricmp(c, "fastrestart"))
    {
        const auto *connection = CL_GetLocalClientConnection(localClientNum);
        if (!NET_CompareAdr(from, connection->serverAddress)) return 0;
        if ((!I_stricmp(c, "requeststats") || !I_stricmp(c, "loadingnewmap"))
            && connstate < CA_CONNECTED) return 0;
    }
    if (I_stricmp(c, "challengeResponse"))
    {
        if (I_stricmp(c, "connectResponse"))
        {
            if (I_stricmp(c, "statResponse"))
            {
                if (!I_stricmp(c, "infoResponse"))
                {
                    CL_ServerInfoPacket(from, msg, time);
                    return 1;
                }
                if (!I_stricmp(c, "statusResponse"))
                {
                    CL_ServerStatusResponse(from, msg);
                    return 1;
                }
                if (I_stricmp(c, "disconnect"))
                {
                    if (!I_stricmp(c, "echo"))
                    {
                        v9 = Cmd_Argv(1);
                        v10 = va("%s", v9);
                        NET_OutOfBandPrint((netsrc_t)localClientNum, from, v10);
                        return 1;
                    }
                    if (!I_stricmp(c, "keyAuthorize"))
                        return 1;
                    if (!I_stricmp(c, "print"))
                    {
                        clcd = CL_GetLocalClientConnection(localClientNum);
                        s = MSG_ReadBigString(msg);
                        I_strncpyz(clcd->serverMessage, s, 256);
                        Com_sprintf(printBuf, 0x800u, "%s", s);
                        Com_PrintMessage(CON_CHANNEL_CLIENT, printBuf, 0);
                        return 1;
                    }
                    if (I_stricmp(c, "error"))
                    {
                        if (!I_stricmp(c, "updateResponse"))
                        {
                            CL_UpdateInfoPacket(from);
                            return 1;
                        }
                        if (!I_strncmp(c, "getserversResponse", 18))
                        {
                            CL_ServersResponsePacket(from, msg);
                            return 1;
                        }
                        if (!I_strncmp(c, "needcdkey", 9))
                        {
                            clce = CL_GetLocalClientConnection(localClientNum);
                            I_strncpyz(clce->serverMessage, "EXE_AWAITINGCDKEYAUTH", 256);
                            SEH_LocalizeTextMessage("EXE_AWAITINGCDKEYAUTH", "need cd key message", LOCMSG_SAFE);
                            Com_Printf(CON_CHANNEL_CLIENT, "%s\n", clce->serverMessage);
                            CL_RequestAuthorization(localClientNum);
                            return 1;
                        }
                        if (I_stricmp(c, "loadingnewmap"))
                        {
                            if (!I_stricmp(c, "requeststats"))
                            {
                                if (cls.downloadName[0])
                                    return 1;

                                if (CL_GetLocalClientConnectionState(localClientNum) != CA_SENDINGSTATS)
                                {
                                    if (localClientNum)
                                        MyAssertHandler(
                                            "c:\\trees\\cod3\\src\\client_mp\\client_mp.h",
                                            1120,
                                            0,
                                            "client doesn't index STATIC_MAX_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                                            localClientNum,
                                            1);
                                    clientUIActives[localClientNum].connectionState = CA_SENDINGSTATS;
                                    LocalClientConnection = CL_GetLocalClientConnection(localClientNum);
                                    LocalClientConnection->statPacketSendTime[0] = 0;
                                    LocalClientConnection->statPacketSendTime[1] = 0;
                                    LocalClientConnection->statPacketSendTime[2] = 0;
                                    LocalClientConnection->statPacketSendTime[3] = 0;
                                    LocalClientConnection->statPacketSendTime[4] = 0;
                                    LocalClientConnection->statPacketSendTime[5] = 0;
                                    LocalClientConnection->statPacketSendTime[6] = 0;
                                    LocalClientConnection->statPacketsToSend = 127;
                                    LocalClientConnection->lastPacketTime = cls.realtime;
                                    LocalClientConnection->lastPacketSentTime = -9999;
                                }
                            }
                            if (I_stricmp(c, "fastrestart"))
                            {
                                return 0;
                            }
                            else
                            {
                                clcb = CL_GetLocalClientConnection(localClientNum);
                                v17 = CL_GetLocalClientConnectionState(localClientNum) == CA_ACTIVE && NET_CompareBaseAdr(from, clcb->serverAddress);
                                clcb->isServerRestarting = v17;
                                return 1;
                            }
                        }
                        else
                        {
                            serverAddress = CL_GetLocalClientConnection(localClientNum)->serverAddress;
                            if (NET_CompareBaseAdr(from, serverAddress))
                            {
                                if (cls.downloadName[0])
                                    return 1;
                                UI_CloseAllMenus(localClientNum);
                                Cbuf_AddText(localClientNum, "uploadStats\n");
                                StringLine = MSG_ReadStringLine(msg);
                                I_strncpyz(mapname, StringLine, 64);
                                clientUIActives[localClientNum].connectionState = CA_CONNECTED;
                                v12 = MSG_ReadStringLine(msg);
                                CL_SetupForNewServerMap(mapname, v12);
                            }
                            return 1;
                        }
                    }
                    else if (connstate
                        && (v14 = CL_GetLocalClientConnection(localClientNum)->serverAddress, NET_CompareBaseAdr(from, v14)))
                    {
                        sa = MSG_ReadBigString(msg);
                        Com_Error(ERR_DROP, "%s", sa);
                        return 1;
                    }
                    else
                    {
                        return 0;
                    }
                }
                else
                {
                    if (Cmd_Argc() <= 1)
                    {
                        CL_DisconnectPacket(localClientNum, from, 0);
                    }
                    else
                    {
                        v8 = (char *)Cmd_Argv(1);
                        CL_DisconnectPacket(localClientNum, from, v8);
                    }
                    return 1;
                }
            }
            else if (connstate <= CA_SENDINGSTATS)
            {
                if (connstate == CA_SENDINGSTATS)
                {
                    clca = CL_GetLocalClientConnection(localClientNum);
                    v7 = Cmd_Argv(1);
                    if (!ReadHandshakeInteger(v7, &statPacketsNeeded)) return 0;
                    // KISAK: test the 7-bit mask, not the raw value. A reply such as 128 used to leave
                    // statPacketsToSend == 0 while still in CA_SENDINGSTATS, which drove the packet picker to -1.
                    if (statPacketsNeeded & 0x7F)
                    {
                        clca->statPacketsToSend = statPacketsNeeded & 0x7F;
                    }
                    else
                    {
                        if (localClientNum)
                            MyAssertHandler(
                                "c:\\trees\\cod3\\src\\client_mp\\client_mp.h",
                                1120,
                                0,
                                "client doesn't index STATIC_MAX_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                                localClientNum,
                                1);
                        clientUIActives[localClientNum].connectionState = CA_CONNECTED;
                        clca->statPacketsToSend = 0;
                    }
                    clca->lastPacketTime = cls.realtime;
                    clca->lastPacketSentTime = -9999;
                    return 1;
                }
                else
                {
                    Com_Printf(CON_CHANNEL_CLIENT, "statResponse packet while not syncing stats.  Ignored.\n");
                    return 0;
                }
            }
            else
            {
                Com_Printf(CON_CHANNEL_CLIENT, "Dup statResponse received.  Ignored.\n");
                return 0;
            }
        }
        else if (connstate < CA_CONNECTED)
        {
            if (connstate == CA_CHALLENGING)
            {
                clc = CL_GetLocalClientConnection(localClientNum);
                if (NET_CompareAdr(from, clc->serverAddress))
                {
                    if (autoupdateChecked
                        && NET_CompareAdr(cls.autoupdateServer, clc->serverAddress)
                        && cl_updateavailable->current.enabled)
                    {
                        autoupdateStarted = 1;
                    }
                    Netchan_Setup(
                        (netsrc_t)localClientNum,
                        &clc->netchan,
                        from,
                        localClientNum + g_qport,
                        clc->netchanOutgoingBuffer,
                        2048,
                        clc->netchanIncomingBuffer,
                        0x20000);
                    if (Cmd_Argc() <= 1)
                        v18 = (char *)"";
                    else
                        v18 = (char *)Cmd_Argv(1);
                    if (I_stricmp(v18, fs_gameDirVar->current.string))
                        LiveStorage_ReadStatsFromDir(v18);
                    if (localClientNum)
                        MyAssertHandler(
                            "c:\\trees\\cod3\\src\\client_mp\\client_mp.h",
                            1120,
                            0,
                            "client doesn't index STATIC_MAX_LOCAL_CLIENTS\n\t%i not in [0, %i)",
                            localClientNum,
                            1);
                    clientUIActives[localClientNum].connectionState = CA_SENDINGSTATS;
                    clc->statPacketSendTime[0] = 0;
                    clc->statPacketSendTime[1] = 0;
                    clc->statPacketSendTime[2] = 0;
                    clc->statPacketSendTime[3] = 0;
                    clc->statPacketSendTime[4] = 0;
                    clc->statPacketSendTime[5] = 0;
                    clc->statPacketSendTime[6] = 0;
                    clc->statPacketsToSend = 127;
                    clc->lastPacketTime = cls.realtime;
                    clc->lastPacketSentTime = -9999;
                    return 1;
                }
                else
                {
                    Com_Printf(CON_CHANNEL_CLIENT, "connectResponse from a different address.  Ignored.\n");
                    v16 = NET_AdrToString(clc->serverAddress);
                    v6 = NET_AdrToString(from);
                    Com_Printf(CON_CHANNEL_CLIENT, "%s should have been %s\n", v6, v16);
                    return 0;
                }
            }
            else
            {
                Com_Printf(CON_CHANNEL_CLIENT, "connectResponse packet while not connecting.  Ignored.\n");
                return 0;
            }
        }
        else
        {
            Com_Printf(CON_CHANNEL_CLIENT, "Dup connect received.  Ignored.\n");
            return 0;
        }
    }
    else if (connstate == CA_CONNECTING)
    {
        clcc = CL_GetLocalClientConnection(localClientNum);
        v5 = Cmd_Argv(1);
        int32_t challenge;
        if (!CL_ChallengeResponseAllowed(localClientNum, from)
            || !ReadHandshakeInteger(v5, &challenge)) return 0;
        CL_ResetConnectionHandshake(localClientNum);
        clcc->challenge = challenge;
        clientUIActives[localClientNum].connectionState = CA_CHALLENGING;
        clcc->connectPacketCount = 0;
        clcc->connectTime = -99999;
        clcc->serverAddress = from;
        Com_DPrintf(CON_CHANNEL_CLIENT, "challenge: %d\n", clcc->challenge);
        return 1;
    }
    else
    {
        Com_Printf(CON_CHANNEL_CLIENT, "Unwanted challenge response received.  Ignored.\n");
        return 0;
    }
}

void __cdecl CL_DisconnectPacket(int32_t localClientNum, netadr_t from, char *reason)
{
    if (CL_GetLocalClientConnectionState(localClientNum) >= CA_CONNECTING)
    {
        clientConnection_t *clc = CL_GetLocalClientConnection(localClientNum);
        if (NET_CompareAdr(from, clc->netchan.remoteAddress))
        {
            if (cls.realtime - clc->lastPacketTime >= 3000)
            {
                if (cls.wwwDlDisconnected)
                {
                    CL_Disconnect(localClientNum);
                }
                else if (reason)
                {
                    CL_DisconnectError(reason);
                }
                else
                {
                    Com_Error(ERR_DROP, "EXE_SERVER_DISCONNECTED");
                }
            }
        }
    }
}

void __cdecl CL_CheckTimeout(int32_t localClientNum)
{
    clientActive_t *LocalClientGlobals; // [esp+8h] [ebp-Ch]
    clientConnection_t *clc; // [esp+10h] [ebp-4h]

    connstate_t connstate = CL_GetLocalClientConnectionState(localClientNum);

    if (connstate >= CA_CONNECTING)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
        clc = CL_GetLocalClientConnection(localClientNum);
        if (cl_paused->current.integer && sv_paused->current.integer
            || connstate < CA_PRIMED
            || clc->lastPacketTime <= 0
            || cl_timeout->current.value * 1000.0 >= (double)(cls.realtime - clc->lastPacketTime))
        {
            if (connstate <= CA_CONNECTING
                || connstate >= CA_PRIMED
                || clc->lastPacketTime <= 0
                || cl_connectTimeout->current.value * 1000.0 >= (double)(cls.realtime - clc->lastPacketTime))
            {
                if (connstate != CA_CONNECTING
                    || clc->lastPacketTime
                    || clc->connectPacketCount <= cl_connectionAttempts->current.integer)
                {
                    LocalClientGlobals->timeoutcount = 0;
                }
                else if (++LocalClientGlobals->timeoutcount > 5)
                {
                    CL_ServerTimedOut();
                }
            }
            else if (++LocalClientGlobals->timeoutcount > 5)
            {
                CL_ServerTimedOut();
            }
        }
        else if (++LocalClientGlobals->timeoutcount > 5)
        {
            CL_ServerTimedOut();
        }
    }
    else if (localClientNum < 1)
    {
        CL_GetLocalClientGlobals(localClientNum)->timeoutcount = 0;
    }
}

void __cdecl CL_ServerTimedOut()
{
    Com_Error(ERR_DROP, "EXE_ERR_SERVER_TIMEOUT");
}
