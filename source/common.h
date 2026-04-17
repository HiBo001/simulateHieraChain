#include <string>
#include <queue>
#include <map>

using namespace std;

#ifndef COMMON_H
#define COMMON_H

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
    int type; // 交易类型，1表示片内交易，2表示跨片交易
    string txId;
    vector<string> RWSet; // 交易读写集
    vector<int> invlovedShardIds;
    double sendedTime;
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
