// net_internal.h -- declaration surface for the POSIX (Linux) network layer.
//
// Counterpart of src/win32/win_net.h.  src/linux/net.cpp implements everything
// declared here; nothing else in the engine is allowed to guess at these
// signatures, because the consumer translation units that need them
// (qcommon/net_chan_mp.cpp, server_mp/*, client_mp/*) currently reach for
// <win32/win_net.h> and <win32/win_local.h> by path.  Point those includes at
// this header (or include it from the Linux stand-in for win_local.h) instead
// of re-declaring the symbols.
//
// Only `struct sockaddr` is forward-declared on purpose: this header must stay
// includable from engine TUs that have not pulled in the system socket headers.

#pragma once

#include <universal/q_shared.h>

#ifdef KISAK_MP
#include <qcommon/net_chan_mp.h>
#elif defined(KISAK_SP)
#include <qcommon/net_chan.h>
#else
#error The Linux network layer requires KISAK_MP or KISAK_SP
#endif

struct sockaddr;

// ---- subsystem lifetime ----------------------------------------------------

void NET_Init();
void NET_Shutdown();
void NET_Restart();
void NET_Config(bool enableNetworking);

// ---- diagnostics -----------------------------------------------------------

// strerror() text for the current errno, with the numeric code appended.
const char *NET_ErrorString();

// Prints every IPv4 address discovered by NET_GetLocalAddress().
void Sys_ShowIP();

// ---- UDP transport --------------------------------------------------------

// Sleeps up to msec, waking early when a network socket becomes readable.
void NET_Sleep(int msec);

// Blocks-free port of Sys_GetPacket(): returns qtrue and fills net_from /
// net_message when one datagram was consumed, qfalse when the socket is
// empty.  Never blocks.
qboolean Sys_GetPacket(netadr_t *net_from, msg_t *net_message);

// Returns non-zero when the datagram was handed to the kernel (or silently
// dropped as non-fatal), zero when the send genuinely failed.
char Sys_SendPacket(int length, uint8_t *data, netadr_t to);

// ---- loopback local-auth test guard ---------------------------------------
//
// sv_loopbackTestAuth skips Steam ticket verification for local testing only.
// It is off by default. The per-packet guard requires both explicit loopback
// configuration and a live loopback-bound socket; it does not skip the server's
// protocol, challenge, password or capacity checks.

// Registers the startup-only "sv_loopbackTestAuth" cvar and freezes its value
// for the rest of the process.  NET_Init() calls this after the engine's
// start-up dvars exist and before the first bind, so the server handshake
// path sees a settled answer.  Calling it again never changes the answer: a
// NET_Shutdown()/NET_Init() cycle cannot turn the bypass on or off.
void NET_InitLoopbackAuthTest();

// The start-up decision alone: true when sv_loopbackTestAuth was on when the
// process captured it.  This says nothing about whether a bypass is currently
// usable - no socket may be bound and the cvar may have been changed since -
// so it must not be used to authorise anything.  NET_CanBypassLoopbackAuth()
// is the authorisation question.
bool NET_LoopbackAuthTestEnabled();

// Per-packet authorisation for the unauthenticated handshake path.  True only
// when all of the following hold: the frozen start-up answer is on, the cvar
// still reads on (an engine-internal Dvar_Set that turns it off revokes the
// bypass immediately), the server is dedicated, net_ip currently names literal
// 127.0.0.1, the UDP socket is really bound there according to getsockname(),
// and the sender is exactly
// 127.0.0.1 with type NA_IP.  The sender's port is free.  Everything else -
// LAN, public, broadcast, NA_LOOPBACK/NA_BOT/IPX pseudo-types - is rejected.
bool NET_CanBypassLoopbackAuth(netadr_t from);

// ---- address conversion ---------------------------------------------------

void NetadrToSockadr(netadr_t *a, struct sockaddr *s);
void SockadrToNetadr(struct sockaddr *s, netadr_t *a);

// Resolves "1.2.3.4", legacy short forms and host names to an IPv4 sockaddr.
qboolean Sys_StringToSockaddr(const char *s, struct sockaddr *sadr);

// Resolves a host string straight to the engine address type.
qboolean Sys_StringToAdr(const char *s, netadr_t *a);

// ---- discovery -------------------------------------------------------------

// Fills the local IPv4 table from getifaddrs(); returns 0 on success, an errno
// value otherwise.
int NET_GetLocalAddress();

bool Sys_IsLANAddress(netadr_t adr);
bool Sys_IsLANAddress_IgnoreSubnet(netadr_t adr);

// ---- debug socket ----------------------------------------------------------
//
// win_net_debug.cpp is Winsock-only and is not part of the Linux target, but
// qcommon/common.cpp calls these unconditionally from Com_Init and Com_Close.
// On this platform they are no-ops: the remote debug socket is not ported.

void NET_InitDebug();
void NET_ShutdownDebug();
void NET_RestartDebug();

// ---- stream sockets --------------------------------------------------------

// type 0 binds (server side), type 1 connects (client side).  Returns the file
// descriptor, or -1 (0xFFFFFFFF) on failure.
//
// NOTE: this differs from win_net.cpp, which returned 0 on failure.  On POSIX
// -1 is the invalid socket and 0 is a perfectly valid descriptor (stdin closed),
// so callers must test `!= -1` rather than truthiness.
uint32_t NET_TCPIPSocket(const char *net_interface, int port, int type);
