// Real command/dvar/NET ownership. The registered marker/error callbacks are
// explicit test commands; UDP uses a local peer and production engine sockets.
#include <linux/client/diagnostic_host.h>
#include <linux/client/discovery_session.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks=0,failures=0;
std::vector<std::string> executed;
cmd_function_s marker{},drop{};
void Check(bool value,const char *name)
{
    ++checks;failures+=!value;std::printf("%s: %s\n",value?"PASS":"FAIL",name);
}
void Mark() { if(Cmd_Argc()==2)executed.emplace_back(Cmd_Argv(1)); }
void Drop() { Com_Error(ERR_DROP,"explicit lifecycle error fixture\n"); }

class Peer {
public:
    Peer() {
        socket_=socket(AF_INET,SOCK_DGRAM,0);
        if(socket_<0)throw std::runtime_error("peer socket");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(bind(socket_,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0) {
            close(socket_);throw std::runtime_error("peer bind");
        }
        socklen_t size=sizeof(address);
        if(getsockname(socket_,reinterpret_cast<sockaddr*>(&address),&size)!=0) {
            close(socket_);throw std::runtime_error("peer address");
        }
        endpoint_="127.0.0.1:"+std::to_string(ntohs(address.sin_port));
    }
    ~Peer() { close(socket_); }
    const std::string &Endpoint() const { return endpoint_; }
    std::string Query() {
        pollfd descriptor{socket_,POLLIN,0};
        if(poll(&descriptor,1,1000)!=1)throw std::runtime_error("discovery query deadline");
        char buffer[256];socklen_t size=sizeof(client_);
        const auto count=recvfrom(socket_,buffer,sizeof(buffer),0,reinterpret_cast<sockaddr*>(&client_),&size);
        if(count<0)throw std::runtime_error("query receive");
        std::string query(buffer,count);
        const std::string prefix="\xff\xff\xff\xffgetinfo ";
        if(!query.starts_with(prefix))throw std::runtime_error("unexpected discovery query");
        query.erase(0,prefix.size());
        if(!query.empty() && query.back()=='\0')query.pop_back();
        if(query.size()!=16)throw std::runtime_error("unexpected challenge size");
        return query;
    }
    void Send(const std::string &text) {
        if(sendto(socket_,text.data(),text.size(),0,reinterpret_cast<sockaddr*>(&client_),sizeof(client_))!=static_cast<ssize_t>(text.size()))
            throw std::runtime_error("peer send");
    }
    static std::string Reply(const std::string &challenge) {
        return "\xff\xff\xff\xffinfoResponse\n\\challenge\\"+challenge;
    }
private:
    int socket_=-1;
    sockaddr_in client_{};
    std::string endpoint_;
};
}

int main()
{
    using kisak::client::DiscoverySession;
    using kisak::client::DiscoveryState;
    using kisak::client::Runtime;
    try {
        for(const char *endpoint:{"", "localhost:1234", "127.0.0.1:0", "127.0.0.1:65536", "127.0.0.1:5x"}) {
            bool rejected=false;
            try { DiscoverySession bad(endpoint,100); }catch(const std::invalid_argument&) { rejected=true; }
            Check(rejected,"invalid endpoint rejected without runtime startup");
        }
        for(unsigned timeout:{0u,10001u}) {
            bool rejected=false;
            try { DiscoverySession bad("127.0.0.1:1234",timeout); }catch(const std::invalid_argument&) { rejected=true; }
            Check(rejected,"invalid discovery timeout rejected");
        }
        Peer peer;
        for(int cycle=0;cycle<2;++cycle) {
            Runtime runtime;
            Check(&Runtime::Current()==&runtime && cmd_args.nesting==-1,"shared runtime owns clean command tokenizer");
            bool duplicate=false;
            try { Runtime other; }catch(const std::logic_error&) { duplicate=true; }
            Check(duplicate,"duplicate runtime rejected without disturbing owner");
            Check(Dvar_FindVar("lifecycle_owned")==nullptr,"previous runtime dvar storage is gone");
            const auto *dvar=Dvar_RegisterString("lifecycle_owned","initial",DVAR_NOFLAG,"lifecycle fixture");
            Dvar_SetString(dvar,"changed allocated string");
            Cmd_AddCommandInternal("lifecycle_mark",Mark,&marker);
            Cmd_AddCommandInternal("lifecycle_drop",Drop,&drop);
            executed.clear();
            runtime.Queue("lifecycle_mark first; wait 2; lifecycle_mark second\n");
            runtime.Tick();Check(executed==std::vector<std::string>{"first"},"real command buffer pauses remaining commands");
            runtime.Tick();Check(executed.size()==1,"wait spans a later host tick");
            runtime.Tick();Check(executed==std::vector<std::string>({"first","second"}),"queued command resumes on expected tick");
            runtime.Queue("lifecycle_unknown invalid; lifecycle_mark \"quoted;argument\"\n");runtime.Tick();
            Check(executed.back()=="quoted;argument" && cmd_args.nesting==-1,"unknown command returns and quote-aware execution continues");

            DiscoverySession idle(peer.Endpoint(),100);idle.Cancel();idle.Poll();
            bool cancelledStart=false;try { idle.Start(); }catch(const std::logic_error&) { cancelledStart=true; }
            Check(idle.State()==DiscoveryState::cancelled && cancelledStart,"idle cancellation forbids starting cancelled transaction");
            DiscoverySession cancelled(peer.Endpoint(),1000);
            Check(cancelled.Start(),"real engine discovery send succeeds");const auto oldChallenge=peer.Query();
            bool startedTwice=false;try { cancelled.Start(); }catch(const std::logic_error&) { startedTwice=true; }
            Check(startedTwice,"pending transaction cannot be started twice");
            const auto queued=Peer::Reply(oldChallenge);peer.Send(queued);cancelled.Cancel();cancelled.Poll();
            uint8_t buffer[65507];msg_t message{};message.data=buffer;message.maxsize=sizeof(buffer);netadr_t from{};
            Check(cancelled.State()==DiscoveryState::cancelled && NET_GetClientPacket(&from,&message) &&
                message.cursize==static_cast<int>(queued.size()),"cancelled poll leaves socket datagrams for its next owner");

            DiscoverySession budget(peer.Endpoint(),1000);budget.Start();const auto challenge=peer.Query();
            for(int packet=0;packet<8;++packet)peer.Send("invalid packet");
            peer.Send(Peer::Reply(challenge));budget.Poll();
            Check(budget.State()==DiscoveryState::pending && budget.Discarded()==8,"one discovery pump consumes at most eight datagrams");
            budget.Poll();Check(budget.State()==DiscoveryState::received,"next pump consumes remaining valid response");
            DiscoverySession fresh(peer.Endpoint(),1000);fresh.Start();const auto freshChallenge=peer.Query();
            peer.Send(Peer::Reply(challenge));peer.Send(Peer::Reply(freshChallenge));fresh.Poll();
            Check(fresh.State()==DiscoveryState::received && fresh.Discarded()==1,"new transaction rejects stale prior challenge");
            DiscoverySession timeout(peer.Endpoint(),10);timeout.Start();peer.Query();NET_Sleep(20);timeout.Poll();
            Check(timeout.State()==DiscoveryState::timeout && timeout.MillisecondsRemaining()==0,"discovery deadline terminates without a reply");
            runtime.RestartNetwork();
            DiscoverySession restarted(peer.Endpoint(),1000);restarted.Start();peer.Send(Peer::Reply(peer.Query()));restarted.Poll();
            Check(restarted.State()==DiscoveryState::received,"same runtime discovers after network restart");

            bool dropped=false;
            runtime.Queue("lifecycle_drop; lifecycle_mark stale_after_error\n");
            try { runtime.Tick(); }catch(const std::runtime_error&) { dropped=true; }
            runtime.Queue("lifecycle_mark after_error\n");runtime.Tick();
            Check(dropped && cmd_args.nesting==-1 && executed.back()=="after_error" &&
                std::find(executed.begin(),executed.end(),"stale_after_error")==executed.end(),
                "engine error unwinds tokenizer and discards command backlog");
            runtime.Queue("wait 100; lifecycle_mark stale_after_shutdown\n");runtime.Tick();
        }
        bool absent=false;try { Runtime::Current(); }catch(const std::logic_error&) { absent=true; }
        Check(absent && Cmd_FindCommand("lifecycle_mark")==nullptr,"shutdown releases owner and caller-owned command registry");
    } catch(const std::exception &error) {
        std::fprintf(stderr,"runtime lifecycle failed: %s\n",error.what());++failures;
    }
    std::printf("-- %d checks, %d failures --\n",checks,failures);
    return failures?1:0;
}
