#include "message.h"
#include "shard.h"  // 添加这一行

#include <iostream>
#include <sstream>

using namespace std;

std::string serializeMessagePayload(Message* msg) {
    std::ostringstream oss;
    oss << msg->type << "|" 
        << msg->srcShardId << "|" 
        << msg->dstShardId << "|" 
        << msg->txs.size();
    
    for (auto& tx : msg->txs) {
        oss << "|" << tx.serialize(); // 调用 transaction 的序列化
    }
    return oss.str();
}

bool deserializeMessagePayload(const std::string& payload, Message& outMessage) {
    if (payload.empty()) return false;

    std::vector<std::string> tokens;
    std::string token;
    std::istringstream f(payload);
    while (std::getline(f, token, '|')) {
        tokens.push_back(token);
    }

    if (tokens.size() < 4) return false;

    try {
        outMessage.type = std::stoi(tokens[0]);
        outMessage.srcShardId = std::stoi(tokens[1]);
        outMessage.dstShardId = std::stoi(tokens[2]);
        
        size_t txCount = std::stoul(tokens[3]);
        outMessage.txs.clear();

        if (tokens.size() != 4 + txCount) return false;

        for (size_t i = 0; i < txCount; ++i) {
            outMessage.txs.push_back(transaction::deserialize(tokens[4 + i]));
        }
    } catch (...) {
        return false;
    }
    return true;
}

std::string messageTypeToString(int type) {
    switch (static_cast<MessageType>(type)) {
        case MessageType::CROSS_SHARD_TX_REQUEST:
            return "CROSS_SHARD_TX_REQUEST";
        case MessageType::CROSS_SHARD_TX_COMMIT_MSG:
            return "CROSS_SHARD_TX_COMMIT_MSG";
        default:
            return "MSG_UNKNOWN(" + std::to_string(type) + ")";
    }
}

MessageDispatcher::MessageDispatcher(Shard& owner):m_owner(owner) {
    registerBuiltInDefaultHandlers();
}

void MessageDispatcher::registerBuiltInDefaultHandlers() {
    registerHandler(MessageType::CROSS_SHARD_TX_REQUEST,
                    [this](const Message& msg) { crossShardTxsHandler(msg); });
    registerHandler(MessageType::CROSS_SHARD_TX_COMMIT_MSG,
                    [this](const Message& msg) { crossShardCommittedMsgHandler(msg); });
}

void MessageDispatcher::registerHandler(MessageType type, Handler handler) {
    registerCustomHandler(static_cast<int>(type), handler);
}

void MessageDispatcher::registerCustomHandler(int type, Handler handler) {
    std::lock_guard<std::mutex> lock(handlersMutex);
    handlers[type] = handler;
}

// void MessageDispatcher::setDefaultHandler(Handler handler) {
//     std::lock_guard<std::mutex> lock(handlersMutex);
//     fallbackHandler = handler;
// }

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

void MessageDispatcher::crossShardTxsHandler(const Message& message) const{
    
    int sourceShardId = message.srcShardId;
    cout << "收到了来自分片 " << sourceShardId << "的跨片交易任务....." << endl;
    
    auto txs = message.txs;
    for (auto tx : txs) {
        tx.type = 1.5; // 标记为需要立即处理的跨片交易
    }
    m_owner.enqueueTransactions(txs);
}

void MessageDispatcher::crossShardCommittedMsgHandler(const Message& message) const{
    cout << "收到了来自下层的跨片交易提交信息....." << endl;
    


}


void MessageDispatcher::defaultLogHandler(const Message& message) const {
    // std::cout << "[MessageDispatcher] receive "
    //           << messageTypeToString(message.type)
    //           << ", src=" << message.srcShardId
    //           << ", dst=" << message.dstShardId
    //           << ", body=" << message.body << std::endl;
}
