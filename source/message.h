#ifndef MESSAGE_H
#define MESSAGE_H

#include <functional>
#include <map>
#include <mutex>
#include <string>

enum class MessageType : int {
    MSG_CROSS_TX_REQUEST = 1,
    MSG_CROSS_TX_VOTE = 2,
    MSG_CROSS_TX_COMMIT = 3,
    MSG_HEARTBEAT = 4,
    MSG_CUSTOM = 1000
};

struct Message {
    int type;
    int srcShardId;
    int dstShardId;
    std::string body;
};

std::string serializeMessagePayload(const Message& msg);
bool deserializeMessagePayload(const std::string& payload, Message& outMessage);
std::string messageTypeToString(int type);

class MessageDispatcher {
public:
    using Handler = std::function<void(const Message&)>;

    MessageDispatcher();
    void registerHandler(MessageType type, Handler handler);
    void registerCustomHandler(int type, Handler handler);
    void setDefaultHandler(Handler handler);
    void dispatch(const Message& message) const;

private:
    void registerBuiltInDefaultHandlers();
    void defaultLogHandler(const Message& message) const;

private:
    mutable std::mutex handlersMutex;
    std::map<int, Handler> handlers;
    Handler fallbackHandler;
};

#endif // MESSAGE_H
