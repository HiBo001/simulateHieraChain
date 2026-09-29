#include <iostream>
#include <stdlib.h>
#include <thread>
#include <vector>
#include <regex>
#include <set>
#include <chrono>
#include "shard.h"
#include "message.h"

using namespace std;

// 配置项
namespace Config {
    // 配置项
    int orderingCapacity = 5000;
    int executionCapacity = 8000; // 暂未启用
    int batchFetchSize = 5000;
    int transactionSendRate = 5000;
    string ownedStateIdsDir = "../accessControlList";
    string shardsTopologyDir = "../shardsTopology";
    string workLoadDir = "../workloadProfile";
    string shardIdDir = "shardId";
    string topShardIdDir = "../topShardId";
    string networkConfigDir = "../networkConfig";
}

// 全局变量
map<int, int> throughputs; // 每个分片的吞吐
map<int, pair<int, double>> latencys; // 每个分片的所有交易延迟

int main(){

    Shard* shard = new Shard();
    std::this_thread::sleep_for(std::chrono::milliseconds(10000)); // 等待10秒
    shard->start();

    // auto leafShardIds = shard->helper->leafShardIds;
    // cout << "叶子分片如下: " << endl;
    // for(auto leafShardid : leafShardIds){
    //     cout << "leafShardid :" << leafShardid << endl;
    // }

    // cout << "topShardId = " << shard->topshardId << endl;

    // // 测试发送
    // if (shard->shardId == 5){
    //     Message msg = {1, 5, 1, {}};
    //     shard->networkManager->sendMessage(&msg, 5, 10);
    // }

    while (true) { // 主线程常驻
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    
    return 0;
}
