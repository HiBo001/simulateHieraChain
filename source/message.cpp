#include "message.h"

#include <iostream>
#include <sstream>

using namespace std;

std::string serializeMessagePayload(const Message& msg) {
    std::ostringstream oss;
    oss << msg.type << "|"
        << msg.srcShardId << "|"
        << msg.dstShardId << "|"
        << msg.body.size() << "|"
        << msg.body;
    return oss.str();
}

bool deserializeMessagePayload(const std::string& payload, Message& outMessage) {
    size_t p1 = payload.find('|');
    if (p1 == string::npos) return false;
    size_t p2 = payload.find('|', p1 + 1);
    if (p2 == string::npos) return false;
    size_t p3 = payload.find('|', p2 + 1);
    if (p3 == string::npos) return false;
    size_t p4 = payload.find('|', p3 + 1);
    if (p4 == string::npos) return false;

    try {
        outMessage.type = std::stoi(payload.substr(0, p1));
        outMessage.srcShardId = std::stoi(payload.substr(p1 + 1, p2 - p1 - 1));
        outMessage.dstShardId = std::stoi(payload.substr(p2 + 1, p3 - p2 - 1));
        int bodyLength = std::stoi(payload.substr(p3 + 1, p4 - p3 - 1));
        if (bodyLength < 0) return false;
        if (payload.size() - (p4 + 1) != static_cast<size_t>(bodyLength)) return false;
        outMessage.body = payload.substr(p4 + 1, bodyLength);
    } catch (const std::exception&) {
        return false;
    }

    return true;
}

std::string messageTypeToString(int type) {
    switch (static_cast<MessageType>(type)) {
        case MessageType::MSG_CROSS_TX_REQUEST:
            return "MSG_CROSS_TX_REQUEST";
        case MessageType::MSG_CROSS_TX_VOTE:
            return "MSG_CROSS_TX_VOTE";
        case MessageType::MSG_CROSS_TX_COMMIT:
            return "MSG_CROSS_TX_COMMIT";
        case MessageType::MSG_HEARTBEAT:
            return "MSG_HEARTBEAT";
        case MessageType::MSG_CUSTOM:
            return "MSG_CUSTOM";
        default:
            return "MSG_UNKNOWN(" + std::to_string(type) + ")";
    }
}

MessageDispatcher::MessageDispatcher() {
    registerBuiltInDefaultHandlers();
    fallbackHandler = [this](const Message& msg) { defaultLogHandler(msg); };
}

void MessageDispatcher::registerBuiltInDefaultHandlers() {
    registerHandler(MessageType::MSG_CROSS_TX_REQUEST,
                    [this](const Message& msg) { defaultLogHandler(msg); });
    registerHandler(MessageType::MSG_CROSS_TX_VOTE,
                    [this](const Message& msg) { defaultLogHandler(msg); });
    registerHandler(MessageType::MSG_CROSS_TX_COMMIT,
                    [this](const Message& msg) { defaultLogHandler(msg); });
    registerHandler(MessageType::MSG_HEARTBEAT,
                    [this](const Message& msg) { defaultLogHandler(msg); });
    registerHandler(MessageType::MSG_CUSTOM,
                    [this](const Message& msg) { defaultLogHandler(msg); });
}

void MessageDispatcher::registerHandler(MessageType type, Handler handler) {
    registerCustomHandler(static_cast<int>(type), handler);
}

void MessageDispatcher::registerCustomHandler(int type, Handler handler) {
    std::lock_guard<std::mutex> lock(handlersMutex);
    handlers[type] = handler;
}

void MessageDispatcher::setDefaultHandler(Handler handler) {
    std::lock_guard<std::mutex> lock(handlersMutex);
    fallbackHandler = handler;
}

void MessageDispatcher::dispatch(const Message& message) const {
    Handler handlerToRun;
    {
        std::lock_guard<std::mutex> lock(handlersMutex);
        auto it = handlers.find(message.type);
        if (it != handlers.end()) {
            handlerToRun = it->second;
        } else {
            handlerToRun = fallbackHandler;
        }
    }

    if (handlerToRun) {
        handlerToRun(message);
    }
}

void MessageDispatcher::defaultLogHandler(const Message& message) const {
    std::cout << "[MessageDispatcher] receive "
              << messageTypeToString(message.type)
              << ", src=" << message.srcShardId
              << ", dst=" << message.dstShardId
              << ", body=" << message.body << std::endl;
}
