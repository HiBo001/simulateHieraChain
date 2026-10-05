#pragma once
#include "common.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <string_view>
#include <zlib.h>

namespace arbor {
namespace detail {
// The wire budget remains small even when recovery contains a large snapshot.
// Ordinary frames retain their original format; the high bit selects a zlib
// body prefixed by its uncompressed JSON size in network byte order.
inline constexpr size_t MAX_NETWORK_FRAME = 16 * 1024 * 1024;
inline constexpr size_t MAX_NETWORK_MESSAGE = 128 * 1024 * 1024;
inline constexpr uint32_t COMPRESSED_NETWORK_FRAME = 0x80000000U;
enum class FrameResult { Ok, Oversized, Invalid };
inline FrameResult networkMessageSize(size_t length) {
    if (!length) return FrameResult::Invalid;
    return length > MAX_NETWORK_MESSAGE ? FrameResult::Oversized : FrameResult::Ok;
}
inline FrameResult encodeNetworkFrame(std::string_view payload, std::string& frame) {
    frame.clear();
    auto sizeResult=networkMessageSize(payload.size());
    if (sizeResult!=FrameResult::Ok) return sizeResult;
    if (payload.size()<=MAX_NETWORK_FRAME) {
        uint32_t length=htonl(static_cast<uint32_t>(payload.size()));
        frame.assign(reinterpret_cast<const char*>(&length),4); frame.append(payload.data(),payload.size());
        return FrameResult::Ok;
    }
    // Compress into the fixed wire budget, rather than allocating compressBound
    // for a potentially 128 MiB message or enqueueing fragments piecemeal.
    frame.resize(MAX_NETWORK_FRAME+4);
    uLongf compressedLength=MAX_NETWORK_FRAME-4;
    int result=compress2(reinterpret_cast<Bytef*>(frame.data()+8),&compressedLength,
        reinterpret_cast<const Bytef*>(payload.data()),static_cast<uLong>(payload.size()),Z_BEST_SPEED);
    if (result!=Z_OK) {
        frame.clear();
        return result==Z_BUF_ERROR ? FrameResult::Oversized : FrameResult::Invalid;
    }
    uint32_t length=htonl(COMPRESSED_NETWORK_FRAME|static_cast<uint32_t>(compressedLength+4));
    uint32_t rawLength=htonl(static_cast<uint32_t>(payload.size()));
    memcpy(frame.data(),&length,4); memcpy(frame.data()+4,&rawLength,4);
    // Keep pending-task memory proportional to the actual reserved wire bytes;
    // resize alone would retain a 16 MiB allocation for every small result.
    std::string compact(frame.data(),compressedLength+8); frame.swap(compact);
    return FrameResult::Ok;
}
inline FrameResult compressedNetworkMessageSize(std::string_view body, size_t& rawLength) {
    if (body.size()<4) return FrameResult::Invalid;
    uint32_t length; memcpy(&length,body.data(),4); rawLength=ntohl(length);
    return networkMessageSize(rawLength);
}
inline FrameResult decodeNetworkPayload(std::string_view body, bool compressed, std::string& payload) {
    payload.clear();
    if (body.empty()) return FrameResult::Invalid;
    if (body.size()>MAX_NETWORK_FRAME) return FrameResult::Oversized;
    if (!compressed) { payload.assign(body.data(),body.size()); return FrameResult::Ok; }
    size_t rawLength=0;
    auto sizeResult=compressedNetworkMessageSize(body,rawLength);
    if (sizeResult!=FrameResult::Ok) return sizeResult;
    z_stream stream{};
    stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(body.data()+4));
    stream.avail_in=static_cast<uInt>(body.size()-4);
    if (inflateInit(&stream)!=Z_OK) return FrameResult::Invalid;
    struct Inflater { z_stream* stream; ~Inflater() { inflateEnd(stream); } } cleanup{&stream};
    // Inflate in bounded chunks. An untrusted declared size never causes a large
    // upfront allocation; actual output may not exceed that size or the cap.
    payload.reserve(std::min(rawLength,size_t(1024*1024)));
    char chunk[65536];
    for (;;) {
        stream.next_out=reinterpret_cast<Bytef*>(chunk); stream.avail_out=sizeof(chunk);
        auto before=stream.avail_in;
        int result=inflate(&stream,Z_NO_FLUSH);
        size_t produced=sizeof(chunk)-stream.avail_out;
        if (produced>rawLength-payload.size()) { payload.clear(); return FrameResult::Invalid; }
        payload.append(chunk,produced);
        if (result==Z_STREAM_END) {
            if (payload.size()==rawLength && stream.avail_in==0) return FrameResult::Ok;
            payload.clear(); return FrameResult::Invalid;
        }
        if (result!=Z_OK || (!produced && before==stream.avail_in)) {
            payload.clear(); return FrameResult::Invalid;
        }
    }
}
inline int networkPollTimeout(Clock::duration remaining) {
    if (remaining <= Clock::duration::zero()) return 0;
    // poll accepts whole milliseconds. Truncating a positive remainder below
    // 1 ms produces poll(..., 0) and burns CPU until the message becomes due.
    // Keep the existing 10 ms cap; a new earlier task interrupts via wake.
    if (remaining >= std::chrono::milliseconds(10)) return 10;
    return int(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
}
}
struct Endpoint { std::string host; int port; };

// One nonblocking I/O thread. Timed messages never sleep in the PBFT thread,
// and connect/write to an unavailable peer cannot stall other destinations.
class Network {
    struct Task {
        Clock::time_point due;
        uint64_t serial;
        Endpoint to;
        std::string frame;
        double enqueued;
        int delay;
        bool operator<(const Task& x) const { return due == x.due ? serial > x.serial : due > x.due; }
    };
    struct Connection {
        bool outgoing;
        Endpoint peer;
        std::string buffer;
        size_t offset = 0;
        uint64_t frames = 0;
        Clock::time_point deadline;
        std::deque<size_t> frameEnds;
    };
    int listener = -1;
    int wake[2] = {-1, -1};
    std::atomic<bool> active{false};
    std::thread worker;
    std::mutex mutex;
    std::priority_queue<Task> pending;
    // Reserved bytes include timed tasks and bytes not yet written to sockets.
    // A slow destination cannot move an unbounded backlog out of pending.
    std::map<std::string,size_t> reservedByPeer;
    std::map<int, Connection> connections;
    std::map<std::string,int> outgoing;
    uint64_t serial = 0;
    std::function<void(json)> receive;
    std::ofstream trace;
    static constexpr size_t MAX_PENDING = 100000;
    static constexpr size_t MAX_BUFFERED_BYTES = 128 * 1024 * 1024;
    static constexpr size_t MAX_PEER_BUFFERED_BYTES = 32 * 1024 * 1024;
    static constexpr size_t MAX_CONNECTIONS = 2048;
    static constexpr size_t MAX_INCOMING = 1024;
    static std::string key(const Endpoint& e) { return e.host + ":" + std::to_string(e.port); }
    static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
    static sockaddr_in address(const Endpoint& e) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(e.port);
        if (inet_pton(AF_INET, e.host.c_str(), &a.sin_addr) != 1) throw std::runtime_error("expected IPv4 address");
        return a;
    }
    void releaseBytesLocked(const Endpoint& peer, size_t bytes) {
        auto it=reservedByPeer.find(key(peer));
        if (it!=reservedByPeer.end()) {
            it->second-=bytes;
            if (!it->second) reservedByPeer.erase(it);
        }
        buffered_bytes-=bytes;
    }
    void releaseBytes(const Endpoint& peer, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex); releaseBytesLocked(peer,bytes);
    }
    void dropTask(const Task& t) {
        failed++; releaseBytesLocked(t.to,t.frame.size());
    }
    void openTask(const Task& t) {
        auto destination=key(t.to);
        auto existing=outgoing.find(destination);
        if (existing!=outgoing.end()) {
            auto& c=connections.at(existing->second);
            bool wasIdle=c.offset==c.buffer.size();
            // Reclaim the sent prefix before appending. The reservation limit
            // bounds unsent bytes, so retaining all sent prefixes would defeat it.
            if (c.offset) {
                c.buffer.erase(0,c.offset);
                for (auto& end:c.frameEnds) end-=c.offset;
                c.offset=0;
            }
            c.buffer+=t.frame; c.frames++; connections_reused++;
            c.frameEnds.push_back(c.buffer.size());
            if (wasIdle) c.deadline=Clock::now()+std::chrono::seconds(3);
            if (trace.is_open()) trace << json{{"enqueue_ms",t.enqueued},{"due_ms",millis(t.due)},
                {"release_ms",millis(Clock::now())},{"delay_ms",t.delay},{"dst_port",t.to.port}}.dump() << '\n';
            return;
        }
        if (connections.size()>=MAX_CONNECTIONS) { queue_errors++; dropTask(t); return; }
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { socket_errors++; dropTask(t); return; }
        nonblock(fd);
        int yes = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        sockaddr_in a{};
        try { a=address(t.to); } catch (...) { close(fd); connect_errors++; dropTask(t); return; }
        connect_attempts++;
        int rc = connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        if (rc < 0 && errno != EINPROGRESS) { close(fd); connect_errors++; dropTask(t); return; }
        connections.emplace(fd, Connection{true,t.to,t.frame,0,1,Clock::now()+std::chrono::seconds(3),{}});
        connections.at(fd).frameEnds.push_back(t.frame.size());
        outgoing[destination]=fd;
        active_connections=connections.size();
        if (trace.is_open()) trace << json{{"enqueue_ms",t.enqueued},{"due_ms",millis(t.due)},
            {"release_ms",millis(Clock::now())},{"delay_ms",t.delay},{"dst_port",t.to.port}}.dump() << '\n';
    }
    void loop() {
        while (active) {
            int timeout = 10;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (size_t released=0; released<256 && !pending.empty() &&
                     pending.top().due <= Clock::now(); ++released) {
                    auto t = pending.top(); pending.pop(); openTask(t);
                }
                if (!pending.empty())
                    timeout = detail::networkPollTimeout(pending.top().due - Clock::now());
            }
            std::vector<pollfd> fds{{listener,POLLIN,0},{wake[0],POLLIN,0}};
            for (const auto& [fd,c] : connections)
                fds.push_back({fd,short(c.outgoing ? (POLLIN | (c.offset<c.buffer.size()?POLLOUT:0)) : POLLIN),0});
            int rc = poll(fds.data(), fds.size(), timeout);
            if (rc < 0 && errno != EINTR) break;
            if (fds[1].revents & POLLIN) { char b[256]; while (read(wake[0],b,sizeof(b)) > 0) {} }
            if (fds[0].revents & POLLIN) {
                for (int count = 0; count < 64 && connections.size() < MAX_CONNECTIONS &&
                     connections.size()-outgoing.size() < MAX_INCOMING; ++count) {
                    int fd = accept(listener,nullptr,nullptr);
                    if (fd < 0) break;
                    nonblock(fd);
                    connections.emplace(fd, Connection{false,{},{},0,0,Clock::now()+std::chrono::seconds(30),{}});
                    active_connections=connections.size();
                }
            }
            for (size_t i = 2; i < fds.size(); ++i) {
                int fd = fds[i].fd; auto it = connections.find(fd);
                if (it == connections.end()) continue;
                auto& c = it->second; bool done = Clock::now() > c.deadline;
                if (done && c.outgoing && c.offset<c.buffer.size()) timeout_errors++;
                auto events = fds[i].revents;
                if (events & (POLLERR|POLLNVAL)) { if (c.outgoing && !done) connect_errors++; done = true; }
                if (!done && c.outgoing && c.offset<c.buffer.size() && (events & POLLOUT)) {
                    int error = 0; socklen_t len = sizeof(error); getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len);
                    if (error) { connect_errors++; done = true; }
                    else {
                        auto n = ::send(fd,c.buffer.data()+c.offset,c.buffer.size()-c.offset,0);
                        if (n > 0) { c.offset += n; bytes_sent += n; releaseBytes(c.peer,n);
                            c.deadline=Clock::now()+std::chrono::seconds(3);
                            while (!c.frameEnds.empty() && c.frameEnds.front()<=c.offset) {
                                c.frameEnds.pop_front(); c.frames--; sent++;
                            }
                        }
                        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { write_errors++; done = true; }
                        if (c.offset == c.buffer.size()) {
                            c.buffer.clear(); c.offset=0; c.frames=0;
                            c.deadline=Clock::now()+std::chrono::seconds(30);
                        }
                    }
                }
                if (!done && c.outgoing && (events & (POLLIN|POLLHUP))) {
                    char b[1]; auto n=recv(fd,b,sizeof(b),0);
                    if (n==0 || (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) done=true;
                }
                if (!done && !c.outgoing && (events & (POLLIN|POLLHUP))) {
                    char b[65536]; auto n = recv(fd,b,sizeof(b),0);
                    bool rejectedFrame=false;
                    if (n > 0) { c.buffer.append(b,n); bytes_received += n;
                        c.deadline=Clock::now()+std::chrono::seconds(30); }
                    else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) done = true;
                    while (c.buffer.size() >= 4) {
                        uint32_t header; memcpy(&header,c.buffer.data(),4); header=ntohl(header);
                        bool compressed=(header&detail::COMPRESSED_NETWORK_FRAME)!=0;
                        size_t length=header&~detail::COMPRESSED_NETWORK_FRAME;
                        if (!length || length>detail::MAX_NETWORK_FRAME || (compressed && length<5)) {
                            done=true; rejectedFrame=true; failed++;
                            if (length>detail::MAX_NETWORK_FRAME) oversized_errors++; else parse_errors++;
                            break;
                        }
                        // Reject a declared oversized logical body as soon as its
                        // size prefix arrives, before buffering the rest of it.
                        if (compressed && c.buffer.size()>=8) {
                            size_t rawLength;
                            auto result=detail::compressedNetworkMessageSize(std::string_view(c.buffer).substr(4,4),rawLength);
                            if (result!=detail::FrameResult::Ok) {
                                done=true; rejectedFrame=true; failed++;
                                if (result==detail::FrameResult::Oversized) oversized_errors++; else parse_errors++;
                                break;
                            }
                        }
                        if (c.buffer.size() >= length+4) {
                            std::string payload;
                            auto result=detail::decodeNetworkPayload(std::string_view(c.buffer).substr(4,length),compressed,payload);
                            if (result!=detail::FrameResult::Ok) {
                                failed++;
                                if (result==detail::FrameResult::Oversized) oversized_errors++; else parse_errors++;
                                done=true; rejectedFrame=true; break;
                            }
                            try { receive(json::parse(payload)); received++; }
                            catch (...) { failed++; parse_errors++; }
                            c.buffer.erase(0,length+4);
                        } else break;
                    }
                    if (done && !rejectedFrame && !c.buffer.empty()) { failed++; parse_errors++; }
                }
                if (done) {
                    if (c.outgoing && c.offset != c.buffer.size()) {
                        failed+=c.frames; releaseBytes(c.peer,c.buffer.size()-c.offset);
                    }
                    if (c.outgoing) outgoing.erase(key(c.peer));
                    close(fd); connections.erase(it);
                    active_connections=connections.size();
                }
            }
        }
        active=false;
        for (const auto& [fd,c] : connections) {
            if (c.outgoing && c.offset<c.buffer.size()) {
                failed+=c.frames; releaseBytes(c.peer,c.buffer.size()-c.offset);
            }
            close(fd);
        }
        connections.clear();
        outgoing.clear();
        active_connections=0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            while (!pending.empty()) { dropTask(pending.top()); pending.pop(); }
        }
        if (trace.is_open()) trace.flush();
    }
public:
    std::atomic<uint64_t> sent{0}, received{0}, failed{0}, bytes_sent{0}, bytes_received{0};
    std::atomic<uint64_t> socket_errors{0}, connect_errors{0}, write_errors{0}, timeout_errors{0},
                          parse_errors{0}, oversized_errors{0}, queue_errors{0};
    std::atomic<uint64_t> buffered_bytes{0}, active_connections{0}, connect_attempts{0}, connections_reused{0};
    int localPort() const { sockaddr_in a{}; socklen_t n = sizeof(a); getsockname(listener,reinterpret_cast<sockaddr*>(&a),&n); return ntohs(a.sin_port); }
    void start(const Endpoint& local, std::function<void(json)> handler, const std::string& tracePath = "") {
        if (active || worker.joinable()) throw std::runtime_error("network already started");
        receive = std::move(handler);
        listener = socket(AF_INET,SOCK_STREAM,0);
        if (listener < 0) throw std::runtime_error("socket failed");
        int yes = 1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        sockaddr_in a{};
        try { a=address(local); } catch (...) { close(listener); listener=-1; throw; }
        if (bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof(a)) || listen(listener,256)) {
            close(listener); listener = -1; throw std::runtime_error("bind/listen failed on port " + std::to_string(local.port));
        }
        nonblock(listener);
        if (pipe(wake)) { close(listener); listener=-1; throw std::runtime_error("pipe failed"); }
        nonblock(wake[0]); nonblock(wake[1]);
        if (!tracePath.empty()) trace.open(tracePath);
        active = true; worker = std::thread([this]{loop();});
    }
    bool send(const Endpoint& to, const json& message, int delay) {
        auto payload = message.dump();
        std::string frame;
        auto result=detail::encodeNetworkFrame(payload,frame);
        if (result!=detail::FrameResult::Ok) {
            failed++;
            if (result==detail::FrameResult::Oversized) oversized_errors++; else parse_errors++;
            return false;
        }
        auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto destination=key(to);
            auto existing=reservedByPeer.find(destination);
            size_t peerBytes=existing==reservedByPeer.end()?0:existing->second;
            if (!active || pending.size() >= MAX_PENDING ||
                buffered_bytes.load()+frame.size()>MAX_BUFFERED_BYTES ||
                peerBytes+frame.size()>MAX_PEER_BUFFERED_BYTES) {
                failed++; queue_errors++; return false;
            }
            reservedByPeer[destination]=peerBytes+frame.size(); buffered_bytes+=frame.size();
            pending.push(Task{now+std::chrono::milliseconds(delay),serial++,to,std::move(frame),millis(now),delay});
        }
        char c = 0; (void)write(wake[1],&c,1); return true;
    }
    size_t queued() { std::lock_guard<std::mutex> lock(mutex); return pending.size(); }
    void stop() {
        active = false;
        if (worker.joinable()) worker.join();
        if (listener >= 0) { close(listener); listener = -1; }
        for (auto& fd : wake) if (fd >= 0) { close(fd); fd = -1; }
        if (trace.is_open()) trace.close();
    }
    ~Network() { stop(); }
};
}
