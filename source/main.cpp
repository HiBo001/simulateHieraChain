#include <iostream>
#include <stdlib.h>
#include <thread>
#include <vector>
#include <regex>
#include <set>
#include <chrono>

#include "shard.h"
#include "shardsManager.h"

using namespace std;

// 配置项
int orderingCapacity = 5000;
int executionCapacity = 8000;
int batchFetchSize = 5000;
int transactionSendRate = 2000;
string accessControlListDir = "../../accessControlList";
string shardsTopologyDir = "../../shardsTopology";
string workLoadDir = "../../workloadProfile";
string shardIdDir = "../../shardId";

// 全局变量
map<int, int> throughputs; // 每个分片的吞吐
map<int, pair<int, double>> latencys; // 每个分片的所有交易延迟

int main(){

    int leafShardCount = 3; // 启动叶子分片
    int totalShardCount = 4;
    int CoordinatorShardCount = 1; // 所有分片个数


    // // 初始化叶子分片数组
    // thread_safety_map <int, Shard*> shards;
    // for(int shardid = 1; shardid <= leafShardCount; shardid++){
    //     map<string, string> permission_list; // 权限列表
    //     int state_number = 1000; // 分片中的状态数量

    //     srand(unsigned(time(0)));
    //     for(int i = 0; i < state_number; i++){
    //         int stateid = i + (shardid - 1) * state_number;
    //         string key = "state_" + to_string(stateid);
    //         string value = to_string(rand() % 100);
    //         permission_list.insert(make_pair(key, value)); // 权限列表
    //     }

    //     // int order_capability = 5000;
    //     // int execution_capability = 8000;
    //     // int processBatch = 5000;
    //     // int tx_sendRate = 2000;
    //     Shard* shard = new Shard(shardid, orderingCapacity, executionCapacity, batchFetchSize, transactionSendRate, permission_list);
    //     shards.insert(shardid, shard); // 初始化分片
    // }


    ShardsManager* manager = new ShardsManager(leafShardCount, CoordinatorShardCount, accessControlListDir);
    manager->parseTopology();
    manager->printShardTopology();
    manager->parseWorkload();
    manager->printWorkload();

    manager->startAllShards();

    // std::this_thread::sleep_for(std::chrono::milliseconds(10000)); // sleep10秒，等待全部分片启动完毕

    // globalPerformanceStats* status = new globalPerformanceStats(leafShardCount, totalShardCount); // 启动监控TPS和延迟的线程，每秒打印
    // status->startMetrics();






    // // 启动叶子分片
    // for(int shardid = 1; shardid <= leaf_shard_number; shardid++){
    //     auto shard = shards.find(shardid);
    //     cout << "启动分片" << shard->shardId << "..." << endl;
    //     shard->start();
    // }

    // // 启动上层分片
    // vector<int> leafshardids = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36};
    // topology_analyzer analyzer(leafshardids);

    // analyzer.extract_topologyLines(topologyLines);
    // analyzer.extract_workloadLines(loadLines);

    // // 开始准备生成排序者分片
    // int order_capability = 5000;
    // int processBatch = 5000;
    // // int tx_sendRate = 4000;

    // // 开始生成排序者对象
    // vector<shared_ptr<CoordinatorShard>> coord_shards;
    // auto cooshard_cstNums = analyzer.cooshard_cstNums; // 排序任务
    // auto cross_workloads = analyzer.Cross_workloads; // 跨片负载

    // for(auto cooshard_cstNum: cooshard_cstNums){
    //     int coord_shardid = cooshard_cstNum.first;
    //     int cstNum = cooshard_cstNum.second;
    //     vector<pair<pair<int, int>, double>> destination_pairs;

    //     cout << "coord_shardid = " << coord_shardid << ", cstNum = " << cstNum << endl;

    //     for(auto cross_workload: cross_workloads){
    //         if(cross_workload.lca == coord_shardid){
    //             int shard1 = cross_workload.shardNums.at(0);
    //             int shard2 = cross_workload.shardNums.at(1);
    //             double percentage = double(cross_workload.txnum) / cooshard_cstNums.at(coord_shardid);
    //             destination_pairs.push_back(make_pair(make_pair(shard1, shard2), percentage));
    //         }
    //     }

    //     int tx_sendRate = cstNum;
    //     auto cooshard = make_shared<CoordinatorShard>(coord_shardid, tx_sendRate, processBatch, order_capability, destination_pairs);
    //     coord_shards.push_back(cooshard);
    // }

    // for(auto coord_shard : coord_shards){
    //     coord_shard->start(shards); // 启动上层分片
    // }

    // 主线程常驻
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    
    return 0;
}