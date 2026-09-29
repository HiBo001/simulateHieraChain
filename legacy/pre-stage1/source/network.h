#ifndef NETWORK_H
#define NETWORK_H

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include "message.h"

class Shard;

class NetworkManager {
public:
    struct Endpoint {
        std::string ip;
        uint16_t port;
    };

public:
    explicit NetworkManager(int localShardId, Shard& owner);
    ~NetworkManager();

    bool loadConfig(const std::string& configPath);
    bool start();
    void stop();
    bool sendMessage(Message* message, int maxRetries = 5, int retryDelayMs = 200);
    void registerHandler(MessageType type, MessageDispatcher::Handler handler);
    void registerCustomHandler(int type, MessageDispatcher::Handler handler);

private:
    void acceptLoop();
    void handleConnection(int clientFd);
    bool sendAll(int fd, const char* data, size_t length);
    bool recvAll(int fd, char* buffer, size_t length);

private:
    int localShardId;
    int serverFd;
    std::atomic<bool> running;
    std::thread listenerThread;

    int networkDelayMs;
    std::map<int, Endpoint> shardEndpoints;
    std::mutex sendMutex;
    MessageDispatcher dispatcher;

    static const int maxSendingThreads = 3;
    int runningThreads = 0;
    std::mutex mtx;
    std::condition_variable cv;
};

#endif // NETWORK_H
