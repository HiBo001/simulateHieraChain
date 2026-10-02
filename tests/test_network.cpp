#include "network.h"
#include <algorithm>
#include <csignal>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

using namespace arbor;

static void check(bool value, const std::string& reason) {
    if (!value) throw std::runtime_error(reason);
}
template<class F> static bool waitFor(F predicate, int timeoutMs=3000) {
    auto end=Clock::now()+std::chrono::milliseconds(timeoutMs);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (Clock::now()<end);
    return predicate();
}
struct Listener {
    int fd=-1;
    Endpoint endpoint{"127.0.0.1",0};
    Listener() {
        fd=socket(AF_INET,SOCK_STREAM,0); check(fd>=0,"raw socket");
        int size=4096; setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size));
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        check(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)),"raw bind");
        check(!listen(fd,16),"raw listen");
        socklen_t n=sizeof(a); getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n);
        endpoint.port=ntohs(a.sin_port);
    }
    int acceptOne() {
        pollfd p{fd,POLLIN,0}; check(poll(&p,1,3000)>0,"raw accept timeout");
        int accepted=accept(fd,nullptr,nullptr); check(accepted>=0,"raw accept"); return accepted;
    }
    ~Listener() { if(fd>=0) close(fd); }
};
static std::string frame(const json& message) {
    auto bytes=message.dump(); uint32_t n=htonl(bytes.size());
    return std::string(reinterpret_cast<const char*>(&n),4)+bytes;
}
static void writeAll(int fd, const std::string& bytes) {
    size_t offset=0;
    while(offset<bytes.size()) {
        auto n=::send(fd,bytes.data()+offset,bytes.size()-offset,0);
        check(n>0,"raw write failed"); offset+=n;
    }
}

static void persistentAndFragmentedFrames() {
    Network sender, receiver;
    std::mutex mutex; std::vector<int> ids;
    receiver.start({"127.0.0.1",0},[&](json value){std::lock_guard<std::mutex> lock(mutex);ids.push_back(value.at("id"));});
    sender.start({"127.0.0.1",0},[](json){});
    Endpoint target{"127.0.0.1",receiver.localPort()};
    for(int i=0;i<256;++i) check(sender.send(target,{{"id",i}},0),"persistent enqueue");
    check(waitFor([&]{return receiver.received==256;}),"persistent frames missing");
    check(sender.connect_attempts==1 && sender.connections_reused==255,"connection was not reused");
    {std::lock_guard<std::mutex> lock(mutex);for(int i=0;i<256;++i) check(ids.at(i)==i,"TCP order changed");}
    int fd=socket(AF_INET,SOCK_STREAM,0); check(fd>=0,"fragment socket");
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(target.port);
    check(!connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)),"fragment connect");
    std::string bytes=frame({{"id",256}})+frame({{"id",257}})+frame({{"id",258}});
    // Split a length prefix and a payload, then coalesce the following frames.
    writeAll(fd,bytes.substr(0,2)); std::this_thread::sleep_for(std::chrono::milliseconds(15));
    writeAll(fd,bytes.substr(2,5)); std::this_thread::sleep_for(std::chrono::milliseconds(15));
    writeAll(fd,bytes.substr(7)); close(fd);
    check(waitFor([&]{return receiver.received==259;}),"fragmented/coalesced frames missing");
    check(sender.buffered_bytes==0,"written bytes remained reserved");
    sender.stop(); receiver.stop();
    check(sender.active_connections==0 && sender.buffered_bytes==0,"stop leaked transport state");
    std::cout<<"PASS persistent connection, ordered multi-frame and fragmented frame delivery\n";
}

static void reconnectAfterPeerCloses() {
    Network sender, receiver; std::atomic<int> arrivals{0};
    receiver.start({"127.0.0.1",0},[&](json){++arrivals;});
    Endpoint target{"127.0.0.1",receiver.localPort()};
    sender.start({"127.0.0.1",0},[](json){});
    check(sender.send(target,{{"id",1}},0),"first reconnect enqueue");
    check(waitFor([&]{return arrivals==1;}),"first reconnect delivery");
    receiver.stop();
    check(waitFor([&]{return sender.active_connections==0;}),"closed peer was not discarded");
    receiver.start(target,[&](json){++arrivals;});
    check(sender.send(target,{{"id",2}},0),"second reconnect enqueue");
    check(waitFor([&]{return arrivals==2;}),"delivery after reconnect");
    check(sender.connect_attempts==2,"reconnect did not create exactly one replacement connection");
    sender.stop();receiver.stop();
    std::cout<<"PASS closed peer detection and subsequent protocol retry reconnect\n";
}

static void independentDelays() {
    Network sender, slow, healthy;
    std::atomic<double> slowMs{-1}, healthyMs{-1}; auto begin=Clock::now();
    slow.start({"127.0.0.1",0},[&](json){slowMs=millis(Clock::now())-millis(begin);});
    healthy.start({"127.0.0.1",0},[&](json){healthyMs=millis(Clock::now())-millis(begin);});
    sender.start({"127.0.0.1",0},[](json){});
    check(sender.send({"127.0.0.1",slow.localPort()},{{"id",1}},200),"timed enqueue");
    check(sender.send({"127.0.0.1",healthy.localPort()},{{"id",2}},0),"independent enqueue");
    check(waitFor([&]{return healthyMs>=0 && slowMs>=0;}),"timed messages missing");
    check(healthyMs<150 && slowMs>=195,"one destination's delay blocked an independent destination");
    sender.stop();slow.stop();healthy.stop();
    std::cout<<"PASS independent destination delay scheduling\n";
}

static void slowPeerBackpressureAndProgressDeadline() {
    Listener stalled; Network sender, healthy; std::atomic<int> arrivals{0};
    healthy.start({"127.0.0.1",0},[&](json){++arrivals;});
    sender.start({"127.0.0.1",0},[](json){});
    json large={{"blob",std::string(1024*1024,'x')}};
    check(sender.send(stalled.endpoint,large,0),"initial stalled enqueue");
    int fd=stalled.acceptOne();
    uint64_t rejected=0, maximum=0;
    for(int i=0;i<70;++i) {
        if(!sender.send(stalled.endpoint,large,0)) ++rejected;
        maximum=std::max(maximum,sender.buffered_bytes.load());
        if(i==25) {
            auto begin=Clock::now();
            check(sender.send({"127.0.0.1",healthy.localPort()},{{"id",1}},0),"healthy enqueue during stalled traffic");
            check(waitFor([&]{return arrivals==1;},1000),"stalled peer blocked healthy delivery");
            check(Clock::now()-begin<std::chrono::seconds(1),"healthy destination was stalled");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    check(rejected>0 && sender.queue_errors>0,"slow peer was allowed unlimited buffered bytes");
    check(maximum<=32ULL*1024*1024,"per-destination reservation cap was exceeded");
    check(sender.timeout_errors>0,"new enqueues kept a non-progressing socket alive indefinitely");
    close(fd); sender.stop(); healthy.stop();
    check(sender.buffered_bytes==0,"stop failed to release queued/in-flight bytes");
    std::cout<<"PASS stalled peer byte cap and write-progress timeout, healthy delivery independent\n";
}

static void timedQueueGlobalByteBudget() {
    Network sender; sender.start({"127.0.0.1",0},[](json){});
    json large={{"blob",std::string(1024*1024,'x')}}; int refused=0;
    // Each destination stays below its individual cap. Future release times
    // retain all messages in pending, so only the global byte cap can reject.
    for(int peer=0;peer<8;++peer) for(int frame=0;frame<20;++frame)
        if(!sender.send({"127.0.0.1",30000+peer},large,60000)) ++refused;
    check(refused>0 && sender.queue_errors>0,"timed pending queue exceeded global byte budget");
    check(sender.buffered_bytes<=128ULL*1024*1024,"global byte reservation exceeded cap");
    check(sender.connect_attempts==0,"timed tasks were released before due time");
    sender.stop();check(sender.buffered_bytes==0 && sender.queued()==0,"stop retained timed tasks");
    check(!sender.send({"127.0.0.1",30000},{{"id",1}},0),"stopped sender accepted a message");
    std::cout<<"PASS global timed-queue byte budget and shutdown reservation release\n";
}

int main() {
    signal(SIGPIPE,SIG_IGN);
    try {
        persistentAndFragmentedFrames(); reconnectAfterPeerCloses(); independentDelays();
        slowPeerBackpressureAndProgressDeadline(); timedQueueGlobalByteBudget();
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
    return 0;
}
