#include <thread>
#include <functional>
#include <chrono>
#include <iomanip>
#include <ctime>
#include "network.h"
#include "shard.h"
#include <stdlib.h>
#include <time.h>
#include <sstream>
#include <fstream>
#include <set>
#include <regex>
#include <vector>
#include <algorithm>
#include <random>
#include <numeric>
#include "message.h"
#include "shard_helper.h"

using namespace std;

#ifndef SHARD_CPP
#define SHARD_CPP

// 模拟单笔片内交易的计算开销
// complexity: 复杂度参数，值越大 CPU 占用时间越长
void Shard::simulateExecution(int complexity) {
    volatile double result = 0.0;
    for (int i = 0; i < complexity; ++i) {
        // 使用正弦/对数运算模拟非平凡的计算任务
        result += std::sin(i) * std::log(i + 1.0);
    }
}

void Shard::printTransaction(transaction& tx){

    std::cout << "========================================" << std::endl;
    std::cout << "Transaction ID: " << tx.txId << std::endl;
    
    // 打印交易类型
    std::cout << "Type: " << (tx.type == 1 ? "Intra-Shard" : "Cross-Shard") 
              << " (" << tx.type << ")" << std::endl;

    // 打印发送时间（保留两位小数）
    std::cout << "Sended Time: " << std::fixed << std::setprecision(2) 
              << tx.sendedTime << std::endl;

    // 打印涉及的分片 ID
    std::cout << "Involved Shards: [ ";
    for (size_t i = 0; i < tx.invlovedShardIds.size(); ++i) {
        std::cout << tx.invlovedShardIds[i] 
                  << (i == tx.invlovedShardIds.size() - 1 ? "" : ", ");
    }
    std::cout << " ]" << std::endl;

    // 打印读写集 (RWSet)
    std::cout << "Read/Write Set: { ";
    for (size_t i = 0; i < tx.RWSet.size(); ++i) {
        std::cout << "\"" << tx.RWSet[i] << "\"" 
                  << (i == tx.RWSet.size() - 1 ? "" : ", ");
    }
    std::cout << " }" << std::endl;
    std::cout << "========================================" << std::endl;
}

void Shard::generateTransactions(vector<transaction*>& txs){

    // 从 intraShardTxsDistribution 和 crossShardTxsDistribution 中寻找当前分片负责生成的任务
    // cout << "开始生成交易...." << endl;
    txsDistribution* myTxsDistribution;
    if (this->role == ShardRole::LEAF) {
        // 1. 在片内交易 map 中查找
        auto itIntra = intraShardTxsDistribution.find(shardId);
        if (itIntra != intraShardTxsDistribution.end()) {
            // 找到了，itIntra->second 就是对应的 txsDistribution 结构体
            myTxsDistribution = &(itIntra->second);
        }

    } else {
        // 2. 在跨片交易 map 中查找
        auto itCross = crossShardTxsDistribution.find(shardId);
        if (itCross != crossShardTxsDistribution.end()) {
            // 找到了
            myTxsDistribution = &(itCross->second);
        }
    }
    
    // 开始生成交易, 每次生成 transactionSendRate 笔
    double currentTime = helper->getCurrentTimestamp();

    for(int i = 0; i < Config::transactionSendRate; i++){

        string prefixTxId = to_string(shardId) + to_string(txId);
        double type = this->role == ShardRole::LEAF ? 1 : 2;

        // 从  ownedStateIds 中随机选择两个元素作为本次读写集
        // 1. 初始化下标向量 [0, 1, 2, ..., n-1]
        std::vector<size_t> indices(ownedStateIds.size());
        std::iota(indices.begin(), indices.end(), 0); 

        // 2. 静态随机数引擎
        static std::random_device rd;
        static std::mt19937 gen(rd());

        // 3. 洗牌：只洗前两个元素其实就够了（为了效率），或者全洗
        std::shuffle(indices.begin(), indices.end(), gen);

        // 4. 取出前两个下标对应的元素
        std::vector<string> rwset;
        rwset.push_back(to_string(ownedStateIds[indices[0]]));
        rwset.push_back(to_string(ownedStateIds[indices[1]]));

        // 设置 vector<int> invlovedShardIds
        // 如果 当前分片属于叶子分片, invlovedShard是本地分片
        // 如果 当前分片属于协调者分片, invlovedShard是随机选中的两个状态所属的分片

        vector<int> invlovedShardIds;
        if (this->role == ShardRole::LEAF) {
            // --- 场景 A：当前分片是叶子分片 ---
            // 交易只涉及本地分片（片内交易）
            type = 1; // 假设 1 表示片内交易
            invlovedShardIds.push_back(this->shardId);
        }else {
            // --- 场景 B：当前分片是协调者分片 ---
            type = 2; // 2 表示跨片交易
            const std::vector<int>& leafPool = topologyMap[this->shardId];

            if (leafPool.size() >= 2) {
                // 随机选择算法
                std::vector<int> shuffledPool = leafPool;
                static std::random_device rd;
                static std::mt19937 g(rd());
                
                // 打乱顺序并取前两个
                std::shuffle(shuffledPool.begin(), shuffledPool.end(), g);
                invlovedShardIds.push_back(shuffledPool[0]);
                invlovedShardIds.push_back(shuffledPool[1]);
            }
            else if (!leafPool.empty()) {
                // 如果管辖的叶子不足两个，则全部加入
                invlovedShardIds = leafPool;
            }
        }
        
        transaction* tx = new transaction{type, prefixTxId, rwset, invlovedShardIds, currentTime};
        txs.push_back(tx);
        txId++;
    }
    // cout << "本轮交易生成完毕...." << endl;
}

void Shard::enqueueTransactions(vector<transaction>& txs){ // 将一批交易 txs 压入

    mempoolMutex.lock(); // 加锁
    for(auto tx: txs){
        transactionMempool.push(&tx); // 交易进入交易池
    }
    mempoolMutex.unlock(); // 解锁
}


void Shard::enqueueTransactions(){

    while (true){
        vector<transaction*> txs;
        generateTransactions(txs); // 生成交易

        mempoolMutex.lock(); // 加锁
        int tx_size = txs.size();
        for(int i = 0; i < tx_size; i++){
            transactionMempool.push(txs.at(i)); // 交易进入交易池
        }
        mempoolMutex.unlock(); // 解锁

        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 客户端每1秒发送1次(txs中包含 transactionSendRate 笔交易)
    }
}

void Shard::runExecution(){
    while (true) {
    
        std::vector<transaction*> txsToExecute;

        // 待发送集合：Key 是目标分片 ID，Value 是需要发给它的交易列表
        std::map<int, std::vector<transaction*>> pendingSendQueue;

        // 1. 加锁保护队列
        executionMempoolMutex.lock();

        // 2. 计算本次实际可以提取的数量
        // logic: 取 (池中剩余数量) 和 (执行容量) 的较小值
        int remove_size = std::min((int)executeTransactionsMempool.size(), executionCapacity);

        // 3. 循环提取
        for (int i = 0; i < remove_size; ++i) {
            transaction* tx = executeTransactionsMempool.front();
            txsToExecute.push_back(tx);
            executeTransactionsMempool.pop();
        }

        // 4. 提取完毕，立即解锁
        executionMempoolMutex.unlock();

        if(txsToExecute.size() == 0){
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 5. 后续处理拿到的交易
        int executedIntraTxCount = 0;
        double totalLatency = 0;
        double currentTime = helper->getCurrentTimestamp();

        bool existIntraTransactions = false;

        cout << "txsToExecute.size = " << txsToExecute.size() << endl;

        for (auto tx : txsToExecute) {
            // 因为 tx 是指针，使用 -> 访问成员            
            // 执行具体逻辑
            if (tx->type == 1) { // 片内交易
                existIntraTransactions = true;
                simulateExecution();
                executedIntraTxCount++;
                double latency = currentTime - tx->sendedTime;
                totalLatency += latency;
            }else if (tx->type == 1.5){
                existIntraTransactions = true; // 将
                simulateExecution();
                executedIntraTxCount += 0.5;
                double latency = currentTime - tx->sendedTime;
                totalLatency += latency;
            }else if (tx->type == 2){ // 跨片交易
                
                // 核心逻辑：解析 RWSet 判定目标分片
                std::set<int> targetShards; // 使用 set 自动去重
                for (const std::string& stateKey : tx->RWSet) {
                    // 这里假设你有一个全局或本地的路由表 stateToShardMap
                    // 或者通过 stateKey 里的数字解析出它所属的分片
                    int targetShardId = helper->lookupShardByState(stateKey);

                    // 如果目标分片不是当前分片自己，则加入待发送名单
                    if (targetShardId != this->shardId) {
                        targetShards.insert(targetShardId);
                    }
                }
                
                // 将该交易引用加入到每一个涉及的远端分片发送队列中
                for (int sid : targetShards) {
                    pendingSendQueue[sid].push_back(tx);
                }
            }

            // 3. 模拟发送逻辑（将待发送集合转发给通信模块）
            if (!pendingSendQueue.empty()) {
                for (const auto& entry : pendingSendQueue) {

                    int dstShardId = entry.first;
                    std::vector<transaction*> txsPointers = entry.second;
                    std::vector<transaction> txs;

                    for (auto txsPointer: txsPointers){
                        txs.push_back(*txsPointer);
                    }

                    Message requestMsg{ // 初始化Message
                        static_cast<int>(MessageType::CROSS_SHARD_TX_REQUEST),
                        this->shardId,
                        dstShardId,
                        txs
                    };

                    if (networkManager) {
                        networkManager->sendMessage(&requestMsg); // 发送消息
                    }
                }
            }
        }

        if(existIntraTransactions){ // 这里执行的时候只能统计到片内交易的信息，跨片需要协调者分片收齐commit消息后统一处理
            performanceMetricsMutex.lock();
            committedTxCount += executedIntraTxCount; // 调整 committedTxCount
            committedTxTotalLatency += totalLatency;
            performanceMetricsMutex.unlock();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Shard::runConsensus() {
    while (true) {
        int remove_size = 0;

        // 1. 只有从内存池提取数据时才锁定 mempool
        mempoolMutex.lock(); 
        executionMempoolMutex.lock();

        if (!transactionMempool.empty()) {
            int current_mempool_size = transactionMempool.size();
            
            // 每次最多拿 orderingCapacity 笔
            remove_size = std::min(current_mempool_size, orderingCapacity);

            for (int i = 0; i < remove_size; i++) {
                auto tx = transactionMempool.front();
                executeTransactionsMempool.push(tx); // 将交易放入执行队列
                transactionMempool.pop();
            }
        }

        executionMempoolMutex.unlock();
        mempoolMutex.unlock(); // 提取完毕立即释放，允许 generateTransactions 继续存入

        int sleep_time = double(remove_size) / orderingCapacity * 1000;   
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time + 10));
    }
}

void Shard::start(){
    if (networkManager && !networkManager->start()) { // 启动网络监听线程
        std::cerr << "分片 " << this->shardId << " 启动通信监听失败，进程退出。" << std::endl;
        exit(1);
    }

    // 生成交易并向交易池添加交易
    std::thread injectTxThread([this] {
        enqueueTransactions();
    });

    // 从交易池拉取交易并共识
    std::thread consensusThread([this] {
        runConsensus();
    });

    // 执行共识完的交易
    std::thread executeThread([this] {
        runExecution();
    });

    // 计算当前分片的TPS
    std::thread monitor_thread([this] {
        startMetrics();
    });

    injectTxThread.detach();
    consensusThread.detach();
    executeThread.detach();
    monitor_thread.detach();

    cout << "启动分片" << this->shardId << " ..."<< endl;
}

void Shard::printPerformanceStats(){

    performanceMetricsMutex.lock();
    
    if(committedTxCount == 0){
        cout << "当前分片"<< shardId << ", tps = 0, " << "latency = 0" << endl;
    }else{
        cout << "当前分片" << shardId << ", tps = "<< committedTxCount << " , latency = "<< committedTxTotalLatency / committedTxCount << endl;
    }

    committedTxTotalLatency = 0;
    committedTxCount = 0;
    committedSubTxCount = 0;
    performanceMetricsMutex.unlock();
}

void Shard::startMetrics(){ // 统计分片当前的交易吞吐和延迟

    while (true){
        printPerformanceStats();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 每秒统计一次吞吐和延迟
    }
}

// 初始化分片的网络模块
void Shard::initNetwork(){
    
    networkManager = std::unique_ptr<NetworkManager>(new NetworkManager(this->shardId, *this));
    if (!networkManager->loadConfig(Config::networkConfigDir)) {
        std::cout << "加载网络配置失败: " << Config::networkConfigDir << std::endl;
        exit(1);
    }else{
        std::cout << "网络模块初始化成功: " << Config::networkConfigDir << std::endl;
    }

    // 注册消息包处理函数
    // MessageDispatcher();
}


Shard::Shard() : helper(new ShardHelper(*this)) {

    this->orderingCapacity = Config::orderingCapacity;
    this->executionCapacity = Config::executionCapacity;
    this->batchFetchSize = Config::batchFetchSize;
    this->transactionSendRate = Config::transactionSendRate;

    // 获取分片id
    this->shardId = helper->parseShardId();
    initNetwork(); // 初始化网络模块

    helper->parseTopology(); // 解析系统拓扑
    helper->printShardTopology(); // 打印系统拓扑结构

    helper->parseOwnedStateIds(); // 解析访问权限列表
    helper->printOwnedStateIds();

    helper->parseWorkload(); // 解析负载
    helper->printWorkload();
}

Shard::~Shard() {
    if (networkManager) {
        networkManager->stop();
    }
}

#endif // SHARD_CPP
