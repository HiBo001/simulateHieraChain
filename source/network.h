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
        std::string buffer;
        size_t offset = 0;
        Clock::time_point deadline;
    };
    int listener = -1;
    int wake[2] = {-1, -1};
    std::atomic<bool> active{false};
    std::thread worker;
    std::mutex mutex;
    std::priority_queue<Task> pending;
    std::map<int, Connection> connections;
    uint64_t serial = 0;
    std::function<void(json)> receive;
    std::ofstream trace;
    static constexpr size_t MAX_FRAME = 16 * 1024 * 1024;
    static constexpr size_t MAX_PENDING = 100000;
    static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
    static sockaddr_in address(const Endpoint& e) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(e.port);
        if (inet_pton(AF_INET, e.host.c_str(), &a.sin_addr) != 1) throw std::runtime_error("expected IPv4 address");
        return a;
    }
    void openTask(const Task& t) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { failed++; return; }
        nonblock(fd);
        int yes = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        auto a = address(t.to);
        int rc = connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        if (rc < 0 && errno != EINPROGRESS) { close(fd); failed++; return; }
        connections.emplace(fd, Connection{true, t.frame, 0, Clock::now() + std::chrono::seconds(3)});
        if (trace.is_open()) trace << json{{"enqueue_ms",t.enqueued},{"due_ms",millis(t.due)},
            {"release_ms",millis(Clock::now())},{"delay_ms",t.delay},{"dst_port",t.to.port}}.dump() << '\n';
    }
    void loop() {
        while (active) {
            int timeout = 10;
            {
                std::lock_guard<std::mutex> lock(mutex);
                while (!pending.empty() && pending.top().due <= Clock::now() && connections.size() < 1024) {
                    auto t = pending.top(); pending.pop(); openTask(t);
                }
                if (!pending.empty() && connections.size() < 1024)
                    timeout = std::max(0, std::min(10, int(std::chrono::duration_cast<std::chrono::milliseconds>(pending.top().due - Clock::now()).count())));
            }
            std::vector<pollfd> fds{{listener,POLLIN,0},{wake[0],POLLIN,0}};
            for (const auto& [fd,c] : connections) fds.push_back({fd,short(c.outgoing ? POLLOUT : POLLIN),0});
            int rc = poll(fds.data(), fds.size(), timeout);
            if (rc < 0 && errno != EINTR) break;
            if (fds[1].revents & POLLIN) { char b[256]; while (read(wake[0],b,sizeof(b)) > 0) {} }
            if (fds[0].revents & POLLIN) {
                for (int count = 0; count < 64 && connections.size() < 2048; ++count) {
                    int fd = accept(listener,nullptr,nullptr);
                    if (fd < 0) break;
                    nonblock(fd);
                    connections.emplace(fd, Connection{false,{},0,Clock::now()+std::chrono::seconds(3)});
                }
            }
            for (size_t i = 2; i < fds.size(); ++i) {
                int fd = fds[i].fd; auto it = connections.find(fd);
                if (it == connections.end()) continue;
                auto& c = it->second; bool done = Clock::now() > c.deadline;
                auto events = fds[i].revents;
                if (events & (POLLERR|POLLNVAL)) done = true;
                if (!done && c.outgoing && (events & POLLOUT)) {
                    int error = 0; socklen_t len = sizeof(error); getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len);
                    if (error) done = true;
                    else {
                        auto n = ::send(fd,c.buffer.data()+c.offset,c.buffer.size()-c.offset,0);
                        if (n > 0) { c.offset += n; bytes_sent += n; }
                        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) done = true;
                        if (c.offset == c.buffer.size()) { sent++; done = true; }
                    }
                }
                if (!done && !c.outgoing && (events & (POLLIN|POLLHUP))) {
                    char b[65536]; auto n = recv(fd,b,sizeof(b),0);
                    if (n > 0) { c.buffer.append(b,n); bytes_received += n; }
                    else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) done = true;
                    if (c.buffer.size() >= 4) {
                        uint32_t length; memcpy(&length,c.buffer.data(),4); length = ntohl(length);
                        if (!length || length > MAX_FRAME) { done = true; failed++; }
                        else if (c.buffer.size() >= length+4) {
                            try { receive(json::parse(c.buffer.substr(4,length))); received++; }
                            catch (...) { failed++; }
                            done = true;
                        }
                    }
                }
                if (done) {
                    if (c.outgoing && c.offset != c.buffer.size()) failed++;
                    close(fd); connections.erase(it);
                }
            }
        }
        for (const auto& [fd,c] : connections) { (void)c; close(fd); }
        connections.clear();
        if (trace.is_open()) trace.flush();
    }
public:
    std::atomic<uint64_t> sent{0}, received{0}, failed{0}, bytes_sent{0}, bytes_received{0};
    int localPort() const { sockaddr_in a{}; socklen_t n = sizeof(a); getsockname(listener,reinterpret_cast<sockaddr*>(&a),&n); return ntohs(a.sin_port); }
    void start(const Endpoint& local, std::function<void(json)> handler, const std::string& tracePath = "") {
        receive = std::move(handler);
        listener = socket(AF_INET,SOCK_STREAM,0);
        if (listener < 0) throw std::runtime_error("socket failed");
        int yes = 1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        auto a = address(local);
        if (bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof(a)) || listen(listener,256)) {
            close(listener); listener = -1; throw std::runtime_error("bind/listen failed on port " + std::to_string(local.port));
        }
        nonblock(listener);
        if (pipe(wake)) throw std::runtime_error("pipe failed");
        nonblock(wake[0]); nonblock(wake[1]);
        if (!tracePath.empty()) trace.open(tracePath);
        active = true; worker = std::thread([this]{loop();});
    }
    bool send(const Endpoint& to, const json& message, int delay) {
        auto payload = message.dump();
        if (payload.size() > MAX_FRAME) { failed++; return false; }
        uint32_t len = htonl(payload.size());
        std::string frame(reinterpret_cast<char*>(&len),4); frame += payload;
        auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (pending.size() >= MAX_PENDING) { failed++; return false; }
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
    }
    ~Network() { stop(); }
};
}
