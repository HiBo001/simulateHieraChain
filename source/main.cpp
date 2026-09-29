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
                {"seen",json::object()},{"requests",json::object()},{"executed",0},{"ordered_cst",0}};
    std::map<std::string,json> pending;
    std::map<std::string,Clock::time_point> pingStarts;
    std::map<std::string,std::string> pingPeers;
    json probes=json::object();
    std::ofstream journal, events;
    Clock::time_point lastProgress=Clock::now(),lastRetry=Clock::now(),lastStatus=Clock::now(),batchStart=Clock::now();
    uint64_t rejected=0, duplicates=0, viewCount=0, executionNs=0;
    std::atomic<uint64_t> inboxDropped{0};
    int timeoutMs, batchSize, checkpointEvery;
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
    bool validVote(const json& e,const std::string& type,int v,int seq,const std::string& digest) const {
        try {
            const auto& b=e.at("body");
            return members.replicaMessage(e) && b.at("shard")==shard && b.at("type")==type &&
                b.at("view")==v && b.at("seq")==seq && b.at("digest")==digest;
        } catch (...) { return false; }
    }
    bool validRequest(const json& e) const {
        try {
            if (!members.clientMessage(e)) return false;
            const auto& b=e.at("body");
            if(b.at("type")!="CLIENT" || b.at("target")!=shard || !b.at("id").is_string() ||
               b.at("id").get<std::string>().size()>200 || !b.at("txs").is_array() || b.at("txs").empty() || int(b.at("txs").size())>batchSize) return false;
            const auto& reply=b.at("reply");
            in_addr address{};
            if (inet_pton(AF_INET,reply.at("host").get<std::string>().c_str(),&address)!=1 || reply.at("port").get<int>()<=0 || reply.at("port").get<int>()>65535) return false;
            std::set<std::string> ids;
            for(const auto& t:b.at("txs")) {
                auto id=t.at("id").get<std::string>(); auto key=t.at("key").get<std::string>();
                if(id.empty() || id.size()>200 || key.empty() || key.size()>128 || !ids.insert(id).second ||
                   !t.at("value").is_number_unsigned() || !t.at("participants").is_array() || members.lca(t.at("participants"))!=shard) return false;
                std::set<int> ps;
                for (auto p:t.at("participants")) if(!ps.insert(p.get<int>()).second) return false;
                if (members.leaves.count(shard) && ps.size()!=1) return false;
            }
            return true;
        } catch (...) { return false; }
    }
    bool validValue(const json& value) const {
        try {
            int n=0;
            if (!value.at("requests").is_array()) return false;
            for(const auto& e:value.at("requests")) {
                if (!validRequest(e)) return false;
                n+=e.at("body").at("txs").size();
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
                {"seen",json::object()},{"requests",json::object()},{"executed",0},{"ordered_cst",0}}; }
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
                reply(it->second,state["requests"][it->first]["results"]);
                it=pending.erase(it);
            } else ++it;
        }
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
    void applyReady() {
        while(certificates.count(applied+1)) {
            int n=applied+1; const auto cert=certificates.at(n); const auto& value=cert.at("proposal").at("body").at("value");
            auto before=Clock::now();
            for(const auto& req:value.at("requests")) {
                const auto& rb=req.at("body"); std::string rid=rb.at("id");
                auto requestHash=hash(rb.at("txs").dump());
                json results=json::array();
                if(state["requests"].contains(rid) && state["requests"][rid]["txs_hash"]!=requestHash) {
                    for(const auto& tx:rb.at("txs")) results.push_back({{"id",tx.at("id")},{"error","request_id_conflict"}});
                    reply(req,results); pending.erase(rid); continue;
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
                pending.erase(rid); reply(req,results);
            }
            state["chain"]=hash(state["chain"].get<std::string>()+std::to_string(n)+value.dump());
            applied=n; state["seq"]=n;
            executionNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-before).count();
            journal<<json{{"seq",n},{"value_digest",hash(value.dump())},{"state_digest",hash(state.dump())},{"certificate",cert}}.dump()<<'\n'; journal.flush();
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
        if(changing || me!=view%4 || pending.empty() || applied+1>stableSeq+window) return;
        // One fresh batch in flight. Recovery slots may coexist after a view change.
        if(slots.count(applied+1) && !slots.at(applied+1).proposal.is_null()) return;
        if(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-batchStart).count()<members.config.at("consensus").at("batch_wait_ms").get<int>()) return;
        json reqs=json::array(); int count=0;
        for(auto it=pending.begin();it!=pending.end();) {
            if(state["requests"].contains(it->first)) { reply(it->second,state["requests"][it->first]["results"]); it=pending.erase(it); continue; }
            int size=it->second.at("body").at("txs").size();
            if(count+size>batchSize) break;
            reqs.push_back(it->second); count+=size; ++it;
        }
        if(reqs.empty()) return;
        json value={{"requests",reqs}};
        broadcast(make("PREPREPARE",{{"seq",applied+1},{"digest",hash(value.dump())},{"value",value}}));
        batchStart=Clock::now();
    }
    void handle(const json& e) {
        const auto& b=e.at("body"); std::string type=b.at("type");
        if(type=="CLIENT") {
            if(!validRequest(e)) {rejected++;return;}
            std::string id=b.at("id");
            if(state["requests"].contains(id)) {
                if(state["requests"][id]["txs_hash"]==hash(b.at("txs").dump())) reply(e,state["requests"][id]["results"]);
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
        json s={{"ready",ready},{"run_id",members.run},{"pid",getpid()},{"shard",shard},{"replica",me},
            {"role",members.leaves.count(shard)?"leaf":"coordinator"},{"view",view},{"primary",view%4},
            {"changing_view",changing},{"target_view",targetView},{"applied_batches",applied},{"stable_seq",stableSeq},
            {"executed_transactions",state["executed"]},{"ordered_cst_transactions",state["ordered_cst"]},
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
            bool waiting=!pending.empty();
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
    double rate=workload.at("rate");double nextDue=0;uint64_t executed=0,ordered=0,errors=0;
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
        {"ordered_only_transactions",ordered},{"errors",errors},{"elapsed_s",elapsed},
        {"completed_tps",executed/elapsed},{"avg_latency_s",avgLatency},
        {"p50_s",percentile(.5)},{"p95_s",percentile(.95)},
        {"p99_s",percentile(.99)},{"timings",timings},{"workload",workload}};
    writeJson(output,summary);
    bool finished=complete==jobs.size() && errors==0;
    std::cout<<"completed="<<executed<<" ordered_only="<<ordered<<" requests="<<complete<<"/"<<jobs.size();
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
