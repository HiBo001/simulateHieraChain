#pragma once
#include "common.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <thread>

namespace arbor {
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
    static constexpr size_t MAX_FRAME = 16 * 1024 * 1024;
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
                    timeout = std::max(0, std::min(10, int(std::chrono::duration_cast<std::chrono::milliseconds>(pending.top().due - Clock::now()).count())));
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
                    if (n > 0) { c.buffer.append(b,n); bytes_received += n;
                        c.deadline=Clock::now()+std::chrono::seconds(30); }
                    else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) done = true;
                    while (c.buffer.size() >= 4) {
                        uint32_t length; memcpy(&length,c.buffer.data(),4); length = ntohl(length);
                        if (!length || length > MAX_FRAME) { done = true; failed++; break; }
                        if (c.buffer.size() >= length+4) {
                            try { receive(json::parse(c.buffer.substr(4,length))); received++; }
                            catch (...) { failed++; parse_errors++; }
                            c.buffer.erase(0,length+4);
                        } else break;
                    }
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
        if (payload.size() > MAX_FRAME) { failed++; oversized_errors++; return false; }
        uint32_t len = htonl(payload.size());
        std::string frame(reinterpret_cast<char*>(&len),4); frame += payload;
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
