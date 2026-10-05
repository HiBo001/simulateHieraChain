#include "network.h"
#include "state_digest.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <set>

using namespace arbor;
static volatile std::sig_atomic_t stopping = 0;
static void onSignal(int) { stopping = 1; }
static std::string identity(int shard, int replica) { return std::to_string(shard)+":"+std::to_string(replica); }

struct Membership {
    json config;
    std::string run;
    std::map<std::string,Endpoint> endpoints;
    std::map<std::string,Key> publicKeys;
    std::map<int,int> parent;
    std::set<int> leaves;
#ifdef ARBOR_AHL
    int ahlRoot=-1;
#endif
    Key clientKey;
    explicit Membership(const json& c): config(c), run(c.at("run_id")) {
        if(c.at("consensus").contains("pipeline_window"))
            throw std::runtime_error("consensus.pipeline_window has been removed; remove this option and start a fresh run");
        for (const auto& s:c.at("shards")) {
            int id=s.at("id");
            parent[id]=s.at("parent").is_null() ? -1 : s.at("parent").get<int>();
            leaves.insert(id);
        }
        for (const auto& [id,p]:parent) { (void)id; leaves.erase(p); }
#ifdef ARBOR_AHL
        // Enforce the AHL topology in the binary as well as in the launcher.
        // This also protects clients and direct node invocations with raw configs.
        std::vector<int> roots;
        for(const auto& [id,p]:parent) if(p==-1) roots.push_back(id);
        if(roots.size()!=1 || leaves.size()<2 || parent.size()!=leaves.size()+1 ||
           parent.size()!=c.at("shards").size() || leaves.count(roots.front()))
            throw std::runtime_error("AHL requires one upper shard and at least two direct leaf shards");
        for(const auto& [id,p]:parent) if(id!=roots.front() && p!=roots.front())
            throw std::runtime_error("AHL requires every leaf to be a direct child of its sole upper shard");
        ahlRoot=roots.front();
#endif
        for (const auto& n:c.at("nodes")) {
            auto id=identity(n.at("shard"),n.at("replica"));
            endpoints[id]={n.at("host"),n.at("port")};
            publicKeys[id]=readKey(n.at("public_key"),false);
        }
        clientKey=readKey(c.at("client_public_key"),false);
    }
    int lca(const json& ids) const {
        if (!ids.is_array() || ids.empty()) return -1;
        int candidate=ids[0];
        if (!leaves.count(candidate)) return -1;
        for (auto x:ids) {
            int leaf=x;
            if (!leaves.count(leaf)) return -1;
            std::set<int> ancestors;
            for (int s=candidate;s!=-1;s=parent.at(s)) ancestors.insert(s);
            while (!ancestors.count(leaf)) leaf=parent.at(leaf);
            candidate=leaf;
        }
        return candidate;
    }
    std::set<int> coordinators() const {
        std::set<int> result;
        for(const auto& [id,p]:parent) { (void)p;if(!leaves.count(id)) result.insert(id); }
        return result;
    }
    bool multiLayer() const { return coordinators().size()>1; }
#ifdef ARBOR_AHL
    int ahlCoordinator() const { return ahlRoot; }
#endif
    std::set<int> ancestors(int leaf) const {
        std::set<int> result;
        for(int id=parent.at(leaf);id!=-1;id=parent.at(id)) result.insert(id);
        return result;
    }
    std::set<int> descendants(int origin) const {
        std::set<int> result;
        for(int leaf:leaves) if(ancestors(leaf).count(origin)) result.insert(leaf);
        return result;
    }
    int delay(int from,int to) const {
        const auto& n=config.at("network");
        if (from==to) return n.at("intra_shard_delay_ms");
        auto key=std::to_string(from)+":"+std::to_string(to);
        if (n.at("resolved_links").contains(key)) return n.at("resolved_links").at(key);
        return n.at("default_inter_shard_delay_ms");
    }
    bool replicaMessage(const json& env) const {
        try {
            const auto& b=env.at("body");
            if (b.at("run")!=run) return false;
            auto it=publicKeys.find(identity(b.at("shard"),b.at("from")));
            if(it==publicKeys.end()) return false;
            return verify(env,it->second);
        } catch (...) { return false; }
    }
    bool clientMessage(const json& env) const {
        try {
            const auto& body=env.at("body");if(body.at("run")!=run) return false;
            return verify(env,clientKey);
        }
        catch (...) { return false; }
    }
};

// A bounded PBFT sequence window, batching, stable checkpoints and certified
// view changes. PREPARE votes are from backups only (2f); COMMIT needs 2f+1.
class Replica {
#ifdef ARBOR_SHARPER_TESTS
    friend struct SharPerProtocolTest;
#endif
    struct Slot {
        json proposal;
        std::map<int,json> prepares,commits;
        bool prepared=false, commitSent=false, committed=false;
    };
    Membership members;
    int shard, me, view=0, targetView=0;
    bool changing=false;
    int applied=0, stableSeq=0, catchupTarget=0;
    std::string dir;
    Key privateKey;
    Network net;
    std::mutex inboxMutex;
    std::queue<json> inbox;
    std::map<int,Slot> slots;
    std::map<int,json> preparedHistory, certificates;
    std::map<int,json> snapshots;
    std::map<int,std::string> snapshotDigests;
    std::map<int,std::map<int,json>> checkpointVotes;
    std::map<int,std::map<int,json>> viewChanges;
    json stableProof=json::array(), stableState;
    json state=genesis();
    std::map<std::string,json> pending;
    // CLIENT envelopes remain signed and intact. Overlapping requests wait for
    // the first owner's result rather than launching concurrent consensus.
    std::map<std::string,json> deferredRequests, completedTxResults;
    std::map<std::string,std::pair<std::string,std::string>> pendingTx;
    std::map<std::string,std::set<std::string>> deferredByTx;
    std::set<std::string> changedTx;
    uint64_t dedupIndexRebuilds=0, dedupPendingChecks=0, dedupWaiterChecks=0;
    std::map<std::string,json> pendingCst;
    // Local selection cache. Its verdict is invalidated by every dependency
    // or replicated-state change; it is never an authentication shortcut.
    mutable bool cstSelectionDirty=true;
    mutable json cstSelection=nullptr;
    mutable uint64_t cstSelectionLookups=0, cstSelectionRebuilds=0;
    std::map<std::string,json> stagedRecords, executionWitnesses, orderCertificates;
    std::map<int,json> slotWitnesses;
    std::map<int,std::map<int,json>> roundCertificates;
    int desiredRound=0;
    std::set<std::string> completionResendNeeded, pendingLocalAckQcs;
    std::map<std::string,std::map<int,json>> preparedVotes, ackVotes;
    std::map<std::string,std::map<int,json>> preparedProofs, ackProofs;
    std::set<std::string> activeAckBatches;
    std::set<std::string> forwardedPrepared, forwardedAcks;
    std::map<std::string,json> completedCstResults;
    std::map<int,json> outstandingOrders;
    std::set<std::string> completedCstBatches;
    size_t completedCstTransactions=0;
    std::string cachedStateDigest, cachedKvDigest;
    StateDigest stateDigest;
    bool kvDirty=true;
    mutable std::set<std::string> checkedCloses, checkedOrders, checkedRecords, checkedProofs,
                                  checkedWitnesses;
    std::map<std::string,Clock::time_point> pingStarts;
    std::map<std::string,std::string> pingPeers;
    json probes=json::object();
    std::ofstream journal, events;
    Clock::time_point lastProgress=Clock::now(),lastRetry=Clock::now(),lastCstRetry=Clock::now(),lastSync=Clock::now(),
                      lastStatus=Clock::now(),batchStart=Clock::now(),lastForwardHeartbeat=Clock::now();
    uint64_t rejected=0, duplicates=0, viewCount=0, executionNs=0;
    std::atomic<uint64_t> inboxDropped{0};
    int timeoutMs, batchSize, crossShardBatchSize, crossShardBatchWaitMs, checkpointEvery;
    uint64_t expectedFib=0;
    json myViewChange, lastNewView;
    int window=64;
    json make(std::string type,json fields=json::object(),int v=-1) {
        fields["type"]=type; fields["run"]=members.run; fields["shard"]=shard;
        fields["from"]=me; fields["view"]= v<0 ? view : v;
        return sign(fields,privateKey);
    }
    void log(const std::string& name,json detail=json::object()) {
        detail["event"]=name; detail["view"]=view; detail["steady_ms"]=millis(Clock::now());
        events<<detail.dump()<<'\n'; events.flush();
    }
    void sendTo(int s,int r,const json& env) {
        if (s==shard && r==me) { std::lock_guard<std::mutex> l(inboxMutex); inbox.push(env); }
        else net.send(members.endpoints.at(identity(s,r)),env,members.delay(shard,s));
    }
    void broadcast(const json& env) { for(int r=0;r<4;++r) sendTo(shard,r,env); }
    bool isForward() const { return !changing && me==view%4; }
    void sendShard(int destination,const json& env) {
        for(int replica=0;replica<4;++replica) sendTo(destination,replica,env);
    }
    void refreshDigests(bool rebuild=false) {
        if(rebuild) {stateDigest.rebuild(state);kvDirty=true;}
        else {
            for(const char* field:{"seq","chain","executed","ordered_cst","leaf_ordered_cst",
                                  "last_cst_seq","cst_order_index","cst_round"}) stateDigest.markField(field);
        }
        cachedStateDigest=stateDigest.refresh(state);
        // Keep the public KV digest compatible with SHA256(canonical JSON).
        // Root ORDERs do not change KV, so they do not serialize it again.
        if(kvDirty) {cachedKvDigest=hash(state["kv"].dump());kvDirty=false;}
    }
    bool isCoordinator() const { return !members.leaves.count(shard); }
    int batchOrigin(const std::string& key) const {
        auto colon=key.find(':');
        if(colon==std::string::npos) throw std::runtime_error("invalid batch key");
        size_t usedOrigin=0,usedSequence=0;
        int id=std::stoi(key.substr(0,colon),&usedOrigin),sequence=std::stoi(key.substr(colon+1),&usedSequence);
        if(usedOrigin!=colon || usedSequence!=key.size()-colon-1 || sequence<=0 ||
           std::to_string(id)+":"+std::to_string(sequence)!=key || !members.coordinators().count(id))
            throw std::runtime_error("invalid coordinator batch key");
        return id;
    }
    std::set<int> participants(const json& requests) const {
        std::set<int> result;
        for(const auto& req:requests) for(const auto& tx:req.at("body").at("txs"))
            for(auto id:tx.at("participants")) result.insert(id.get<int>());
        return result;
    }
    std::set<int> orderParticipants(const json& cert) const {
        return participants(cert.at("proposal").at("body").at("value").at("requests"));
    }
    static bool touches(const json& tx,int owner) {
        for(auto id:tx.at("participants")) if(id==owner) return true;
        return false;
    }
    int projection(const json& value,int owner) const {
        return value.at("cst_watermarks").at(std::to_string(owner)).get<int>();
    }
    int localProjection(int origin) const {
        return state.at("cst_indices").value(std::to_string(origin),0);
    }
    json initialAccount() const { return {{"version",uint64_t(0)},{"digest",hash("initial")},{"value",uint64_t(0)}}; }
    uint64_t fibonacci() const {
        uint64_t a=0,b=1;
        int loops=members.config.at("execution").at("fib_iterations");
        for(int k=0;k<loops;++k) { uint64_t next=a+b; a=b; b=next; }
        return a;
    }
    bool validVote(const json& e,const std::string& type,int v,int seq,const std::string& digest) const {
        try {
            const auto& b=e.at("body");
            return members.replicaMessage(e) && b.at("shard")==shard && b.at("type")==type &&
                b.at("view")==v && b.at("seq")==seq && b.at("digest")==digest;
        } catch (...) { return false; }
    }
    bool validRequestForShard(const json& e,int target) const {
#ifdef ARBOR_SHARPER
        return sharperValidRequest(e,target);
#else
        try {
            if (!members.clientMessage(e)) return false;
            const auto& b=e.at("body");
            if(b.at("type")!="CLIENT" || b.at("target")!=target || !b.at("id").is_string() ||
               b.at("id").get<std::string>().size()>200 || !b.at("txs").is_array() || b.at("txs").empty() || int(b.at("txs").size())>batchSize) return false;
            if(!members.leaves.count(target) && int(b.at("txs").size())>crossShardBatchSize) return false;
            const auto& reply=b.at("reply");
            in_addr address{};
            if (inet_pton(AF_INET,reply.at("host").get<std::string>().c_str(),&address)!=1 || reply.at("port").get<int>()<=0 || reply.at("port").get<int>()>65535) return false;
            std::set<std::string> ids;
            for(const auto& t:b.at("txs")) {
                auto id=t.at("id").get<std::string>(); auto key=t.at("key").get<std::string>();
                if(id.empty() || id.size()>200 || key.empty() || key.size()>128 || !ids.insert(id).second ||
                   !t.at("value").is_number_unsigned() || !t.at("participants").is_array() || members.lca(t.at("participants"))!=target) return false;
                std::set<int> ps;
                for (auto p:t.at("participants")) if(!ps.insert(p.get<int>()).second) return false;
                if (members.leaves.count(target) && ps.size()!=1) return false;
                if (ps.size()>1) {
                    if(!t.contains("accesses") || !t.at("accesses").is_array() || t.at("accesses").size()!=ps.size()) return false;
                    std::set<int> accessed;
                    for(const auto& a:t.at("accesses")) {
                        int owner=a.at("shard"); auto localKey=a.at("key").get<std::string>();
                        if(!ps.count(owner) || !accessed.insert(owner).second || localKey.empty() || localKey.size()>128 ||
                           !a.at("value").is_number_unsigned()) return false;
                    }
                    if(accessed!=ps) return false;
                } else if(t.contains("accesses")) return false;
            }
            return true;
        } catch (...) { return false; }
#endif
    }
    bool validRequest(const json& e) const { return validRequestForShard(e,shard); }
    bool validForeignCertificate(const json& c,int origin) const {
        try {
            const auto& pp=c.at("proposal"); const auto& b=pp.at("body");
            int v=b.at("view"),seq=b.at("seq");
            if(b.at("shard")!=origin || v<0 || seq<=0 || b.at("type")!="PREPREPARE" ||
               b.at("from")!=v%4 || !members.replicaMessage(pp) ||
               b.at("digest")!=hash(b.at("value").dump())) return false;
            std::set<int> prepares,commits;
            for(const auto& vote:c.at("prepares")) {
                const auto& vb=vote.at("body"); int signer=vb.at("from");
                if(signer==v%4 || !prepares.insert(signer).second || !members.replicaMessage(vote) ||
                   vb.at("shard")!=origin || vb.at("type")!="PREPARE" || vb.at("view")!=v ||
                   vb.at("seq")!=seq || vb.at("digest")!=b.at("digest")) return false;
            }
            for(const auto& vote:c.at("commits")) {
                const auto& vb=vote.at("body"); int signer=vb.at("from");
                if(!commits.insert(signer).second || !members.replicaMessage(vote) ||
                   vb.at("shard")!=origin || vb.at("type")!="COMMIT" || vb.at("view")!=v ||
                   vb.at("seq")!=seq || vb.at("digest")!=b.at("digest")) return false;
            }
            return prepares.size()>=2 && commits.size()>=3;
        } catch (...) { return false; }
    }
    bool validOrderClose(const json& c) const {
        try {
            auto fingerprint=hash(c.dump());if(checkedCloses.count(fingerprint)) return true;
            const auto& b=c.at("proposal").at("body");int origin=b.at("shard");
            if(!members.coordinators().count(origin) || !validForeignCertificate(c,origin)) return false;
            const auto& value=b.at("value");const auto& requests=value.at("requests");
            if(!requests.is_array() || !value.at("cst_watermarks").is_object()) return false;
            auto descendants=members.descendants(origin);
            if(value.at("cst_watermarks").size()!=descendants.size()) return false;
            for(int leaf:descendants) {
                const auto& index=value.at("cst_watermarks").at(std::to_string(leaf));
                if(!index.is_number_integer() || index.get<int>()<0) return false;
            }
            if(members.multiLayer()) {
                if(!value.at("cst_round").is_number_integer() || value.at("cst_round").get<int>()<=0) return false;
            } else if(value.contains("cst_round")) return false;
            size_t fields=2+(members.multiLayer()?1:0)+(requests.empty()?0:1);
            if(value.size()!=fields || (requests.empty() && (!members.multiLayer() || value.contains("cst_order_index")))) return false;
            if(!requests.empty() && (!value.at("cst_order_index").is_number_integer() || value.at("cst_order_index").get<int>()<=0)) return false;
            int total=0;std::string group;std::set<std::string> rids,txids;
            for(const auto& req:requests) {
                if(!validRequestForShard(req,origin) || !rids.insert(req.at("body").at("id")).second) return false;
                auto candidate=participantGroup(req);
                if(group.empty()) group=candidate;else if(group!=candidate) return false;
                for(const auto& tx:req.at("body").at("txs")) {
                    if(!txids.insert(tx.at("id")).second) return false;
                    ++total;
                }
            }
            for(int leaf:participants(requests)) if(projection(value,leaf)<=0) return false;
            if(total>crossShardBatchSize) return false;
            if(checkedCloses.size()<10000) checkedCloses.insert(fingerprint);return true;
        } catch(...) { return false; }
    }
    bool validCoordinatorCertificate(const json& c) const {
        try {
            auto fingerprint=hash(c.dump());
            if(checkedOrders.count(fingerprint)) return true;
            if(!validOrderClose(c) || c.at("proposal").at("body").at("value").at("requests").empty()) return false;
            if(checkedOrders.size()<10000) checkedOrders.insert(fingerprint);
            return true;
        } catch(...) {return false;}
    }
    bool validLeafOrder(const json& cert,const json& frontier) const {
        try {
            if(!members.leaves.count(shard) || !validCoordinatorCertificate(cert) || !orderParticipants(cert).count(shard) ||
               state.at("cst_batches").contains(cstKey(cert))) return false;
            const auto& body=cert.at("proposal").at("body");int origin=body.at("shard");const auto& value=body.at("value");
            if(!members.multiLayer()) return frontier.empty() && projection(value,shard)==localProjection(origin)+1;
            int round=value.at("cst_round");
            if(round<state.at("cst_round").get<int>() || !frontier.is_array() || frontier.size()!=members.ancestors(shard).size()) return false;
            std::map<int,json> closes;
            for(const auto& close:frontier) {
                const auto& cb=close.at("proposal").at("body");int co=cb.at("shard");
                if(!members.ancestors(shard).count(co) || !validOrderClose(close) ||
                   cb.at("value").at("cst_round")!=round || !closes.emplace(co,close).second) return false;
            }
            int first=-1;
            for(const auto& [co,close]:closes) {
                const auto& cv=close.at("proposal").at("body").at("value");
                bool has=orderParticipants(close).count(shard)>0;
                bool done=has && state.at("cst_batches").contains(cstKey(close));
                if(done && state.at("cst_batches").at(cstKey(close))!=close.at("proposal").at("body").at("digest")) return false;
                if(projection(cv,shard)!=localProjection(co)+(has&&!done?1:0)) return false;
                if(has && !done && first==-1) first=co;
            }
            return first==origin && closes.at(origin).at("proposal").at("body").at("digest")==body.at("digest");
        } catch(...) {return false;}
    }
    const json& nextCstOrder() const {
        ++cstSelectionLookups;
        if(!cstSelectionDirty) return cstSelection;
        ++cstSelectionRebuilds;
        cstSelection=nullptr;
        std::map<std::pair<int,int>,const json*> candidates;
        for(const auto& [key,message]:pendingCst) {
            if(state.at("cst_batches").contains(key)) continue;
            const auto& cert=message.at("body").at("certificate");const auto& b=cert.at("proposal").at("body");
            int round=members.multiLayer()?b.at("value").at("cst_round").get<int>():b.at("value").at("cst_order_index").get<int>();
            candidates.emplace(std::make_pair(round,b.at("shard").get<int>()),&cert);
        }
        for(const auto& [position,certificate]:candidates) {
            const auto& cert=*certificate;
            json frontier=json::array();
            if(members.multiLayer()) {
                auto row=roundCertificates.find(position.first);if(row==roundCertificates.end()) continue;
                bool complete=true;
                for(int co:members.ancestors(shard)) {
                    if(!row->second.count(co)) {complete=false;break;}
                    frontier.push_back(row->second.at(co));
                }
                if(!complete) continue;
            }
            if(validLeafOrder(cert,frontier)) {
                cstSelection={{"certificate",cert},{"frontier",frontier}};
                break;
            }
        }
        cstSelectionDirty=false;
        return cstSelection;
    }
    static std::string cstKey(const json& cert) {
        const auto& b=cert.at("proposal").at("body");
        return std::to_string(b.at("shard").get<int>())+":"+std::to_string(b.at("seq").get<int>());
    }
    json accessFor(const json& tx,int owner) const {
        for(const auto& access:tx.at("accesses")) if(access.at("shard")==owner) return access;
        throw std::runtime_error("missing participant access");
    }
    std::string participantGroup(const json& request) const {
        std::string group;
        for(const auto& tx:request.at("body").at("txs")) {
            std::set<int> ids;
            for(const auto& id:tx.at("participants")) ids.insert(id.get<int>());
            json normalized=ids;
            auto key=normalized.dump();
            if(group.empty()) group=key;
            else if(group!=key) return "mixed:"+request.at("body").at("id").get<std::string>();
        }
        return group;
    }
    bool validPreparedRecord(const json& record,int origin) const {
        try {
            auto fingerprint=std::to_string(origin)+":"+hash(record.dump());
            if(checkedRecords.count(fingerprint)) return true;
            if(!members.leaves.count(origin) || !validCoordinatorCertificate(record.at("order_certificate")) ||
               !orderParticipants(record.at("order_certificate")).count(origin) ||
               record.at("shard")!=origin || record.at("batch_key")!=cstKey(record.at("order_certificate")) ||
               record.at("order_digest")!=record.at("order_certificate").at("proposal").at("body").at("digest") ||
               !record.at("reads").is_object() || !record.at("writes").is_array()) return false;
            std::set<std::string> keys;
            size_t index=0;
            const auto& requests=record.at("order_certificate").at("proposal").at("body").at("value").at("requests");
            for(const auto& request:requests) for(const auto& tx:request.at("body").at("txs")) {
                if(!touches(tx,origin)) continue;
                auto access=accessFor(tx,origin);
                if(index>=record.at("writes").size()) return false;
                const auto& write=record.at("writes")[index++];
                if(write.at("id")!=tx.at("id") || write.at("tx_digest")!=hash(tx.dump()) ||
                   write.at("key")!=access.at("key") || write.at("value")!=access.at("value") ||
                   write.at("fib")!=expectedFib || !write.at("duplicate").is_boolean()) return false;
                if(!write.at("duplicate").get<bool>()) keys.insert(access.at("key"));
            }
            if(index!=record.at("writes").size() || record.at("reads").size()!=keys.size()) return false;
            for(const auto& key:keys) {
                const auto& read=record.at("reads").at(key);
                if(!read.at("version").is_number_integer() || !read.at("value").is_number_integer() ||
                   (read.at("version").is_number_integer() && !read.at("version").is_number_unsigned() && read.at("version").get<int64_t>()<0) ||
                   (read.at("value").is_number_integer() && !read.at("value").is_number_unsigned() && read.at("value").get<int64_t>()<0) ||
                   !read.at("digest").is_string()) return false;
            }
            if(checkedRecords.size()<10000) checkedRecords.insert(fingerprint);
            return true;
        } catch (...) { return false; }
    }
    bool validPreparedVote(const json& vote) const {
        try {
            const auto& body=vote.at("body"); int origin=body.at("shard");
            return members.replicaMessage(vote) && body.at("type")=="CST_PREPARED" &&
                body.at("batch_key")==vote.at("record").at("batch_key") &&
                body.at("record_digest")==hash(recordPayload(vote.at("record")).dump()) &&
                validPreparedRecord(vote.at("record"),origin);
        } catch (...) { return false; }
    }
    bool validPreparedProof(const json& proof,int origin) const {
        try {
            auto fingerprint=std::to_string(origin)+":"+hash(proof.dump());
            if(checkedProofs.count(fingerprint)) return true;
            const auto& votes=proof.at("votes");
            const auto& record=proof.at("record");
            if(!votes.is_array() || votes.size()!=3 || !validPreparedRecord(record,origin)) return false;
            auto recordDigest=hash(recordPayload(record).dump());
            std::set<int> signers;
            for(const auto& vote:votes) {
                const auto& b=vote.at("body");
                if(b.at("shard")!=origin || b.at("type")!="CST_PREPARED" ||
                   b.at("batch_key")!=record.at("batch_key") || b.at("record_digest")!=recordDigest ||
                   !members.replicaMessage(vote) || vote.contains("record") ||
                   !signers.insert(b.at("from").get<int>()).second) return false;
            }
            if(checkedProofs.size()<10000) checkedProofs.insert(fingerprint);
            return true;
        } catch (...) { return false; }
    }
    json proofRecord(const json& proof) const { return proof.at("record"); }
    json recordPayload(const json& record) const {
        // A certificate's signer subset is transport evidence, not execution identity.
        return {{"shard",record.at("shard")},{"batch_key",record.at("batch_key")},
                {"order_digest",record.at("order_digest")},{"reads",record.at("reads")},
                {"writes",record.at("writes")}};
    }
    json witnessRecord(const json& witness,int owner) const {
        for(const auto& proof:witness.at("proofs"))
            if(proof.at("record").at("shard")==owner) return proof.at("record");
        throw std::runtime_error("execution witness missing participant");
    }
    std::string executionDigest(const json& witness) const {
        json records=json::array();
        for(const auto& proof:witness.at("proofs")) records.push_back(recordPayload(proof.at("record")));
        return hash(records.dump());
    }
    bool validExecutionWitness(const json& witness) const {
        try {
            auto fingerprint=hash(witness.dump());if(checkedWitnesses.count(fingerprint)) return true;
            if(!witness.at("batch_key").is_string() || !witness.at("proofs").is_array() || witness.at("proofs").size()<2) return false;
            int previous=-1;std::set<int> owners;std::string order;json cert;
            std::map<std::string,bool> duplicateFlags;
            for(const auto& proof:witness.at("proofs")) {
                const auto& record=proof.at("record");int owner=record.at("shard");
                if(owner<=previous || !owners.insert(owner).second || !validPreparedProof(proof,owner) || record.at("batch_key")!=witness.at("batch_key")) return false;
                previous=owner;
                if(order.empty()) {order=record.at("order_digest");cert=record.at("order_certificate");}
                else if(record.at("order_digest")!=order) return false;
                for(const auto& write:record.at("writes")) {
                    auto [it,inserted]=duplicateFlags.emplace(write.at("id"),write.at("duplicate").get<bool>());
                    if(!inserted && it->second!=write.at("duplicate").get<bool>()) return false;
                }
            }
            if(owners!=orderParticipants(cert)) return false;
            if(checkedWitnesses.size()<10000) checkedWitnesses.insert(fingerprint);
            return true;
        } catch(...) {return false;}
    }
    bool validValue(const json& value) const {
#ifdef ARBOR_SAGUARO
        return saguaroValidValue(value);
#elif defined(ARBOR_SHARPER)
        return sharperValidValue(value);
#else
        try {
            int n=0;
            std::map<std::string,std::string> requestIds, transactionIds;
            if (!value.at("requests").is_array()) return false;
            for(const auto& e:value.at("requests")) {
                if (!validRequest(e)) return false;
                auto rid=e.at("body").at("id").get<std::string>();
                auto digest=hash(e.at("body").at("txs").dump());
                if(requestIds.count(rid) || (state.at("requests").contains(rid) &&
                   state.at("requests").at(rid).at("txs_hash")!=digest)) return false;
                requestIds[rid]=digest;
                for(const auto& tx:e.at("body").at("txs")) {
                    auto id=tx.at("id").get<std::string>(),d=hash(tx.dump());
                    if(transactionIds.count(id) || (state.at("seen").contains(id) &&
                       state.at("seen").at(id).at("tx_digest")!=d)) return false;
                    transactionIds[id]=d;
                }
                n+=e.at("body").at("txs").size();
            }
            if(isCoordinator()) {
                std::map<std::string,std::string> ids;
                std::string group;
                for(const auto& request:value.at("requests"))
                    for(const auto& tx:request.at("body").at("txs")) {
                        auto id=tx.at("id").get<std::string>(),digest=hash(tx.dump());
                        if((state.at("seen").contains(id) && state.at("seen").at(id).at("tx_digest")!=digest) ||
                           (ids.count(id) && ids.at(id)!=digest)) return false;
                        ids[id]=digest;
                    }
                for(const auto& request:value.at("requests")) {
                    auto candidate=participantGroup(request);
                    if(group.empty()) group=candidate;
                    else if(group!=candidate) return false;
                }
            }
            for(auto it=value.begin();it!=value.end();++it)
                if(it.key()!="requests" && it.key()!="cst_order_index" && it.key()!="cst_orders" && it.key()!="cst_watermarks" && it.key()!="cst_round" && it.key()!="cst_frontier") return false;
            if(value.contains("cst_order_index")) {
                if(!isCoordinator() || value.at("requests").empty() ||
                   value.contains("cst_orders") ||
                   value.at("cst_order_index")!=state.at("cst_order_index").get<int>()+1) return false;
            } else if(isCoordinator() && !value.at("requests").empty()) return false;
            if(isCoordinator()) {
                bool close=value.contains("cst_watermarks");
                if(close) {
                    if(value.at("requests").empty() && !members.multiLayer()) return false;
                    auto ps=participants(value.at("requests"));auto expected=json::object();
                    for(int leaf:members.descendants(shard)) expected[std::to_string(leaf)]=
                        state.at("participant_indices").value(std::to_string(leaf),0)+(ps.count(leaf)?1:0);
                    if(value.at("cst_watermarks")!=expected) return false;
                    if(members.multiLayer() && value.at("cst_round")!=state.at("cst_round").get<int>()+1) return false;
                    if(!members.multiLayer() && value.contains("cst_round")) return false;
                } else if(!value.at("requests").empty() || value.contains("cst_round")) return false;
                if(value.contains("cst_frontier") || value.contains("cst_orders")) return false;
            } else if(value.contains("cst_watermarks") || value.contains("cst_round")) return false;
            if(value.contains("cst_orders")) {
                if(!members.leaves.count(shard) || !value.at("cst_orders").is_array() || value.at("cst_orders").size()!=1 || !value.at("requests").empty()) return false;
                json frontier=value.value("cst_frontier",json::array());
                if(!validLeafOrder(value.at("cst_orders")[0],frontier)) return false;
                for(const auto& req:value.at("cst_orders")[0].at("proposal").at("body").at("value").at("requests"))
                    for(const auto& tx:req.at("body").at("txs")) {
                        auto id=tx.at("id").get<std::string>();
                        if(state.at("cst_seen").contains(id) && state.at("cst_seen").at(id).at("tx_digest")!=hash(tx.dump())) return false;
                        if(touches(tx,shard)) ++n;
                    }
            } else if(value.contains("cst_frontier")) return false;
            return n<=(isCoordinator() && !value.at("requests").empty()
                        ?crossShardBatchSize:batchSize);
        } catch (...) { return false; }
#endif
    }
    // Both methods use exactly the same PBFT vote and certificate validation.
    bool validSignedProposal(const json& e) const {
        try {
            const auto& b=e.at("body"); int v=b.at("view");
            return v>=0 && b.at("from")==v%4 &&
                validVote(e,"PREPREPARE",v,b.at("seq"),hash(b.at("value").dump()));
        } catch (...) { return false; }
    }
    bool validProposal(const json& e) const {
        return validSignedProposal(e) && validValue(e.at("body").at("value"));
    }
    bool validPrepared(const json& p) const {
#ifdef ARBOR_SHARPER
        if(sharperCrossProof(p)) return sharperValidPrepared(p);
#endif
        try {
            const auto& pp=p.at("proposal"); const auto& b=pp.at("body");
            // Prepared evidence describes an earlier state. The proposal's
            // signatures are checked here; its state-dependent checks were
            // performed by the honest replicas before they signed PREPARE.
            if(!validSignedProposal(pp)) return false;
            int v=b.at("view"); std::set<int> voters;
            for(const auto& e:p.at("prepares")) {
                int r=e.at("body").at("from");
                if(r==v%4 || !voters.insert(r).second || !validVote(e,"PREPARE",v,b.at("seq"),b.at("digest"))) return false;
            }
            return voters.size()>=2;
        } catch (...) { return false; }
    }
    bool validCertificate(const json& c) const {
#ifdef ARBOR_SHARPER
        if(sharperCrossProof(c)) return sharperValidCertificate(c);
#endif
        try {
            const auto& b=c.at("proposal").at("body");
            if(!validPrepared(c)) return false;
            std::set<int> voters;
            for(const auto& e:c.at("commits")) {
                if(!voters.insert(e.at("body").at("from").get<int>()).second || !validVote(e,"COMMIT",b.at("view"),b.at("seq"),b.at("digest"))) return false;
            }
            return voters.size()>=3;
        } catch (...) { return false; }
    }
    json preparedProof(const Slot& s) const {
#ifdef ARBOR_SHARPER
        if(sharperCrossValue(s.proposal.at("body").at("value"))) return sharperPreparedProof(s.proposal);
#endif
        json votes=json::array(); const auto& b=s.proposal.at("body");
        for(const auto& [r,p]:s.prepares) if(r!=b.at("view").get<int>()%4 && validVote(p,"PREPARE",b.at("view"),b.at("seq"),b.at("digest"))) votes.push_back(p);
        return {{"proposal",s.proposal},{"prepares",votes}};
    }
    bool validStable(const json& s) const {
        try {
            int seq=s.at("seq");
            if(seq==0) return s.at("state")==genesis() && s.at("proof").empty();
            if(seq<0 || s.at("state").at("seq")!=seq) return false;
            auto digest=StateDigest::fromState(s.at("state")); std::set<int> voters;
            for(const auto& e:s.at("proof")) {
                const auto& b=e.at("body");
                if(!members.replicaMessage(e) || b.at("shard")!=shard || b.at("type")!="CHECKPOINT" ||
                   b.at("seq")!=seq || b.at("digest")!=digest || !voters.insert(b.at("from").get<int>()).second) return false;
            }
            return voters.size()>=3;
        } catch (...) {return false;}
    }
    static json genesis() { json result={{"seq",0},{"chain",hash("arbor-genesis")},{"kv",json::object()},
                {"seen",json::object()},{"requests",json::object()},{"executed",0},{"ordered_cst",0},
                {"cst_batches",json::object()},{"cst_seen",json::object()},{"leaf_ordered_cst",0},{"last_cst_seq",0},
                {"cst_finalized",json::object()},{"cst_orders",json::object()},
                {"cst_order_index",0},{"cst_round",0},{"cst_indices",json::object()},
                {"participant_indices",json::object()},{"cst_rounds",json::object()}};
#ifdef ARBOR_SAGUARO
        return saguaroGenesis(std::move(result));
#elif defined(ARBOR_SHARPER)
        return sharperGenesis(std::move(result));
#else
        return result;
#endif
    }
    bool validVC(const json& e,int v) const {
        try {
            const auto& b=e.at("body");
            if(!members.replicaMessage(e) || b.at("shard")!=shard || b.at("type")!="VIEW_CHANGE" || b.at("view")!=v || !validStable(b.at("stable"))) return false;
#ifdef ARBOR_SHARPER
            if(!sharperValidRecovery(e)) return false;
#endif
            int h=b.at("stable").at("seq"); std::set<int> sequences;
            if(b.at("prepared").size()>size_t(window)) return false;
            for(const auto& p:b.at("prepared")) {
                const auto& pb=p.at("proposal").at("body"); int seq=pb.at("seq");
                if(!validPrepared(p) || (p.contains("commits") && !validCertificate(p)) || pb.at("view").get<int>()>=v || seq<=h || seq>h+window || !sequences.insert(seq).second) return false;
            }
            return true;
        } catch (...) {return false;}
    }
    // Returns the highest certified checkpoint and the deterministic recovery
    // sequence required by the highest prepared view for each sequence number.
    std::pair<json,std::map<int,json>> recovery(const json& vcs,int v) const {
        if(vcs.size()!=3) throw std::runtime_error("new view needs exactly three distinct view changes");
        std::set<int> voters; json best={{"seq",0},{"state",genesis()},{"proof",json::array()}};
        std::map<int,json> selected;
        for(const auto& e:vcs) {
            if(!validVC(e,v) || !voters.insert(e.at("body").at("from").get<int>()).second) throw std::runtime_error("invalid view change");
            const auto& s=e.at("body").at("stable"); if(s.at("seq").get<int>()>best.at("seq").get<int>()) best=s;
            for(const auto& p:e.at("body").at("prepared")) {
                const auto& b=p.at("proposal").at("body"); int n=b.at("seq");
                if(!selected.count(n) || b.at("view").get<int>()>selected[n].at("proposal").at("body").at("view").get<int>()) selected[n]=p;
            }
        }
        std::map<int,json> values;
#ifdef ARBOR_SHARPER
        sharperAugmentRecovery(selected,vcs,best.at("seq"));
#endif
        int h=best.at("seq"); int high=selected.empty()?h:std::max(h,selected.rbegin()->first);
        if(high>h+window) throw std::runtime_error("view recovery outside window");
        for(int n=h+1;n<=high;++n) values[n]=selected.count(n)?selected.at(n).at("proposal").at("body").at("value"):json{{"requests",json::array()}};
        return {best,values};
    }
    void startViewChange(int v) {
        if(v<=view || v<targetView) return;
        targetView=v; changing=true;
        json ps=json::array();
        for(const auto& [n,p]:preparedHistory) if(n>stableSeq) ps.push_back(p);
        myViewChange=make("VIEW_CHANGE",{{"stable",{{"seq",stableSeq},{"state",stableState},{"proof",stableProof}}},{"prepared",ps}},v);
#ifdef ARBOR_SHARPER
        auto fields=myViewChange.at("body");fields["sharper_recovery"]=sharperRecoveryEvidence();
        myViewChange=sign(fields,privateKey);
#endif
        broadcast(myViewChange); lastProgress=Clock::now();
        log("view_change_started",{{"target_view",v}});
    }
    void maybeNewView(int v) {
        if(v%4!=me || v<targetView || v<=view || viewChanges[v].size()<3) return;
        json vcs=json::array();
        // Include this primary's own proof, as required by PBFT.
        if(!viewChanges[v].count(me)) { if(v>targetView || !changing) startViewChange(v); return; }
        vcs.push_back(viewChanges[v].at(me));
        for(const auto& [r,e]:viewChanges[v]) if(r!=me && vcs.size()<3) vcs.push_back(e);
        auto [checkpoint,values]=recovery(vcs,v); (void)checkpoint;
        json proposals=json::array();
        for(const auto& [n,value]:values) proposals.push_back(make("PREPREPARE",{{"seq",n},{"digest",hash(value.dump())},{"value",value}},v));
        lastNewView=make("NEW_VIEW",{{"changes",vcs},{"proposals",proposals}},v);
        broadcast(lastNewView);
    }
    void installStable(const json& s) {
        int h=s.at("seq"); if(h<stableSeq) return;
        cstSelectionDirty=true;
        bool restored=h>applied;
        if(restored) {
            state=s.at("state"); applied=h;
            refreshDigests(true);
            log("state_sync",{{"seq",h}});
            pendingLocalAckQcs.clear();
            if(members.leaves.count(shard)) for(auto it=state["cst_finalized"].begin();it!=state["cst_finalized"].end();++it)
                if(!ackProofs.count(it.key()) || !ackProofs.at(it.key()).count(shard)) pendingLocalAckQcs.insert(it.key());
        }
        stableSeq=h; stableState=s.at("state"); stableProof=s.at("proof");
        for(auto it=slots.begin();it!=slots.end();) it=it->first<=h?slots.erase(it):std::next(it);
        for(auto it=preparedHistory.begin();it!=preparedHistory.end();) it=it->first<=h?preparedHistory.erase(it):std::next(it);
        for(auto it=certificates.begin();it!=certificates.end();) it=it->first<=h?certificates.erase(it):std::next(it);
        for(auto it=snapshots.begin();it!=snapshots.end();) it=it->first<h?snapshots.erase(it):std::next(it);
        for(auto it=snapshotDigests.begin();it!=snapshotDigests.end();) it=it->first<h?snapshotDigests.erase(it):std::next(it);
        for(auto it=checkpointVotes.begin();it!=checkpointVotes.end();) it=it->first<h?checkpointVotes.erase(it):std::next(it);
        if(restored) {
#ifdef ARBOR_SAGUARO
            saguaroRestore();
#elif defined(ARBOR_SHARPER)
            sharperRestore();
#endif
            rebuildPendingIndex();
        }
        for(auto it=pendingCst.begin();it!=pendingCst.end();)
            it=state["cst_batches"].contains(it->first)?pendingCst.erase(it):std::next(it);
        for(auto it=stagedRecords.begin();it!=stagedRecords.end();)
            it=state["cst_finalized"].contains(it->first)?stagedRecords.erase(it):std::next(it);
        for(auto it=slotWitnesses.begin();it!=slotWitnesses.end();)
            it=it->first<=h?slotWitnesses.erase(it):std::next(it);
        drainDeferred();requestResultProofs();
    }
    void acceptNewView(const json& e) {
        const auto& b=e.at("body"); int v=b.at("view");
        if(v<=view || v<targetView || b.at("from")!=v%4) return;
        auto [checkpoint,values]=recovery(b.at("changes"),v);
        if(!std::any_of(b.at("changes").begin(),b.at("changes").end(),[&](const json& c){return c.at("body").at("from")==v%4;})) throw std::runtime_error("missing primary view change");
        if(b.at("proposals").size()!=values.size()) throw std::runtime_error("incomplete recovery");
        auto it=values.begin();
        for(const auto& p:b.at("proposals")) {
            const auto& pb=p.at("body");
            if(!validSignedProposal(p) || pb.at("view")!=v || pb.at("seq")!=it->first || pb.at("value")!=it->second ||
               (certificates.count(it->first) && certificates.at(it->first).at("proposal").at("body").at("digest")!=pb.at("digest")))
                throw std::runtime_error("unsafe new-view proposal");
            ++it;
        }
        std::map<int,json> committed;
        for(const auto& change:b.at("changes")) for(const auto& proof:change.at("body").at("prepared")) {
            int n=proof.at("proposal").at("body").at("seq");
            if(n<=checkpoint.at("seq").get<int>() || !proof.contains("commits")) continue;
            if(!validCertificate(proof) || !values.count(n) ||
               proof.at("proposal").at("body").at("digest")!=hash(values.at(n).dump()) ||
               (certificates.count(n) && certificates.at(n).at("proposal").at("body").at("digest")!=
                    proof.at("proposal").at("body").at("digest"))) throw std::runtime_error("conflicting new-view commit evidence");
            committed.emplace(n,proof);
        }
        installStable(checkpoint); slots.clear();
        for(const auto& [n,proof]:committed) if(n>applied) {certificates.emplace(n,proof);preparedHistory[n]=proof;}
        view=v; targetView=v; changing=false; viewCount++; lastProgress=Clock::now(); lastNewView=e;
#ifdef ARBOR_SHARPER
        sharperInstallRecovery(b.at("changes"));
#endif
        lastForwardHeartbeat=Clock::now();
        for(auto i=viewChanges.begin();i!=viewChanges.end();) i=i->first<=v?viewChanges.erase(i):std::next(i);
        log("new_view_installed",{{"primary",view%4}});
        for(const auto& p:b.at("proposals")) acceptProposal(p,true);
        // The selected three view changes can omit the only replica holding
        // a full commit proof. Share it as evidence, without another PBFT vote.
        json waiting=json::array();
        for(const auto& [n,proof]:certificates) if(n>applied) waiting.push_back(proof);
        if(!waiting.empty()) {
            json fields={{"stable",{{"seq",stableSeq},{"state",stableState},{"proof",stableProof}}},
                {"certificates",waiting},{"execution_witnesses",json::array()}};
            broadcast(make("SYNC",fields));
        }
    }
    void acceptProposal(const json& e,bool recovered=false) {
#ifdef ARBOR_SHARPER
        if(sharperCrossValue(e.at("body").at("value"))) {sharperAcceptAssignment(e,recovered);return;}
#endif
        const auto& b=e.at("body"); int n=b.at("seq");
        if(changing || b.at("view")!=view || n<=applied || n>stableSeq+window) return;
        // A certified future slot can expose a missing prefix even when its
        // state-dependent frontier is not yet admissible. Request catchup,
        // without treating this recovery evidence as a view-change timeout.
        if(n>applied+1 && validSignedProposal(e)) catchupTarget=std::max(catchupTarget,n-1);
        auto existing=slots.find(n);
        if(existing!=slots.end() && !existing->second.proposal.is_null()) {
            if(existing->second.proposal.at("body").at("digest")!=b.at("digest")) rejected++;
            return;
        }
        if(!(recovered?validSignedProposal(e):validProposal(e))) {
            if(!recovered && validSignedProposal(e))
                log("invalid_preprepare_value",{{"seq",n},{"digest",b.at("digest")}});
            return;
        }
        auto& s=slots[n];
        // A committed sequence may be replayed in a new view, never replaced.
        if(certificates.count(n) && certificates[n].at("proposal").at("body").at("digest")!=b.at("digest")) throw std::runtime_error("conflicting committed sequence");
        s.proposal=e;
        if(certificates.count(n)) {
            s.committed=true;applyReady();return;
        }
        lastProgress=Clock::now();
        log("preprepare",{{"seq",n},{"digest",b.at("digest")},
                          {"frame_bytes",e.dump().size()}});
        if(me!=view%4) broadcast(make("PREPARE",{{"seq",n},{"digest",b.at("digest")}}));
        advance(n);
    }
    void advance(int n) {
        auto& s=slots.at(n); if(s.proposal.is_null()) return;
#ifdef ARBOR_SHARPER
        if(sharperCrossValue(s.proposal.at("body").at("value"))) {sharperAdvance(n);return;}
#endif
        if(s.committed) return;
        auto proof=preparedProof(s); const auto& b=s.proposal.at("body");
        if(!s.prepared && proof.at("prepares").size()>=2) {
            s.prepared=true; preparedHistory[n]=proof;
            if(!s.commitSent) { s.commitSent=true; broadcast(make("COMMIT",{{"seq",n},{"digest",b.at("digest")}})); }
            log("prepared",{{"seq",n},{"digest",b.at("digest")}});
        }
        json votes=json::array();
        for(const auto& [r,e]:s.commits) { (void)r; if(validVote(e,"COMMIT",view,n,b.at("digest"))) votes.push_back(e); }
        if(s.prepared && votes.size()>=3 && !s.committed) {
            s.committed=true; proof["commits"]=votes; certificates[n]=proof;preparedHistory[n]=proof;
            log("committed_local",{{"seq",n},{"digest",b.at("digest")}});
            applyReady();
        }
    }
    void reply(const json& req,const json& results) {
        const auto& b=req.at("body"); const auto& ep=b.at("reply");
        auto e=make("REPLY",{{"request",b.at("id")},{"results",results}});
        net.send({ep.at("host"),ep.at("port")},e,0);
    }
    void replyConflict(const json& req,const std::string& error="id_conflict") {
        json results=json::array();
        for(const auto& tx:req.at("body").at("txs"))
            results.push_back({{"id",tx.at("id")},{"error",error}});
        reply(req,results);
    }
    bool replyCompletedTransactions(const json& req) {
#ifdef ARBOR_SHARPER
        return sharperReplyCompleted(req);
#endif
        json results=json::array();
        for(const auto& tx:req.at("body").at("txs")) {
            auto id=tx.at("id").get<std::string>();
            if(state["seen"].contains(id) && state["seen"].at(id).at("tx_digest")!=hash(tx.dump())) {
                replyConflict(req);rejected++;return true;
            }
        }
        for(const auto& tx:req.at("body").at("txs")) {
            auto id=tx.at("id").get<std::string>();
            if(!state["seen"].contains(id)) return false;
            if(isCoordinator() && !completedTxResults.count(id)) return false;
            json result=completedTxResults.count(id)?completedTxResults.at(id):state["seen"].at(id).at("result");
            result["kind"]="duplicate"; results.push_back(result);
        }
        reply(req,results); duplicates+=results.size(); return true;
    }
    bool awaitingResult(const std::string& id) const {
#ifdef ARBOR_SHARPER
        return state.at("seen").contains(id) && state.at("seen").at(id).contains("sharper_batch") && !completedTxResults.count(id);
#else
        return isCoordinator() &&
            state.at("seen").contains(id) && !completedTxResults.count(id);
#endif
    }
    void erasePending(const std::string& rid) {
        auto it=pending.find(rid);if(it==pending.end()) return;
        for(const auto& tx:it->second.at("body").at("txs")) {
            auto id=tx.at("id").get<std::string>();auto owner=pendingTx.find(id);
            if(owner!=pendingTx.end() && owner->second.second==rid) {
                pendingTx.erase(owner);changedTx.insert(id);
            }
        }
        pending.erase(it);
    }
    void deferRequest(const json& request) {
        auto rid=request.at("body").at("id").get<std::string>();
        if(!deferredRequests.emplace(rid,request).second) return;
        for(const auto& tx:request.at("body").at("txs"))
            deferredByTx[tx.at("id").get<std::string>()].insert(rid);
    }
    json takeDeferred(const std::string& rid) {
        auto request=deferredRequests.at(rid);
        for(const auto& tx:request.at("body").at("txs")) {
            auto id=tx.at("id").get<std::string>();auto it=deferredByTx.find(id);
            if(it==deferredByTx.end()) continue;
            it->second.erase(rid);if(it->second.empty()) deferredByTx.erase(it);
        }
        deferredRequests.erase(rid);return request;
    }
    void rebuildPendingIndex() {
        // Only snapshot catchup changes state without visiting committed txs.
        // Re-admit whole signed requests against that restored state.
        dedupIndexRebuilds++;
        std::vector<json> requests;
        for(const auto& [rid,request]:pending) {(void)rid;requests.push_back(request);}
        for(const auto& [rid,request]:deferredRequests) {(void)rid;requests.push_back(request);}
        pending.clear();pendingTx.clear();deferredRequests.clear();deferredByTx.clear();changedTx.clear();
        for(const auto& request:requests) handle(request);
    }
    void drainDeferred() {
        // Commit/completion releases owners and wakes only requests indexed by
        // affected tx IDs. Unrelated backlog is never rehashed or scanned.
        while(!changedTx.empty()) {
            auto changed=std::move(changedTx);changedTx.clear();
            std::set<std::string> owners,waiters;
            for(const auto& id:changed) {
                auto owner=pendingTx.find(id);
                if(owner!=pendingTx.end()) owners.insert(owner->second.second);
                auto waiting=deferredByTx.find(id);
                if(waiting!=deferredByTx.end()) waiters.insert(waiting->second.begin(),waiting->second.end());
            }
            for(const auto& rid:owners) {
                auto it=pending.find(rid);if(it==pending.end()) continue;
                dedupPendingChecks++;
                if(replyCompletedTransactions(it->second)) {erasePending(rid);continue;}
                bool blocked=false;
                for(const auto& tx:it->second.at("body").at("txs")) {
                    auto id=tx.at("id").get<std::string>();
                    if(awaitingResult(id)) blocked=true;
                    auto owner=pendingTx.find(id);
                    if(state["seen"].contains(id) && owner!=pendingTx.end() && owner->second.second==rid) {
                        pendingTx.erase(owner);changedTx.insert(id);
                    }
                }
                if(blocked) {auto request=it->second;erasePending(rid);deferRequest(request);}
            }
            for(const auto& rid:waiters) {
                auto it=deferredRequests.find(rid);if(it==deferredRequests.end()) continue;
                dedupWaiterChecks++;
                if(replyCompletedTransactions(it->second)) {takeDeferred(rid);continue;}
                bool blocked=false;
                for(const auto& tx:it->second.at("body").at("txs")) {
                    auto id=tx.at("id").get<std::string>();
                    if(pendingTx.count(id) || awaitingResult(id)) {blocked=true;break;}
                }
                if(!blocked) handle(takeDeferred(rid));
            }
        }
    }
    void forwardCstOrder(const json& cert,const json& value,bool retry=false) {
        if(!isCoordinator() || !value.contains("cst_watermarks")) return;
        auto destinations=orderParticipants(cert);
        if(!destinations.empty() && !retry) {
            orderCertificates[cstKey(cert)]=cert;
            outstandingOrders[value.at("cst_order_index").get<int>()]=cert;
        }
        if(members.multiLayer()) {
            roundCertificates[value.at("cst_round").get<int>()][shard]=cert;
            cstSelectionDirty=true;
        }
        if(!isForward()) return;
        if(members.multiLayer()) {
            for(int leaf:members.descendants(shard)) sendShard(leaf,make("CST_ROUND_CLOSE",{{"target",leaf},{"certificate",cert}}));
        } else for(int leaf:destinations) sendShard(leaf,make("CST_ORDER",{{"target",leaf},{"certificate",cert}}));
        log("cst_order_forwarded",{{"coordinator_seq",cert.at("proposal").at("body").at("seq")},{"destinations",destinations},{"forward",me},{"retry",retry}});
    }
    void requestRound(int round) {
        desiredRound=std::max(desiredRound,round);
        if(!isForward()) return;
        for(int co:members.coordinators()) if(co!=shard)
            sendShard(co,make("CST_ROUND_REQUEST",{{"target",co},{"round",round}}));
    }
    bool rememberRound(const json& cert) {
        if(!validOrderClose(cert)) return false;
        const auto& b=cert.at("proposal").at("body");int origin=b.at("shard"),round=b.at("value").at("cst_round");
        if(members.leaves.count(shard)?!members.ancestors(shard).count(origin):origin!=shard) return false;
        if(roundCertificates.size()>=100000 && !roundCertificates.count(round)) return false;
        auto [it,inserted]=roundCertificates[round].emplace(origin,cert);
        if(inserted) cstSelectionDirty=true;
        if(!inserted && it->second.at("proposal").at("body").at("digest")!=b.at("digest")) return false;
        if(members.leaves.count(shard) && orderParticipants(cert).count(shard) && !state["cst_batches"].contains(cstKey(cert))) {
            if(pendingCst.size()>=10000 && !pendingCst.count(cstKey(cert))) return false;
            // This local envelope is only a cache; its certificate is independently authenticated.
            // No network sender may use it as a signed replica message.
            if(pending.empty() && pendingCst.empty()) {lastProgress=Clock::now();batchStart=Clock::now();}
            pendingCst.emplace(cstKey(cert),json{{"body",{{"certificate",cert}}}});
            cstSelectionDirty=true;
        }
        return true;
    }
    void queryRounds() {
        if(!members.multiLayer() || !isForward()) return;
        if(members.leaves.count(shard)) {
            if(pendingCst.empty() && stagedRecords.empty()) return;
            std::set<int> rounds;
            for(const auto& [key,msg]:pendingCst) {
                (void)key;rounds.insert(msg.at("body").at("certificate").at("proposal").at("body").at("value").at("cst_round").get<int>());
                if(rounds.size()>=32) break;
            }
            for(int co:members.ancestors(shard)) sendShard(co,make("CST_ROUND_QUERY",
                {{"target",co},{"rounds",rounds},{"leaf",shard},{"after_index",localProjection(co)}}));
        } else if(roundCertificates.size()<state.at("cst_rounds").size()) {
            json missing=json::array();
            for(auto it=state.at("cst_rounds").begin();it!=state.at("cst_rounds").end();++it) {
                int round=std::stoi(it.key());
                if(!roundCertificates.count(round)) missing.push_back(round);
                if(missing.size()>=32) break;
            }
            if(!missing.empty()) broadcast(make("CST_ROUND_QUERY",{{"target",shard},{"rounds",missing},{"leaf",-1},{"after_index",-1}}));
        }
    }
    void sendPrepared(const std::string& key) {
        if(!members.leaves.count(shard) || !stagedRecords.count(key)) return;
        const auto& record=stagedRecords.at(key);
        auto vote=make("CST_PREPARED",{{"batch_key",key},{"record_digest",hash(recordPayload(record).dump())}});
        vote["record"]=record;sendTo(shard,view%4,vote);
    }
    json dependencyProof(const std::string& key,int owner) const {
        if(preparedProofs.count(key) && preparedProofs.at(key).count(owner)) return preparedProofs.at(key).at(owner);
        if(executionWitnesses.count(key)) for(const auto& proof:executionWitnesses.at(key).at("proofs"))
            if(proof.at("record").at("shard")==owner) return proof;
        return nullptr;
    }
    void forwardPrepared(const std::string& key,bool retry=false) {
        if(!isForward()) return;
        auto proof=dependencyProof(key,shard);if(proof.is_null()) return;
        auto token=key+":"+std::to_string(view);
        if(!retry && !forwardedPrepared.insert(token).second) return;
        broadcast(make("CST_PREPARED_QC",{{"target",shard},{"proof",proof}}));
        for(int leaf:orderParticipants(proof.at("record").at("order_certificate"))) if(leaf!=shard)
            sendShard(leaf,make("CST_PREPARED_QC",{{"target",leaf},{"proof",proof}}));
        log("cst_prepared_forwarded",{{"batch_key",key},{"forward",me},{"signers",proof.at("votes").size()}});
    }
    void stageCst(const json& cert) {
        auto key=cstKey(cert);if(stagedRecords.count(key)) return;
        auto before=Clock::now();
        json record={{"shard",shard},{"batch_key",key},
                     {"order_digest",cert.at("proposal").at("body").at("digest")},
                     {"order_certificate",cert},{"reads",json::object()},{"writes",json::array()}};
        for(const auto& request:cert.at("proposal").at("body").at("value").at("requests"))
            for(const auto& tx:request.at("body").at("txs")) {
                if(!touches(tx,shard)) continue;
                auto access=accessFor(tx,shard);auto localKey=access.at("key").get<std::string>();
                std::string id=tx.at("id");bool duplicate=state["cst_seen"].contains(id);
                if(duplicate && state["cst_seen"].at(id).at("tx_digest")!=hash(tx.dump()))
                    throw std::runtime_error("cross-shard transaction ID reused with different content");
                if(!duplicate && !record["reads"].contains(localKey))
                    record["reads"][localKey]=state["kv"].contains(localKey)?state["kv"].at(localKey):initialAccount();
                record["writes"].push_back({{"id",tx.at("id")},{"tx_digest",hash(tx.dump())},
                    {"key",localKey},{"value",access.at("value")},
                    {"fib",duplicate?expectedFib:fibonacci()},{"duplicate",duplicate}});
            }
        stagedRecords[key]=record;
        executionNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-before).count();
        log("cst_staged",{{"batch_key",key},{"transactions",record["writes"].size()}});
        sendPrepared(key);
    }
    json availableWitness(const std::string& key) const {
        if(executionWitnesses.count(key)) return executionWitnesses.at(key);
        if(!preparedProofs.count(key) || preparedProofs.at(key).empty()) return nullptr;
        auto ps=orderParticipants(preparedProofs.at(key).begin()->second.at("record").at("order_certificate"));
        json proofs=json::array();
        for(int leaf:ps) {
            if(!preparedProofs.at(key).count(leaf)) return nullptr;
            proofs.push_back(preparedProofs.at(key).at(leaf));
        }
        return {{"batch_key",key},{"proofs",proofs}};
    }
    bool importWitness(const json& witness) {
        if(!validExecutionWitness(witness) || !orderParticipants(witness.at("proofs")[0].at("record").at("order_certificate")).count(shard)) return false;
        auto key=witness.at("batch_key").get<std::string>();
        if(state["cst_finalized"].contains(key)) {
            if(executionDigest(witness)!=state["cst_finalized"].at(key).at("execution_digest")) return false;
            executionWitnesses[key]=witness;return true;
        }
        if(preparedProofs.size()>=10000 && !preparedProofs.count(key)) return false;
        for(const auto& proof:witness.at("proofs")) {
            int owner=proof.at("record").at("shard");
            auto existing=dependencyProof(key,owner);
            if(!existing.is_null() && recordPayload(existing.at("record"))!=recordPayload(proof.at("record"))) return false;
        }
        for(const auto& proof:witness.at("proofs")) preparedProofs[key][proof.at("record").at("shard").get<int>()]=proof;
        return true;
    }
    void requestDependencies(const std::string& key) {
        sendTo(shard,view%4,make("CST_DEPENDENCY_QUERY",{{"target",shard},{"batch_keys",json::array({key})}}));
        if(!isForward() || !stagedRecords.count(key)) return;
        for(int leaf:orderParticipants(stagedRecords.at(key).at("order_certificate"))) if(leaf!=shard)
            sendShard(leaf,make("CST_DEPENDENCY_QUERY",{{"target",leaf},{"batch_keys",json::array({key})}}));
    }
    void finalizeCst(const json& witness) {
        auto key=witness.at("batch_key").get<std::string>();auto mine=witnessRecord(witness,shard);
        if(recordPayload(mine)!=recordPayload(stagedRecords.at(key))) throw std::runtime_error("local execution record differs from certified dependency");
        std::map<int,json> working;std::map<std::string,bool> flags;
        for(const auto& proof:witness.at("proofs")) {
            const auto& record=proof.at("record");working[record.at("shard").get<int>()]=record.at("reads");
            for(const auto& write:record.at("writes")) flags[write.at("id")]=write.at("duplicate").get<bool>();
        }
        json localWrites=json::array();
        const auto& requests=mine.at("order_certificate").at("proposal").at("body").at("value").at("requests");
        for(const auto& request:requests) for(const auto& tx:request.at("body").at("txs")) {
            if(flags.at(tx.at("id").get<std::string>())) continue;
            std::map<int,json> old,next;
            for(auto participant:tx.at("participants")) {
                int owner=participant;auto access=accessFor(tx,owner);
                old[owner]=working.at(owner).at(access.at("key").get<std::string>());
            }
            for(const auto& [owner,previous]:old) {
                auto access=accessFor(tx,owner);uint64_t result=access.at("value").get<uint64_t>()+expectedFib;
                std::string dependency=previous.dump();
                for(const auto& [remote,account]:old) if(remote!=owner) {result+=account.at("value").get<uint64_t>();dependency+=account.dump();}
                next[owner]={{"version",previous.at("version").get<uint64_t>()+1},{"value",result},{"fib",expectedFib},
                    {"digest",hash(dependency+tx.dump()+std::to_string(owner))}};
            }
            for(const auto& [owner,account]:next) working[owner][accessFor(tx,owner).at("key").get<std::string>()]=account;
            if(!touches(tx,shard)) continue;
            auto access=accessFor(tx,shard);
            localWrites.push_back({{"id",tx.at("id")},{"key",access.at("key")},{"state",next.at(shard)}});
            state["cst_seen"][tx.at("id").get<std::string>()]={{"tx_digest",hash(tx.dump())},{"coordinator_batch",key},{"committed",true}};
            stateDigest.markEntry("cst_seen",tx.at("id").get<std::string>());
            state["executed"]=state["executed"].get<uint64_t>()+1;
            state["leaf_ordered_cst"]=state["leaf_ordered_cst"].get<uint64_t>()+1;
        }
        for(auto it=working.at(shard).begin();it!=working.at(shard).end();++it) {
            state["kv"][it.key()]=it.value();stateDigest.markEntry("kv",it.key());kvDirty=true;
        }
        state["cst_finalized"][key]={{"order_digest",mine.at("order_digest")},{"execution_digest",executionDigest(witness)},{"result_digest",hash(localWrites.dump())}};
        stateDigest.markEntry("cst_finalized",key);
        executionWitnesses[key]=witness;stagedRecords.erase(key);preparedProofs.erase(key);
        if(!ackProofs.count(key) || !ackProofs.at(key).count(shard)) pendingLocalAckQcs.insert(key);
        for(auto it=preparedVotes.begin();it!=preparedVotes.end();) it=it->first.rfind(key+"|",0)==0?preparedVotes.erase(it):std::next(it);
        log("cst_executed",{{"batch_key",key},{"transactions",localWrites.size()}});
    }
    void sendAck(const std::string& key) {
        if(!state["cst_finalized"].contains(key)) return;
        const auto& done=state["cst_finalized"].at(key);
        auto ack=make("CST_ACK",{{"batch_key",key},{"target",batchOrigin(key)},
            {"order_digest",done.at("order_digest")},{"execution_digest",done.at("execution_digest")},
            {"result_digest",done.at("result_digest")}});
        sendTo(shard,view%4,ack);
    }
    bool validAckProof(const json& proof,int origin,const std::string& key,const std::string& order) const {
        try {
            if(!members.leaves.count(origin) || !proof.is_array() || proof.size()!=3) return false;
            std::set<int> signers;std::string result,execution;
            for(const auto& vote:proof) {
                const auto& b=vote.at("body");
                if(!members.replicaMessage(vote) || b.at("type")!="CST_ACK" || b.at("shard")!=origin ||
                   b.at("target")!=batchOrigin(key) || b.at("batch_key")!=key || b.at("order_digest")!=order ||
                   !b.at("execution_digest").is_string() || !b.at("result_digest").is_string() ||
                   order.size()!=64 || b.at("execution_digest").get<std::string>().size()!=64 ||
                   b.at("result_digest").get<std::string>().size()!=64 || !signers.insert(b.at("from").get<int>()).second) return false;
                if(signers.size()==1) {result=b.at("result_digest");execution=b.at("execution_digest");}
                else if(result!=b.at("result_digest") || execution!=b.at("execution_digest")) return false;
            }
            return true;
        } catch(...) {return false;}
    }
    void forwardAck(const std::string& key,bool retry=false) {
        if(!isForward() || !ackProofs.count(key) || !ackProofs.at(key).count(shard)) return;
        auto token=key+":"+std::to_string(view);
        if(!retry && !forwardedAcks.insert(token).second) return;
        auto proof=ackProofs.at(key).at(shard);
        broadcast(make("CST_ACK_QC",{{"target",shard},{"proof",proof}}));
        sendShard(batchOrigin(key),make("CST_ACK_QC",{{"target",batchOrigin(key)},{"proof",proof}}));
        completionResendNeeded.erase(key);pendingLocalAckQcs.erase(key);
        log("cst_ack_forwarded",{{"batch_key",key},{"forward",me},{"signers",proof.size()}});
    }
    void requestResultProofs() {
        if(!isCoordinator() || completedCstBatches.size()==state["cst_orders"].size()) return;
        // Restore original results before any later batch that references
        // them as duplicates. JSON object keys are not numeric sequence order.
        std::map<int,std::string> missing;
        for(auto it=state["cst_orders"].begin();it!=state["cst_orders"].end();++it)
            if(!completedCstBatches.count(it.key())) missing.emplace(it.value().at("order_index").get<int>(),it.key());
        json keys=json::array();
        for(const auto& [index,key]:missing) {(void)index;if(keys.size()>=32) break;keys.push_back(key);}
        if(!keys.empty()) broadcast(make("CST_RESULT_QUERY",{{"batch_keys",keys}}));
    }
    void maybeCompleteBatch(const std::string& key,bool notifyClient=true) {
        if(!state["cst_orders"].contains(key) || completedCstBatches.count(key)) return;
        const auto& order=state["cst_orders"].at(key);
        std::map<int,std::string> digests;std::string execution;
        for(int leaf:participants(order.at("requests"))) {
            if(!ackProofs.count(key) || !ackProofs.at(key).count(leaf)) return;
            const auto& proof=ackProofs.at(key).at(leaf);
            if(!validAckProof(proof,leaf,key,order.at("order_digest"))) return;
            auto d=proof[0].at("body").at("execution_digest").get<std::string>();
            if(execution.empty()) execution=d;else if(execution!=d) {rejected++;return;}
            digests[leaf]=proof[0].at("body").at("result_digest");
        }
        std::string resultDigests;for(const auto& [leaf,digest]:digests) {(void)leaf;resultDigests+=digest;}
        size_t checkIndex=0;
        for(const auto& request:order.at("requests")) for(const auto& tx:request.at("body").at("txs"))
            if(order.at("duplicates")[checkIndex++].get<bool>() && !completedTxResults.count(tx.at("id").get<std::string>())) return;
        size_t index=0;
        for(const auto& request:order.at("requests")) {
            std::string rid=request.at("body").at("id");
            if(completedCstResults.count(rid)) {index+=request.at("body").at("txs").size();continue;}
            json results=json::array();
            for(const auto& tx:request.at("body").at("txs")) {
                bool duplicate=order.at("duplicates")[index++];
                json result={{"id",tx.at("id")},{"kind",duplicate?"duplicate":"executed"},
                    {"digest",hash(tx.at("id").get<std::string>()+order.at("order_digest").get<std::string>()+
                        execution+resultDigests)}};
                if(duplicate) {result=completedTxResults.at(tx.at("id").get<std::string>());result["kind"]="duplicate";}
                else completedTxResults.emplace(tx.at("id"),result);
                changedTx.insert(tx.at("id").get<std::string>());
                results.push_back(result);
            }
            completedCstResults[rid]=results;completedCstTransactions+=results.size();
            if(notifyClient) reply(request,results);
        }
        completedCstBatches.insert(key);activeAckBatches.erase(key);
        for(auto it=outstandingOrders.begin();it!=outstandingOrders.end();)
            it=cstKey(it->second)==key?outstandingOrders.erase(it):std::next(it);
        log("cst_complete",{{"batch_key",key}});drainDeferred();
    }
    // Shared application execution keeps the comparison workload identical.
    void applyClientRequests(const json& requests,int n) {
        for(const auto& req:requests) {
            const auto& rb=req.at("body"); std::string rid=rb.at("id");
            auto requestHash=hash(rb.at("txs").dump());
            json results=json::array();
            if(state["requests"].contains(rid) && state["requests"][rid]["txs_hash"]!=requestHash) {
                for(const auto& tx:rb.at("txs")) results.push_back({{"id",tx.at("id")},{"error","request_id_conflict"}});
                if(members.leaves.count(shard)) reply(req,results);
                erasePending(rid); continue;
            }
            for(const auto& tx:rb.at("txs")) {
                std::string id=tx.at("id"); auto d=hash(tx.dump());
                if(state["seen"].contains(id)) {
                    duplicates++;
                    if(state["seen"][id]["tx_digest"]!=d) results.push_back({{"id",id},{"error","id_conflict"}});
                    else {auto result=state["seen"][id]["result"];result["kind"]="duplicate";results.push_back(result);}
                    continue;
                }
                json result={{"id",id},{"seq",n},{"kind",members.leaves.count(shard)?"executed":"ordered_only"}};
                if(members.leaves.count(shard)) {
                    uint64_t a=0,b=1;
                    int loops=members.config.at("execution").at("fib_iterations");
                    for(int k=0;k<loops;++k) { uint64_t next=a+b; a=b; b=next; }
                    std::string key=tx.at("key");
                    json previous=state["kv"].contains(key)?state["kv"][key]:json{{"version",0},{"digest",hash("initial")}};
                    auto digest=hash(previous.dump()+tx.dump()+std::to_string(a));
                    state["kv"][key]={{"version",previous["version"].get<int>()+1},{"digest",digest},{"value",tx.at("value")},{"fib",a}};
                    stateDigest.markEntry("kv",key);kvDirty=true;
                    result["digest"]=digest; state["executed"]=state["executed"].get<uint64_t>()+1;
                } else { result["digest"]=d; state["ordered_cst"]=state["ordered_cst"].get<uint64_t>()+1; }
                state["seen"][id]={{"tx_digest",d},{"result",result}}; results.push_back(result);
                changedTx.insert(id);stateDigest.markEntry("seen",id);
            }
            state["requests"][rid]={{"txs_hash",requestHash},{"results",results}};
            stateDigest.markEntry("requests",rid);
            erasePending(rid);
            if(members.leaves.count(shard)) reply(req,results);
        }
    }
    void applyReady() {
        while(certificates.count(applied+1)) {
            int n=applied+1; const auto cert=certificates.at(n); const auto& value=cert.at("proposal").at("body").at("value");
            // The PBFT slot is committed before dependencies are exchanged.
            // Its replicated state advances only after execution has finished.
            json witness=nullptr;
#if !defined(ARBOR_SAGUARO) && !defined(ARBOR_SHARPER)
            if(value.contains("cst_orders")) {
                const auto& root=value.at("cst_orders")[0];auto key=cstKey(root);
                stageCst(root);witness=availableWitness(key);
                if(witness.is_null() || !validExecutionWitness(witness) ||
                   recordPayload(witnessRecord(witness,shard))!=recordPayload(stagedRecords.at(key))) break;
            }
#endif
            auto before=Clock::now();
#ifdef ARBOR_SAGUARO
            saguaroApply(value,cert,n);
#elif defined(ARBOR_SHARPER)
            sharperApply(value,cert,n);
#else
            if(value.contains("cst_order_index")) {
                json duplicates=json::array();
                for(const auto& req:value.at("requests")) for(const auto& tx:req.at("body").at("txs"))
                    duplicates.push_back(state["seen"].contains(tx.at("id").get<std::string>()));
                state["cst_orders"][cstKey(cert)]={{"order_digest",cert.at("proposal").at("body").at("digest")},
                    {"order_index",value.at("cst_order_index")},{"requests",value.at("requests")},{"duplicates",duplicates}};
                stateDigest.markEntry("cst_orders",cstKey(cert));
            }
            applyClientRequests(value.at("requests"),n);
            if(value.contains("cst_order_index")) state["cst_order_index"]=value.at("cst_order_index");
            if(value.contains("cst_watermarks")) {
                state["participant_indices"]=value.at("cst_watermarks");stateDigest.markField("participant_indices");
                if(value.contains("cst_round")) {
                    state["cst_round"]=value.at("cst_round");
                    state["cst_rounds"][std::to_string(value.at("cst_round").get<int>())]={{"seq",n},{"digest",cert.at("proposal").at("body").at("digest")},{"value",value}};
                    stateDigest.markEntry("cst_rounds",std::to_string(value.at("cst_round").get<int>()));
                }
            }
            if(value.contains("cst_orders")) {
                const auto& root=value.at("cst_orders")[0];auto key=cstKey(root);
                finalizeCst(witness);
                state["cst_batches"][key]=root.at("proposal").at("body").at("digest");
                stateDigest.markEntry("cst_batches",key);
                int origin=root.at("proposal").at("body").at("shard");
                state["cst_indices"][std::to_string(origin)]=projection(root.at("proposal").at("body").at("value"),shard);
                stateDigest.markEntry("cst_indices",std::to_string(origin));
                state["last_cst_seq"]=root.at("proposal").at("body").at("value").at("cst_order_index");
                if(members.multiLayer()) state["cst_round"]=root.at("proposal").at("body").at("value").at("cst_round");
                pendingCst.erase(key);slotWitnesses[n]=witness;
                log("cst_ordered_at_leaf",{{"coordinator_batch",key},{"digest",state["cst_batches"].at(key)}});
            }
#endif
            state["chain"]=hash(state["chain"].get<std::string>()+std::to_string(n)+value.dump());
            applied=n; state["seq"]=n;
            cstSelectionDirty=true;
            refreshDigests();
            executionNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-before).count();
            json entry={{"seq",n},{"value_digest",hash(value.dump())},{"state_digest",cachedStateDigest},{"certificate",cert}};
            if(!witness.is_null()) entry["execution_witness"]=witness;
            journal<<entry.dump()<<'\n';journal.flush();
#ifdef ARBOR_SAGUARO
            saguaroAfterApply(value,cert,n);
#elif defined(ARBOR_SHARPER)
            sharperAfterApply(value,cert,n);
#else
            if(!members.leaves.count(shard)) {
                forwardCstOrder(cert,value);
                if(value.contains("cst_order_index")) maybeCompleteBatch(cstKey(cert));
            } else if(value.contains("cst_orders")) sendAck(cstKey(value.at("cst_orders")[0]));
#endif
            lastProgress=Clock::now();
            if(n%checkpointEvery==0) {
                snapshots[n]=state;snapshotDigests[n]=cachedStateDigest;
                broadcast(make("CHECKPOINT",{{"seq",n},{"digest",cachedStateDigest}}));
                checkStable(n);
            }
            drainDeferred();
        }
    }
    void checkStable(int n) {
        if(!snapshots.count(n) || n<=stableSeq) return;
        const auto& d=snapshotDigests.at(n); json proof=json::array();
        for(const auto& [r,e]:checkpointVotes[n]) { (void)r; if(e.at("body").at("digest")==d) proof.push_back(e); }
        if(proof.size()>=3) installStable({{"seq",n},{"state",snapshots[n]},{"proof",proof}});
    }
    void propose() {
#ifdef ARBOR_SAGUARO
        saguaroPropose();
#elif defined(ARBOR_SHARPER)
        sharperPropose();
#else
        if(changing || me!=view%4 || applied+1>stableSeq+window) return;
        // One fresh batch in flight. Recovery slots may coexist after a view change.
        if(slots.count(applied+1) && !slots.at(applied+1).proposal.is_null()) return;
        if(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-batchStart).count()<members.config.at("consensus").at("batch_wait_ms").get<int>()) return;
        if(!stagedRecords.empty() || certificates.count(applied+1)) return;
        bool flowControlled=isCoordinator() && outstandingOrders.size()>=8;
        bool closeRound=isCoordinator() && members.multiLayer() && desiredRound>state.at("cst_round").get<int>();
        if(flowControlled && !closeRound) return;
        if(pending.empty() && pendingCst.empty() && !closeRound) return;
        bool crossShardOrder=isCoordinator() && !pending.empty() && !flowControlled;
        int proposalLimit=crossShardOrder?crossShardBatchSize:batchSize;
        if(crossShardOrder && std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-batchStart).count()<crossShardBatchWaitMs) {
            int available=0;
            std::string group;
            std::set<std::string> selected;
            bool blocked=false;
            for(const auto& [id,request]:pending) {
                if(state["requests"].contains(id)) continue;
                bool overlap=false,conflict=false;
                for(const auto& tx:request.at("body").at("txs")) {
                    auto tid=tx.at("id").get<std::string>();
                    if(selected.count(tid)) overlap=true;
                    if(state["seen"].contains(tid) && state["seen"].at(tid).at("tx_digest")!=hash(tx.dump())) conflict=true;
                }
                if(overlap || conflict) continue;
                auto candidate=participantGroup(request);
                int size=request.at("body").at("txs").size();
                if((!group.empty() && group!=candidate) || available+size>proposalLimit) {
                    blocked=available>0;break;
                }
                if(group.empty()) group=candidate;
                for(const auto& tx:request.at("body").at("txs")) selected.insert(tx.at("id").get<std::string>());
                available+=size;
                if(available==proposalLimit) break;
            }
            // Complete signed requests cannot be split to fill spare capacity.
            // Flush when the next eligible request cannot fit, even below the cap.
            if(available<proposalLimit && !blocked) return;
        }
        json reqs=json::array(); int count=0;
        std::string group;
        std::map<std::string,std::string> selectedIds;
        for(auto it=pending.begin();it!=pending.end() && !flowControlled;) {
            if(state["requests"].contains(it->first)) {
                if(completedCstResults.count(it->first)) reply(it->second,completedCstResults.at(it->first));
                else if(members.leaves.count(shard)) reply(it->second,state["requests"][it->first]["results"]);
                auto rid=it->first;++it;erasePending(rid);continue;
            }
            int size=it->second.at("body").at("txs").size();
            if(crossShardOrder) {
                bool conflict=false;
                for(const auto& tx:it->second.at("body").at("txs")) {
                    auto id=tx.at("id").get<std::string>(),digest=hash(tx.dump());
                    if((state["seen"].contains(id) && state["seen"].at(id).at("tx_digest")!=digest) ||
                       (selectedIds.count(id) && selectedIds.at(id)!=digest)) conflict=true;
                }
                if(conflict) {replyConflict(it->second);auto rid=it->first;++it;erasePending(rid);continue;}
            }
            bool overlap=false;
            for(const auto& tx:it->second.at("body").at("txs"))
                if(selectedIds.count(tx.at("id").get<std::string>())) {overlap=true;break;}
            if(overlap) {++it;continue;}
            if(count+size>proposalLimit) break;
            if(crossShardOrder) {
                auto candidate=participantGroup(it->second);
                if(group.empty()) group=candidate;
                else if(group!=candidate) break;
            }
            for(const auto& tx:it->second.at("body").at("txs"))
                selectedIds[tx.at("id").get<std::string>()]=hash(tx.dump());
            reqs.push_back(it->second); count+=size; ++it;
        }
        json value={{"requests",reqs}};
        if(!reqs.empty() && isCoordinator())
            value["cst_order_index"]=state.at("cst_order_index").get<int>()+1;
        if(isCoordinator() && (!reqs.empty() || closeRound)) {
            auto ps=participants(reqs);json watermark=json::object();
            for(int leaf:members.descendants(shard)) watermark[std::to_string(leaf)]=state.at("participant_indices").value(std::to_string(leaf),0)+(ps.count(leaf)?1:0);
            value["cst_watermarks"]=watermark;
            if(members.multiLayer()) value["cst_round"]=state.at("cst_round").get<int>()+1;
        }
        if(reqs.empty() && !pendingCst.empty()) {
            const auto& next=nextCstOrder();
            if(!next.is_null()) {
                value["cst_orders"]=json::array({next.at("certificate")});
                if(members.multiLayer()) value["cst_frontier"]=next.at("frontier");
            }
        }
        if(reqs.empty() && !value.contains("cst_orders") && !value.contains("cst_watermarks")) return;
        if(isCoordinator() && value.contains("cst_round")) requestRound(value.at("cst_round"));
        broadcast(make("PREPREPARE",{{"seq",applied+1},{"digest",hash(value.dump())},{"value",value}}));
        batchStart=Clock::now();
#endif
    }
    void handle(const json& e) {
        const auto& b=e.at("body"); std::string type=b.at("type");
        if(type=="CLIENT") {
#ifdef ARBOR_SAGUARO
            saguaroClient(e);
#elif defined(ARBOR_SHARPER)
            sharperClient(e);
#else
            if(isCoordinator() &&
               b.at("target")==shard && b.at("txs").is_array() && int(b.at("txs").size())>crossShardBatchSize &&
               int(b.at("txs").size())<=batchSize) {
                if(!members.clientMessage(e)) {rejected++;return;}
                json results=json::array();
                for(const auto& tx:b.at("txs")) results.push_back({{"id",tx.at("id")},{"error","cross_shard_batch_too_large"}});
                reply(e,results);rejected++;return;
            }
            if(!validRequest(e)) {rejected++;return;}
            std::string id=b.at("id");
            for(const auto& tx:b.at("txs")) {
                    auto tid=tx.at("id").get<std::string>(),digest=hash(tx.dump());
                    if((state["seen"].contains(tid) && state["seen"].at(tid).at("tx_digest")!=digest) ||
                       (pendingTx.count(tid) && pendingTx.at(tid).first!=digest)) {
                        replyConflict(e);rejected++;return;
                    }
            }
            if(state["requests"].contains(id)) {
                if(state["requests"][id]["txs_hash"]==hash(b.at("txs").dump())) {
                    if(completedCstResults.count(id)) reply(e,completedCstResults.at(id));
                    else if(members.leaves.count(shard)) reply(e,state["requests"][id]["results"]);
                }
                else {replyConflict(e,"request_id_conflict");rejected++;}
                duplicates++; return;
            }
            if(pending.count(id)) {
                if(pending.at(id).at("body").at("txs")!=b.at("txs")) {replyConflict(e,"request_id_conflict");rejected++;}
                else duplicates++;
                return;
            }
            if(deferredRequests.count(id)) {
                if(deferredRequests.at(id).at("body").at("txs")!=b.at("txs")) {replyConflict(e,"request_id_conflict");rejected++;}
                else duplicates++;
                return;
            }
            if(replyCompletedTransactions(e)) return;
            if(pending.size()+deferredRequests.size()>=10000) {rejected++;return;}
            for(const auto& tx:b.at("txs")) {
                auto tid=tx.at("id").get<std::string>();
                if(pendingTx.count(tid) || (isCoordinator() &&
                   state["seen"].contains(tid) && !completedTxResults.count(tid))) {
                    deferRequest(e);
                    if(me!=view%4) sendTo(shard,view%4,e);
                    return;
                }
            }
            if(pending.empty()) {lastProgress=Clock::now();batchStart=Clock::now();}
            auto [it,inserted]=pending.emplace(id,e);
            if(!inserted && it->second.at("body").at("txs")!=b.at("txs")) {rejected++;return;}
            if(inserted) for(const auto& tx:b.at("txs"))
                if(!state["seen"].contains(tx.at("id").get<std::string>()))
                    pendingTx.emplace(tx.at("id"),std::make_pair(hash(tx.dump()),id));
            if(inserted && me!=view%4) sendTo(shard,view%4,e);
#endif
            return;
        }
        if(type=="PROBE") {
            if(!members.clientMessage(e) || b.at("source")!=identity(shard,me)) {rejected++;return;}
            std::string id=b.at("id"); int dst=b.at("dst_shard"),r=b.at("dst_replica");
            if(!members.endpoints.count(identity(dst,r)) || pingStarts.size()>=100) return;
            pingStarts[id]=Clock::now();
            pingPeers[id]=identity(dst,r);
            sendTo(dst,r,make("PING",{{"id",id},{"dst_shard",dst},{"dst_replica",r}})); return;
        }
        if(!members.replicaMessage(e)) {rejected++;return;}
        int source=b.at("shard"),sender=b.at("from");
#ifdef ARBOR_SAGUARO
        if(saguaroHandle(e)) return;
        if(type.rfind("CST_",0)==0 || type.rfind("SH_",0)==0) {rejected++;return;}
#elif defined(ARBOR_SHARPER)
        if(sharperHandle(e)) return;
        if(type.rfind("CST_",0)==0 || type.rfind("SAG_",0)==0) {rejected++;return;}
#else
        if(type.rfind("SAG_",0)==0 || type.rfind("SH_",0)==0) {rejected++;return;}
#endif
        if(type=="PING") {
            if(b.at("dst_shard")!=shard || b.at("dst_replica")!=me) return;
            sendTo(source,sender,make("PONG",{{"id",b.at("id")}})); return;
        }
        if(type=="PONG") {
            auto id=b.at("id").get<std::string>();
            if(pingStarts.count(id) && pingPeers.at(id)==identity(source,sender)) {
                probes[id]={{"rtt_ms",millis(Clock::now())-millis(pingStarts[id])},{"peer",identity(source,sender)}};
                pingStarts.erase(id); pingPeers.erase(id); while(probes.size()>100) probes.erase(probes.begin());
            }
            return;
        }
        // Reject messages from the removed protocol before they reach PBFT.
        if(type.rfind("PIPE_",0)==0) {rejected++;return;}
        if(type=="CST_ROUND_REQUEST") {
            if(!members.multiLayer() || !isCoordinator() || !members.coordinators().count(source) || b.at("target")!=shard ||
               !b.at("round").is_number_integer()) {rejected++;return;}
            int round=b.at("round");
            if(round<=0) {rejected++;return;}
            round=std::min(round,state.at("cst_round").get<int>()+64);
            if(round>desiredRound) {desiredRound=round;lastProgress=Clock::now();}
            return;
        }
        if(type=="CST_ROUND_CLOSE") {
            if(!members.multiLayer() || !members.leaves.count(shard) || b.at("target")!=shard ||
               source!=b.at("certificate").at("proposal").at("body").at("shard") || !rememberRound(b.at("certificate"))) {rejected++;return;}
            auto key=cstKey(b.at("certificate"));
            if(pendingCst.count(key)) {pendingCst[key]=e;cstSelectionDirty=true;}
            if(state["cst_batches"].contains(key)) {completionResendNeeded.insert(key);sendAck(key);forwardAck(key,true);}
            return;
        }
        if(type=="CST_ROUND_QUERY") {
            if(!members.multiLayer() || !isCoordinator() || b.at("target")!=shard || !b.at("rounds").is_array() || b.at("rounds").size()>32) {rejected++;return;}
            int leaf=b.at("leaf"),after=b.at("after_index");
            bool local=source==shard;
            if(!local && (!isForward() || leaf!=source || !members.descendants(shard).count(leaf) || after<0)) return;
            std::set<int> wanted;
            for(auto r:b.at("rounds")) {int round=r;if(round<=0) {rejected++;return;}wanted.insert(round);}
            if(!local && !wanted.empty()) desiredRound=std::max(desiredRound,std::min(*wanted.rbegin(),state.at("cst_round").get<int>()+64));
            if(!local) {
                // Include the first missing projected orders, even if their round
                // predates every currently buffered ORDER at the requesting leaf.
                for(const auto& [round,closes]:roundCertificates) {
                    if(closes.count(shard) && orderParticipants(closes.at(shard)).count(leaf) &&
                       projection(closes.at(shard).at("proposal").at("body").at("value"),leaf)>after) wanted.insert(round);
                    if(wanted.size()>=32) break;
                }
            }
            json proofs=json::array(),missing=json::array();
            for(int round:wanted) {
                if(roundCertificates.count(round) && roundCertificates.at(round).count(shard)) proofs.push_back(roundCertificates.at(round).at(shard));
                else if(state["cst_rounds"].contains(std::to_string(round))) missing.push_back(round);
                if(proofs.size()+missing.size()>=32) break;
            }
            if(!proofs.empty() && (source!=shard || sender!=me)) {
                sendTo(source,sender,make("CST_ROUND_PROOFS",{{"target",source},{"certificates",proofs}}));
                log("round_proofs_sent",{{"destination",source},{"batches",proofs.size()}});
            }
            if(!local && !missing.empty()) broadcast(make("CST_ROUND_QUERY",{{"target",shard},{"rounds",missing},{"leaf",-1},{"after_index",-1}}));
            return;
        }
        if(type=="CST_ROUND_PROOFS") {
            if(!members.multiLayer() || b.at("target")!=shard || !b.at("certificates").is_array() || b.at("certificates").size()>32) {rejected++;return;}
            for(const auto& cert:b.at("certificates")) {
                if(cert.at("proposal").at("body").at("shard")!=source || !rememberRound(cert)) {rejected++;continue;}
                if(isCoordinator() && source==shard && isForward()) forwardCstOrder(cert,cert.at("proposal").at("body").at("value"),true);
            }
            return;
        }
        if(type=="CST_ORDER") {
            if(!members.leaves.count(shard) || b.at("target")!=shard ||
               source!=b.at("certificate").at("proposal").at("body").at("shard") ||
               !members.ancestors(shard).count(source) || !orderParticipants(b.at("certificate")).count(shard) || !validCoordinatorCertificate(b.at("certificate"))) {rejected++;return;}
            const auto& cert=b.at("certificate");std::string key=cstKey(cert);
            std::string digest=cert.at("proposal").at("body").at("digest");
            if(state["cst_batches"].contains(key)) {
                if(state["cst_batches"].at(key)!=digest) {rejected++;return;}
                duplicates++;completionResendNeeded.insert(key);sendAck(key);forwardAck(key,true);
                return;
            }
            if(pendingCst.size()>=10000) {rejected++;return;}
            if(pending.empty() && pendingCst.empty()) {lastProgress=Clock::now();batchStart=Clock::now();}
            auto [it,inserted]=pendingCst.emplace(key,e);
            if(inserted) cstSelectionDirty=true;
            if(!inserted && it->second.at("body").at("certificate").at("proposal").at("body").at("digest")!=digest)
                rejected++;
            else if(!inserted) duplicates++;
            else if(me!=view%4) sendTo(shard,view%4,e);
            return;
        }
        if(type=="CST_PREPARED") {
            if(!members.leaves.count(shard) || source!=shard || !isForward() ||
               !validPreparedVote(e)) {rejected++;return;}
            const auto& record=e.at("record");auto key=record.at("batch_key").get<std::string>();
            if(state["cst_finalized"].contains(key)) return;
            auto existing=dependencyProof(key,shard);
            if(!existing.is_null()) {forwardPrepared(key);applyReady();return;}
            if(stagedRecords.count(key) && recordPayload(stagedRecords.at(key))!=recordPayload(record)) {rejected++;return;}
            auto group=key+"|"+std::to_string(source)+"|"+b.at("record_digest").get<std::string>();
            if(preparedVotes.size()>=10000 && !preparedVotes.count(group)) {rejected++;return;}
            if(!preparedVotes[group].emplace(sender,e).second) duplicates++;
            const auto& votes=preparedVotes.at(group);
            if(votes.size()>=3) {
                json selected=json::array();
                for(const auto& [r,vote]:votes) {
                    (void)r;if(selected.size()<3) selected.push_back({{"body",vote.at("body")},{"signature",vote.at("signature")}});
                }
                json proof={{"record",record},{"votes",selected}};
                if(!validPreparedProof(proof,shard)) {rejected++;return;}
                preparedProofs[key][shard]=proof;lastProgress=Clock::now();forwardPrepared(key);
                preparedVotes.erase(group);
            }
            applyReady();return;
        }
        if(type=="CST_PREPARED_QC") {
            if(!members.leaves.count(shard) || !members.leaves.count(source) ||
               b.at("target")!=shard || !validPreparedProof(b.at("proof"),source) ||
               !orderParticipants(b.at("proof").at("record").at("order_certificate")).count(shard)) {rejected++;return;}
            const auto& proof=b.at("proof");auto key=proof.at("record").at("batch_key").get<std::string>();
            auto existing=dependencyProof(key,source);
            if(!existing.is_null() && recordPayload(existing.at("record"))!=recordPayload(proof.at("record"))) {rejected++;return;}
            if(state["cst_finalized"].contains(key)) return;
            if(preparedProofs.size()>=10000 && !preparedProofs.count(key)) {rejected++;return;}
            if(existing.is_null()) {preparedProofs[key][source]=proof;lastProgress=Clock::now();} else duplicates++;
            applyReady();return;
        }
        if(type=="CST_DEPENDENCY_QUERY") {
            if(!members.leaves.count(shard) || !members.leaves.count(source) ||
               b.at("target")!=shard || !b.at("batch_keys").is_array() || b.at("batch_keys").size()>32) {rejected++;return;}
            // Cross-shard replies use the forward; local replicas may supply
            // an archive lost by a forward that restored a checkpoint.
            if(source!=shard && !isForward()) return;
            json witnesses=json::array();
            for(const auto& item:b.at("batch_keys")) {
                auto key=item.get<std::string>();auto witness=availableWitness(key);
                if(!witness.is_null() && validExecutionWitness(witness)) {
                    if(source!=shard && !orderParticipants(witness.at("proofs")[0].at("record").at("order_certificate")).count(source)) {rejected++;continue;}
                    witnesses.push_back(witness);
                }
                else {
                    auto proof=dependencyProof(key,shard);
                    if(!proof.is_null()) {
                        if(source!=shard && !orderParticipants(proof.at("record").at("order_certificate")).count(source)) {rejected++;continue;}
                        sendTo(source,sender,make("CST_PREPARED_QC",{{"target",source},{"proof",proof}}));
                    }
                    else if(source!=shard) broadcast(make("CST_DEPENDENCY_QUERY",{{"target",shard},{"batch_keys",json::array({key})}}));
                }
            }
            if(!witnesses.empty()) {
                sendTo(source,sender,make("CST_DEPENDENCY_PROOFS",{{"target",source},{"witnesses",witnesses}}));
                log("cst_dependency_proofs_sent",{{"destination",source},{"batches",witnesses.size()}});
            }
            return;
        }
        if(type=="CST_DEPENDENCY_PROOFS") {
            if(!members.leaves.count(shard) || !members.leaves.count(source) ||
               b.at("target")!=shard || !b.at("witnesses").is_array() || b.at("witnesses").size()>32) {rejected++;return;}
            size_t accepted=0;
            for(const auto& witness:b.at("witnesses")) {
                if(validExecutionWitness(witness) && (source==shard ||
                   orderParticipants(witness.at("proofs")[0].at("record").at("order_certificate")).count(source)) && importWitness(witness)) {
                    accepted++;
                    if(source==shard && isForward()) forwardPrepared(witness.at("batch_key"),true);
                } else rejected++;
            }
            if(accepted) log("cst_dependency_proofs_received",{{"source",source},{"batches",accepted}});
            applyReady();return;
        }
        if(type=="CST_ACK") {
            if(!members.leaves.count(shard) || source!=shard || !isForward() ||
               !b.at("batch_key").is_string() || b.at("target")!=batchOrigin(b.at("batch_key").get<std::string>()) ||
               !b.at("order_digest").is_string() || !b.at("execution_digest").is_string() ||
               !b.at("result_digest").is_string()) {rejected++;return;}
            auto key=b.at("batch_key").get<std::string>();
            if(state["cst_finalized"].contains(key)) {
                const auto& done=state["cst_finalized"].at(key);
                if(done.at("order_digest")!=b.at("order_digest") || done.at("execution_digest")!=b.at("execution_digest") ||
                   done.at("result_digest")!=b.at("result_digest")) {rejected++;return;}
            }
            if(ackProofs.count(key) && ackProofs.at(key).count(shard)) {
                // A replica restored from a checkpoint may lack this volatile QC.
                // Return it locally without repeating the cross-shard broadcast.
                if(sender!=me) sendTo(shard,sender,make("CST_ACK_QC",{{"target",shard},{"proof",ackProofs.at(key).at(shard)}}));
                forwardAck(key);return;
            }
            auto group=key+"|"+b.at("order_digest").get<std::string>()+"|"+
                b.at("execution_digest").get<std::string>()+"|"+b.at("result_digest").get<std::string>();
            if(ackVotes.size()>=10000 && !ackVotes.count(group)) {rejected++;return;}
            if(!ackVotes[group].emplace(sender,e).second) duplicates++;
            if(ackVotes[group].size()>=3) {
                json proof=json::array();
                for(const auto& [r,vote]:ackVotes[group]) {(void)r;if(proof.size()<3) proof.push_back(vote);}
                if(!validAckProof(proof,shard,key,b.at("order_digest"))) {rejected++;return;}
                ackProofs[key][shard]=proof;ackVotes.erase(group);lastProgress=Clock::now();forwardAck(key);
            }
            return;
        }
        if(type=="CST_ACK_QC") {
            if(!members.leaves.count(source) || b.at("target")!=shard ||
               (!isCoordinator() && shard!=source) || !b.at("proof").is_array() || b.at("proof").size()!=3) {rejected++;return;}
            const auto& proof=b.at("proof");const auto& vote=proof[0].at("body");
            auto key=vote.at("batch_key").get<std::string>();std::string digest=vote.at("order_digest");
            if((isCoordinator() && batchOrigin(key)!=shard) || !validAckProof(proof,source,key,digest) ||
               (state["cst_orders"].contains(key) && digest!=state["cst_orders"].at(key).at("order_digest"))) {rejected++;return;}
            if(state["cst_finalized"].contains(key)) {
                const auto& done=state["cst_finalized"].at(key);
                if(done.at("order_digest")!=digest || done.at("execution_digest")!=vote.at("execution_digest") ||
                   done.at("result_digest")!=vote.at("result_digest")) {rejected++;return;}
            }
            if(isCoordinator() && !completedCstBatches.count(key)) {
                if(!activeAckBatches.count(key) && activeAckBatches.size()>=10000) {rejected++;return;}
                activeAckBatches.insert(key);
            }
            auto [it,inserted]=ackProofs[key].emplace(source,proof);
            const auto& old=it->second[0].at("body");
            if(old.at("order_digest")!=digest || old.at("execution_digest")!=vote.at("execution_digest") ||
               old.at("result_digest")!=vote.at("result_digest")) {rejected++;return;}
            if(inserted) lastProgress=Clock::now();else duplicates++;
            if(source==shard) {
                pendingLocalAckQcs.erase(key);
                if(sender==view%4) completionResendNeeded.erase(key);
            }
            if(isCoordinator()) maybeCompleteBatch(key);
            return;
        }
        if(source!=shard) {rejected++;return;}
        if(type=="CST_RESULT_QUERY") {
            if(!isCoordinator() || !b.at("batch_keys").is_array() ||
               b.at("batch_keys").size()>32) {rejected++;return;}
            json proofs=json::array();
            for(const auto& item:b.at("batch_keys")) {
                auto key=item.get<std::string>();if(!state["cst_orders"].contains(key)) continue;
                json leaves=json::array();
                for(int leaf:participants(state["cst_orders"].at(key).at("requests"))) if(ackProofs.count(key) && ackProofs.at(key).count(leaf))
                    leaves.push_back({{"shard",leaf},{"votes",ackProofs.at(key).at(leaf)}});
                json proof={{"batch_key",key},{"leaves",leaves}};
                if(orderCertificates.count(key)) proof["order_certificate"]=orderCertificates.at(key);
                if(!leaves.empty() || proof.contains("order_certificate")) proofs.push_back(proof);
            }
            if(sender!=me && !proofs.empty()) sendTo(shard,sender,make("CST_RESULT_PROOFS",{{"proofs",proofs}}));
            return;
        }
        if(type=="CST_RESULT_PROOFS") {
            if(!isCoordinator() || !b.at("proofs").is_array() ||
               b.at("proofs").size()>32) {rejected++;return;}
            for(const auto& item:b.at("proofs")) {
                auto key=item.at("batch_key").get<std::string>();
                if(completedCstBatches.count(key) || !state["cst_orders"].contains(key)) continue;
                const auto& order=state["cst_orders"].at(key);
                if(!item.at("leaves").is_array() || item.at("leaves").size()>participants(order.at("requests")).size()) {rejected++;continue;}
                std::map<int,json> recovered;bool valid=true;
                for(const auto& proof:item.at("leaves")) {
                    int leaf=proof.at("shard");
                    if(!participants(order.at("requests")).count(leaf) || !validAckProof(proof.at("votes"),leaf,key,order.at("order_digest")) ||
                       !recovered.emplace(leaf,proof.at("votes")).second) {valid=false;break;}
                }
                if(item.contains("order_certificate")) {
                    const auto& cert=item.at("order_certificate");
                    if(!validCoordinatorCertificate(cert) || cstKey(cert)!=key ||
                       cert.at("proposal").at("body").at("digest")!=order.at("order_digest") ||
                       cert.at("proposal").at("body").at("value").at("cst_order_index")!=order.at("order_index")) valid=false;
                }
                if(!valid) {rejected++;continue;}
                for(const auto& [leaf,proof]:recovered) ackProofs[key].emplace(leaf,proof);
                if(item.contains("order_certificate")) {
                    orderCertificates[key]=item.at("order_certificate");
                    if(members.multiLayer()) rememberRound(item.at("order_certificate"));
                    outstandingOrders[order.at("order_index").get<int>()]=item.at("order_certificate");
                }
                maybeCompleteBatch(key,false);
            }
            return;
        }
        int v=b.at("view"); if(v<0) return;
        if(type=="FORWARD_HEARTBEAT") {
            if(v==view && sender==view%4) lastForwardHeartbeat=Clock::now();
            return;
        }
        if(type=="VIEW_CHANGE") {
            if(v<=view || v>std::max(view,targetView)+64 || !validVC(e,v)) return;
            viewChanges[v].emplace(sender,e);
            // f+1 distinct replicas requesting a higher view are needed; one
            // Byzantine replica cannot independently force a view change.
            std::set<int> higher;
            int smallest=INT_MAX;
            for(const auto& [vv,vs]:viewChanges) if(vv>std::max(view,targetView)) {
                smallest=std::min(smallest,vv); for(const auto& [r,msg]:vs) {(void)msg;higher.insert(r);}
            }
            if(higher.size()>=2) startViewChange(smallest);
            maybeNewView(v); return;
        }
        if(type=="NEW_VIEW") {acceptNewView(e);return;}
        if(type=="CHECKPOINT") {
            int n=b.at("seq"); if(n<=stableSeq || n>stableSeq+window) return;
            if(n>applied) catchupTarget=std::max(catchupTarget,n);
            checkpointVotes[n].emplace(sender,e);checkStable(n);return;
        }
        if(type=="SYNC_REQUEST") {
            int start=b.at("after");
            json cs=json::array(),witnesses=json::array();
            // A commit proof establishes order, not completed execution. The
            // receiver still stages and waits unless its witnesses are valid.
            size_t waiting=0;
            for(const auto& [n,c]:certificates) if(n>start) {
                if(n>applied) waiting++;
                cs.push_back(c);
                if(slotWitnesses.count(n)) witnesses.push_back({{"seq",n},{"witness",slotWitnesses.at(n)}});
            }
            if(cs.empty() && start>=applied && b.value("stable_seq",0)>=stableSeq) return;
            json checkpoint=b.value("stable_seq",0)>=stableSeq ?
                json{{"seq",0},{"state",genesis()},{"proof",json::array()}} :
                json{{"seq",stableSeq},{"state",stableState},{"proof",stableProof}};
            json fields={{"stable",checkpoint},{"certificates",cs},{"execution_witnesses",witnesses}};
            sendTo(shard,sender,make("SYNC",fields));
            log("sync_proofs_sent",{{"destination_replica",sender},{"after",start},{"certificates",cs.size()},
                {"execution_witnesses",witnesses.size()},{"committed_waiting",waiting}});
            return;
        }
        if(type=="SYNC") {
            if(!validStable(b.at("stable")) || !b.at("certificates").is_array() ||
               b.at("certificates").size()>size_t(window)) {rejected++;return;}
            std::map<int,json> incoming,witnesses;
            for(const auto& c:b.at("certificates")) {
                if(!validCertificate(c)) {rejected++;return;}
                int n=c.at("proposal").at("body").at("seq");
                if(!incoming.emplace(n,c).second) {rejected++;return;}
                if(certificates.count(n) && certificates.at(n).at("proposal").at("body").at("digest")!=
                   c.at("proposal").at("body").at("digest")) {rejected++;return;}
            }
            if(b.contains("execution_witnesses")) {
                if(!b.at("execution_witnesses").is_array() || b.at("execution_witnesses").size()>size_t(window)) {rejected++;return;}
                for(const auto& item:b.at("execution_witnesses")) {
                    int n=item.at("seq");const auto& witness=item.at("witness");
                    if(!incoming.count(n) || !validExecutionWitness(witness) || !witnesses.emplace(n,witness).second) {rejected++;return;}
                    const auto& value=incoming.at(n).at("proposal").at("body").at("value");
                    if(!value.contains("cst_orders") || value.at("cst_orders").size()!=1 ||
                       witness.at("batch_key")!=cstKey(value.at("cst_orders")[0]) ||
                       witnessRecord(witness,shard).at("order_digest")!=value.at("cst_orders")[0].at("proposal").at("body").at("digest")) {rejected++;return;}
                }
            }
            installStable(b.at("stable"));
            for(const auto& [n,c]:incoming) if(n>applied && n<=stableSeq+window) {
                if(witnesses.count(n) && !importWitness(witnesses.at(n))) {rejected++;continue;}
                certificates.emplace(n,c);preparedHistory[n]=c;
            }
            applyReady();return;
        }
        if(changing || v!=view) return;
        int n=b.at("seq"); if(n<=applied || n>stableSeq+window) return;
        if(type=="PREPREPARE") {acceptProposal(e);return;}
        if(type=="PREPARE" || type=="COMMIT") {
            if(type=="PREPARE" && sender==view%4) return;
            auto& s=slots[n]; auto& votes=type=="PREPARE"?s.prepares:s.commits;
            if(!votes.emplace(sender,e).second) {duplicates++;return;}
            advance(n);
        }
    }
    void status(bool ready=true) {
        json s={{"ready",ready},{"run_id",members.run},{"pid",getpid()},{"shard",shard},{"replica",me},
            {"role",members.leaves.count(shard)?"leaf":"coordinator"},{"view",view},{"primary",view%4},
            {"forward_replica",view%4},{"is_forward",isForward()},
            {"changing_view",changing},{"target_view",targetView},{"applied_batches",applied},{"stable_seq",stableSeq},
            {"executed_transactions",state["executed"]},{"ordered_cst_transactions",state["ordered_cst"]},
            {"leaf_ordered_cst_transactions",state["leaf_ordered_cst"]},{"last_cst_seq",state["last_cst_seq"]},
            {"cst_order_index",state["cst_order_index"]},{"cst_round",state["cst_round"]},{"cst_indices",state["cst_indices"]},
            {"catchup_target",catchupTarget},{"rounds_requested",desiredRound},{"round_certificate_count",roundCertificates.size()},
            {"avg_cst_batch_size",state["cst_order_index"].get<int>()>0 ?
                state["ordered_cst"].get<double>()/state["cst_order_index"].get<int>() : 0.0},
            {"pending_cst_batches",pendingCst.size()},{"staged_cst_batches",stagedRecords.size()},
            {"cst_selection_lookups",cstSelectionLookups},{"cst_selection_rebuilds",cstSelectionRebuilds},
            {"finalized_cst_batches",state["cst_finalized"].size()},
            {"decided_cst_batches",0},{"completed_cst_transactions",completedCstTransactions},
            {"kv_entries",state["kv"].size()},{"kv_digest",cachedKvDigest},
            {"state_digest",cachedStateDigest},{"state_digest_algorithm","arbor-merkle-v1"},{"chain_digest",state["chain"]},{"pending_requests",pending.size()},
            {"dedup_waiting_requests",deferredRequests.size()},
            {"dedup_index_rebuilds",dedupIndexRebuilds},{"dedup_pending_checks",dedupPendingChecks},
            {"dedup_waiter_checks",dedupWaiterChecks},
            {"rejected_messages",rejected},{"duplicate_messages",duplicates},{"view_changes",viewCount},
            {"execution_ns",executionNs},{"messages_sent",net.sent.load()},{"messages_received",net.received.load()},
            {"network_failures",net.failed.load()},{"network_socket_errors",net.socket_errors.load()},
            {"network_connect_errors",net.connect_errors.load()},
            {"network_write_errors",net.write_errors.load()},
            {"network_timeout_errors",net.timeout_errors.load()},
            {"network_parse_errors",net.parse_errors.load()},
            {"network_oversized_errors",net.oversized_errors.load()},
            {"network_queue_errors",net.queue_errors.load()},{"bytes_sent",net.bytes_sent.load()},
            {"network_buffered_bytes",net.buffered_bytes.load()},{"network_active_connections",net.active_connections.load()},
            {"network_connect_attempts",net.connect_attempts.load()},{"network_connections_reused",net.connections_reused.load()},
            {"inbox_dropped",inboxDropped.load()},
            {"bytes_received",net.bytes_received.load()},{"network_queue",net.queued()},{"probes",probes}};
#ifdef ARBOR_SAGUARO
        saguaroStatus(s);
#ifdef ARBOR_AHL
        s["method"]="ahl";
        s["ahl_coordinator"]=members.ahlCoordinator();
        s["ahl_topology"]="two-layer";
#endif
#elif defined(ARBOR_SHARPER)
        sharperStatus(s);
#else
        s["method"]="arbor";
#endif
        writeJson(dir+"/status.json",s);
    }
#ifdef ARBOR_SAGUARO
#ifdef ARBOR_AHL
#include "../baseline/ahl/protocol.inc"
#else
#include "../baseline/saguaro/protocol.inc"
#endif
#elif defined(ARBOR_SHARPER)
#include "../baseline/sharper/protocol.inc"
#endif
public:
    Replica(const json& cfg,int sid,int rid,const std::string& d):members(cfg),shard(sid),me(rid),dir(d) {
        if(!members.endpoints.count(identity(sid,rid))) throw std::runtime_error("unknown replica");
        for(const auto& n:cfg.at("nodes")) if(n.at("shard")==sid && n.at("replica")==rid) privateKey=readKey(n.at("private_key"),true);
        timeoutMs=cfg.at("consensus").at("view_timeout_ms"); batchSize=cfg.at("consensus").at("batch_size");
        crossShardBatchSize=cfg.at("consensus").at("cross_shard_batch_size");
        crossShardBatchWaitMs=cfg.at("consensus").at("cross_shard_batch_wait_ms");
        checkpointEvery=cfg.at("consensus").at("checkpoint_batches");
#ifdef ARBOR_SAGUARO
        saguaroInitialize();
#endif
        stableState=state;
        expectedFib=fibonacci();
        refreshDigests(true);
        if(std::filesystem::exists(dir+"/commits.jsonl")) throw std::runtime_error("replica restart within a run is not supported; start a fresh run to prevent double voting");
        journal.open(dir+"/commits.jsonl"); events.open(dir+"/events.jsonl");
        if(!journal || !events) throw std::runtime_error("cannot open node logs");
    }
    void run() {
        net.start(members.endpoints.at(identity(shard,me)),[this](json e){
            std::lock_guard<std::mutex> l(inboxMutex);
            if(inbox.size()<100000) inbox.push(std::move(e)); else inboxDropped++;
        },members.config.at("network").at("trace").get<bool>()?dir+"/network.jsonl":"");
        log("started"); status();
        while(!stopping) {
            std::vector<json> work;
            {std::lock_guard<std::mutex> l(inboxMutex); for(int i=0;i<256 && !inbox.empty();++i){work.push_back(std::move(inbox.front()));inbox.pop();}}
            for(const auto& e:work) try {handle(e);} catch(const std::exception& ex) {rejected++;log("rejected",{{"reason",ex.what()}});}
            auto now=Clock::now();
            for(auto it=pingStarts.begin();it!=pingStarts.end();) {
                if(std::chrono::duration_cast<std::chrono::seconds>(now-it->second).count()>130) {
                    pingPeers.erase(it->first); it=pingStarts.erase(it);
                } else ++it;
            }
            int backoff=std::min(16,1<<std::min(4,std::max(0,targetView-view)));
#ifdef ARBOR_SAGUARO
            bool waiting=saguaroWaiting(now);
#elif defined(ARBOR_SHARPER)
            bool waiting=sharperWaiting(now);
#else
            bool staged=!stagedRecords.empty();
            bool flowControlled=isCoordinator() && outstandingOrders.size()>=8;
            if(isCoordinator() && members.multiLayer() && !pending.empty() && !flowControlled)
                desiredRound=std::max(desiredRound,state.at("cst_round").get<int>()+1);
            bool orderAvailable=members.leaves.count(shard) && !staged && !nextCstOrder().is_null();
            bool waiting=!staged && ((!flowControlled && !pending.empty()) || orderAvailable ||
                (isCoordinator() && desiredRound>state.at("cst_round").get<int>()));
            bool crossWork=staged || !outstandingOrders.empty() || !pendingLocalAckQcs.empty() || !completionResendNeeded.empty();
            if(crossWork && std::chrono::duration_cast<std::chrono::milliseconds>(now-lastForwardHeartbeat).count()>timeoutMs)
                waiting=true;
#endif
            for(const auto& [n,slot]:slots) if(n>applied && !slot.proposal.is_null() && !certificates.count(n)) {
#ifdef ARBOR_SHARPER
                if(sharperCrossValue(slot.proposal.at("body").at("value"))) continue;
#endif
                waiting=true;
            }
            if((waiting || changing) && std::chrono::duration_cast<std::chrono::milliseconds>(now-lastProgress).count()>timeoutMs*(changing?backoff:1)) {
                if(!changing || viewChanges[targetView].size()>=3) startViewChange(std::max(view,targetView)+1);
            }
            if(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastRetry).count()>250) {
                if(isForward()) {
                    broadcast(make("FORWARD_HEARTBEAT"));
#if !defined(ARBOR_SAGUARO) && !defined(ARBOR_SHARPER)
                    if(isCoordinator() && members.multiLayer() && desiredRound>state.at("cst_round").get<int>()) requestRound(desiredRound);
#endif
                }
                if(changing && !myViewChange.is_null()) broadcast(myViewChange);
                if(!lastNewView.is_null() && lastNewView.at("body").at("view")==view && me==view%4) broadcast(lastNewView);
                for(const auto& [n,s]:slots) if(n>applied && !s.proposal.is_null()) {
#ifdef ARBOR_SHARPER
                    if(sharperCrossValue(s.proposal.at("body").at("value"))) continue;
#endif
                    if(!changing && s.proposal.at("body").at("view")==view) {
                        if(me==view%4) broadcast(s.proposal);
                        if(s.prepares.count(me)) broadcast(s.prepares.at(me));
                        if(s.commits.count(me)) broadcast(s.commits.at(me));
                    }
                }
                if(!changing && !pending.empty() && me!=view%4) sendTo(shard,view%4,pending.begin()->second);
#ifdef ARBOR_SAGUARO
                saguaroTick(now);
#elif defined(ARBOR_SHARPER)
                sharperTick(now);
#else
                if(!changing && !pendingCst.empty() && me!=view%4 && pendingCst.begin()->second.contains("signature")) sendTo(shard,view%4,pendingCst.begin()->second);
                if(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastCstRetry).count()>2000) {
                    if(members.leaves.count(shard)) {
                        for(const auto& [key,record]:stagedRecords) {
                            (void)record;sendPrepared(key);forwardPrepared(key,true);requestDependencies(key);
                        }
                        auto resend=pendingLocalAckQcs;
                        resend.insert(completionResendNeeded.begin(),completionResendNeeded.end());
                        for(const auto& key:resend) {sendAck(key);forwardAck(key,true);}
                    } else {
                        requestResultProofs();
                        int resent=0;
                        for(const auto& [index,cert]:outstandingOrders) {
                            (void)index;
                            if(resent++>=4) break;
                            forwardCstOrder(cert,cert.at("proposal").at("body").at("value"),true);
                        }
                    }
                    queryRounds();
                    lastCstRetry=now;
                }
#endif
                if(applied>stableSeq && snapshots.count(applied)) broadcast(make("CHECKPOINT",{{"seq",applied},{"digest",snapshotDigests.at(applied)}}));
                // Sync is a recovery path, not periodic traffic on a healthy
                // replica. Replies carry a checkpoint snapshot and can grow
                // large as cross-shard order metadata accumulates.
                if((waiting || changing || catchupTarget>applied) &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(now-lastProgress).count()>
                       std::max(1000,timeoutMs/2) &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(now-lastSync).count()>2000) {
                    broadcast(make("SYNC_REQUEST",{{"after",applied},{"stable_seq",stableSeq}}));
                    log("sync_requested",{{"after",applied},{"target",catchupTarget}});
                    lastSync=now;
                }
                lastRetry=now;
            }
            drainDeferred();
            propose();
            if(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastStatus).count()>100) {status();lastStatus=now;}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        net.stop();status(false);log("stopped");
    }
};

static int client(const json& cfg,const json& workload,const std::string& output) {
    Membership members(cfg); Key key=readKey(cfg.at("client_private_key"),true);
    Network net; std::mutex mutex; std::queue<json> inbox;
    net.start({workload.value("host",std::string("127.0.0.1")),0},[&](json e){std::lock_guard<std::mutex> l(mutex);inbox.push(std::move(e));});
    struct Request {json envelope;int shard;Clock::time_point start,last;std::map<std::string,std::set<int>> votes;bool done=false;int retries=0;};
    std::map<std::string,Request> requests;
    json timings=json::array();std::map<std::string,size_t> confirmedIds;
    size_t next=0;auto begin=Clock::now();auto lastSend=begin,lastReport=begin;
    uint64_t submittedTransactions=0,confirmedRequests=0;
    double rate=workload.at("rate");double nextDue=0;uint64_t executed=0,ordered=0,duplicates=0,errors=0;
    const auto& jobs=workload.at("requests");
    double timeout=workload.value("timeout_s",30.0);
    while(!stopping && std::chrono::duration<double>(Clock::now()-begin).count()<timeout) {
        auto now=Clock::now();double elapsed=std::chrono::duration<double>(now-begin).count();
        while(next<jobs.size() && elapsed>=nextDue) {
            auto body=jobs[next++];int shard=body.at("target");std::string id=body.at("id");
#ifdef ARBOR_AHL
            // A shared Arbor workload may name an intermediate NCA. Route its
            // unchanged business transactions to AHL's single upper shard.
            if(!body.at("txs").empty() && body.at("txs")[0].at("participants").size()>1) {
                shard=members.ahlCoordinator();body["target"]=shard;
            }
#endif
#ifdef ARBOR_SHARPER
            if(!body.at("txs").empty() && body.at("txs")[0].at("participants").size()>1) {
                const auto ps=body.at("txs")[0].at("participants").get<std::vector<int>>();
                shard=*std::min_element(ps.begin(),ps.end());body["target"]=shard;
            }
#endif
            body["type"]="CLIENT";body["run"]=members.run;body["reply"]={{"host",workload.value("host",std::string("127.0.0.1"))},{"port",net.localPort()}};
            auto env=sign(body,key);requests.emplace(id,Request{env,shard,now,now,{},false,0});
            submittedTransactions+=body.at("txs").size();
            // f=1: two distinct initial recipients guarantee one honest node.
            // Backups relay to the current primary; retries expand to all four.
            for(int r=0;r<2;++r) net.send(members.endpoints.at(identity(shard,r)),env,0);
            nextDue += body.at("txs").size()/rate;lastSend=now;
        }
        std::vector<json> messages;
        {std::lock_guard<std::mutex> l(mutex);while(!inbox.empty()){messages.push_back(std::move(inbox.front()));inbox.pop();}}
        for(const auto& e:messages) try {
            const auto& b=e.at("body");
            if(!members.replicaMessage(e) || b.at("type")!="REPLY") continue;
            std::string id=b.at("request");auto it=requests.find(id);
            if(it==requests.end() || it->second.done || b.at("shard")!=it->second.shard) continue;
            auto& req=it->second;auto result=b.at("results");
            if(result.size()!=req.envelope.at("body").at("txs").size()) continue;
            bool matching=true;
            for(size_t k=0;k<result.size();++k) if(result[k].at("id")!=req.envelope.at("body").at("txs")[k].at("id")) matching=false;
            if(!matching) continue;
            auto& voters=req.votes[hash(result.dump())];voters.insert(b.at("from").get<int>());
            if(voters.size()<2) continue; // f+1 authenticated matching replies.
            req.done=true;
            confirmedRequests++;
            for(const auto& tx:result) {
                auto tid=tx.at("id").get<std::string>();
                auto prior=confirmedIds.find(tid);
                if(prior!=confirmedIds.end()) {
                    // An alias may confirm before the original request. An
                    // executed confirmation must win over duplicate for TPS.
                    auto& old=timings[prior->second];
                    if(!tx.contains("error") && tx.at("kind")=="executed" &&
                       old.at("result").value("kind",std::string())=="duplicate") {
                        duplicates--;executed++;old["result"]=tx;
                        old["latency_s"]=std::chrono::duration<double>(Clock::now()-req.start).count();
                        old["completion_s"]=std::chrono::duration<double>(Clock::now()-begin).count();
                    }
                    continue;
                }
                confirmedIds[tid]=timings.size();
                if(tx.contains("error")) errors++;
                else if(tx.at("kind")=="executed") executed++;
                else if(tx.at("kind")=="duplicate") duplicates++;
                else ordered++;
                auto confirmedAt=Clock::now();
                timings.push_back({{"id",tx.at("id")},
                    {"latency_s",std::chrono::duration<double>(confirmedAt-req.start).count()},
                    {"completion_s",std::chrono::duration<double>(confirmedAt-begin).count()},{"result",tx}});
            }
        } catch(...) {}
        bool all=next==jobs.size();
        for(auto& [id,req]:requests) {
            (void)id;
            if(!req.done) {
                all=false;
                int retryMs=500*(1<<std::min(req.retries,3));
                if(std::chrono::duration_cast<std::chrono::milliseconds>(now-req.last).count()>retryMs) {
                    for(int r=0;r<4;++r) net.send(members.endpoints.at(identity(req.shard,r)),req.envelope,0);
                    req.last=now;req.retries++;
                }
            }
        }
        if(all) break;
        if(std::chrono::duration_cast<std::chrono::seconds>(now-lastReport).count()>=5) {
            std::cout<<"progress submitted="<<submittedTransactions<<" completed="<<executed
                     <<" requests="<<confirmedRequests<<"/"<<jobs.size()
                     <<" elapsed_s="<<std::chrono::duration<double>(now-begin).count()<<std::endl;
            lastReport=now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (void)lastSend;net.stop();
    double elapsed=std::chrono::duration<double>(Clock::now()-begin).count();
    std::vector<double> lat;double latencySum=0;
    for(const auto& t:timings) {double seconds=t.at("latency_s");lat.push_back(seconds);latencySum+=seconds;}
    std::sort(lat.begin(),lat.end());
    double avgLatency=lat.empty()?0:latencySum/lat.size();
    auto percentile=[&](double p){return lat.empty()?0:lat[std::min(lat.size()-1,size_t(std::ceil(p*lat.size())-1))];};
    size_t complete=0;for(const auto& [id,r]:requests){(void)id;if(r.done)complete++;}
    json summary={{"requests",jobs.size()},{"completed_requests",complete},{"executed_transactions",executed},
        {"ordered_only_transactions",ordered},{"duplicate_transactions",duplicates},{"errors",errors},{"elapsed_s",elapsed},
        {"completed_tps",executed/elapsed},{"avg_latency_s",avgLatency},
        {"p50_s",percentile(.5)},{"p95_s",percentile(.95)},
        {"p99_s",percentile(.99)},{"timings",timings},{"workload",workload}};
    writeJson(output,summary);
    bool finished=complete==jobs.size() && errors==0;
    std::cout<<"completed="<<executed<<" ordered_only="<<ordered;
    if(duplicates) std::cout<<" duplicates="<<duplicates;
    std::cout<<" requests="<<complete<<"/"<<jobs.size();
    if(finished) std::cout<<" completed_tps="<<executed/elapsed;
    else std::cout<<" incomplete=true";
    std::cout<<" avg_latency_s="<<avgLatency<<" p50_s="<<percentile(.5)
             <<" p95_s="<<percentile(.95)<<" p99_s="<<percentile(.99);
    std::cout<<'\n';
    return finished?0:2;
}

int main(int argc,char** argv) {
    signal(SIGINT,onSignal);signal(SIGTERM,onSignal);signal(SIGPIPE,SIG_IGN);
    try {
        if(argc==5 && std::string(argv[1])=="sign") {writeJson(argv[4],sign(readJson(argv[2]),readKey(argv[3],true)));return 0;}
        if(argc==5 && std::string(argv[1])=="client") return client(readJson(argv[2]),readJson(argv[3]),argv[4]);
        if(argc!=6 || std::string(argv[1])!="node") throw std::runtime_error("usage: arbor_node node RESOLVED_CONFIG SHARD REPLICA NODE_DIR | client CONFIG WORKLOAD OUTPUT | sign INPUT KEY OUTPUT");
        Replica replica(readJson(argv[2]),std::stoi(argv[3]),std::stoi(argv[4]),argv[5]);replica.run();
        return 0;
    } catch(const std::exception& e) {std::cerr<<"error: "<<e.what()<<'\n';return 1;}
}
