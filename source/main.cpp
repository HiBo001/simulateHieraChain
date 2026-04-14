#include <iostream>
#include <stdlib.h>
#include <thread>
#include <vector>
#include <regex>
#include <set>
#include <chrono>
#include "shard.h"

using namespace std;

// 配置项
namespace Config {
    // 配置项
    int orderingCapacity = 5000;
    int executionCapacity = 8000;
    int batchFetchSize = 5000;
    int transactionSendRate = 2000;
    string accessControlListDir = "../../accessControlList";
    string shardsTopologyDir = "../../shardsTopology";
    string workLoadDir = "../../workloadProfile";
    string shardIdDir = "../../shardId";
}

// 全局变量
map<int, int> throughputs; // 每个分片的吞吐
map<int, pair<int, double>> latencys; // 每个分片的所有交易延迟

int main(){

    Shard* shard = new Shard();

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    shard->start();

    // 主线程常驻
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    
    return 0;
}