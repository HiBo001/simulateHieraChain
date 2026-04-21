#ifndef COMMON_H      // ← 移到最顶部
#define COMMON_H

#include <string>
#include <queue>
#include <map>
#include <sstream>

using namespace std;

// 声明一些全局变量
namespace Config {
    extern int orderingCapacity;
    extern int executionCapacity;
    extern int batchFetchSize;
    extern int transactionSendRate;
    extern string ownedStateIdsDir;
    extern string shardsTopologyDir;
    extern string workLoadDir;
    extern string shardIdDir;
    extern string networkConfigDir;
}

extern std::mutex globalMetricsMutex; // 整体吞吐的延迟的锁
extern map<int, int> throughputs;
extern map<int, pair<int, double>> latencys;


// 定义交易
struct transaction{
    double type; // 交易类型，1表示片内交易、2表示跨片交易、1.5表示已经由上层定过顺序的跨片交易，需要立即处理
    string txId;
    vector<string> RWSet; // 交易读写集
    vector<int> invlovedShardIds;
    double sendedTime;

    // 将 transaction 内部序列化为一个字符串
    std::string serialize() const {
        std::ostringstream oss;
        oss << type << "," << txId << ",";
        
        // 处理 RWSet (使用 # 分隔)
        for (size_t i = 0; i < RWSet.size(); ++i) {
            oss << RWSet[i] << (i == RWSet.size() - 1 ? "" : "#");
        }
        oss << ",";

        // 处理 invlovedShardIds (使用 # 分隔)
        for (size_t i = 0; i < invlovedShardIds.size(); ++i) {
            oss << invlovedShardIds[i] << (i == invlovedShardIds.size() - 1 ? "" : "#");
        }
        oss << "," << sendedTime;
        
        return oss.str();
    }

    // 从字符串解析回 transaction
    static transaction deserialize(const std::string& s) {
        std::vector<std::string> parts;
        std::string part;
        std::istringstream iss(s);
        while (std::getline(iss, part, ',')) parts.push_back(part);

        transaction tx;
        tx.type = std::stoi(parts[0]);
        tx.txId = parts[1];

        // 解析 RWSet
        std::istringstream rss(parts[2]);
        std::string item;
        while (std::getline(rss, item, '#')) if(!item.empty()) tx.RWSet.push_back(item);

        // 解析 invlovedShardIds
        std::istringstream iss_ids(parts[3]);
        while (std::getline(iss_ids, item, '#')) if(!item.empty()) tx.invlovedShardIds.push_back(std::stoi(item));

        tx.sendedTime = std::stod(parts[4]);
        return tx;
    }
};

// 定义交易
struct txsDistribution{
    int type; // 交易类型，1表示片内交易，2表示跨片交易
    int txCount;
    vector<int> invlovedShardIds;
};

// 定义线程安全数据结构
// Map
template <typename K, typename V>
class thread_safety_map{
    
    public:
        void insert(K& key, V& value){
            std::lock_guard<std::mutex> lock(map_mtx);
            map_.insert(std::pair<K, V>(key, value));
        }

        V& find(K& key){
            std::lock_guard<std::mutex> lock(map_mtx);
            return map_.at(key);
        }
    
    public:
        std::mutex map_mtx;
        std::map<K, V> map_;
};

class globalPerformanceStats{

    public:
        int leafShardCount;
        int totalShardCount;
        queue<int> recent_throughputs;
        queue<double> recent_latencys;

    public:
        globalPerformanceStats(int _leafShardCount, int _totalShardCount){
            leafShardCount = _leafShardCount;
            totalShardCount = _totalShardCount;

            for(int i = 0; i < _totalShardCount; i++){
                int shardId = i + 1;
                throughputs.insert(make_pair(shardId, 0));
                pair<int, double> latency; // 交易总延迟/交易数量
                latencys.insert(make_pair(shardId, latency));
            }
        }

        void printPerformanceStats();
        void startMetrics();
        double getCurrentTimestamp();
};

#endif // COMMON_H
