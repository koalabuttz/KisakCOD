// TEST-ONLY stand-in for <qcommon/net_chan_mp.h>: the engine address types.
// Sizes and field order are copied from the real header (netadr_t is 20 bytes,
// msg_t is 40 bytes) and asserted in test_net.cpp.  Not part of any build.
#pragma once

#include <universal/q_shared.h>

enum netadrtype_t
{
    NA_BOT,
    NA_BAD,
    NA_LOOPBACK,
    NA_BROADCAST,
    NA_IP,
    NA_IPX,
    NA_BROADCAST_IPX,
};

enum netsrc_t : int
{
    NS_CLIENT1 = 0x0,
    NS_SERVER = 0x1,
    NS_MAXCLIENTS = 0x1,
    NS_PACKET = 0x2,
};

struct netadr_t
{
    netadrtype_t type;
    unsigned char ip[4];
    unsigned short port;
    unsigned char ipx[10];
};

struct msg_t
{
    int overflowed;
    int readOnly;
    uint8_t *data;
    uint8_t *splitData;
    int maxsize;
    int cursize;
    int splitSize;
    int readcount;
    int bit;
    int lastEntityRef;
};

const char *NET_AdrToString(netadr_t a);
