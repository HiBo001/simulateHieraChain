// Exercise the actual Arbor selector, authentication and PBFT without sockets.
#define ARBOR_BATCHING_TESTS 1
#define main arbor_node_program_main
#include "../source/main.cpp"
#undef main

struct ArborBatchingTest {
    static void require(bool okay,const std::string& message) {
        if(!okay) throw std::runtime_error(message);
    }
    static Key replicaKey(const json& cfg,int shard,int replica) {
        for(const auto& node:cfg.at("nodes")) if(node.at("shard")==shard && node.at("replica")==replica)
            return readKey(node.at("private_key"),true);
        throw std::runtime_error("missing test replica key");
    }
    static json message(const json& cfg,int shard,int replica,const std::string& type,json fields,int view=0) {
        fields["type"]=type;fields["run"]=cfg.at("run_id");fields["shard"]=shard;
        fields["from"]=replica;fields["view"]=view;
        return sign(fields,replicaKey(cfg,shard,replica));
    }
    static json request(const json& cfg,const std::string& rid,const json& ps,int amount) {
        json txs=json::array();
        for(int index=0;index<amount;++index) {
            const std::string tid=rid+":tx:"+std::to_string(index);
            json accesses=json::array();
            for(int sid:ps) accesses.push_back({{"shard",sid},{"key","account:"+std::to_string(sid)+":"+tid},
                                             {"value",uint64_t(index+1)}});
            txs.push_back({{"id",tid},{"key",accesses[0].at("key")},{"value",uint64_t(index+1)},
                           {"participants",ps},{"accesses",accesses}});
        }
        json body={{"type","CLIENT"},{"run",cfg.at("run_id")},{"id",rid},{"target",5},
                   {"reply",{{"host","127.0.0.1"},{"port",30000}}},{"txs",txs}};
        return sign(body,readKey(cfg.at("client_private_key"),true));
    }
    static void queued(Replica& replica,const json& envelope,Clock::time_point arrived) {
        auto rid=envelope.at("body").at("id").get<std::string>();
        require(replica.validRequest(envelope),"test envelope must be authentic and structurally valid");
        replica.pending.emplace(rid,envelope);
        for(const auto& tx:envelope.at("body").at("txs"))
            replica.pendingTx.emplace(tx.at("id"),std::make_pair(hash(tx.dump()),rid));
        replica.arborPendingAdded(rid,envelope,arrived);
    }
    static int transactions(const json& requests) {
        int count=0;for(const auto& req:requests) count+=req.at("body").at("txs").size();return count;
    }
    static json group(const json& requests) {return requests[0].at("body").at("txs")[0].at("participants");}
    static json popProposal(Replica& replica) {
        while(!replica.inbox.empty()) {
            auto envelope=replica.inbox.front();replica.inbox.pop();
            if(envelope.at("body").at("type")=="PREPREPARE") return envelope;
            replica.handle(envelope);
        }
        return nullptr;
    }
    static json proposed(Replica& replica) {
        replica.batchStart=Clock::now()-std::chrono::milliseconds(20);
        replica.propose();return popProposal(replica);
    }
    static void voteCommit(Replica& replica,const json& cfg,const json& proposal) {
        require(!proposal.is_null(),"a real PBFT proposal is required");
        const auto& body=proposal.at("body");int seq=body.at("seq"),view=body.at("view");
        for(int voter:{1,2}) replica.handle(message(cfg,replica.shard,voter,"PREPARE",{{"seq",seq},{"digest",body.at("digest")}},view));
        while(!replica.inbox.empty()) {auto next=replica.inbox.front();replica.inbox.pop();replica.handle(next);}
        for(int voter:{1,2}) replica.handle(message(cfg,replica.shard,voter,"COMMIT",{{"seq",seq},{"digest",body.at("digest")}},view));
    }
    static void commit(Replica& replica,const json& cfg,const json& proposal) {
        replica.handle(proposal);voteCommit(replica,cfg,proposal);
        int seq=proposal.at("body").at("seq");
        require(replica.applied==seq,"signed PBFT quorum must apply the selected batch");
    }
    static json orderValue(Replica& replica,const json& requests,int order,int round) {
        json watermarks=json::object();auto ps=replica.participants(requests);
        for(int leaf:replica.members.descendants(replica.shard)) watermarks[std::to_string(leaf)]=ps.count(leaf)?order:0;
        json value={{"requests",requests},{"cst_round",round},{"cst_watermarks",watermarks}};
        if(!requests.empty()) value["cst_order_index"]=order;
        return value;
    }
    static json proposal(const json& cfg,int shard,int seq,const json& value) {
        return message(cfg,shard,0,"PREPREPARE",{{"seq",seq},{"digest",hash(value.dump())},{"value",value}});
    }
    static json certificate(const json& cfg,const json& pp) {
        const auto& body=pp.at("body");int shard=body.at("shard"),seq=body.at("seq"),view=body.at("view");
        json prepares=json::array(),commits=json::array();
        for(int voter:{1,2}) prepares.push_back(message(cfg,shard,voter,"PREPARE",{{"seq",seq},{"digest",body.at("digest")}},view));
        for(int voter:{0,1,2}) commits.push_back(message(cfg,shard,voter,"COMMIT",{{"seq",seq},{"digest",body.at("digest")}},view));
        return {{"proposal",pp},{"prepares",prepares},{"commits",commits}};
    }
    static json initialWitness(Replica& replica,const json& cfg,const json& order) {
        auto key=Replica::cstKey(order);json proofs=json::array();
        for(int owner:replica.orderParticipants(order)) {
            json record={{"shard",owner},{"batch_key",key},{"order_digest",order.at("proposal").at("body").at("digest")},
                {"order_certificate",order},{"reads",json::object()},{"writes",json::array()}};
            for(const auto& req:order.at("proposal").at("body").at("value").at("requests"))
                for(const auto& tx:req.at("body").at("txs")) if(replica.touches(tx,owner)) {
                    auto access=replica.accessFor(tx,owner);
                    record["reads"][access.at("key").get<std::string>()]=replica.initialAccount();
                    record["writes"].push_back({{"id",tx.at("id")},{"tx_digest",hash(tx.dump())},{"key",access.at("key")},
                        {"value",access.at("value")},{"fib",replica.expectedFib},{"duplicate",false}});
                }
            json votes=json::array();
            for(int voter:{0,1,2}) votes.push_back(message(cfg,owner,voter,"CST_PREPARED",
                {{"batch_key",key},{"record_digest",hash(replica.recordPayload(record).dump())}}));
            proofs.push_back({{"record",record},{"votes",votes}});
        }
        return {{"batch_key",key},{"proofs",proofs}};
    }
    static json run(const json& cfg,const std::string& directory) {
        json checks=json::array();int sequence=0;
        auto check=[&](bool okay,const std::string& name) {require(okay,name);checks.push_back(name);};
        auto makeReplica=[&]() {
            const auto path=directory+"/case-"+std::to_string(++sequence);
            std::filesystem::create_directories(path);
            return std::make_unique<Replica>(cfg,5,0,path);
        };
        auto makeBackup=[&](int shard=5) {
            const auto path=directory+"/case-"+std::to_string(++sequence);
            std::filesystem::create_directories(path);
            return std::make_unique<Replica>(cfg,shard,3,path);
        };
        const json a=json::array({1,2}),b=json::array({1,8}),c=json::array({2,8});
        auto now=Clock::now();
        auto expired=now-std::chrono::milliseconds(cfg.at("consensus").at("cross_shard_batch_wait_ms").get<int>()+10);
        {
            auto r=makeReplica();now=Clock::now();
            for(int index=0;index<20;++index) queued(*r,request(cfg,"interleaved:"+std::to_string(100+index),index%2?a:b,10),now);
            auto selected=r->selectArborClientBatch(now);
            check(!selected.is_null() && transactions(selected)==100,"interleaved participant groups fill one 100-transaction batch");
            for(const auto& env:selected) check(group(json::array({env}))==group(selected) &&
                env==r->pending.at(env.at("body").at("id")),"selected requests retain their original signatures and one participant group");
            auto rebuilt=r->arborBatchSelectionRebuilds,digests=r->arborPendingDigestComputations;
            for(int index=0;index<20;++index) require(r->selectArborClientBatch(now)==selected,"cached selector changed without an event");
            check(r->arborBatchSelectionRebuilds==rebuilt && r->arborPendingDigestComputations==digests,
                  "idle selector polls do not rebuild the batch or rehash transactions");
        }
        {
            auto r=makeReplica();now=Clock::now();queued(*r,request(cfg,"short:A",a,10),now);queued(*r,request(cfg,"short:B",b,10),now);
            check(r->selectArborClientBatch(now).is_null(),"different participant groups do not prematurely flush a short batch");
            check(r->selectArborClientBatch(now+std::chrono::milliseconds(1000)).is_array(),
                  "a cached empty selection becomes ready when its group deadline expires");
            for(int poll=0;poll<20;++poll) {
                r->maybeRequestArborRound(Clock::now());
                require(r->desiredRound==0,"an unexpired group self-requested a round");
                require(proposed(*r).is_null(),"an unexpired group emitted an unsolicited empty close");
            }
            check(r->desiredRound==0,"repeated polling of unexpired groups does not create empty watermark rounds");
            auto pp=proposed(*r);
            check(pp.is_null(),"unexpired short groups do not emit a business proposal or unsolicited empty close");
            r->handle(message(cfg,7,0,"CST_ROUND_REQUEST",{{"target",5},{"round",1}}));
            pp=proposed(*r);
            check(!pp.is_null() && pp.at("body").at("value").at("requests").empty()
                  && pp.at("body").at("value").contains("cst_watermarks"),
                  "an authenticated external round request may close a round while business groups wait");
        }
        {
            auto r=makeReplica();now=Clock::now();queued(*r,request(cfg,"first:A",a,10),now);
            for(int index=0;index<10;++index) queued(*r,request(cfg,"later:B:"+std::to_string(index),b,10),now);
            auto selected=r->selectArborClientBatch(now);
            check(!selected.is_null() && group(selected)==b && transactions(selected)==100,
                  "an unexpired short group does not block another full participant group");
            for(const auto& env:selected) r->erasePending(env.at("body").at("id"));
            selected=r->selectArborClientBatch(now+std::chrono::milliseconds(1000));
            check(!selected.is_null() && group(selected)==a && transactions(selected)==10,
                  "the remaining short group is eventually released after its deadline");
        }
        {
            auto r=makeReplica();now=Clock::now();auto first=request(cfg,"size:00",a,60),second=request(cfg,"size:01",a,50),third=request(cfg,"size:02",a,40);
            queued(*r,first,now);queued(*r,second,now);queued(*r,third,now);
            auto selected=r->selectArborClientBatch(now);
            check(!selected.is_null() && transactions(selected)==100 && selected.size()==2,
                  "whole signed requests may skip a non-fitting request to fill the transaction cap");
            check(selected[0]==first && selected[1]==third,"non-fitting signed requests are neither split nor altered");
            auto pp=proposed(*r);
            check(!pp.is_null() && r->validValue(pp.at("body").at("value")),"an interleaved selection forms a valid certified-order value");
            commit(*r,cfg,pp);
            check(r->pending.size()==1 && r->pending.count(second.at("body").at("id")),
                  "applying a batch retains the whole unselected request");
        }
        {
            auto r=makeReplica();now=Clock::now();auto first=request(cfg,"fit:00",a,70),second=request(cfg,"fit:01",a,40);
            queued(*r,first,now);queued(*r,second,now);
            auto selected=r->selectArborClientBatch(now);
            check(!selected.is_null() && transactions(selected)==70 && selected.size()==1,
                  "an eligible whole request that cannot fit promptly releases its group without exceeding the cap");
            check(selected[0]==first,"a capacity-limited proposal retains the complete signed 70-transaction request");
        }
        {
            auto r=makeReplica();now=Clock::now();r->crossShardBatchSize=64;r->crossShardBatchWaitMs=2500;
            for(int index=0;index<7;++index) queued(*r,request(cfg,"cap64:"+std::to_string(index),a,10),now);
            auto selected=r->selectArborClientBatch(now);
            check(!selected.is_null() && selected.size()==6 && transactions(selected)==60,
                  "seven ten-transaction requests promptly fill an indivisible 64-transaction batch to sixty");
            for(const auto& env:selected) require(env==r->pending.at(env.at("body").at("id")),
                                                  "a 64-cap batch altered or split its signed input");
        }
        for(bool conflicting:{false,true}) {
            auto r=makeReplica();now=Clock::now();
            auto original=request(cfg,"blocked:original",a,70),other=request(cfg,"blocked:other",a,40);
            auto body=other.at("body");body["txs"][0]=original.at("body").at("txs")[0];
            if(conflicting) body["txs"][0]["value"]=uint64_t(999);
            other=sign(body,readKey(cfg.at("client_private_key"),true));
            queued(*r,original,now);queued(*r,other,now);
            check(r->selectArborClientBatch(now).is_null(),conflicting?
                  "a conflicting transaction does not falsely mark its group capacity-blocked":
                  "a transaction-overlapping request does not falsely mark its group capacity-blocked");
        }
        {
            auto r=makeReplica();now=Clock::now();
            auto older=request(cfg,"fifo:zz-older",a,70),later=request(cfg,"fifo:aa-later",a,40);
            queued(*r,older,now-std::chrono::milliseconds(10));queued(*r,later,now);
            auto selected=r->selectArborClientBatch(now+std::chrono::milliseconds(1000));
            check(!selected.is_null() && selected.size()==1 && selected[0]==older,
                  "an older high-ID request precedes a newer low-ID request within the same group");
        }
        {
            auto r=makeReplica();now=Clock::now();
            for(int cycle=0;cycle<2;++cycle) for(const auto& ps:{a,b,c})
                queued(*r,request(cfg,"fair:"+ps.dump()+":"+std::to_string(cycle),ps,50),expired);
            std::set<std::string> chosen;
            for(int cycle=0;cycle<3;++cycle) {
                auto pp=proposed(*r);require(!pp.is_null(),"fairness test failed to propose");
                auto requests=pp.at("body").at("value").at("requests");
                require(transactions(requests)==100,"fairness proposal exceeded or missed its full group");
                chosen.insert(group(requests).dump());commit(*r,cfg,pp);
                // Keep the first group busy while other groups remain eligible.
                if(cycle==0) queued(*r,request(cfg,"fair:refill",group(requests),100),expired);
            }
            check(chosen.size()==3,"real PBFT proposals rotate participant groups despite a continuously replenished first group");
        }
        {
            auto r=makeReplica();now=Clock::now();auto original=request(cfg,"dedup:original",a,10);r->handle(original);r->handle(original);
            check(r->pending.size()==1,"a retried signed request has exactly one pending owner");
            auto conflict=original.at("body");conflict["txs"][0]["value"]=uint64_t(999);
            r->handle(sign(conflict,readKey(cfg.at("client_private_key"),true)));
            check(r->pending.size()==1 && r->pending.at("dedup:original")==original,
                  "conflicting payloads cannot replace an already admitted request");
            auto overlapping=request(cfg,"dedup:overlap",a,10);auto body=overlapping.at("body");
            body["txs"][0]=original.at("body").at("txs")[0];overlapping=sign(body,readKey(cfg.at("client_private_key"),true));
            r->handle(overlapping);
            check(r->pending.size()==1 && r->deferredRequests.size()==1,"transaction overlap waits instead of entering another consensus batch");
        }
        {
            auto r=makeReplica();now=Clock::now();auto old=request(cfg,"restore:old",a,100),next=request(cfg,"restore:next",b,100);
            queued(*r,old,expired);queued(*r,next,expired);
            check(!r->selectArborClientBatch(now).is_null(),"a ready batch may be cached before state restoration");
            auto state=r->state;state["seq"]=1;
            state["requests"]["restore:old"]={{"txs_hash",hash(old.at("body").at("txs").dump())},{"results",json::array()}};
            for(const auto& tx:old.at("body").at("txs")) state["seen"][tx.at("id").get<std::string>()]=
                {{"tx_digest",hash(tx.dump())},{"result",{{"id",tx.at("id")},{"kind","ordered_only"}}}};
            r->installStable({{"seq",1},{"state",state},{"proof",json::array()}});
            auto selected=r->selectArborClientBatch(Clock::now()+std::chrono::milliseconds(1000));
            check(!selected.is_null() && group(selected)==b,"snapshot restoration invalidates stale selections and excludes already ordered requests");
            r->changing=true;check(proposed(*r).is_null(),"view change blocks fresh batch proposals");
            r->view=4;r->targetView=4;r->changing=false;r->rebuildPendingIndex();
            auto pp=proposed(*r);
            check(!pp.is_null() && pp.at("body").at("view")==4 && group(pp.at("body").at("value").at("requests"))==b,
                  "the recovered primary emits intact eligible requests in the new view");
        }
        {
            auto r=makeBackup();
            auto first=proposal(cfg,5,1,orderValue(*r,json::array({request(cfg,"early:first",a,1)}),1,1));
            auto second=proposal(cfg,5,2,orderValue(*r,json::array({request(cfg,"early:second",a,1)}),2,2));
            require(r->validSignedProposal(second) && !r->validValue(second.at("body").at("value")),
                    "the future fixture must require an unapplied order and watermark prefix");
            r->handle(second);
            check(r->arborFutureProposals.count(2)==1 && r->slots.count(2)==0 && r->inbox.empty() && r->applied==0,
                  "an authenticated future proposal is retained without a PBFT slot, PREPARE, or execution");
            commit(*r,cfg,first);
            r->retryArborFutureProposals(Clock::now());
            check(r->arborFutureProposals.empty() && r->arborFutureProposalBytes==0 &&
                  r->slots.at(2).proposal==second && r->applied==1,
                  "applying the signed prefix admits the retained proposal without another PREPREPARE");
            check(!r->inbox.empty() && r->inbox.front().at("body").at("type")=="PREPARE" &&
                  r->inbox.front().at("body").at("seq")==2,
                  "the retained proposal earns its first PREPARE only after full state validation");
            voteCommit(*r,cfg,second);
            check(r->applied==2 && r->state.at("cst_order_index")==2 && r->state.at("cst_round")==2,
                  "a retained coordinator proposal completes real signed PBFT in prefix order");
        }
        {
            // Match the live failure: the first leaf slot is committed, but
            // its remote execution proofs arrive after the next PREPREPARE.
            auto leaf=makeBackup(1),coordinator=makeReplica(),root=makeBackup(7);
            auto firstOrder=certificate(cfg,proposal(cfg,5,1,
                orderValue(*coordinator,json::array({request(cfg,"frontier:first",a,1)}),1,1)));
            auto secondOrder=certificate(cfg,proposal(cfg,5,2,
                orderValue(*coordinator,json::array({request(cfg,"frontier:second",a,1)}),2,2)));
            auto firstClose=certificate(cfg,proposal(cfg,7,1,orderValue(*root,json::array(),0,1)));
            auto secondClose=certificate(cfg,proposal(cfg,7,2,orderValue(*root,json::array(),0,2)));
            auto first=proposal(cfg,1,1,{{"requests",json::array()},{"cst_orders",json::array({firstOrder})},
                                       {"cst_frontier",json::array({firstOrder,firstClose})}});
            auto second=proposal(cfg,1,2,{{"requests",json::array()},{"cst_orders",json::array({secondOrder})},
                                        {"cst_frontier",json::array({secondOrder,secondClose})}});
            require(leaf->validValue(first.at("body").at("value")),"the first certified NCA frontier must be valid");
            leaf->handle(first);voteCommit(*leaf,cfg,first);
            check(leaf->certificates.count(1) && leaf->applied==0 && leaf->stagedRecords.count("5:1"),
                  "a committed leaf slot waits for authenticated execution dependencies before advancing");
            require(!leaf->validValue(second.at("body").at("value")),"the second NCA frontier must depend on prior execution");
            leaf->handle(second);leaf->retryArborFutureProposals(Clock::now());
            check(leaf->arborFutureProposals.count(2) && !leaf->slots.count(2) && leaf->applied==0,
                  "a leaf retains an early proposal while its previous committed slot waits for remote reads");
            require(leaf->importWitness(initialWitness(*leaf,cfg,firstOrder)),"real signed first execution proofs must validate");
            leaf->applyReady();leaf->retryArborFutureProposals(Clock::now());
            check(leaf->applied==1 && leaf->slots.at(2).proposal==second && leaf->arborFutureProposals.empty(),
                  "verified remote reads unblock the retained NCA frontier without proposal retransmission or SYNC");
            voteCommit(*leaf,cfg,second);
            check(leaf->applied==1 && leaf->certificates.count(2),
                  "accepting a retained leaf proposal does not bypass its own execution dependencies");
            require(leaf->importWitness(initialWitness(*leaf,cfg,secondOrder)),"real signed second execution proofs must validate");
            leaf->applyReady();
            check(leaf->applied==2 && leaf->state.at("executed")==2 && leaf->state.at("cst_indices").at("5")==2 &&
                  leaf->state.at("cst_batches").size()==2 && leaf->arborFutureProposalBytes==0,
                  "both delayed leaf batches execute exactly once and preserve the certified frontier");
        }
        {
            auto r=makeBackup();
            auto pp=proposal(cfg,5,1,orderValue(*r,json::array({request(cfg,"retry:invalid-prefix",a,1)}),2,2));
            r->handle(pp);auto attempted=r->arborFutureProposals.at(1).lastAttemptTime;
            auto tries=r->arborFutureProposalRetries;
            for(int poll=0;poll<20;++poll) r->retryArborFutureProposals(attempted+std::chrono::milliseconds(50));
            check(r->arborFutureProposalRetries==tries && r->slots.empty() && r->inbox.empty(),
                  "repeated polls do not revalidate or vote for an unchanged invalid head within one hundred milliseconds");
            r->retryArborFutureProposals(attempted+std::chrono::milliseconds(101));
            require(r->arborFutureProposalRetries==tries+1,"an unchanged head should be retried after its pacing deadline");
            auto last=r->arborFutureProposals.at(1).lastAttemptTime;
            r->handle(pp);r->retryArborFutureProposals(last+std::chrono::milliseconds(50));
            check(r->arborFutureProposals.at(1).lastAttemptTime==last && r->arborFutureProposalRetries==tries+1,
                  "a repeated identical PREPREPARE does not reset the bounded retry timer");
            r->startViewChange(4);r->retryArborFutureProposals(last+std::chrono::milliseconds(500));
            check(r->changing && r->targetView==4 && r->view==0 && r->slots.empty(),
                  "cached proposals cannot cancel an issued view change or grant a vote during recovery");
            json changes=json::array();
            for(int voter:{0,1,2}) changes.push_back(message(cfg,5,voter,"VIEW_CHANGE",
                {{"stable",{{"seq",0},{"state",r->stableState},{"proof",json::array()}}},{"prepared",json::array()}},4));
            r->handle(message(cfg,5,0,"NEW_VIEW",{{"changes",changes},{"proposals",json::array()}},4));
            check(r->view==4 && !r->changing && r->arborFutureProposals.empty() && r->arborFutureProposalBytes==0,
                  "a certified new view clears cached proposals from the previous primary");
        }
        {
            auto r=makeBackup();
            auto first=proposal(cfg,5,2,orderValue(*r,json::array({request(cfg,"immutable:first",a,1)}),2,2));
            auto conflict=proposal(cfg,5,2,orderValue(*r,json::array({request(cfg,"immutable:conflict",a,1)}),2,2));
            r->handle(first);auto bytes=r->arborFutureProposalBytes;
            r->handle(conflict);
            check(r->arborFutureProposals.at(2).envelope==first && r->arborFutureProposalBytes==bytes && r->slots.empty(),
                  "a conflicting signed proposal cannot replace the first cached digest for a sequence");
            auto forged=proposal(cfg,5,3,orderValue(*r,json::array({request(cfg,"forged",a,1)}),3,3));
            forged["body"]["value"]["cst_round"]=4;r->handle(forged);
            auto wrongPrimary=message(cfg,5,1,"PREPREPARE",conflict.at("body"));wrongPrimary["body"]["seq"]=3;
            // Sign again after changing the sequence: rejection must be due
            // to leader identity, independently of signature authentication.
            wrongPrimary=sign(wrongPrimary.at("body"),replicaKey(cfg,5,1));r->handle(wrongPrimary);
            auto outside=proposal(cfg,5,r->window+1,orderValue(*r,json::array({request(cfg,"outside-window",a,1)}),65,65));
            r->handle(outside);
            check(r->arborFutureProposals.size()==1 && r->arborFutureProposalBytes==bytes && r->slots.empty(),
                  "forged signatures, a non-primary signer, and out-of-window sequences never enter the proposal cache");
            auto state=r->state;state["seq"]=2;
            r->installStable({{"seq",2},{"state",state},{"proof",json::array()}});r->retryArborFutureProposals(Clock::now());
            check(r->arborFutureProposals.empty() && r->arborFutureProposalBytes==0,
                  "installing an applied checkpoint releases obsolete cached proposals and their byte accounting");
        }
        {
            auto r=makeBackup();
            for(int seq=1;seq<=r->window+1;++seq) {
                // seq1 has an intentionally unmet order prefix, so even the
                // head remains only cached while the legal window is filled.
                int order=seq==1?2:seq;
                r->handle(proposal(cfg,5,seq,orderValue(*r,json::array({request(cfg,"window:"+std::to_string(seq),a,1)}),order,order)));
            }
            check(r->arborFutureProposals.size()==size_t(r->window) && !r->arborFutureProposals.count(r->window+1) && r->slots.empty(),
                  "authenticated unvalidated proposals remain bounded by the existing PBFT sequence window");
            size_t counted=0;for(const auto& [seq,cached]:r->arborFutureProposals) {(void)seq;counted+=cached.envelope.dump().size();}
            check(counted==r->arborFutureProposalBytes && counted<=r->ARBOR_FUTURE_PROPOSAL_BYTE_LIMIT,
                  "cached envelope byte accounting matches all retained messages");
        }
        {
            auto r=makeBackup();
            auto large=[&](int seq) {
                auto value=orderValue(*r,json::array({request(cfg,"bytes:"+std::to_string(seq),a,1)}),seq,seq);
                return message(cfg,5,0,"PREPREPARE",{{"seq",seq},{"digest",hash(value.dump())},{"value",value},
                    {"padding",std::string(r->ARBOR_FUTURE_PROPOSAL_BYTE_LIMIT/2+4096,'x')}});
            };
            auto first=large(2);require(first.dump().size()<r->ARBOR_FUTURE_PROPOSAL_BYTE_LIMIT,"each cache-limit fixture fits one network frame");
            r->handle(first);auto bytes=r->arborFutureProposalBytes;
            auto second=large(3);require(r->validSignedProposal(second),"the second large fixture must carry a real primary signature");r->handle(second);
            check(r->arborFutureProposals.size()==1 && r->arborFutureProposals.at(2).envelope==first &&
                  r->arborFutureProposalBytes==bytes && bytes<=r->ARBOR_FUTURE_PROPOSAL_BYTE_LIMIT && r->slots.empty(),
                  "signed messages that exceed the total byte budget are rejected without evicting the retained digest");
        }
        return {{"checks",checks},{"network_started",false}};
    }
};

int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: test_arbor_batching CONFIG OUTPUT_DIRECTORY");
        std::cout<<ArborBatchingTest::run(readJson(argv[1]),argv[2]).dump()<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
