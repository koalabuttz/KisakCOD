#include "discovery_session.h"

#include <arpa/inet.h>
#include <sys/random.h>
#include <cerrno>
#include <cstdio>
#include <stdexcept>

namespace kisak::client {
namespace {
bool Decimal(std::string_view text,unsigned maximum,unsigned &value)
{
    if(text.empty())return false;
    value=0;
    for(char c:text) {
        if(c<'0' || c>'9' || value>(maximum-unsigned(c-'0'))/10)return false;
        value=value*10+unsigned(c-'0');
    }
    return value>0 && value<=maximum;
}

bool ValidResponse(std::string_view packet,std::string_view challenge)
{
    constexpr std::string_view prefix="\xff\xff\xff\xffinfoResponse\n";
    if(!packet.starts_with(prefix))return false;
    packet.remove_prefix(prefix.size());
    if(!packet.empty() && packet.back()=='\0')packet.remove_suffix(1);
    if(packet.find('\0')!=packet.npos)return false;
    bool matched=false;
    while(!packet.empty()) {
        if(packet.front()!='\\')return false;
        packet.remove_prefix(1);
        size_t separator=packet.find('\\');
        if(!separator || separator==packet.npos)return false;
        auto key=packet.substr(0,separator);
        packet.remove_prefix(separator+1);
        separator=packet.find('\\');
        auto value=packet.substr(0,separator);
        if(key=="challenge") {
            if(matched || value!=challenge)return false;
            matched=true;
        }
        if(separator==packet.npos)break;
        packet.remove_prefix(separator);
    }
    return matched;
}
}

DiscoverySession::DiscoverySession(std::string_view endpoint,unsigned timeoutMilliseconds)
    : timeoutMilliseconds_(timeoutMilliseconds)
{
    if(timeoutMilliseconds==0 || timeoutMilliseconds>10000)
        throw std::invalid_argument("Discovery timeout must be 1..10000 milliseconds");
    const auto colon=endpoint.find(':');
    unsigned port=0;
    sockaddr_in address{};
    address.sin_family=AF_INET;
    const std::string host(endpoint.substr(0,colon));
    if(colon==endpoint.npos || !Decimal(endpoint.substr(colon+1),65535,port) ||
       host.find('\0')!=host.npos || inet_pton(AF_INET,host.c_str(),&address.sin_addr)!=1)
        throw std::invalid_argument("Invalid IPv4:port");
    address.sin_port=htons(port);
    SockadrToNetadr(reinterpret_cast<sockaddr*>(&address),&peer_);
}

bool DiscoverySession::Start()
{
    if(state_!=DiscoveryState::idle)
        throw std::logic_error("Discovery session has already started or been cancelled");
    unsigned char random[8];
    ssize_t generated;
    do { generated=getrandom(random,sizeof(random),0); } while(generated<0 && errno==EINTR);
    if(generated!=sizeof(random)) {
        state_=DiscoveryState::error;detail_="challenge entropy unavailable";return false;
    }
    char challenge[17];
    for(unsigned i=0;i<sizeof(random);++i)std::snprintf(challenge+i*2,3,"%02x",random[i]);
    challenge_=challenge;
    const std::string query="getinfo "+challenge_;
    if(!NET_OutOfBandPrint(NS_CLIENT1,peer_,query.c_str())) {
        state_=DiscoveryState::error;detail_="Engine discovery send failed";return false;
    }
    deadline_=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMilliseconds_);
    state_=DiscoveryState::pending;
    return true;
}

void DiscoverySession::Poll()
{
    if(state_!=DiscoveryState::pending)return;
    // One pump remains bounded even under continuous unrelated/invalid traffic.
    for(unsigned packet=0;packet<8;++packet) {
        if(std::chrono::steady_clock::now()>=deadline_) {
            state_=DiscoveryState::timeout;return;
        }
        // Full UDP payload capacity prevents NET_GetClientPacket's oversized-
        // datagram discard loop from bypassing this frontend's receive budget.
        uint8_t response[65507];
        msg_t message{};
        message.data=response;message.maxsize=sizeof(response);
        netadr_t from{};
        if(!NET_GetClientPacket(&from,&message))return;
        if(!NET_CompareAdr(from,peer_))continue;
        if(message.cursize>2048 || message.cursize<0 || !ValidResponse(
            std::string_view(reinterpret_cast<char*>(response),message.cursize),challenge_)) {
            ++discarded_;continue;
        }
        state_=DiscoveryState::received;return;
    }
}

int DiscoverySession::MillisecondsRemaining() const noexcept
{
    if(state_!=DiscoveryState::pending)return 0;
    const auto remaining=deadline_-std::chrono::steady_clock::now();
    if(remaining<=decltype(remaining)::zero())return 0;
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
}

void DiscoverySession::Cancel() noexcept
{
    if(state_==DiscoveryState::idle || state_==DiscoveryState::pending)
        state_=DiscoveryState::cancelled;
}
} // namespace kisak::client
