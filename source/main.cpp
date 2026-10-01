#include "network.h"
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
    Key clientKey;
    explicit Membership(const json& c): config(c), run(c.at("run_id")) {
        for (const auto& s:c.at("shards")) {
            int id=s.at("id");
            parent[id]=s.at("parent").is_null() ? -1 : s.at("parent").get<int>();
            leaves.insert(id);
        }
        for (const auto& [id,p]:parent) { (void)id; leaves.erase(p); }
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
    bool twoLayer() const {
        int root=-1;
        for(const auto& [id,p]:parent) if(p==-1) root=id;
        if(root<0 || leaves.size()!=2) return false;
        for(int leaf:leaves) if(parent.at(leaf)!=root) return false;
        return true;
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
            return it!=publicKeys.end() && verify(env,it->second);
        } catch (...) { return false; }
    }
    bool clientMessage(const json& env) const {
        try { return env.at("body").at("run")==run && verify(env,clientKey); }
        catch (...) { return false; }
    }
};

// A bounded PBFT sequence window, batching, stable checkpoints and certified
// view changes. PREPARE votes are from backups only (2f); COMMIT needs 2f+1.
class Replica {
    struct Slot {
        json proposal;
        std::map<int,json> prepares,commits;
        bool prepared=false, commitSent=false, committed=false;
    };
    Membership members;
    int shard, me, view=0, targetView=0;
    bool changing=false;
    int applied=0, stableSeq=0;
    std::string dir;
    Key privateKey;
    Network net;
    std::mutex inboxMutex;
    std::queue<json> inbox;
    std::map<int,Slot> slots;
    std::map<int,json> preparedHistory, certificates;
    std::map<int,json> snapshots;
    std::map<int,std::map<int,json>> checkpointVotes;
    std::map<int,std::map<int,json>> viewChanges;
    json stableProof=json::array(), stableState;
    json state={{"seq",0},{"chain",hash("arbor-genesis")},{"kv",json::object()},
                {"seen",json::object()},{"requests",json::object()},{"executed",0},{"ordered_cst",0},
                {"cst_batches",json::object()},{"cst_seen",json::object()},{"leaf_ordered_cst",0},{"last_cst_seq",0},
                {"cst_staged",json::object()},{"cst_finalized",json::object()},{"cst_decisions",json::object()},
                {"cst_order_index",0}};
    std::map<std::string,json> pending;
    std::map<std::string,json> pendingCst;
    std::map<std::string,json> pendingDecisions;
    std::map<std::string,std::map<int,json>> preparedVotes, ackVotes;
    std::map<std::string,std::map<int,json>> readyVotes;
    std::map<std::string,json> readySent, completedCstResults;
    std::map<std::string,json> decisionCerts;
    std::set<std::string> completedCstBatches, ackConfirmed;
    std::map<std::string,std::set<int>> doneVotes;
    mutable std::set<std::string> checkedOrders, checkedRecords, checkedProofs,
                                  checkedReadies, checkedDecisions, checkedDecisionCerts;
    std::map<std::string,Clock::time_point> pingStarts;
    std::map<std::string,std::string> pingPeers;
    json probes=json::object();
    std::ofstream journal, events;
    Clock::time_point lastProgress=Clock::now(),lastRetry=Clock::now(),lastCstRetry=Clock::now(),
                      lastStatus=Clock::now(),batchStart=Clock::now();
    uint64_t rejected=0, duplicates=0, viewCount=0, executionNs=0;
    std::atomic<uint64_t> inboxDropped{0};
    int timeoutMs, batchSize, checkpointEvery;
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
    int coordinator() const {
        for(const auto& [id,parent]:members.parent) if(parent==-1) return id;
        throw std::runtime_error("missing coordinator");
    }
    int otherLeaf(int leaf) const {
        for(int id:members.leaves) if(id!=leaf) return id;
        throw std::runtime_error("missing peer leaf");
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
        try {
            if (!members.clientMessage(e)) return false;
            const auto& b=e.at("body");
            if(b.at("type")!="CLIENT" || b.at("target")!=target || !b.at("id").is_string() ||
               b.at("id").get<std::string>().size()>200 || !b.at("txs").is_array() || b.at("txs").empty() || int(b.at("txs").size())>batchSize) return false;
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
    bool validCoordinatorCertificate(const json& c) const {
        try {
            auto fingerprint=hash(c.dump());
            if(checkedOrders.count(fingerprint)) return true;
            if(!members.twoLayer() || !validForeignCertificate(c,coordinator())) return false;
            const auto& b=c.at("proposal").at("body");
            const auto& value=b.at("value");
            if(!value.at("requests").is_array() || value.at("requests").empty() ||
               value.contains("cst_orders") || value.contains("cst_decisions")) return false;
            if(!value.at("cst_order_index").is_number_integer() || value.at("cst_order_index").get<int>()<=0) return false;
            int total=0,local=0;
            for(const auto& req:value.at("requests")) {
                if(!validRequestForShard(req,coordinator())) return false;
                for(const auto& tx:req.at("body").at("txs")) {
                    ++total;
                    for(auto participant:tx.at("participants")) if(participant==shard) ++local;
                }
            }
            bool valid=total<=batchSize && (!members.leaves.count(shard) || local>0);
            if(valid && checkedOrders.size()<10000) checkedOrders.insert(fingerprint);
            return valid;
        } catch (...) { return false; }
    }
    static std::string cstKey(const json& cert) {
        const auto& b=cert.at("proposal").at("body");
        return std::to_string(b.at("shard").get<int>())+":"+std::to_string(b.at("seq").get<int>());
    }
    json accessFor(const json& tx,int owner) const {
        for(const auto& access:tx.at("accesses")) if(access.at("shard")==owner) return access;
        throw std::runtime_error("missing participant access");
    }
    bool validPreparedRecord(const json& record,int origin) const {
        try {
            auto fingerprint=std::to_string(origin)+":"+hash(record.dump());
            if(checkedRecords.count(fingerprint)) return true;
            if(!members.leaves.count(origin) || !validCoordinatorCertificate(record.at("order_certificate")) ||
               record.at("shard")!=origin || record.at("batch_key")!=cstKey(record.at("order_certificate")) ||
               record.at("order_digest")!=record.at("order_certificate").at("proposal").at("body").at("digest") ||
               !record.at("reads").is_object() || !record.at("writes").is_array()) return false;
            std::set<std::string> keys;
            size_t index=0;
            const auto& requests=record.at("order_certificate").at("proposal").at("body").at("value").at("requests");
            for(const auto& request:requests) for(const auto& tx:request.at("body").at("txs")) {
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
                body.at("record_digest")==hash(vote.at("record").dump()) &&
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
            auto recordDigest=hash(record.dump());
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
    json decisionRecord(const json& decision,int owner) const {
        for(const auto& proof:decision.at("ready")[0].at("body").at("proofs")) {
            auto record=proofRecord(proof);
            if(record.at("shard")==owner) return record;
        }
        throw std::runtime_error("decision missing participant");
    }
    bool validReady(const json& ready) const {
        try {
            auto fingerprint=hash(ready.dump());
            if(checkedReadies.count(fingerprint)) return true;
            const auto& b=ready.at("body"); int sender=b.at("shard");
            if(!members.twoLayer() || !members.leaves.count(sender) || !members.replicaMessage(ready) ||
               b.at("type")!="CST_READY" || b.at("target")!=coordinator() ||
               !b.at("proofs").is_array() || b.at("proofs").size()!=2) return false;
            std::set<int> owners; int previousOwner=-1;
            std::string digest;
            for(const auto& proof:b.at("proofs")) {
                int owner=proofRecord(proof).at("shard");
                if(owner<=previousOwner || !owners.insert(owner).second || !validPreparedProof(proof,owner)) return false;
                previousOwner=owner;
                const auto& rec=proofRecord(proof);
                if(rec.at("batch_key")!=b.at("batch_key")) return false;
                if(digest.empty()) digest=rec.at("order_digest");
                else if(rec.at("order_digest")!=digest) return false;
            }
            bool valid=owners==members.leaves;
            if(valid && checkedReadies.size()<10000) checkedReadies.insert(fingerprint);
            return valid;
        } catch (...) { return false; }
    }
    bool validDecision(const json& decision) const {
        try {
            auto fingerprint=hash(decision.dump());
            if(checkedDecisions.count(fingerprint)) return true;
            const auto& ready=decision.at("ready");
            if(!ready.is_array() || ready.size()!=2 || !validReady(ready[0]) || !validReady(ready[1]) ||
               ready[0].at("body").at("shard")==ready[1].at("body").at("shard") ||
               ready[0].at("body").at("batch_key")!=ready[1].at("body").at("batch_key") ||
               decision.at("batch_key")!=ready[0].at("body").at("batch_key")) return false;
            for(size_t i=0;i<2;++i)
                if(proofRecord(ready[0].at("body").at("proofs")[i])!=
                   proofRecord(ready[1].at("body").at("proofs")[i])) return false;
            auto left=decisionRecord(decision,*members.leaves.begin()),
                 right=decisionRecord(decision,*members.leaves.rbegin());
            for(size_t i=0;i<left.at("writes").size();++i)
                if(left.at("writes")[i].at("duplicate")!=right.at("writes")[i].at("duplicate")) return false;
            if(checkedDecisions.size()<10000) checkedDecisions.insert(fingerprint);
            return true;
        } catch (...) { return false; }
    }
    bool validDecisionCertificate(const json& cert) const {
        try {
            auto fingerprint=hash(cert.dump());
            if(checkedDecisionCerts.count(fingerprint)) return true;
            if(!validForeignCertificate(cert,coordinator())) return false;
            const auto& value=cert.at("proposal").at("body").at("value");
            bool valid=value.at("requests").is_array() && value.at("requests").empty() &&
                !value.contains("cst_orders") && value.at("cst_decisions").is_array() &&
                value.at("cst_decisions").size()==1 && validDecision(value.at("cst_decisions")[0]);
            if(valid && checkedDecisionCerts.size()<10000) checkedDecisionCerts.insert(fingerprint);
            return valid;
        } catch (...) { return false; }
    }
    bool validValue(const json& value) const {
        try {
            int n=0;
            if (!value.at("requests").is_array()) return false;
            if(!state.at("cst_staged").empty() && members.leaves.count(shard) &&
               (!value.at("requests").empty() || value.contains("cst_orders"))) return false;
            for(const auto& e:value.at("requests")) {
                if (!validRequest(e)) return false;
                n+=e.at("body").at("txs").size();
            }
            if(shard==coordinator() && members.twoLayer()) {
                std::map<std::string,std::string> ids;
                for(const auto& request:value.at("requests"))
                    for(const auto& tx:request.at("body").at("txs")) {
                        auto id=tx.at("id").get<std::string>(),digest=hash(tx.dump());
                        if((state.at("seen").contains(id) && state.at("seen").at(id).at("tx_digest")!=digest) ||
                           (ids.count(id) && ids.at(id)!=digest)) return false;
                        ids[id]=digest;
                    }
            }
            if(value.contains("cst_decisions")) {
                if(shard!=coordinator() || !members.twoLayer() || !value.at("requests").empty() ||
                   value.contains("cst_orders") || value.contains("cst_finalizations") ||
                   !value.at("cst_decisions").is_array() || value.at("cst_decisions").size()!=1 ||
                   !validDecision(value.at("cst_decisions")[0]) ||
                   state.at("cst_decisions").contains(value.at("cst_decisions")[0].at("batch_key").get<std::string>())) return false;
            }
            if(value.contains("cst_order_index")) {
                if(shard!=coordinator() || !members.twoLayer() || value.at("requests").empty() ||
                   value.contains("cst_orders") || value.contains("cst_decisions") ||
                   value.contains("cst_finalizations") ||
                   value.at("cst_order_index")!=state.at("cst_order_index").get<int>()+1) return false;
            } else if(shard==coordinator() && members.twoLayer() && !value.at("requests").empty()) return false;
            if(value.contains("cst_finalizations")) {
                if(!members.leaves.count(shard) || !members.twoLayer() || !value.at("requests").empty() ||
                   value.contains("cst_orders") || value.contains("cst_decisions") ||
                   !value.at("cst_finalizations").is_array() || value.at("cst_finalizations").size()!=1) return false;
                const auto& cert=value.at("cst_finalizations")[0];
                if(!validDecisionCertificate(cert)) return false;
                const auto& decision=cert.at("proposal").at("body").at("value").at("cst_decisions")[0];
                auto key=decision.at("batch_key").get<std::string>();
                if(!state.at("cst_staged").contains(key) ||
                   state.at("cst_staged").at(key)!=decisionRecord(decision,shard)) return false;
            }
            if(value.contains("cst_orders")) {
                if(!members.leaves.count(shard) || !members.twoLayer() || !value.at("cst_orders").is_array() ||
                   value.at("cst_orders").size()>1 || value.contains("cst_decisions") ||
                   value.contains("cst_finalizations")) return false;
                for(const auto& cert:value.at("cst_orders")) {
                    if(!validCoordinatorCertificate(cert) ||
                       cert.at("proposal").at("body").at("value").at("cst_order_index").get<int>()!=
                           state.at("last_cst_seq").get<int>()+1) return false;
                    for(const auto& req:cert.at("proposal").at("body").at("value").at("requests"))
                        for(const auto& tx:req.at("body").at("txs"))
                            for(auto participant:tx.at("participants")) if(participant==shard) ++n;
                }
            }
            return n<=batchSize;
        } catch (...) { return false; }
    }
    bool validProposal(const json& e) const {
        try {
            const auto& b=e.at("body"); int v=b.at("view");
            return v>=0 && b.at("from")==v%4 && validValue(b.at("value")) &&
                validVote(e,"PREPREPARE",v,b.at("seq"),hash(b.at("value").dump()));
        } catch (...) { return false; }
    }
    bool validPrepared(const json& p) const {
        try {
            const auto& pp=p.at("proposal"); const auto& b=pp.at("body");
            if(!validProposal(pp)) return false;
            int v=b.at("view"); std::set<int> voters;
            for(const auto& e:p.at("prepares")) {
                int r=e.at("body").at("from");
                if(r==v%4 || !voters.insert(r).second || !validVote(e,"PREPARE",v,b.at("seq"),b.at("digest"))) return false;
            }
            return voters.size()>=2;
        } catch (...) { return false; }
    }
    bool validCertificate(const json& c) const {
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
        json votes=json::array(); const auto& b=s.proposal.at("body");
        for(const auto& [r,p]:s.prepares) if(r!=b.at("view").get<int>()%4 && validVote(p,"PREPARE",b.at("view"),b.at("seq"),b.at("digest"))) votes.push_back(p);
        return {{"proposal",s.proposal},{"prepares",votes}};
    }
    bool validStable(const json& s) const {
        try {
            int seq=s.at("seq");
            if(seq==0) return s.at("state")==genesis() && s.at("proof").empty();
            if(seq<0 || s.at("state").at("seq")!=seq) return false;
            auto digest=hash(s.at("state").dump()); std::set<int> voters;
            for(const auto& e:s.at("proof")) {
                const auto& b=e.at("body");
                if(!members.replicaMessage(e) || b.at("shard")!=shard || b.at("type")!="CHECKPOINT" ||
                   b.at("seq")!=seq || b.at("digest")!=digest || !voters.insert(b.at("from").get<int>()).second) return false;
            }
            return voters.size()>=3;
        } catch (...) {return false;}
    }
    static json genesis() { return {{"seq",0},{"chain",hash("arbor-genesis")},{"kv",json::object()},
                {"seen",json::object()},{"requests",json::object()},{"executed",0},{"ordered_cst",0},
                {"cst_batches",json::object()},{"cst_seen",json::object()},{"leaf_ordered_cst",0},{"last_cst_seq",0},
                {"cst_staged",json::object()},{"cst_finalized",json::object()},{"cst_decisions",json::object()},
                {"cst_order_index",0}}; }
    bool validVC(const json& e,int v) const {
        try {
            const auto& b=e.at("body");
            if(!members.replicaMessage(e) || b.at("shard")!=shard || b.at("type")!="VIEW_CHANGE" || b.at("view")!=v || !validStable(b.at("stable"))) return false;
            int h=b.at("stable").at("seq"); std::set<int> sequences;
            if(b.at("prepared").size()>size_t(window)) return false;
            for(const auto& p:b.at("prepared")) {
                const auto& pb=p.at("proposal").at("body"); int seq=pb.at("seq");
                if(!validPrepared(p) || pb.at("view").get<int>()>=v || seq<=h || seq>h+window || !sequences.insert(seq).second) return false;
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
        if(h>applied) {
            state=s.at("state"); applied=h;
            log("state_sync",{{"seq",h}});
        }
        stableSeq=h; stableState=s.at("state"); stableProof=s.at("proof");
        for(auto it=slots.begin();it!=slots.end();) it=it->first<=h?slots.erase(it):std::next(it);
        for(auto it=preparedHistory.begin();it!=preparedHistory.end();) it=it->first<=h?preparedHistory.erase(it):std::next(it);
        for(auto it=certificates.begin();it!=certificates.end();) it=it->first<=h?certificates.erase(it):std::next(it);
        for(auto it=snapshots.begin();it!=snapshots.end();) it=it->first<h?snapshots.erase(it):std::next(it);
        for(auto it=checkpointVotes.begin();it!=checkpointVotes.end();) it=it->first<h?checkpointVotes.erase(it):std::next(it);
        for(auto it=pending.begin();it!=pending.end();) {
            if(state["requests"].contains(it->first)) {
                if(completedCstResults.count(it->first)) reply(it->second,completedCstResults.at(it->first));
                else if(members.leaves.count(shard) || !members.twoLayer()) reply(it->second,state["requests"][it->first]["results"]);
                it=pending.erase(it);
            } else ++it;
        }
        for(auto it=pendingCst.begin();it!=pendingCst.end();)
            it=state["cst_batches"].contains(it->first)?pendingCst.erase(it):std::next(it);
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
            if(!validProposal(p) || pb.at("view")!=v || pb.at("seq")!=it->first || pb.at("value")!=it->second) throw std::runtime_error("unsafe new-view proposal");
            ++it;
        }
        installStable(checkpoint); slots.clear();
        view=v; targetView=v; changing=false; viewCount++; lastProgress=Clock::now(); lastNewView=e;
        for(auto i=viewChanges.begin();i!=viewChanges.end();) i=i->first<=v?viewChanges.erase(i):std::next(i);
        log("new_view_installed",{{"primary",view%4}});
        for(const auto& p:b.at("proposals")) acceptProposal(p);
    }
    void acceptProposal(const json& e) {
        const auto& b=e.at("body"); int n=b.at("seq");
        if(changing || b.at("view")!=view || n<=stableSeq || n>stableSeq+window || !validProposal(e)) return;
        auto& s=slots[n];
        if(!s.proposal.is_null()) { if(s.proposal.at("body").at("digest")!=b.at("digest")) rejected++; return; }
        // A committed sequence may be replayed in a new view, never replaced.
        if(certificates.count(n) && certificates[n].at("proposal").at("body").at("digest")!=b.at("digest")) throw std::runtime_error("conflicting committed sequence");
        s.proposal=e;
        lastProgress=Clock::now();
        log("preprepare",{{"seq",n},{"digest",b.at("digest")}});
        if(me!=view%4) broadcast(make("PREPARE",{{"seq",n},{"digest",b.at("digest")}}));
        advance(n);
    }
    void advance(int n) {
        auto& s=slots.at(n); if(s.proposal.is_null()) return;
        auto proof=preparedProof(s); const auto& b=s.proposal.at("body");
        if(!s.prepared && proof.at("prepares").size()>=2) {
            s.prepared=true; preparedHistory[n]=proof;
            if(!s.commitSent) { s.commitSent=true; broadcast(make("COMMIT",{{"seq",n},{"digest",b.at("digest")}})); }
            log("prepared",{{"seq",n},{"digest",b.at("digest")}});
        }
        json votes=json::array();
        for(const auto& [r,e]:s.commits) { (void)r; if(validVote(e,"COMMIT",view,n,b.at("digest"))) votes.push_back(e); }
        if(s.prepared && votes.size()>=3 && !s.committed) {
            s.committed=true; proof["commits"]=votes; certificates[n]=proof;
            log("committed_local",{{"seq",n},{"digest",b.at("digest")}});
            applyReady();
        }
    }
    void reply(const json& req,const json& results) {
        const auto& b=req.at("body"); const auto& ep=b.at("reply");
        auto e=make("REPLY",{{"request",b.at("id")},{"results",results}});
        net.send({ep.at("host"),ep.at("port")},e,0);
    }
    void replyConflict(const json& req) {
        json results=json::array();
        for(const auto& tx:req.at("body").at("txs"))
            results.push_back({{"id",tx.at("id")},{"error","id_conflict"}});
        reply(req,results);
    }
    void forwardCstOrder(const json& cert,const json& value) {
        if(members.leaves.count(shard) || !members.twoLayer()) return;
        std::set<int> destinations;
        for(const auto& req:value.at("requests"))
            for(const auto& tx:req.at("body").at("txs"))
                for(auto participant:tx.at("participants")) destinations.insert(participant.get<int>());
        for(int leaf:destinations) {
            auto message=make("CST_ORDER",{{"target",leaf},{"certificate",cert}});
            for(int replica=0;replica<4;++replica) sendTo(leaf,replica,message);
        }
        if(!destinations.empty()) log("cst_order_forwarded",{{"coordinator_seq",cert.at("proposal").at("body").at("seq")},
            {"destinations",destinations}});
    }
    void sendPrepared(const std::string& key) {
        if(!members.leaves.count(shard) || !state["cst_staged"].contains(key)) return;
        const auto& record=state["cst_staged"].at(key);
        auto vote=make("CST_PREPARED",{{"batch_key",key},{"record_digest",hash(record.dump())}});
        vote["record"]=record;
        for(int leaf:members.leaves) for(int replica=0;replica<4;++replica) sendTo(leaf,replica,vote);
    }
    void stageCst(const json& cert) {
        auto key=cstKey(cert);
        json record={{"shard",shard},{"batch_key",key},
                     {"order_digest",cert.at("proposal").at("body").at("digest")},
                     {"order_certificate",cert},{"reads",json::object()},{"writes",json::array()}};
        for(const auto& request:cert.at("proposal").at("body").at("value").at("requests"))
            for(const auto& tx:request.at("body").at("txs")) {
                auto access=accessFor(tx,shard);
                auto localKey=access.at("key").get<std::string>();
                std::string id=tx.at("id");
                bool duplicate=state["cst_seen"].contains(id);
                if(duplicate && state["cst_seen"].at(id).at("tx_digest")!=hash(tx.dump()))
                    throw std::runtime_error("cross-shard transaction ID reused with different content");
                if(!duplicate && !record["reads"].contains(localKey))
                    record["reads"][localKey]=state["kv"].contains(localKey)?state["kv"].at(localKey):initialAccount();
                record["writes"].push_back({{"id",tx.at("id")},{"tx_digest",hash(tx.dump())},
                                             {"key",localKey},{"value",access.at("value")},
                                             {"fib",duplicate?expectedFib:fibonacci()},{"duplicate",duplicate}});
                if(!duplicate) {
                    state["cst_seen"][id]={{"tx_digest",hash(tx.dump())},{"coordinator_batch",key}};
                    state["leaf_ordered_cst"]=state["leaf_ordered_cst"].get<uint64_t>()+1;
                }
            }
        state["cst_staged"][key]=record;
        log("cst_staged",{{"batch_key",key},{"transactions",record["writes"].size()}});
    }
    void maybeReady(const std::string& key) {
        if(!state["cst_staged"].contains(key) || readySent.count(key)) return;
        json proofs=json::array();
        for(int leaf:members.leaves) {
            json proof;
            std::string prefix=key+"|"+std::to_string(leaf)+"|";
            for(const auto& [group,votes]:preparedVotes) if(group.rfind(prefix,0)==0 && votes.size()>=3) {
                json selected=json::array();
                for(const auto& [replica,vote]:votes) {
                    (void)replica;
                    if(selected.size()<3) selected.push_back({{"body",vote.at("body")},{"signature",vote.at("signature")}});
                }
                json candidate={{"record",votes.begin()->second.at("record")},{"votes",selected}};
                if(validPreparedProof(candidate,leaf)) {proof=candidate;break;}
            }
            if(proof.is_null()) return;
            proofs.push_back(proof);
        }
        if(proofRecord(proofs[0]).at("order_digest")!=proofRecord(proofs[1]).at("order_digest")) return;
        auto ready=make("CST_READY",{{"batch_key",key},{"target",coordinator()},{"proofs",proofs}});
        if(!validReady(ready)) throw std::runtime_error("locally constructed CST_READY is invalid");
        readySent[key]=ready;
        sendTo(coordinator(),me,ready);
        log("cst_dependencies_ready",{{"batch_key",key}});
    }
    void forwardDecision(const json& cert) {
        const auto& decision=cert.at("proposal").at("body").at("value").at("cst_decisions")[0];
        for(int leaf:members.leaves) {
            auto env=make("CST_DECISION",{{"target",leaf},{"certificate",cert}});
            sendTo(leaf,me,env);
        }
        (void)decision;
    }
    void sendAck(const std::string& key) {
        if(!state["cst_finalized"].contains(key) || ackConfirmed.count(key)) return;
        const auto& done=state["cst_finalized"].at(key);
        auto ack=make("CST_ACK",{{"batch_key",key},{"target",coordinator()},
                                  {"decision_digest",done.at("decision_digest")},
                                  {"result_digest",done.at("result_digest")}});
        for(int replica=0;replica<4;++replica) sendTo(coordinator(),replica,ack);
    }
    void sendDone(const std::string& key) {
        if(!state["cst_decisions"].contains(key)) return;
        for(int leaf:members.leaves) {
            auto done=make("CST_DONE",{{"target",leaf},{"batch_key",key},
                                       {"decision_digest",hash(state["cst_decisions"].at(key).dump())}});
            for(int replica=0;replica<4;++replica) sendTo(leaf,replica,done);
        }
    }
    void finalizeCst(const json& decision) {
        auto key=decision.at("batch_key").get<std::string>();
        auto mine=decisionRecord(decision,shard), peer=decisionRecord(decision,otherLeaf(shard));
        if(mine!=state["cst_staged"].at(key)) throw std::runtime_error("local prepared result differs from decision");
        std::map<int,json> working={{shard,mine.at("reads")},{otherLeaf(shard),peer.at("reads")}};
        json localWrites=json::array();
        const auto& requests=mine.at("order_certificate").at("proposal").at("body").at("value").at("requests");
        size_t index=0;
        for(const auto& request:requests) for(const auto& tx:request.at("body").at("txs")) {
            bool duplicate=mine.at("writes")[index++].at("duplicate");
            if(duplicate) continue;
            auto a=accessFor(tx,shard), b=accessFor(tx,otherLeaf(shard));
            std::string ak=a.at("key"),bk=b.at("key");
            json oldA=working.at(shard).at(ak),oldB=working.at(otherLeaf(shard)).at(bk);
            uint64_t fib=expectedFib;
            uint64_t newA=a.at("value").get<uint64_t>()+oldB.at("value").get<uint64_t>()+fib;
            uint64_t newB=b.at("value").get<uint64_t>()+oldA.at("value").get<uint64_t>()+fib;
            json nextA={{"version",oldA.at("version").get<uint64_t>()+1},{"value",newA},{"fib",fib},
                        {"digest",hash(oldA.dump()+oldB.dump()+tx.dump()+std::to_string(shard))}};
            json nextB={{"version",oldB.at("version").get<uint64_t>()+1},{"value",newB},{"fib",fib},
                        {"digest",hash(oldB.dump()+oldA.dump()+tx.dump()+std::to_string(otherLeaf(shard)))}};
            working[shard][ak]=nextA;
            working[otherLeaf(shard)][bk]=nextB;
            localWrites.push_back({{"id",tx.at("id")},{"key",ak},{"state",nextA}});
            state["cst_seen"][tx.at("id").get<std::string>()]["committed"]=true;
            state["executed"]=state["executed"].get<uint64_t>()+1;
        }
        for(auto it=working.at(shard).begin();it!=working.at(shard).end();++it) state["kv"][it.key()]=it.value();
        auto digest=hash(localWrites.dump());
        state["cst_finalized"][key]={{"decision_digest",hash(decision.dump())},{"result_digest",digest}};
        state["cst_staged"].erase(key);
        log("cst_finalized",{{"batch_key",key},{"transactions",localWrites.size()}});
    }
    void maybeCompleteBatch(const std::string& key) {
        if(!state["cst_decisions"].contains(key)) return;
        if(completedCstBatches.count(key)) {sendDone(key);return;}
        const auto& decision=state["cst_decisions"].at(key);
        std::map<int,std::string> digests;
        for(int leaf:members.leaves) {
            std::string prefix=key+"|"+std::to_string(leaf)+"|";
            for(const auto& [group,votes]:ackVotes) if(group.rfind(prefix,0)==0 && votes.size()>=3) {
                const auto& body=votes.begin()->second.at("body");
                if(body.at("decision_digest")==hash(decision.dump())) {digests[leaf]=body.at("result_digest");break;}
            }
            if(!digests.count(leaf)) return;
        }
        auto record=decisionRecord(decision,*members.leaves.begin());
        const auto& cert=record.at("order_certificate");
        size_t index=0;
        for(const auto& request:cert.at("proposal").at("body").at("value").at("requests")) {
            std::string rid=request.at("body").at("id");
            if(completedCstResults.count(rid)) {index+=request.at("body").at("txs").size();continue;}
            json results=json::array();
            for(const auto& tx:request.at("body").at("txs")) {
                bool duplicate=record.at("writes")[index++].at("duplicate");
                results.push_back({{"id",tx.at("id")},{"kind",duplicate?"duplicate":"executed"},
                                   {"digest",hash(tx.at("id").get<std::string>()+hash(decision.dump())+
                                                  digests.begin()->second+digests.rbegin()->second)}});
            }
            completedCstResults[rid]=results;
            reply(request,results);
        }
        completedCstBatches.insert(key);
        decisionCerts.erase(key);
        for(auto it=ackVotes.begin();it!=ackVotes.end();) it=it->first.rfind(key+"|",0)==0?ackVotes.erase(it):std::next(it);
        sendDone(key);
        log("cst_complete",{{"batch_key",key}});
    }
    void applyReady() {
        while(certificates.count(applied+1)) {
            int n=applied+1; const auto cert=certificates.at(n); const auto& value=cert.at("proposal").at("body").at("value");
            if(members.leaves.count(shard) && !state["cst_staged"].empty() &&
               (!value.at("requests").empty() || value.contains("cst_orders"))) break;
            auto before=Clock::now();
            for(const auto& req:value.at("requests")) {
                const auto& rb=req.at("body"); std::string rid=rb.at("id");
                auto requestHash=hash(rb.at("txs").dump());
                json results=json::array();
                if(state["requests"].contains(rid) && state["requests"][rid]["txs_hash"]!=requestHash) {
                    for(const auto& tx:rb.at("txs")) results.push_back({{"id",tx.at("id")},{"error","request_id_conflict"}});
                    if(members.leaves.count(shard) || !members.twoLayer()) reply(req,results);
                    pending.erase(rid); continue;
                }
                for(const auto& tx:rb.at("txs")) {
                    std::string id=tx.at("id"); auto d=hash(tx.dump());
                    if(state["seen"].contains(id)) {
                        duplicates++;
                        if(state["seen"][id]["tx_digest"]!=d) results.push_back({{"id",id},{"error","id_conflict"}});
                        else results.push_back(state["seen"][id]["result"]);
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
                        result["digest"]=digest; state["executed"]=state["executed"].get<uint64_t>()+1;
                    } else { result["digest"]=d; state["ordered_cst"]=state["ordered_cst"].get<uint64_t>()+1; }
                    state["seen"][id]={{"tx_digest",d},{"result",result}}; results.push_back(result);
                }
                state["requests"][rid]={{"txs_hash",requestHash},{"results",results}};
                pending.erase(rid);
                if(members.leaves.count(shard) || !members.twoLayer()) reply(req,results);
            }
            if(value.contains("cst_order_index")) state["cst_order_index"]=value.at("cst_order_index");
            if(value.contains("cst_orders")) for(const auto& cst:value.at("cst_orders")) {
                std::string key=cstKey(cst);
                std::string digest=cst.at("proposal").at("body").at("digest");
                if(state["cst_batches"].contains(key) && state["cst_batches"][key]!=digest)
                    throw std::runtime_error("conflicting certified CST batch");
                if(!state["cst_batches"].contains(key)) {
                    state["cst_batches"][key]=digest;
                    state["last_cst_seq"]=cst.at("proposal").at("body").at("value").at("cst_order_index");
                    stageCst(cst);
                    log("cst_ordered_at_leaf",{{"coordinator_batch",key},{"digest",digest}});
                }
                pendingCst.erase(key);
            }
            if(value.contains("cst_decisions")) for(const auto& decision:value.at("cst_decisions")) {
                auto key=decision.at("batch_key").get<std::string>();
                state["cst_decisions"][key]=decision;
                readyVotes.erase(key);
                log("cst_decided",{{"batch_key",key}});
            }
            if(value.contains("cst_finalizations")) for(const auto& decisionCert:value.at("cst_finalizations")) {
                const auto& decision=decisionCert.at("proposal").at("body").at("value").at("cst_decisions")[0];
                auto key=decision.at("batch_key").get<std::string>();
                finalizeCst(decision);
                pendingDecisions.erase(key);
                readySent.erase(key);
            }
            state["chain"]=hash(state["chain"].get<std::string>()+std::to_string(n)+value.dump());
            applied=n; state["seq"]=n;
            executionNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-before).count();
            journal<<json{{"seq",n},{"value_digest",hash(value.dump())},{"state_digest",hash(state.dump())},{"certificate",cert}}.dump()<<'\n'; journal.flush();
            if(!members.leaves.count(shard)) forwardCstOrder(cert,value);
            if(members.leaves.count(shard)) {
                if(value.contains("cst_orders")) for(const auto& cst:value.at("cst_orders")) sendPrepared(cstKey(cst));
                if(value.contains("cst_finalizations")) for(const auto& decisionCert:value.at("cst_finalizations")) {
                    const auto& d=decisionCert.at("proposal").at("body").at("value").at("cst_decisions")[0];
                    sendAck(d.at("batch_key"));
                }
            } else if(value.contains("cst_decisions")) {
                for(const auto& decision:value.at("cst_decisions")) decisionCerts[decision.at("batch_key")]=cert;
                forwardDecision(cert);
                for(const auto& decision:value.at("cst_decisions")) maybeCompleteBatch(decision.at("batch_key"));
            }
            lastProgress=Clock::now();
            if(n%checkpointEvery==0) {
                snapshots[n]=state;
                broadcast(make("CHECKPOINT",{{"seq",n},{"digest",hash(state.dump())}}));
                checkStable(n);
            }
        }
    }
    void checkStable(int n) {
        if(!snapshots.count(n) || n<=stableSeq) return;
        auto d=hash(snapshots[n].dump()); json proof=json::array();
        for(const auto& [r,e]:checkpointVotes[n]) { (void)r; if(e.at("body").at("digest")==d) proof.push_back(e); }
        if(proof.size()>=3) installStable({{"seq",n},{"state",snapshots[n]},{"proof",proof}});
    }
    void propose() {
        if(changing || me!=view%4 || applied+1>stableSeq+window) return;
        // One fresh batch in flight. Recovery slots may coexist after a view change.
        if(slots.count(applied+1) && !slots.at(applied+1).proposal.is_null()) return;
        if(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-batchStart).count()<members.config.at("consensus").at("batch_wait_ms").get<int>()) return;
        if(members.leaves.count(shard) && !state["cst_staged"].empty()) {
            auto key=state["cst_staged"].begin().key();
            auto found=pendingDecisions.find(key);
            if(found==pendingDecisions.end()) return;
            json value={{"requests",json::array()},
                        {"cst_finalizations",json::array({found->second.at("body").at("certificate")})}};
            broadcast(make("PREPREPARE",{{"seq",applied+1},{"digest",hash(value.dump())},{"value",value}}));
            batchStart=Clock::now(); return;
        }
        if(shard==coordinator() && members.twoLayer()) for(const auto& [key,fromLeaves]:readyVotes) {
            if(state["cst_decisions"].contains(key) || fromLeaves.size()!=2) continue;
            json decision={{"batch_key",key},{"ready",json::array({fromLeaves.at(*members.leaves.begin()),
                                                fromLeaves.at(*members.leaves.rbegin())})}};
            if(!validDecision(decision)) continue;
            json value={{"requests",json::array()},{"cst_decisions",json::array({decision})}};
            broadcast(make("PREPREPARE",{{"seq",applied+1},{"digest",hash(value.dump())},{"value",value}}));
            batchStart=Clock::now(); return;
        }
        if(pending.empty() && pendingCst.empty()) return;
        json reqs=json::array(); int count=0;
        std::map<std::string,std::string> selectedIds;
        for(auto it=pending.begin();it!=pending.end();) {
            if(state["requests"].contains(it->first)) {
                if(completedCstResults.count(it->first)) reply(it->second,completedCstResults.at(it->first));
                else if(members.leaves.count(shard) || !members.twoLayer()) reply(it->second,state["requests"][it->first]["results"]);
                it=pending.erase(it); continue;
            }
            int size=it->second.at("body").at("txs").size();
            if(count+size>batchSize) break;
            if(shard==coordinator() && members.twoLayer()) {
                bool conflict=false;
                for(const auto& tx:it->second.at("body").at("txs")) {
                    auto id=tx.at("id").get<std::string>(),digest=hash(tx.dump());
                    if((state["seen"].contains(id) && state["seen"].at(id).at("tx_digest")!=digest) ||
                       (selectedIds.count(id) && selectedIds.at(id)!=digest)) conflict=true;
                }
                if(conflict) {replyConflict(it->second);it=pending.erase(it);continue;}
                for(const auto& tx:it->second.at("body").at("txs"))
                    selectedIds[tx.at("id").get<std::string>()]=hash(tx.dump());
            }
            reqs.push_back(it->second); count+=size; ++it;
        }
        json value={{"requests",reqs}};
        if(!reqs.empty() && shard==coordinator() && members.twoLayer())
            value["cst_order_index"]=state.at("cst_order_index").get<int>()+1;
        if(reqs.empty() && !pendingCst.empty()) {
            int expected=state.at("last_cst_seq").get<int>()+1;
            for(const auto& [key,message]:pendingCst) {
                (void)key;
                const auto& cert=message.at("body").at("certificate");
                if(cert.at("proposal").at("body").at("value").at("cst_order_index")==expected) {
                    value["cst_orders"]=json::array({cert});break;
                }
            }
        }
        if(reqs.empty() && !value.contains("cst_orders")) return;
        broadcast(make("PREPREPARE",{{"seq",applied+1},{"digest",hash(value.dump())},{"value",value}}));
        batchStart=Clock::now();
    }
    void handle(const json& e) {
        const auto& b=e.at("body"); std::string type=b.at("type");
        if(type=="CLIENT") {
            if(!validRequest(e)) {rejected++;return;}
            std::string id=b.at("id");
            if(shard==coordinator() && members.twoLayer()) {
                for(const auto& tx:b.at("txs")) {
                    auto tid=tx.at("id").get<std::string>();
                    if(state["seen"].contains(tid) && state["seen"].at(tid).at("tx_digest")!=hash(tx.dump())) {
                        replyConflict(e);rejected++;return;
                    }
                }
            }
            if(state["requests"].contains(id)) {
                if(state["requests"][id]["txs_hash"]==hash(b.at("txs").dump())) {
                    if(completedCstResults.count(id)) reply(e,completedCstResults.at(id));
                    else if(members.leaves.count(shard) || !members.twoLayer()) reply(e,state["requests"][id]["results"]);
                }
                else rejected++;
                duplicates++; return;
            }
            if(pending.size()>=10000) {rejected++;return;}
            if(pending.empty()) {lastProgress=Clock::now();batchStart=Clock::now();}
            auto [it,inserted]=pending.emplace(id,e);
            if(!inserted && it->second.at("body").at("txs")!=b.at("txs")) {rejected++;return;}
            if(inserted && me!=view%4) sendTo(shard,view%4,e);
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
        if(type=="CST_ORDER") {
            if(!members.leaves.count(shard) || !members.twoLayer() || b.at("target")!=shard ||
               source!=members.parent.at(shard) || !validCoordinatorCertificate(b.at("certificate"))) {rejected++;return;}
            const auto& cert=b.at("certificate");std::string key=cstKey(cert);
            std::string digest=cert.at("proposal").at("body").at("digest");
            if(state["cst_batches"].contains(key)) {
                if(state["cst_batches"][key]!=digest) rejected++;
                else duplicates++;
                return;
            }
            if(pendingCst.size()>=10000) {rejected++;return;}
            if(pending.empty() && pendingCst.empty()) {lastProgress=Clock::now();batchStart=Clock::now();}
            auto [it,inserted]=pendingCst.emplace(key,e);
            if(!inserted && it->second.at("body").at("certificate").at("proposal").at("body").at("digest")!=digest)
                rejected++;
            else if(!inserted) duplicates++;
            else if(me!=view%4) sendTo(shard,view%4,e);
            return;
        }
        if(type=="CST_PREPARED") {
            if(!members.twoLayer() || !members.leaves.count(shard) || !members.leaves.count(source) ||
               !validPreparedVote(e)) {rejected++;return;}
            const auto& record=e.at("record"); auto key=record.at("batch_key").get<std::string>();
            if(state["cst_finalized"].contains(key)) return;
            auto group=key+"|"+std::to_string(source)+"|"+b.at("record_digest").get<std::string>();
            if(preparedVotes.size()>=10000 && !preparedVotes.count(group)) {rejected++;return;}
            if(!preparedVotes[group].emplace(sender,e).second) duplicates++;
            maybeReady(key); return;
        }
        if(type=="CST_READY") {
            if(shard!=coordinator() || !validReady(e)) {rejected++;return;}
            auto key=b.at("batch_key").get<std::string>();
            if(state["cst_decisions"].contains(key)) return;
            if(readyVotes.size()>=10000 && !readyVotes.count(key)) {rejected++;return;}
            readyVotes[key].emplace(source,e);
            if(me!=view%4) sendTo(shard,view%4,e);
            return;
        }
        if(type=="CST_DECISION") {
            if(!members.twoLayer() || !members.leaves.count(shard) || source!=coordinator() ||
               b.at("target")!=shard || !validDecisionCertificate(b.at("certificate"))) {rejected++;return;}
            const auto& decision=b.at("certificate").at("proposal").at("body").at("value").at("cst_decisions")[0];
            auto key=decision.at("batch_key").get<std::string>();
            if(state["cst_finalized"].contains(key)) return;
            auto [it,inserted]=pendingDecisions.emplace(key,e);
            if(!inserted && it->second.at("body").at("certificate").at("proposal").at("body").at("digest")!=
                            b.at("certificate").at("proposal").at("body").at("digest")) rejected++;
            else if(!inserted) duplicates++;
            else {batchStart=Clock::now();lastProgress=Clock::now();}
            if(inserted && me!=view%4) sendTo(shard,view%4,e);
            return;
        }
        if(type=="CST_ACK") {
            if(shard!=coordinator() || !members.twoLayer() || !members.leaves.count(source) ||
               b.at("target")!=shard || !b.at("batch_key").is_string() ||
               !b.at("decision_digest").is_string() || !b.at("result_digest").is_string()) {rejected++;return;}
            auto key=b.at("batch_key").get<std::string>();
            if(state["cst_decisions"].contains(key) &&
               b.at("decision_digest")!=hash(state["cst_decisions"].at(key).dump())) {rejected++;return;}
            if(completedCstBatches.count(key)) {sendDone(key);return;}
            auto group=key+"|"+std::to_string(source)+"|"+b.at("result_digest").get<std::string>();
            if(ackVotes.size()>=10000 && !ackVotes.count(group)) {rejected++;return;}
            if(!ackVotes[group].emplace(sender,e).second) duplicates++;
            maybeCompleteBatch(key); return;
        }
        if(type=="CST_DONE") {
            if(!members.leaves.count(shard) || !members.twoLayer() || source!=coordinator() ||
               b.at("target")!=shard || !b.at("batch_key").is_string()) {rejected++;return;}
            auto key=b.at("batch_key").get<std::string>();
            if(state["cst_finalized"].contains(key) &&
               b.at("decision_digest")==state["cst_finalized"].at(key).at("decision_digest")) {
                doneVotes[key].insert(sender);
                if(doneVotes[key].size()>=2) ackConfirmed.insert(key);
            }
            return;
        }
        if(source!=shard) {rejected++;return;}
        int v=b.at("view"); if(v<0) return;
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
            checkpointVotes[n].emplace(sender,e);checkStable(n);return;
        }
        if(type=="SYNC_REQUEST") {
            int start=b.at("after");
            // Reply only when we have newer execution or checkpoint proof.
            // Full-state replies to equal peers can otherwise flood PBFT.
            if(start>=applied && b.value("stable_seq",0)>=stableSeq) return;
            json cs=json::array();
            for(const auto& [n,c]:certificates) if(n>start) cs.push_back(c);
            sendTo(shard,sender,make("SYNC",{{"stable",{{"seq",stableSeq},{"state",stableState},{"proof",stableProof}}},{"certificates",cs}}));
            return;
        }
        if(type=="SYNC") {
            if(!validStable(b.at("stable"))) return;
            for(const auto& c:b.at("certificates")) if(!validCertificate(c)) return;
            installStable(b.at("stable"));
            for(const auto& c:b.at("certificates")) {
                int n=c.at("proposal").at("body").at("seq");
                if(n>applied && n<=stableSeq+window) { certificates.emplace(n,c); preparedHistory[n]=c; }
            }
            applyReady();return;
        }
        if(changing || v!=view) return;
        int n=b.at("seq"); if(n<=stableSeq || n>stableSeq+window) return;
        if(type=="PREPREPARE") {acceptProposal(e);return;}
        if(type=="PREPARE" || type=="COMMIT") {
            if(type=="PREPARE" && sender==view%4) return;
            auto& s=slots[n]; auto& votes=type=="PREPARE"?s.prepares:s.commits;
            if(!votes.emplace(sender,e).second) duplicates++;
            advance(n);
        }
    }
    void status(bool ready=true) {
        size_t completed=0;
        for(const auto& [request,results]:completedCstResults) {(void)request;completed+=results.size();}
        json s={{"ready",ready},{"run_id",members.run},{"pid",getpid()},{"shard",shard},{"replica",me},
            {"role",members.leaves.count(shard)?"leaf":"coordinator"},{"view",view},{"primary",view%4},
            {"changing_view",changing},{"target_view",targetView},{"applied_batches",applied},{"stable_seq",stableSeq},
            {"executed_transactions",state["executed"]},{"ordered_cst_transactions",state["ordered_cst"]},
            {"leaf_ordered_cst_transactions",state["leaf_ordered_cst"]},{"last_cst_seq",state["last_cst_seq"]},
            {"cst_order_index",state["cst_order_index"]},
            {"pending_cst_batches",pendingCst.size()},{"staged_cst_batches",state["cst_staged"].size()},
            {"finalized_cst_batches",state["cst_finalized"].size()},
            {"decided_cst_batches",state["cst_decisions"].size()},{"completed_cst_transactions",completed},
            {"kv_entries",state["kv"].size()},{"kv_digest",hash(state["kv"].dump())},
            {"state_digest",hash(state.dump())},{"chain_digest",state["chain"]},{"pending_requests",pending.size()},
            {"rejected_messages",rejected},{"duplicate_messages",duplicates},{"view_changes",viewCount},
            {"execution_ns",executionNs},{"messages_sent",net.sent.load()},{"messages_received",net.received.load()},
            {"network_failures",net.failed.load()},{"bytes_sent",net.bytes_sent.load()},
            {"inbox_dropped",inboxDropped.load()},
            {"bytes_received",net.bytes_received.load()},{"network_queue",net.queued()},{"probes",probes}};
        writeJson(dir+"/status.json",s);
    }
public:
    Replica(const json& cfg,int sid,int rid,const std::string& d):members(cfg),shard(sid),me(rid),dir(d) {
        if(!members.endpoints.count(identity(sid,rid))) throw std::runtime_error("unknown replica");
        for(const auto& n:cfg.at("nodes")) if(n.at("shard")==sid && n.at("replica")==rid) privateKey=readKey(n.at("private_key"),true);
        timeoutMs=cfg.at("consensus").at("view_timeout_ms"); batchSize=cfg.at("consensus").at("batch_size");
        checkpointEvery=cfg.at("consensus").at("checkpoint_batches"); stableState=state;
        expectedFib=fibonacci();
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
            bool staged=members.leaves.count(shard) && !state["cst_staged"].empty();
            bool waiting=staged?!pendingDecisions.empty():(!pending.empty() || !pendingCst.empty());
            for(const auto& [n,s]:slots) if(n>applied && !s.proposal.is_null()) waiting=true;
            if((waiting || changing) && std::chrono::duration_cast<std::chrono::milliseconds>(now-lastProgress).count()>timeoutMs*(changing?backoff:1)) {
                if(!changing || viewChanges[targetView].size()>=3) startViewChange(std::max(view,targetView)+1);
            }
            if(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastRetry).count()>250) {
                if(changing && !myViewChange.is_null()) broadcast(myViewChange);
                if(!lastNewView.is_null() && lastNewView.at("body").at("view")==view && me==view%4) broadcast(lastNewView);
                for(const auto& [n,s]:slots) if(n>stableSeq && !s.proposal.is_null() && n>applied-2) {
                    if(!changing && s.proposal.at("body").at("view")==view) {
                        if(me==view%4) broadcast(s.proposal);
                        if(s.prepares.count(me)) broadcast(s.prepares.at(me));
                        if(s.commits.count(me)) broadcast(s.commits.at(me));
                    }
                }
                if(!changing && !pending.empty() && me!=view%4) sendTo(shard,view%4,pending.begin()->second);
                if(!changing && !pendingCst.empty() && me!=view%4) sendTo(shard,view%4,pendingCst.begin()->second);
                if(!changing && !pendingDecisions.empty() && me!=view%4) sendTo(shard,view%4,pendingDecisions.begin()->second);
                if(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastCstRetry).count()>2000) {
                    if(members.leaves.count(shard)) {
                        for(auto it=state["cst_staged"].begin();it!=state["cst_staged"].end();++it)
                            if(!readySent.count(it.key())) sendPrepared(it.key());
                        for(const auto& [key,ready]:readySent) {
                            (void)key;
                            sendTo(coordinator(),me,ready);
                        }
                        for(auto it=state["cst_finalized"].begin();it!=state["cst_finalized"].end();++it)
                            if(!ackConfirmed.count(it.key())) sendAck(it.key());
                    } else {
                        for(const auto& [key,cert]:decisionCerts) if(!completedCstBatches.count(key)) forwardDecision(cert);
                    }
                    lastCstRetry=now;
                }
                if(applied>stableSeq && snapshots.count(applied)) broadcast(make("CHECKPOINT",{{"seq",applied},{"digest",hash(snapshots[applied].dump())}}));
                broadcast(make("SYNC_REQUEST",{{"after",applied},{"stable_seq",stableSeq}}));
                lastRetry=now;
            }
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
    struct Request {json envelope;int shard;Clock::time_point start,last;std::map<std::string,std::set<int>> votes;bool done=false;};
    std::map<std::string,Request> requests;
    json timings=json::array();std::set<std::string> confirmedIds;
    size_t next=0;auto begin=Clock::now();auto lastSend=begin;
    double rate=workload.at("rate");double nextDue=0;uint64_t executed=0,ordered=0,duplicates=0,errors=0;
    const auto& jobs=workload.at("requests");
    double timeout=workload.value("timeout_s",30.0);
    while(!stopping && std::chrono::duration<double>(Clock::now()-begin).count()<timeout) {
        auto now=Clock::now();double elapsed=std::chrono::duration<double>(now-begin).count();
        while(next<jobs.size() && elapsed>=nextDue) {
            auto body=jobs[next++];int shard=body.at("target");std::string id=body.at("id");
            body["type"]="CLIENT";body["run"]=members.run;body["reply"]={{"host",workload.value("host",std::string("127.0.0.1"))},{"port",net.localPort()}};
            auto env=sign(body,key);requests.emplace(id,Request{env,shard,now,now,{},false});
            for(int r=0;r<4;++r) net.send(members.endpoints.at(identity(shard,r)),env,0);
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
            for(const auto& tx:result) {
                if(!confirmedIds.insert(tx.at("id").get<std::string>()).second) continue;
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
                if(std::chrono::duration_cast<std::chrono::milliseconds>(now-req.last).count()>500) {
                    for(int r=0;r<4;++r) net.send(members.endpoints.at(identity(req.shard,r)),req.envelope,0);
                    req.last=now;
                }
            }
        }
        if(all) break;
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
