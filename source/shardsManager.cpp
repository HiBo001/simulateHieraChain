#ifndef SHARDSMANAGER_CPP
#define SHARDSMANAGER_CPP

#include "shardsManager.h"
#include <fstream>
#include <string>
#include <iomanip>
#include <set>
#include <regex>

using namespace std;

void ShardsManager::printWorkload(){
    

}

void ShardsManager::parseWorkload(){

    std::vector<string> workloadDistribution;

    std::vector<std::string> lines;
    std::ifstream file(WorkLoadDir);
    
    if (!file.is_open()) {
        std::cerr << "无法打开文件1: " << WorkLoadDir << std::endl;
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
    

}




/**
* @brief 寻找两个分片的最近公共祖先 (Lowest Common Ancestor)
*/
int ShardsManager::findLCA(int shardA, int shardB) {
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

/**
 * @brief 打印解析后的拓扑结构
 * @param topologyMap 祖先到子分片的映射
 */
void ShardsManager::printShardTopology() {
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

/**
* @brief 解析配置文件并构建拓扑关系
*/
void ShardsManager::parseTopology(){

    std::ifstream configFile(shardsTopologyDir);

    // 检查文件是否成功打开
    if (!configFile.is_open()) {
        std::cerr << "无法打开分片拓扑文件: " << shardsTopologyDir << std::endl;
        exit(1);
    }

    std::string line;
    while (std::getline(configFile, line)) {
        if (line.empty()) continue;

        size_t commaPos = line.find(',');
        if (commaPos == std::string::npos) continue;

        // 解析当前行所属的祖先 ID
        int ancestorId = std::stoi(line.substr(0, commaPos));

        size_t start = line.find('[');
        size_t end = line.find(']');
        if (start != std::string::npos && end != std::string::npos) {
            std::string childrenStr = line.substr(start + 1, end - start - 1);
            std::stringstream ss(childrenStr);
            std::string childIdStr;

            while (std::getline(ss, childIdStr, ',')) {
                if (!childIdStr.empty()) {
                    int childId = std::stoi(childIdStr);
                    topologyMap[ancestorId].push_back(childId);
                    // 核心：记录每个子分片的直接父节点
                    parentMap[childId] = ancestorId;
                }
            }
        }
    }
    configFile.close();
}

void ShardsManager::startAllShards(){
    for (const auto& pair : shards.map_) {
        pair.second->start();
    }
}

ShardsManager::ShardsManager(int leafShardCount, int CoordinatorShardCount, string& accessControlListDir){

    // 出初始化叶子分片
    for(int shardid = 1; shardid <= leafShardCount; shardid++){
        vector<string> accessControlList; // 初始化权限列表

        std::ifstream file(accessControlListDir);  // 假设你的文本文件名为 input.txt
        if (!file.is_open()) {
            std::cerr << "无法打开文件2" << std::endl;
            exit(1);
        }
        
        std::string line;
        std::string currentShardId;

        while (std::getline(file, line)) {
            // 去除行首尾的空白字符
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
            if (line.empty()) {
                continue;  // 跳过空行
            }
            
            // 检查是否是 shard 行（以 shard 开头且包含冒号）
            if (line.find("shard") == 0 && line.find(':') != std::string::npos) {
                size_t colonPos = line.find(':');
                currentShardId = line.substr(0, colonPos);
            }
            else if (!currentShardId.empty()) {
                // 尝试将当前行解析为数字
                std::istringstream iss(line);
                int num;
                if (iss >> num) {
                    accessControlList.push_back(to_string(num));
                }
            }
        }

        file.close();

        Shard* shard = new Shard(shardid, orderingCapacity, executionCapacity, batchFetchSize, transactionSendRate, accessControlList);
        shards.insert(shardid, shard); // 初始化分片
    }

    // 出初始化协调者分片
    for(int shardid = leafShardCount + 1; shardid <= leafShardCount + CoordinatorShardCount; shardid++){



    }





}

#endif // SHARDMANAGER