#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include "server_mp.h"
#include "sv_remote_input.h"
#include "sv_query_limit.h"
#include <qcommon/master_server.h>
#include <qcommon/cmd.h>

#include <cgame_mp/cg_local_mp.h>
#include <game_mp/g_main_mp.h>

static netadr_t adr;
static MasterResolveState s_masterResolve;

// True when this server reports to a master: `dedicated 2` and a master named
// in masterServerName. The setting is empty by default, so by default the
// server contacts nobody.
bool SV_UsesMasterServer()
{
    return com_dedicated && com_masterServerName
        && Master_ServerUsesMaster(com_dedicated->current.integer, com_masterServerName->current.string);
}

// The master's address, or an address of type NA_BAD when no master is set or
// its name does not resolve. The lookup is cached: see Master_ShouldResolve.
const netadr_t *SV_MasterAddress()
{
    const char *name = com_masterServerName->current.string;
    const int port = com_masterPort->current.integer;
    const uint32_t now = Sys_Milliseconds();

    if (!Master_NameIsSet(name))
    {
        memset(&adr, 0, sizeof(adr));
        adr.type = NA_BAD;
        return &adr;
    }
    if (Master_ShouldResolve(&s_masterResolve, name, port, now))
    {
        Com_Printf(CON_CHANNEL_SERVER, "Resolving %s\n", name);
        memset(&adr, 0, sizeof(adr));
        // "localhost" is the in-process address, not somewhere a packet can go.
        if (NET_StringToAdr((char *)name, &adr) && adr.type == NA_IP)
        {
            if (!Master_NameHasPort(name))
                adr.port = BigShort(port);
            Com_Printf(
                CON_CHANNEL_SERVER,
                "%s resolved to %i.%i.%i.%i:%i\n",
                name,
                adr.ip[0],
                adr.ip[1],
                adr.ip[2],
                adr.ip[3],
                (uint16_t)BigShort(adr.port));
            Master_ResolveAttempted(&s_masterResolve, name, port, true, now);
        }
        else
        {
            memset(&adr, 0, sizeof(adr));
            adr.type = NA_BAD;
            Com_Printf(CON_CHANNEL_SERVER, "Couldn't resolve address: %s\n", name);
            Master_ResolveAttempted(&s_masterResolve, name, port, false, now);
        }
    }
    return &adr;
}

void SV_UpdateLastTimeMasterServerCommunicated(netadr_t from)
{
#if 0 // LWSS: I am pretty sure the master server is already dead
    netadr_t v1; // [esp-14h] [ebp-18h]

    v1 = *SV_MasterAddress();
    if (NET_CompareBaseAdr(from, v1))
        svs.sv_lastTimeMasterServerCommunicated = svs.time;
#endif
}


void SV_MasterGameCompleteStatus()
{
    const netadr_t *master;

    if (!SV_UsesMasterServer())
        return;
    master = SV_MasterAddress();
    if (master->type == NA_IP)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Sending gameCompleteStatus to %s\n", com_masterServerName->current.string);
        SVC_GameCompleteStatus(*master);
    }
}

// Upstream commented the heartbeat out because the retail master's replies ran
// into the Steam authentication code, and that master is gone. It is sent
// again, but only to a master the server's owner named: with masterServerName
// empty (the default) this returns before looking anything up.
void SV_MasterHeartbeat(const char *hbname)
{
    const netadr_t *master;

    if (!com_dedicated || !com_masterServerName
        || !Master_HeartbeatDue(
            com_dedicated->current.integer,
            com_masterServerName->current.string,
            svs.time,
            svs.nextHeartbeatTime))
    {
        return;
    }
    svs.nextHeartbeatTime = svs.time + MASTER_HEARTBEAT_MSEC;
    master = SV_MasterAddress();
    if (master->type == NA_IP)
    {
        Com_Printf(CON_CHANNEL_SERVER, "Sending heartbeat to %s\n", com_masterServerName->current.string);
        NET_OutOfBandPrint(NS_SERVER, *master, va("heartbeat %s\n", hbname));
    }
}

void SV_MasterShutdown()
{
    svs.nextHeartbeatTime = 0x80000000;
    SV_MasterHeartbeat("flatline");
}

void SV_AuthorizeIpPacket(netadr_t from)
{
    const char *v1; // eax
    challenge_t *v2; // edx
    challenge_t *v3; // eax
    const char *v4; // eax
    netadr_t v5; // [esp-18h] [ebp-444h]
    const char *v6; // [esp-4h] [ebp-430h]
    const char *cdkeyHashFromAuth; // [esp+14h] [ebp-418h]
    const char *r; // [esp+18h] [ebp-414h]
    char ret[1024]; // [esp+1Ch] [ebp-410h] BYREF
    int challenge; // [esp+41Ch] [ebp-10h]
    const char *s; // [esp+420h] [ebp-Ch]
    int i; // [esp+424h] [ebp-8h]

    if (!NET_CompareBaseAdr(from, svs.authorizeAddress))
    {
        Com_Printf(CON_CHANNEL_SERVER, "SV_AuthorizeIpPacket: not from authorize server\n");
        return;
    }
    v1 = SV_Cmd_Argv(1);
    challenge = atoi(v1);
    for (i = 0; i < 1024 && svs.challenges[i].challenge != challenge; ++i)
        ;
    if (i == 1024)
    {
        Com_Printf(CON_CHANNEL_SERVER, "SV_AuthorizeIpPacket: challenge not found\n");
        return;
    }
    svs.challenges[i].pingTime = svs.time;
    s = SV_Cmd_Argv(2);
    r = SV_Cmd_Argv(3);
    cdkeyHashFromAuth = SV_Cmd_Argv(5);
    if (!I_stricmp(s, "deny"))
    {
        if (r && *r)
        {
            if (!I_stricmp(r, "CLIENT_UNKNOWN_TO_AUTH") || !I_stricmp(r, "BAD_CDKEY"))
            {
                NET_OutOfBandPrint(NS_SERVER, svs.challenges[i].adr, "needcdkey");
            LABEL_22:
                memset(&svs.challenges[i], 0, sizeof(svs.challenges[i]));
                return;
            }
            if (I_stricmp(r, "INVALID_CDKEY"))
            {
                if (I_stricmp(r, "BANNED_CDKEY"))
                {
                    v3 = &svs.challenges[i];
                    *&v5.type = *&v3->adr.type;
                    *&v5.port = *&v3->adr.port;
                    *&v5.ipx[2] = *&v3->adr.ipx[2];
                }
                else
                {
                    v2 = &svs.challenges[i];
                    *&v5.type = *&v2->adr.type;
                    *&v5.port = *&v2->adr.port;
                    *&v5.ipx[2] = *&v2->adr.ipx[2];
                }
                NET_OutOfBandPrint(NS_SERVER, v5, "error\nEXE_ERR_BAD_CDKEY");
                goto LABEL_22;
            }
        }
        NET_OutOfBandPrint(NS_SERVER, svs.challenges[i].adr, "error\nEXE_ERR_CDKEY_IN_USE");
        goto LABEL_22;
    }
    if (!strcmp(svs.challenges[i].cdkeyHash, cdkeyHashFromAuth))
    {
        if (I_stricmp(s, "demo"))
        {
            if (I_stricmp(s, "accept"))
            {
                if (r && *r)
                {
                    snprintf(ret, ARRAYSIZE(ret), "error\n%s", r);
                    NET_OutOfBandPrint(NS_SERVER, svs.challenges[i].adr, ret);
                }
                else
                {
                    NET_OutOfBandPrint(NS_SERVER, svs.challenges[i].adr, "error\nEXE_ERR_BAD_CDKEY");
                }
                memset(&svs.challenges[i], 0, sizeof(svs.challenges[i]));
            }
            else if (!svs.challenges[i].connected)
            {
                v6 = va("challengeResponse %i", svs.challenges[i].challenge);
                NET_OutOfBandPrint(NS_SERVER, svs.challenges[i].adr, v6);
            }
        }
        else if (Dvar_GetBool("fs_restrict"))
        {
            v4 = va("challengeResponse %i", svs.challenges[i].challenge);
            NET_OutOfBandPrint(NS_SERVER, svs.challenges[i].adr, v4);
        }
        else
        {
            NET_OutOfBandPrint(NS_SERVER, svs.challenges[i].adr, "error\nEXE_ERR_NOT_A_DEMO_SERVER");
            memset(&svs.challenges[i], 0, sizeof(svs.challenges[i]));
        }
    }
    else
    {
        Com_Printf(
            CON_CHANNEL_SERVER,
            "rejecting connection due to mismatched GUID: expected \"%s\", got \"%s\"\n",
            svs.challenges[i].cdkeyHash,
            cdkeyHashFromAuth);
        NET_OutOfBandPrint(NS_SERVER, svs.challenges[i].adr, "error\nEXE_ERR_BAD_CDKEY");
        memset(&svs.challenges[i], 0, sizeof(svs.challenges[i]));
    }
}

void SV_FlushRedirect(char *outputbuf)
{
    char c; // [esp+13h] [ebp-525h]
    char buf[1304]; // [esp+18h] [ebp-520h] BYREF
    int len; // [esp+534h] [ebp-4h]

    len = strlen(outputbuf);
    while (len > 1294)
    {
        c = outputbuf[1294];
        outputbuf[1294] = 0;
        Com_sprintf(buf, 0x514u, "print\n%s", outputbuf);
        NET_OutOfBandPrint(NS_SERVER, svs.redirectAddress, buf);
        len -= 1294;
        outputbuf += 1294;
        *outputbuf = c;
    }
    Com_sprintf(buf, 0x514u, "print\n%s", outputbuf);
    NET_OutOfBandPrint(NS_SERVER, svs.redirectAddress, buf);
}

void SVC_RemoteCommand(netadr_t from)
{
    const char *v1; // eax
    const char *v2; // eax
    const char *v3; // eax
    int v4; // [esp-Ch] [ebp-C2Ch]
    const char *v5; // [esp-4h] [ebp-C24h]
    const char *v6; // [esp-4h] [ebp-C24h]
    char remaining[1024]; // [esp+18h] [ebp-C08h] BYREF
    char sv_outputbuf[2032]; // [esp+418h] [ebp-808h] BYREF
    int valid; // [esp+C08h] [ebp-18h]
    int len; // [esp+C0Ch] [ebp-14h]
    const char *password; // [esp+C10h] [ebp-10h]
    int i; // [esp+C18h] [ebp-8h]

    // KISAK: retail allowed one rcon packet per 500 ms from anyone, so a stream of
    // bad passwords locked the admin out.  Now each address has its own limit,
    // checked before the password, and only replies to wrong passwords share a
    // global limit (sv_query_limit.h).  A correct password is never refused for
    // someone else's flood.
    if (SV_QueryLimitAllows(from, SV_QUERY_RCON))
    {
        password = SV_Cmd_Argv(1);
        // KISAK: `current.integer` on a string dvar is the string pointer and never 0,
        // so an unset rcon_password used to accept `rcon "" <command>` from anyone.
        // The password must be set and match; see SV_RconPasswordAccepted.
        if (SV_RconPasswordAccepted(rcon_password->current.string, password))
        {
            valid = 1;
            v6 = SV_Cmd_Argv(2);
            v2 = NET_AdrToString(from);
            Com_Printf(CON_CHANNEL_SERVER, "Rcon from %s:\n%s\n", v2, v6);
        }
        else if (!SV_RconFailureReplyAllowed())
        {
            return; // flood of wrong passwords: drop silently
        }
        else
        {
            valid = 0;
            v5 = SV_Cmd_Argv(2);
            v1 = NET_AdrToString(from);
            Com_Printf(CON_CHANNEL_SERVER, "Bad rcon from %s:\n%s\n", v1, v5);
        }
        svs.redirectAddress = from;
        Com_BeginRedirect(sv_outputbuf, 0x7F0u, SV_FlushRedirect);
        if (rcon_password->current.string && rcon_password->current.string[0])
        {
            if (valid)
            {
                len = 0;
                for (i = 2; i < SV_Cmd_Argc(); ++i)
                {
                    v4 = len;
                    v3 = SV_Cmd_Argv(i);
                    len = Com_AddToString(v3, remaining, v4, 1024, 1);
                    len = Com_AddToString(" ", remaining, len, 1024, 0);
                }
                if (len < 1024)
                {
                    remaining[len] = 0;
                    SV_Cmd_ExecuteString(0, 0, remaining);
                    //if (!I_strnicmp(remaining, "pb_sv_", 6))
                    //    PbServerForceProcess();
                }
            }
            else if (*password)
            {
                Com_Printf(CON_CHANNEL_SERVER, "Invalid password.\n");
            }
            else
            {
                Com_Printf(CON_CHANNEL_SERVER, "You must log in with 'rcon login <password>' before using 'rcon'.\n");
            }
        }
        else
        {
            Com_Printf(CON_CHANNEL_SERVER, "The server must set 'rcon_password' for clients to use 'rcon'.\n");
        }
        Com_EndRedirect();
    }
}


void SV_VoicePacket(netadr_t from, msg_t *msg)
{
    client_t *ClientByAddress; // [esp+0h] [ebp-Ch]
    int qport; // [esp+4h] [ebp-8h]

    qport = MSG_ReadShort(msg);
    ClientByAddress = SV_FindClientByAddress(from, qport);
    if (ClientByAddress && ClientByAddress->header.state != CS_ZOMBIE)
    {
        ClientByAddress->lastPacketTime = svs.time;
        if (ClientByAddress->header.state >= CS_ACTIVE)
        {
            if (!ClientByAddress->gentity)
                MyAssertHandler(".\\server_mp\\sv_main_mp.cpp", 534, 0, "%s", "cl->gentity");
            SV_UserVoice(ClientByAddress, msg);
        }
        else
        {
            SV_PreGameUserVoice(ClientByAddress, msg);
        }
    }
}

