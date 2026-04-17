#ifndef MESSAGE_H
#define MESSAGE_H

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include "common.h"

enum class MessageType : int { // 消息类型
    CROSS_SHARD_TX_REQUEST = 1,
    CROSS_SHARD_TX_COMMIT_MSG = 2
};

struct Message { // 消息结构体
    int type;
    int srcShardId;
    int dstShardId;
    vector<transaction> txs;
};

std::string serializeMessagePayload(const Message& msg); //
bool deserializeMessagePayload(const std::string& payload, Message& outMessage);
std::string messageTypeToString(int type);

class MessageDispatcher {
public:
    using Handler = std::function<void(const Message&)>;

    MessageDispatcher();
    void registerHandler(MessageType type, Handler handler);
    void registerCustomHandler(int type, Handler handler);
    void dispatch(const Message& message) const;
    // void setDefaultHandler(Handler handler);

private:
    void registerBuiltInDefaultHandlers();
    void defaultLogHandler(const Message& message) const;
    void crossShardTxsHandler(const Message& message) const;
    void crossShardCommittedMsgHandler(const Message& message) const;

private:
    mutable std::mutex handlersMutex;
    std::map<int, Handler> handlers;
    Handler fallbackHandler;
};

#endif // MESSAGE_H
