#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <iostream>
#include <thread>
#include "common.h"

using namespace std;

#ifndef COMMON_CPP
#define COMMON_CPP

std::mutex globalMetricsMutex;  // 实际定义

void globalPerformanceStats::startMetrics(){
    std::thread monitor_thread([this] {
        printPerformanceStats();
    });
    monitor_thread.detach();
}

void globalPerformanceStats::printPerformanceStats(){

    while (true){
        
        std::unique_lock<std::mutex> globalMetricsLock(globalMetricsMutex, std::defer_lock); // 加锁
        
        int total_throughput = 0;
        int total_txnum = 0;
        double total_latency = 0;
        double average_latency = 0;

        globalMetricsLock.lock();  // 提前释放

        // 首先统计吞吐（所有分片吞吐求和）
        for(int i = 0; i < totalShardCount; i++){
            int shardid = i + 1;

            total_throughput += throughputs.at(shardid); // 总吞吐
            total_txnum += latencys.at(shardid).first; // 总交易数量
            total_latency += latencys.at(shardid).second; // 总延迟
        }

        globalMetricsLock.unlock();  // 提前释放

        average_latency = total_latency / total_txnum;

        cout << "TPS = " << total_throughput << ", Latency = " << average_latency << endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 每1秒统计1次
    }
}


double globalPerformanceStats::getCurrentTimestamp(){
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();    
    double second = std::chrono::duration<double>(epoch).count();
    return second;
}

#endif // COMMON_CPP