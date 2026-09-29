#ifndef SHARD_H
#define SHARD_H        // ← 守卫必须在最顶部

#include <queue>
#include <string>
#include <mutex>
#include <map>
#include <iostream>
#include <memory>
#include "network.h"
#include "common.h"
#include "shard_helper.h"

extern std::mutex global_performance_mtx;

// 定义枚举类型
enum class ShardRole : int {
    LEAF = 1,        // 叶子分片
    COORDINATOR = 2  // 协调者分片
};

class ShardHelper;

// 叶子分片
class Shard{

public:

    int txId = 0;
    int shardId;
    int topshardId;
    ShardRole role; // 成员变量
    int orderingCapacity; // 交易排序能力（每秒能够处理的交易处理）
    int executionCapacity; // 交易执行能力（美秒能够执行的工作量）
    int batchFetchSize; // 分片单位时间内(500ms)从交易池拉取交易数量
    int transactionSendRate; // 客户端交易发送速率

    // 用于统计交易吞吐和延迟的变量
    double committedTxCount = 0; // 1s内提交的交易数量
    double committedSubTxCount = 0; // 1s内提交的交易数量(跨片交易按1笔交易算)
    double committedTxTotalLatency = 0;

    std::map<int, vector<int>>shardToOwnedStateIds; // shardid -> ownedStateIds
    std::vector<int> ownedStateIds; // 状态权限目录
    std::queue<transaction*> transactionMempool; // 共识内存交易池
    std::queue<transaction*> executeTransactionsMempool; // 共识内存交易池

    std::map<int, std::vector<int>> topologyMap; // 存储 祖先 -> 子分片 的映射
    std::map<int, int> parentMap; // 存储 子分片 -> 父分片 的反向映射，用于向上追溯

    map<int, txsDistribution> intraShardTxsDistribution; // 片内交易负载
    map<int, txsDistribution> crossShardTxsDistribution; // 跨片交易负载

    map<int, double> leafShardsThroughputs; // 各个叶子分片的吞吐
    map<int, double> leafShardsLatencys; // 各个叶子分片的延时

// private:
    std::unique_ptr<ShardHelper> helper;
    std::unique_ptr<NetworkManager> networkManager;
    std::mutex mempoolMutex; // 交易池读写互斥锁
    std::mutex executionMempoolMutex; // 交易池读写互斥锁
    std::mutex performanceMetricsMutex; // 当前分片的交易吞吐和延迟性能读写锁

public:
    
    Shard(); // 初始化函数
    ~Shard();
    void generateTransactions(vector<transaction>& txs); // 生成交易
    void printTransaction(transaction& tx);
    void enqueueTransactions(vector<transaction>& txs); // 向交易池添加一批新来的交易
    void enqueueTransactions();
    void runExecution();
    void runConsensus(); // 从交易池取走一部分交易、最多processBatch个
    void printPerformanceStats();
    void startMetrics(); // 计算分片当前的交易吞吐和延迟
    void start(); // 启动分片
    void simulateExecution(int complexity = 500);
    void initNetwork(); // 初始化分片的网络模块
};

#endif // SHARD_H