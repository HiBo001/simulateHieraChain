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
static int connectRaw(const Endpoint& endpoint) {
    int fd=socket(AF_INET,SOCK_STREAM,0); check(fd>=0,"raw connect socket");
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(endpoint.port);
    check(!connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)),"raw connect");
    return fd;
}
static std::string compressedBody(const std::string& payload) {
    std::string body(compressBound(payload.size())+4,'\0'); uLongf length=body.size()-4;
    check(compress2(reinterpret_cast<Bytef*>(body.data()+4),&length,
        reinterpret_cast<const Bytef*>(payload.data()),payload.size(),Z_BEST_SPEED)==Z_OK,"fixture compression");
    uint32_t raw=htonl(payload.size()); memcpy(body.data(),&raw,4); body.resize(length+4);
    return body;
}
static std::string compressedFrame(const std::string& body) {
    uint32_t header=htonl(detail::COMPRESSED_NETWORK_FRAME|static_cast<uint32_t>(body.size()));
    return std::string(reinterpret_cast<const char*>(&header),4)+body;
}

static void compressedBoundariesAndInvalidBodies() {
    using detail::FrameResult;
    std::string encoded,decoded;
    const auto ordinary=json{{"id",1}}.dump();
    check(detail::encodeNetworkFrame(ordinary,encoded)==FrameResult::Ok && encoded==frame({{"id",1}}),
        "ordinary wire format changed");
    check(detail::networkMessageSize(detail::MAX_NETWORK_MESSAGE)==FrameResult::Ok,"exact logical maximum rejected");
    check(detail::networkMessageSize(detail::MAX_NETWORK_MESSAGE+1)==FrameResult::Oversized,"logical maximum not bounded");
    check(detail::networkMessageSize(0)==FrameResult::Invalid,"empty logical message accepted");
    std::string wireBoundary(detail::MAX_NETWORK_FRAME,'x');
    check(detail::encodeNetworkFrame(wireBoundary,encoded)==FrameResult::Ok,"exact legacy wire maximum rejected");
    uint32_t header; memcpy(&header,encoded.data(),4);
    check(ntohl(header)==detail::MAX_NETWORK_FRAME,"legacy wire boundary was unnecessarily compressed");
    wireBoundary.clear(); encoded.clear();

    auto body=compressedBody(ordinary);
    check(detail::decodeNetworkPayload(body,true,decoded)==FrameResult::Ok && decoded==ordinary,"valid zlib body rejected");
    auto expectInvalid=[&](std::string candidate,const std::string& reason) {
        decoded="old output";
        check(detail::decodeNetworkPayload(candidate,true,decoded)==FrameResult::Invalid && decoded.empty(),reason);
    };
    expectInvalid(body.substr(0,body.size()-1),"truncated compressed stream accepted");
    expectInvalid(body+"trailing","trailing compressed data accepted");
    expectInvalid(body+body.substr(4),"concatenated zlib streams accepted");
    auto corrupt=body; corrupt.back()^=1; expectInvalid(corrupt,"corrupt compressed checksum accepted");
    auto wrongSize=body; uint32_t size=htonl(ordinary.size()+1); memcpy(wrongSize.data(),&size,4);
    expectInvalid(wrongSize,"short decompressed result accepted");
    size=htonl(ordinary.size()-1); memcpy(wrongSize.data(),&size,4);
    expectInvalid(wrongSize,"decompressed result beyond declared size accepted");
    expectInvalid(body.substr(0,4)+"unrecognized-codec","non-zlib codec accepted");
    expectInvalid(std::string(2,'\0'), "truncated raw-size prefix accepted");
    size=htonl(0); memcpy(wrongSize.data(),&size,4); expectInvalid(wrongSize,"zero logical size accepted");
    size=htonl(detail::MAX_NETWORK_MESSAGE+1); memcpy(wrongSize.data(),&size,4);
    check(detail::decodeNetworkPayload(wrongSize,true,decoded)==FrameResult::Oversized && decoded.empty(),
        "oversized declared message allocated or accepted");

    // A raw payload beyond the wire cap cannot sneak through by choosing a
    // compression stream that does not fit. The helper is independent of JSON
    // and allows this boundary check without allocating a 128 MiB document.
    std::string incompressible(detail::MAX_NETWORK_FRAME+1024*1024,'\0'); uint32_t random=42;
    for (auto& byte:incompressible) { random^=random<<13; random^=random>>17; random^=random<<5; byte=char(random&255); }
    check(detail::encodeNetworkFrame(incompressible,encoded)==FrameResult::Oversized && encoded.empty(),
        "oversized compressed wire message accepted");
    std::cout<<"PASS unchanged plain frames, bounded compression, exact lengths, truncation, checksums and codec rejection\n";
}

static void largeSignedRecoveryAndMalformedPeerIsolation() {
    auto ctx=std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)>(
        EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519,nullptr),EVP_PKEY_CTX_free);
    check(bool(ctx) && EVP_PKEY_keygen_init(ctx.get())==1,"large recovery key init");
    EVP_PKEY* raw=nullptr; check(EVP_PKEY_keygen(ctx.get(),&raw)==1,"large recovery key generation");
    Key signing(raw,EVP_PKEY_free);
    Network sender,receiver; std::atomic<int> valid{0}; std::atomic<bool> verified{false};
    receiver.start({"127.0.0.1",0},[&](json message){
        if (message.contains("signature")) {
            verified=verify(message,signing) && message.at("body").at("snapshot").get_ref<const std::string&>().size()==40*1024*1024;
        }
        ++valid;
    });
    sender.start({"127.0.0.1",0},[](json){}); Endpoint target{"127.0.0.1",receiver.localPort()};
    auto large=sign({{"snapshot",std::string(40*1024*1024,'s')},{"kind","NEW_VIEW"}},signing);
    // The logical document exceeds both the former frame cap and the per-peer
    // buffering cap. A single atomic compressed enqueue must still succeed.
    check(sender.send(target,large,0),"large recovery envelope refused");
    check(sender.send(target,{{"id",2}},0),"ordinary message after recovery refused");
    check(waitFor([&]{return valid==2;},10000) && verified,"large recovery failed original Ed25519 verification");
    check(sender.sent==2 && sender.connect_attempts==1 && sender.connections_reused==1,"large frame changed TCP reuse/counters");
    check(sender.bytes_sent<1024*1024 && sender.oversized_errors==0,"large recovery not wire-bounded/compressed");

    auto healthyAfterFailure=[&](const std::string& bad,bool oversized) {
        auto failed=receiver.failed.load(),errors=oversized?receiver.oversized_errors.load():receiver.parse_errors.load();
        int fd=connectRaw(target); writeAll(fd,bad); close(fd);
        check(waitFor([&]{return receiver.failed==failed+1;}),"malformed frame not counted exactly once");
        check((oversized?receiver.oversized_errors.load():receiver.parse_errors.load())==errors+1,"malformed frame error misclassified");
        int before=valid; check(sender.send(target,{{"healthy",before}},0),"healthy enqueue after malicious frame");
        check(waitFor([&]{return valid==before+1;}),"malformed peer blocked healthy persistent connection");
    };
    auto body=compressedBody(json{{"id",3}}.dump());
    auto incompleteFrame=compressedFrame(body); incompleteFrame.pop_back();
    healthyAfterFailure(incompleteFrame,false);
    healthyAfterFailure(incompleteFrame.substr(0,2),false);
    healthyAfterFailure(compressedFrame(body.substr(0,body.size()-1)),false);
    auto mismatch=body; uint32_t rawSize=htonl(999); memcpy(mismatch.data(),&rawSize,4);
    healthyAfterFailure(compressedFrame(mismatch),false);
    healthyAfterFailure(compressedFrame(body+"extra"),false);
    healthyAfterFailure(compressedFrame(body.substr(0,4)+"unsupported-codec"),false);
    // Only the header and raw-size prefix arrive; reject before waiting for the
    // rest of the advertised wire frame or allocating its declared output.
    uint32_t declaredWire=htonl(detail::COMPRESSED_NETWORK_FRAME|1024),declaredRaw=htonl(detail::MAX_NETWORK_MESSAGE+1);
    std::string tooBig(reinterpret_cast<const char*>(&declaredWire),4); tooBig.append(reinterpret_cast<const char*>(&declaredRaw),4);
    healthyAfterFailure(tooBig,true);
    declaredWire=htonl(detail::COMPRESSED_NETWORK_FRAME|uint32_t(detail::MAX_NETWORK_FRAME+1));
    healthyAfterFailure(std::string(reinterpret_cast<const char*>(&declaredWire),4),true);
    declaredWire=htonl(detail::COMPRESSED_NETWORK_FRAME|3U);
    healthyAfterFailure(std::string(reinterpret_cast<const char*>(&declaredWire),4),false);

    sender.stop(); receiver.stop();
    check(sender.buffered_bytes==0,"compressed recovery leaked reservations");
    std::cout<<"PASS 40 MiB signed recovery, persistent TCP, malformed compressed peer isolation and accurate errors\n";
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

static void deadlineWaitRoundingAndWake() {
    using namespace std::chrono;
    check(detail::networkPollTimeout(nanoseconds(-1))==0,"overdue deadline must not sleep");
    check(detail::networkPollTimeout(Clock::duration::zero())==0,"due deadline must not sleep");
    check(detail::networkPollTimeout(nanoseconds(1))==1,"positive sub-ms deadline busy-spins");
    check(detail::networkPollTimeout(microseconds(999))==1,"sub-ms deadline was rounded down");
    check(detail::networkPollTimeout(milliseconds(1))==1,"exact ms deadline changed");
    check(detail::networkPollTimeout(microseconds(1001))==2,"fractional ms deadline was rounded down");
    check(detail::networkPollTimeout(microseconds(9999))==10,"near-cap deadline was rounded down");
    check(detail::networkPollTimeout(milliseconds(10))==10,"exact poll cap changed");
    check(detail::networkPollTimeout(Clock::duration::max())==10,"large deadline overflowed poll cap");

    Network sender, receiver;
    std::mutex mutex; std::vector<int> ids; std::vector<double> elapsed;
    receiver.start({"127.0.0.1",0},[&](json value){
        std::lock_guard<std::mutex> lock(mutex);
        ids.push_back(value.at("id"));
        elapsed.push_back(millis(Clock::now())-value.at("begin_ms").get<double>());
    });
    sender.start({"127.0.0.1",0},[](json){});
    Endpoint target{"127.0.0.1",receiver.localPort()};
    auto enqueue=[&](int id,int delay){
        check(sender.send(target,{{"id",id},{"begin_ms",millis(Clock::now())}},delay),"deadline enqueue");
    };
    enqueue(1,200);
    // Let the worker enter its timed poll before adding an earlier deadline.
    std::this_thread::sleep_for(milliseconds(5));
    enqueue(2,0); enqueue(3,1);
    check(waitFor([&]{return receiver.received==3;}),"staggered deadline messages missing");
    {
        std::lock_guard<std::mutex> lock(mutex);
        check(ids==std::vector<int>({2,3,1}),"new earlier deadline failed to interrupt timed wait");
        check(elapsed.at(1)>=1 && elapsed.at(2)>=200,"message was released before its configured delay");
    }
    sender.stop(); receiver.stop();
    std::cout<<"PASS deadline ceiling, poll cap, wakeup and no early delayed delivery\n";
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
        deadlineWaitRoundingAndWake();
        slowPeerBackpressureAndProgressDeadline(); timedQueueGlobalByteBudget();
        compressedBoundariesAndInvalidBodies(); largeSignedRecoveryAndMalformedPeerIsolation();
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
    return 0;
}
