#include "network.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

NetworkManager::NetworkManager(int localShardId, Shard& owner)
    : localShardId(localShardId), serverFd(-1), running(false), networkDelayMs(0), dispatcher(owner) {} // 传递 Shard 引用给 MessageDispatcher

NetworkManager::~NetworkManager() {
    stop();
}

bool NetworkManager::loadConfig(const std::string& configPath) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        std::cerr << "[NetworkManager] failed to open config: " << configPath << std::endl;
        return false;
    }

    shardEndpoints.clear();
    networkDelayMs = 0;

    std::string line;
    while (std::getline(file, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (line.empty()) continue;

        if (line.rfind("delay=", 0) == 0) {
            try {
                networkDelayMs = std::max(0, std::stoi(line.substr(6)));
            } catch (const std::exception&) {
                std::cerr << "[NetworkManager] invalid delay in config: " << line << std::endl;
                return false;
            }
            continue;
        }

        size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) continue;
        std::string shardName = line.substr(0, equalPos);
        std::string address = line.substr(equalPos + 1);

        if (shardName.rfind("shard", 0) != 0) continue;

        int shardId = -1;
        try {
            shardId = std::stoi(shardName.substr(5));
        } catch (const std::exception&) {
            continue;
        }

        size_t colonPos = address.rfind(':');
        if (colonPos == std::string::npos) continue;

        Endpoint endpoint;
        endpoint.ip = address.substr(0, colonPos);
        try {
            endpoint.port = static_cast<uint16_t>(std::stoi(address.substr(colonPos + 1)));
        } catch (const std::exception&) {
            std::cerr << "[NetworkManager] invalid port: " << line << std::endl;
            return false;
        }

        shardEndpoints[shardId] = endpoint;
    }

    if (shardEndpoints.find(localShardId) == shardEndpoints.end()) {
        std::cerr << "[NetworkManager] local shard endpoint not found, shard=" << localShardId << std::endl;
        return false;
    }

    return true;
}

bool NetworkManager::start() {
    if (running.load()) return true;

    auto it = shardEndpoints.find(localShardId);
    if (it == shardEndpoints.end()) {
        std::cerr << "[NetworkManager] cannot start, endpoint not configured for shard " << localShardId << std::endl;
        return false;
    }

    serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        std::cerr << "[NetworkManager] socket create failed" << std::endl;
        return false;
    }

    int enable = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(it->second.port);
    if (inet_pton(AF_INET, it->second.ip.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "[NetworkManager] invalid local ip: " << it->second.ip << std::endl;
        close(serverFd);
        serverFd = -1;
        return false;
    }

    if (::bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[NetworkManager] bind failed on " << it->second.ip << ":" << it->second.port << std::endl;
        close(serverFd);
        serverFd = -1;
        return false;
    }

    if (listen(serverFd, 64) < 0) {
        std::cerr << "[NetworkManager] listen failed" << std::endl;
        close(serverFd);
        serverFd = -1;
        return false;
    }

    running.store(true);
    listenerThread = std::thread(&NetworkManager::acceptLoop, this);
    return true;
}

void NetworkManager::stop() {
    bool wasRunning = running.exchange(false);
    if (!wasRunning) return;

    if (serverFd >= 0) {
        shutdown(serverFd, SHUT_RDWR);
        close(serverFd);
        serverFd = -1;
    }

    if (listenerThread.joinable()) {
        listenerThread.join();
    }
}

void NetworkManager::acceptLoop() {
    while (running.load()) {
        sockaddr_in peerAddr;
        socklen_t peerLen = sizeof(peerAddr);
        int clientFd = accept(serverFd, reinterpret_cast<sockaddr*>(&peerAddr), &peerLen);
        if (clientFd < 0) {
            if (running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            continue;
        }

        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return runningThreads < maxSendingThreads; });
        runningThreads++;
        lock.unlock();

        std::thread([this, clientFd]() {
            handleConnection(clientFd);

            std::unique_lock<std::mutex> lock(mtx);
            runningThreads--;
            cv.notify_one(); // 通知有线程槽位空出来
        }).detach();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // // 线程数量给个上限或者线程池
        // std::thread(&NetworkManager::handleConnection, this, clientFd).detach(); // 为每一个连接创建一个线程处理
    }
}

void NetworkManager::handleConnection(int clientFd) {
    uint32_t netPayloadSize = 0;
    if (!recvAll(clientFd, reinterpret_cast<char*>(&netPayloadSize), sizeof(netPayloadSize))) {
        close(clientFd);
        return;
    }

    uint32_t payloadSize = ntohl(netPayloadSize);
    if (payloadSize == 0 || payloadSize > 16 * 1024 * 1024) {
        close(clientFd);
        return;
    }

    std::string payload(payloadSize, '\0');
    if (!recvAll(clientFd, &payload[0], payloadSize)) {
        close(clientFd);
        return;
    }

    Message message;
    if (deserializeMessagePayload(payload, message)) {
        dispatcher.dispatch(message);

    } else {
        std::cerr << "[NetworkManager] failed to parse message payload" << std::endl;
    }

    close(clientFd);
}

bool NetworkManager::sendAll(int fd, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool NetworkManager::recvAll(int fd, char* buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        ssize_t n = recv(fd, buffer + received, length - received, 0);
        if (n <= 0) return false;
        received += static_cast<size_t>(n);
    }
    return true;
}

bool NetworkManager::sendMessage(Message* message, int maxRetries, int retryDelayMs) {
    auto endpointIt = shardEndpoints.find(message->dstShardId);
    if (endpointIt == shardEndpoints.end()) {
        std::cerr << "[NetworkManager] destination shard not configured: " << message->dstShardId << std::endl;
        return false;
    }

    if (networkDelayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(networkDelayMs));
    }

    const Endpoint& endpoint = endpointIt->second;
    std::string payload = serializeMessagePayload(message);
    uint32_t payloadSize = htonl(static_cast<uint32_t>(payload.size()));

    for (int attempt = 1; attempt <= maxRetries; ++attempt) {
        int clientFd = socket(AF_INET, SOCK_STREAM, 0);
        if (clientFd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
            continue;
        }

        sockaddr_in serverAddr;
        std::memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(endpoint.port);
        if (inet_pton(AF_INET, endpoint.ip.c_str(), &serverAddr.sin_addr) <= 0) {
            close(clientFd);
            return false;
        }

        if (connect(clientFd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == 0) {
            std::lock_guard<std::mutex> lock(sendMutex);
            bool ok = sendAll(clientFd, reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize)) &&
                      sendAll(clientFd, payload.data(), payload.size());
            close(clientFd);
            return ok;
        }

        close(clientFd);
        std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
    }

    std::cerr << "[NetworkManager] failed to connect shard " << message->dstShardId
              << " after " << maxRetries << " retries" << std::endl;
    return false;
}

void NetworkManager::registerHandler(MessageType type, MessageDispatcher::Handler handler) {
    dispatcher.registerHandler(type, handler);
}

void NetworkManager::registerCustomHandler(int type, MessageDispatcher::Handler handler) {
    dispatcher.registerCustomHandler(type, handler);
}
