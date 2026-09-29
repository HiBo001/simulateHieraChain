#include "message.h"
#include "shard.h"  // 添加这一行

#include <iostream>
#include <sstream>

using namespace std;

// --- 序列化方法 ---
std::string serializeMessagePayload(Message* msg) {
    std::ostringstream oss;
    // 1. 序列化固定头部和新增字段
    oss << msg->type << "|" 
        << msg->srcShardId << "|" 
        << msg->dstShardId << "|"
        << msg->tps << "|"        // 新增
        << msg->latency << "|"    // 新增
        << msg->txs.size();       // 将 size 放在变长数组前
    
    // 2. 序列化变长交易列表
    for (auto& tx : msg->txs) {
        oss << "|" << tx.serialize();
    }
    return oss.str();
}

// --- 反序列化方法 ---
bool deserializeMessagePayload(const std::string& payload, Message& outMessage) {
    if (payload.empty()) return false;

    std::vector<std::string> tokens;
    std::string token;
    std::istringstream f(payload);
    while (std::getline(f, token, '|')) {
        tokens.push_back(token);
    }

    // 现在的最小 token 数变为 6 (type, src, dst, tps, latency, txSize)
    if (tokens.size() < 6) return false;

    try {
        // 按顺序解析
        outMessage.type = std::stoi(tokens[0]);
        outMessage.srcShardId = std::stoi(tokens[1]);
        outMessage.dstShardId = std::stoi(tokens[2]);
        outMessage.tps = std::stod(tokens[3]);     // 解析 double
        outMessage.latency = std::stod(tokens[4]); // 解析 double
        
        size_t txCount = std::stoul(tokens[5]);
        outMessage.txs.clear();

        // 校验总长度：6个基础字段 + txCount个交易字段
        if (tokens.size() != 6 + txCount) return false;

        for (size_t i = 0; i < txCount; ++i) {
            // 交易数据从索引 6 开始
            outMessage.txs.push_back(transaction::deserialize(tokens[6 + i]));
        }
    } catch (...) {
        // 捕捉 stoi, stod 可能抛出的解析异常
        return false;
    }
    return true;
}

// std::string serializeMessagePayload(Message* msg) {
//     std::ostringstream oss;
//     oss << msg->type << "|" 
//         << msg->srcShardId << "|" 
//         << msg->dstShardId << "|" 
//         << msg->txs.size();
    
//     for (auto& tx : msg->txs) {
//         oss << "|" << tx.serialize(); // 调用 transaction 的序列化
//     }
//     return oss.str();
// }

// bool deserializeMessagePayload(const std::string& payload, Message& outMessage) {
//     if (payload.empty()) return false;

//     std::vector<std::string> tokens;
//     std::string token;
//     std::istringstream f(payload);
//     while (std::getline(f, token, '|')) {
//         tokens.push_back(token);
//     }

//     if (tokens.size() < 4) return false;

//     try {
//         outMessage.type = std::stoi(tokens[0]);
//         outMessage.srcShardId = std::stoi(tokens[1]);
//         outMessage.dstShardId = std::stoi(tokens[2]);
        
//         size_t txCount = std::stoul(tokens[3]);
//         outMessage.txs.clear();

//         if (tokens.size() != 4 + txCount) return false;

//         for (size_t i = 0; i < txCount; ++i) {
//             outMessage.txs.push_back(transaction::deserialize(tokens[4 + i]));
//         }
//     } catch (...) {
//         return false;
//     }
//     return true;
// }

std::string messageTypeToString(int type) {
    switch (static_cast<MessageType>(type)) {
        case MessageType::CROSS_SHARD_TX_REQUEST:
            return "CROSS_SHARD_TX_REQUEST";
        case MessageType::CROSS_SHARD_TX_COMMIT_MSG:
            return "CROSS_SHARD_TX_COMMIT_MSG";
        case MessageType::PERFORMANCE_MSG:
            return "PERFORMANCE_MSG";
        default:
            return "MSG_UNKNOWN(" + std::to_string(type) + ")";
    }
}

MessageDispatcher::MessageDispatcher(Shard& owner):m_owner(owner) {
    registerBuiltInDefaultHandlers();
}

void MessageDispatcher::registerBuiltInDefaultHandlers() {
    registerHandler(MessageType::CROSS_SHARD_TX_REQUEST,
                    [this](Message& msg) { crossShardTxsHandler(msg); });
    registerHandler(MessageType::CROSS_SHARD_TX_COMMIT_MSG,
                    [this](Message& msg) { crossShardCommittedMsgHandler(msg); });
    registerHandler(MessageType::PERFORMANCE_MSG,
                    [this](Message& msg) { performanceMsgHandler(msg); });

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

void MessageDispatcher::dispatch(Message& message) {

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

void MessageDispatcher::crossShardTxsHandler(Message& message){
    
    int sourceShardId = message.srcShardId;
    cout << "收到了来自分片 " << sourceShardId << "的跨片交易任务....." << endl;

    auto& txs = message.txs;
    int txSize = txs.size();
    for (int i = 0; i < txSize; i++) {
        txs.at(i).type = 1.5;
    }
    m_owner.enqueueTransactions(txs);
}

void MessageDispatcher::performanceMsgHandler(Message& message){
    int sourceShardId = message.srcShardId;
    double tps = message.tps;
    double latency = message.latency;
    cout << "收到了来自分片" << sourceShardId << "的性能消息...." << endl;
    
    m_owner.performanceMetricsMutex.lock();

    // 1. 更新或插入TPS和延时
    m_owner.leafShardsThroughputs[sourceShardId] = tps;
    m_owner.leafShardsLatencys[sourceShardId] = latency;

    // 2. 计算总交易数（所有分片TPS之和）
    double totalCommittedTransactions = 0.0;
    for (const auto& pair : m_owner.leafShardsThroughputs) {
        totalCommittedTransactions += pair.second;
    }

    // cout << "当前总提交交易数(TPS): " << totalCommittedTransactions << endl;

    // 可选：计算加权平均延迟
    double totalWeightedLatency = 0.0;
    for (const auto& pair : m_owner.leafShardsLatencys) {
        int shardId = pair.first;
        double latency = pair.second;
        double tps = m_owner.leafShardsThroughputs[shardId];  // 获取对应的TPS
        totalWeightedLatency += tps * latency;
    }

    double averageLatency = (totalCommittedTransactions > 0) ?
                            totalWeightedLatency / totalCommittedTransactions : 0.0;

    cout << "当前系统的整体TPS: " << totalCommittedTransactions
                                << ", 延时 " << averageLatency << endl;

    m_owner.performanceMetricsMutex.unlock();
}

void MessageDispatcher::crossShardCommittedMsgHandler(Message& message){
    cout << "收到了来自下层的跨片交易提交信息....." << endl;
    

}


void MessageDispatcher::defaultLogHandler(Message& message) {
    // std::cout << "[MessageDispatcher] receive "
    //           << messageTypeToString(message.type)
    //           << ", src=" << message.srcShardId
    //           << ", dst=" << message.dstShardId
    //           << ", body=" << message.body << std::endl;
}
