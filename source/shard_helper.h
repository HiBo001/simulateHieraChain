#ifndef SHARD_HELPER_H
#define SHARD_HELPER_H

#include <string>
#include <vector>

class Shard;

class ShardHelper {
public:
    std::vector<int> leafShardIds;

    explicit ShardHelper(Shard& shard);
    int parseTopShardId();
    int parseShardId();
    void parseTopology();
    void printShardTopology();
    int findLCA(int shardA, int shardB);
    void parseWorkload();
    void printWorkload();
    void parseOwnedStateIds();
    void printOwnedStateIds();
    int lookupShardByState(std::string stateId);
    double getCurrentTimestamp();

private:
    Shard& shard;
};

#endif // SHARD_HELPER_H
