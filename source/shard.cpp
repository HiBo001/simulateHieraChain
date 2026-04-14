#include <thread>
#include <functional>
#include <chrono>
#include <iomanip>
#include <ctime>
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

using namespace std;

#ifndef SHARD_CPP
#define SHARD_CPP

void Shard::printAccessControlList(){

    // 打印 accessControlList
    std::cout << "Access Control List for Shard " << shardId << ": ";
    if (accessControlList.empty()) {
        std::cout << "None" << std::endl;
        return;
    }
    
    for (size_t i = 0; i < accessControlList.size(); ++i) {
        std::cout << accessControlList[i];
        if (i < accessControlList.size() - 1) {
            std::cout << ", ";
        }
    }
    std::cout << std::endl;
}

void Shard::parseAccessControlList(){
    std::map<std::string, std::vector<int>> shardMap;
    std::ifstream file(Config::accessControlListDir);
    
    if (!file.is_open()) {
        std::cerr << "无法打开文件: " << Config::accessControlListDir << std::endl;
        exit(1);
    }

    string line;
    string currentShard;
    while (std::getline(file, line)) {
        // 去除行首尾的空白字符
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (line.empty()) continue;
        
        // 检查是否是分片标题行（以"shard"开头）
        if (line.find("shard") == 0) {
            // 1. 找到冒号的位置
            size_t colonPos = line.find(':');
            std::string shardName;
            if (colonPos != std::string::npos) {
                shardName = line.substr(0, colonPos);
            } else {
                shardName = line;
            }

            // 2. 提取数字部分：假设格式永远是 "shard" 开头
            // 找到第一个数字出现的位置
            size_t firstDigitPos = shardName.find_first_of("0123456789");
            
            if (firstDigitPos != std::string::npos) {
                // 截取从第一个数字到最后的字符串
                std::string idStr = shardName.substr(firstDigitPos);
                
                try {
                    int shardId = std::stoi(idStr); // 转换为整数
                    
                    // 3. 将其作为 map 的 Key (假设你的 map 是 map<int, vector<int>>)
                    shardMap[to_string(shardId)] = std::vector<int>();
                    
                    // 记录当前的 ID，方便后续数字行插入
                    currentShard = to_string(shardId);
                } catch (const std::exception& e) {
                    std::cerr << "转换 ID 失败: " << idStr << std::endl;
                }
            }
        }
        // 如果是数字行（分片后的数字）
        else if (!currentShard.empty()) {
            std::istringstream iss(line);
            int num;
            while (iss >> num) {
                shardMap[currentShard].push_back(num);
            }
        }
    }

    // 如果当前分片属于叶子分片，直接从 shardMap 中获取 key 为 shardId 的权限列表
    // 如果当前分片属于协调者，收集其所有孩子分片的权限列表
    if(this->role == ShardRole::LEAF){
        accessControlList = shardMap[to_string(shardId)];
    }
    else{
        vector<int> childShardIds = topologyMap[shardId];

        for (std::vector<int>::iterator it = childShardIds.begin();
            it != childShardIds.end(); ++it) {
            int childShardId = *it;

            auto subAccessControlList = shardMap[to_string(childShardId)];

            // 遍历 subAccessControlList 中的每个元素
            for (std::vector<int>::iterator subIt = subAccessControlList.begin();
                subIt != subAccessControlList.end(); ++subIt) {
                accessControlList.push_back(*subIt);
            }
        }
    }
}

void Shard::printWorkload(){

// 使用 lambda 简化重复逻辑
    auto display = [](const std::string& label, const std::map<int, txsDistribution>& data) {
        std::cout << "--- " << label << " ---" << std::endl;
        
        // C++11 基于范围的 for 循环
        for (const std::pair<const int, txsDistribution>& item : data) {
            // item.first 是 map 的 key
            // item.second 是 txsDistribution 结构体
            const int& id = item.first;
            const txsDistribution& dist = item.second;

            std::cout << "[ID: " << id << "] "
                      << "Type: " << (dist.type == 1 ? "Intra" : "Cross") << " | "
                      << "Count: " << dist.txCount << " | "
                      << "Involved Shards: [";
            
            // 打印 vector
            for (size_t i = 0; i < dist.invlovedShardIds.size(); ++i) {
                std::cout << dist.invlovedShardIds[i] 
                          << (i == dist.invlovedShardIds.size() - 1 ? "" : ", ");
            }
            std::cout << "]" << std::endl;
        }
        
        if (data.empty()) {
            std::cout << "(Empty)" << std::endl;
        }
        std::cout << std::endl;
    };

    display("Intra-Shard Txs Distribution", intraShardTxsDistribution);
    display("Cross-Shard Txs Distribution", crossShardTxsDistribution);
}

void Shard::parseWorkload(){

    std::vector<string> workloadDistribution;

    std::vector<std::string> lines;
    std::ifstream file(Config::workLoadDir);
    
    if (!file.is_open()) {
        std::cerr << "无法打开文件1: " << Config::workLoadDir << std::endl;
        exit(1);
    }

    std::string line;
    while (std::getline(file, line)) {
        // 去除行首尾空白
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        if (line.empty()) continue;
        
        // 如果一行中有多个配置（逗号分隔），可以进一步分割
        std::istringstream iss(line);
        std::string item;
        
        // 如果你想将每个逗号分隔的部分作为独立元素
        while (std::getline(iss, item, ',')) {
            // 去除每个item前后的空格
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            
            if (!item.empty()) {
                workloadDistribution.push_back(item);
            }
        }
    }

    file.close();

    for(auto item: workloadDistribution){
        cout << item << endl;
    }

    // 从 workloadDistribution 中解析得到 map<int, txsDistribution> intraShardTxsDistribution 和 map<int, txsDistribution> crossShardTxsDistribution

    // 正则表达式说明：
    // (shard(\d+)) : 匹配第一个分片及其数字 ID
    // (_inner|_shard(\d+)) : 匹配 "_inner" 或者 "_shard" 及其后面的第二个 ID
    // :(\d+) : 匹配冒号后的交易数量
    
    std::regex txDisPattern; 
    txDisPattern = std::regex("shard(\\d+)(?:_inner|_shard(\\d+)):(\\d+)");

    std::smatch match;

    for (const std::string& entry : workloadDistribution) {
        if (std::regex_search(entry, match, txDisPattern)) {
            int shardId1 = std::stoi(match[1].str());
            int txCount = std::stoi(match[match.size() - 1].str());

            // 检查是片内还是跨片
            if (entry.find("inner") != std::string::npos) {
                // --- 片内交易 (Intra-shard) ---
                txsDistribution dist;
                dist.type = 1;
                dist.txCount = txCount;
                dist.invlovedShardIds = {shardId1};

                // Key 为分片 ID
                intraShardTxsDistribution[shardId1] = dist;

            } else {
                // --- 跨片交易 (Cross-shard) ---
                int shardId2 = std::stoi(match[2].str());
                
                txsDistribution dist;
                dist.type = 2;
                dist.txCount = txCount;
                dist.invlovedShardIds = {shardId1, shardId2};

                // Key 为两个分片的最近公共祖先 (LCA)
                int lcaKey = findLCA(shardId1, shardId2);
                
                // 如果找到 LCA，则放入跨片字典
                if (lcaKey != -1) {
                    crossShardTxsDistribution[lcaKey] = dist;
                } else {
                    std::cerr << "Warning: No LCA found for " << entry << std::endl;
                }
            }
        }
    }
    cout << "解析负载完成..." << endl;
}

int Shard::findLCA(int shardA, int shardB){

    if (shardA == shardB) return shardA;
    std::set<int> pathA;
    
    // 1. 记录 shardA 向上到根节点的所有路径
    int curr = shardA;
    pathA.insert(curr);
    while (parentMap.count(curr)) {
        curr = parentMap[curr];
        pathA.insert(curr);
    }

    // 2. 从 shardB 开始向上追溯，第一个在 pathA 中出现的节点就是 LCA
    curr = shardB;
    while (true) {
        if (pathA.count(curr)) {
            return curr;
        }
        if (parentMap.count(curr)) {
            curr = parentMap[curr];
        } else {
            break; // 到达根节点仍未找到
        }
    }
    return -1; // 没有公共祖先
}

void Shard::printShardTopology(){

    std::cout << "\n========== Shard Topology Report ==========" << std::endl;
    std::cout << std::left << std::setw(15) << "Ancestor ID" << " | " << "Child Shard IDs" << std::endl;
    std::cout << "-------------------------------------------" << std::endl;

    for (const auto& [ancestorId, children] : topologyMap) {
        std::cout << std::left << std::setw(15) << ancestorId << " | [ ";
        
        for (size_t i = 0; i < children.size(); ++i) {
            std::cout << children[i] << (i == children.size() - 1 ? "" : ", ");
        }
        
        std::cout << " ]" << std::endl;
    }
    std::cout << "===========================================\n" << std::endl;
}

// 这个解析拓扑结构函数需要修改一下，topologyMap 中存储的是 存储 祖先 -> 子分片 的映射，这里的子分片是指位于底层的叶子分片
// 例如， 1和2祖先是5，3和4祖先是6，5和6的祖先是7，那么7的孩子分片应该是1 2 3 4
void Shard::parseTopology() {
    std::ifstream configFile(Config::shardsTopologyDir);
    if (!configFile.is_open()) {
        std::cerr << "无法打开文件: " << Config::shardsTopologyDir << std::endl;
        exit(1);
    }

    std::map<int, std::vector<int>> directChildrenMap;
    std::string line;

    while (std::getline(configFile, line)) {
        if (line.empty()) continue;

        // 1. 处理角色判定（保持原样）
        if (line.find("leaf:") != std::string::npos) {
            size_t start = line.find('{'), end = line.find('}');
            if (start != std::string::npos && end != std::string::npos) {
                std::string idsStr = line.substr(start + 1, end - start - 1);
                std::stringstream ss(idsStr);
                std::string segment;
                while (std::getline(ss, segment, ',')) {
                    if (!segment.empty() && std::stoi(segment) == this->shardId) {
                        this->role = ShardRole::LEAF;
                    }
                }
            }
            continue;
        }

        // 2. 构建基础树结构 (Ancestor -> Direct Children)
        size_t commaPos = line.find(',');
        if (commaPos == std::string::npos) continue;

        int ancestorId = std::stoi(line.substr(0, commaPos));
        size_t start = line.find('['), end = line.find(']');
        if (start != std::string::npos && end != std::string::npos) {
            std::string childrenStr = line.substr(start + 1, end - start - 1);
            std::stringstream ss(childrenStr);
            std::string childIdStr;
            while (std::getline(ss, childIdStr, ',')) {
                if (!childIdStr.empty()) {
                    int childId = std::stoi(childIdStr);
                    directChildrenMap[ancestorId].push_back(childId);
                    parentMap[childId] = ancestorId;
                }
            }
        }
    }
    configFile.close();

    // 3. 核心改进：定义一个 Lambda 函数来递归寻找所有底层叶子
    // 逻辑：如果一个节点没有子节点，它就是叶子；否则递归它的所有子节点
    std::function<void(int, std::vector<int>&)> findLeaves = 
        [&](int nodeId, std::vector<int>& leafList) {
        
        // 如果该节点在 directChildrenMap 中没有记录，说明它是最底层的叶子
        if (directChildrenMap.find(nodeId) == directChildrenMap.end() || 
            directChildrenMap[nodeId].empty()) {
            leafList.push_back(nodeId);
            return;
        }

        // 否则，它是祖先/中间节点，继续向下找
        for (int childId : directChildrenMap[nodeId]) {
            findLeaves(childId, leafList);
        }
    };

    // 4. 为每个祖先节点生成最终的叶子映射
    for (auto const& pair : directChildrenMap) {
        int ancestorId = pair.first;
        std::vector<int> allBottomLeaves;
        findLeaves(ancestorId, allBottomLeaves);
        
        // 去重（防止拓扑配置重复导致的 ID 重复）
        std::sort(allBottomLeaves.begin(), allBottomLeaves.end());
        allBottomLeaves.erase(std::unique(allBottomLeaves.begin(), allBottomLeaves.end()), allBottomLeaves.end());
        
        topologyMap[ancestorId] = allBottomLeaves;
    }

    // 设置默认角色
    if (this->role != ShardRole::LEAF) this->role = ShardRole::COORDINATOR;

    // if(this->role == ShardRole::LEAF){
    //     cout << "分片" << shardId << "是叶子分片" << endl;
    // }
    // else{
    //     cout << "分片" << shardId << "是协调者分片" << endl;
    // }
}

// 提取分片Id
int Shard::parseShardId(){

    int shardId;
    std::ifstream file(Config::shardIdDir);
    if (!file.is_open()) {
        std::cerr << "无法打开文件！" << std::endl;
        exit(1);
    }

    std::string line;
    if (std::getline(file, line)) {
        // 1. 找到 '=' 的位置
        size_t pos = line.find('=');
        
        if (pos != std::string::npos) {
            // 2. 截取 '=' 之后的部分
            std::string valueStr = line.substr(pos + 1);

            // 3. 转换为整数 (stoi 会自动处理前导空格)
            try {
                shardId = std::stoi(valueStr);
            } catch (const std::exception& e) {
                std::cerr << "Failed to convert shardId number: " << e.what() << std::endl;
            }
        }
    }

    file.close();
    return shardId;
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
    double currentTime = getCurrentTimestamp();

    for(int i = 0; i < Config::transactionSendRate; i++){

        string prefixTxId = to_string(shardId) + to_string(txId);
        int type = this->role == ShardRole::LEAF ? 1 : 2;

        // 从  accessControlList 中随机选择两个元素作为本次读写集
        // 1. 初始化下标向量 [0, 1, 2, ..., n-1]
        std::vector<size_t> indices(accessControlList.size());
        std::iota(indices.begin(), indices.end(), 0); 

        // 2. 静态随机数引擎
        static std::random_device rd;
        static std::mt19937 gen(rd());

        // 3. 洗牌：只洗前两个元素其实就够了（为了效率），或者全洗
        std::shuffle(indices.begin(), indices.end(), gen);

        // 4. 取出前两个下标对应的元素
        std::vector<string> rwset;
        rwset.push_back(to_string(accessControlList[indices[0]]));
        rwset.push_back(to_string(accessControlList[indices[1]]));

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
        
        transaction tx = {type, prefixTxId, rwset, invlovedShardIds, currentTime};
        txs.push_back(&tx);
        // printTransaction(tx);
        txId++;
    }
    // cout << "本轮交易生成完毕...." << endl;
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

void Shard::fetchTransactions() { 
    while (true) {
        std::vector<transaction*> txs;
        int remove_size = 0;

        // 1. 只有从内存池提取数据时才锁定 mempool
        mempoolMutex.lock(); 
        if (!transactionMempool.empty()) {
            int current_mempool_size = transactionMempool.size();
            
            // 每次最多拿 orderingCapacity 笔
            remove_size = std::min(current_mempool_size, orderingCapacity);

            for (int i = 0; i < remove_size; i++) {
                auto tx = transactionMempool.front();
                txs.push_back(tx);
                transactionMempool.pop();
            }
        }
        mempoolMutex.unlock(); // 提取完毕立即释放，允许 generateTransactions 继续存入

        // // 2. 如果拿到了交易，开始处理性能统计和共识 // 这个放在执行部分
        // if (!txs.empty()) {
        //     // performance_mtx.lock();  // 保护统计变量
            
        //     double time = getCurrentTimestamp();
        //     for (auto tx : txs) {
        //         // 根据交易类型计算贡献度
        //         if (tx->type == 1) {
        //             committedTxCount += 1.0;
        //         } else if (tx->type == 2) {
        //             committedTxCount += 0.5; // 跨片交易可能只贡献一半工作量
        //         }

        //         double latency = time - tx->sendedTime;
        //         totalLatency += latency;
        //     }
        //     committedSubTxCount += remove_size;
            
        //     performance_mtx.unlock(); 

        //     // 3. 执行共识 (在锁外执行，避免阻塞其他线程)
        //     runConsensus(txs);
        // }

        // 4. 模拟共识耗时 (Ordering Latency)
        // 逻辑：如果拿得少，说明负载低，睡眠时间稍长；如果拿满了，说明负载高，处理会更快连续
        int sleep_time = double(remove_size) / orderingCapacity * 1000;        
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time + 10)); 
        // cout << "remove_size = "<< remove_size << ", 交易共识完毕..." << endl;
    }
}

// void Shard::fetchTransactions(){ // 从交易池子中拿交易 (每次最多拿 orderingCapacity 笔)
    
//     while (true){
//         mempoolMutex.lock(); // 手动加锁

//         int remove_size = 0;

//         if(!transactionMempool.empty()){

//             performance_mtx.lock();  // 加锁

//             // double time = getCurrentTimestamp();
//             vector<transaction*> txs;
//             int remaining_size = transactionMempool.size(); // 交易池中剩下的交易数

//             if(remaining_size >= batchFetchSize){
//                 remove_size = batchFetchSize;
//             }
//             else{
//                 remove_size = remaining_size;
//             }

//             // int execution_workload = 0;
//             for(int i = 0; i < remove_size; i++){

//                 auto tx = transactionMempool.front();
//                 txs.push_back(tx);
//                 transactionMempool.pop();

//                 if(tx->type == 1){
//                     // execution_workload += 1;
//                     committedTxCount += 1;
//                 }
//                 else if(tx->type == 2){
//                     // execution_workload += 1;
//                     committedTxCount += 0.5;
//                 }

//                 // double latency = time - tx->sendedTime;
//                 // totalLatency += latency;
//             }

//             committedSubTxCount += remove_size;

//             performance_mtx.unlock(); // 解锁

//             // 调换顺序
//             runConsensus(txs);
//             // executionTxs(txs);
//         }

//         mempoolMutex.unlock(); // 手动释放锁

//         int remaining_per = double(orderingCapacity - remove_size) / orderingCapacity * 1000;
//         std::this_thread::sleep_for(std::chrono::milliseconds(remaining_per)); // 模拟共识过程
//         std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 模拟共识过程
//     }
// }

void Shard::enqueueRemoteTransactions(vector<transaction*>& txs){

    mempoolMutex.lock(); // 手动加锁

    int tx_size = txs.size();
    for(int i = 0; i < tx_size; i++){
        transactionMempool.push(txs.at(i));
    }

    mempoolMutex.unlock(); // 手动加锁
}

void Shard::runConsensus(vector<transaction*>& txs){
    int tx_size = txs.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // std::this_thread::sleep_for(std::chrono::milliseconds(int((float(tx_size) / order_capability) * 500)));
}

void Shard::executeTransactions(vector<transaction*>& txs){

    std::lock_guard<std::mutex> performance_lock(performance_mtx);

    // 暂时假设系统中全部是片内交易
    int tx_size = txs.size();
    int execution_workload = 0;

    for(int i = 0; i < tx_size; i++){ // 交易的总执行负载
        if(txs.at(i)->type == 1){
            execution_workload += 1;
            committedTxCount += 1;
        }
        else if(txs.at(i)->type == 2) {
            execution_workload += 1;
            committedTxCount += 0.5;
        }
    }

    // std::this_thread::sleep_for(std::chrono::milliseconds(int((float(execution_workload) / execution_capability) * 500)));

    committedSubTxCount += tx_size;

    double time = getCurrentTimestamp();
    for(int i = 0; i < tx_size; i++){
        double latency = time - txs.at(i)->sendedTime;
        totalLatency += latency;
    }
}

void Shard::start(){

    // 向交易池添加交易
    std::thread addTransactions_thread([this] {
        enqueueTransactions();
    });

    // 从交易池拉取交易
    std::thread removeTransactions_thread([this] {
        fetchTransactions();
    });

    // 计算当前分片的TPS
    std::thread monitor_thread([this] {
        startMetrics();
    });

    addTransactions_thread.detach();
    removeTransactions_thread.detach();
    monitor_thread.detach();

    cout << "启动分片" << this->shardId << " ..."<< endl;
}

double Shard::getCurrentTimestamp(){

    // 获取当前系统时间
    auto now = std::chrono::system_clock::now();
    // 获取当前时间距离 Unix 时间戳的持续时间（包括小数部分）
    auto epoch = now.time_since_epoch();
    // 转换为浮动类型，秒数可以包含小数
    double second = std::chrono::duration<double>(epoch).count();
    return second;
}

void Shard::printPerformanceStats(){

    std::lock_guard<std::mutex> performance_lock(performance_mtx);

    // if(committedTxCount == 0){
    //     // cout << "分片"<< shardid << "交易延迟 = 0" << endl;
    //     // cout << "分片"<< shardid << "交易吞吐 = 0" << endl;
    // }
    // else{
    //     throughputs.at(shardId) = committedTxCount;
    //     auto latency = make_pair(committedSubTxCount, totalLatency);
    //     latencys.at(shardId) = latency;
    //     cout << "分片" << shardid << "交易吞吐 = "<< committedTxCount << ", 交易延迟 = "<< total_latency / committedTxCount << endl;
    //     committedTxCount = 0;
    //     committedSubTxCount = 0;
    //     totalLatency = 0;
    // }
}

void Shard::startMetrics(){ // 统计分片当前的交易吞吐和延迟

    while (true){
        printPerformanceStats();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 每秒统计一次吞吐和延迟
    }
}

Shard::Shard() {

    this->orderingCapacity = Config::orderingCapacity;
    this->executionCapacity = Config::executionCapacity;
    this->batchFetchSize = Config::batchFetchSize;
    this->transactionSendRate = Config::transactionSendRate;

    // 获取分片id
    this->shardId = parseShardId();

    parseTopology(); // 解析系统拓扑
    printShardTopology(); // 打印系统拓扑结构

    parseAccessControlList(); // 解析访问权限列表
    printAccessControlList();

    parseWorkload(); // 解析负载
    printWorkload();
}

#endif // SHARD_CPP