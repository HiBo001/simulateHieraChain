// Exercise Byzantine recovery evidence without starting any network process.
#define ARBOR_SHARPER 1
#define ARBOR_SHARPER_TESTS 1
#define main sharper_node_program_main
#include "../source/main.cpp"
#undef main

struct SharPerProtocolTest {
    struct Batch {
        json proposal,claim1,claim2,value,assignment,accepts,seqs,claims;
        json record1,record2,commits,prepared,certificate;
    };
    static void require(bool condition,const std::string& message) {
        if(!condition) throw std::runtime_error(message);
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
    static json accept(const json& cfg,const Batch& batch,int owner,int replica,int view=0) {
        const auto& p=batch.proposal.at("body");const auto& claim=owner==1?batch.claim1:batch.claim2;
        return message(cfg,owner,replica,"SH_ACCEPT",{{"batch",p.at("batch")},{"request_digest",p.at("request_digest")},
            {"origin_seq",p.at("origin_seq")},{"local_seq",claim.at("body").at("seq")},
            {"claim",claim},{"claim_digest",hash(claim.dump())}},view);
    }
    static Batch batch(Replica& replica,const json& cfg,const std::string& label,int seq=1,int view=0) {
        Batch out;const std::string id="unit:"+label+":tx",key1="account:1:shared",key2="account:2:shared";
        json tx={{"id",id},{"key",key1},{"value",uint64_t(11)},{"participants",json::array({1,2})},
                 {"accesses",json::array({json{{"shard",1},{"key",key1},{"value",uint64_t(11)}},
                                          json{{"shard",2},{"key",key2},{"value",uint64_t(22)}}})}};
        json client={{"type","CLIENT"},{"run",cfg.at("run_id")},{"id","unit:"+label+":request"},
                     {"target",1},{"reply",{{"host","127.0.0.1"},{"port",30000}}},{"txs",json::array({tx})}};
        json requests=json::array({sign(client,readKey(cfg.at("client_private_key"),true))});
        out.proposal=message(cfg,1,view%4,"SH_SUPER_PROPOSE",{{"batch",hash(label)},{"origin_seq",seq},
            {"participants",json::array({1,2})},{"request_digest",hash(requests.dump())}},view);
        out.proposal["requests"]=requests;
        auto claim=[&](int owner) {
            return message(cfg,owner,view%4,"SH_LOCAL_CLAIM",{{"batch",hash(label)},{"origin_seq",seq},
                {"seq",seq},{"ballot",uint64_t(1)},{"request_digest",hash(requests.dump())},
                {"proposal_digest",hash(replica.shHeader(out.proposal).dump())}},view);
        };
        out.claim1=claim(1);out.claim2=claim(2);
        out.value={{"requests",requests},{"sharper_propose",replica.shHeader(out.proposal)},{"sharper_claim",out.claim1}};
        out.assignment=message(cfg,1,view%4,"PREPREPARE",{{"seq",seq},{"digest",hash(out.value.dump())},{"value",out.value}},view);
        out.accepts=json::array();out.commits=json::array();
        out.seqs={{"1",seq},{"2",seq}};
        out.claims={{"1",hash(out.claim1.dump())},{"2",hash(out.claim2.dump())}};
        auto record=[&](int owner,const std::string& key,uint64_t value) {
            return json{{"shard",owner},{"seq",seq},{"batch",hash(label)},{"request_digest",hash(requests.dump())},
                {"reads",{{key,replica.initialAccount()}}},
                {"writes",json::array({json{{"id",id},{"tx_digest",hash(tx.dump())},{"key",key},
                                                {"value",value},{"duplicate",false}}})}};
        };
        out.record1=record(1,key1,11);out.record2=record(2,key2,22);
        for(int owner:{1,2}) for(int voter=0;voter<3;++voter) {
            out.accepts.push_back(accept(cfg,out,owner,voter,view));
            const auto& ownRecord=owner==1?out.record1:out.record2;
            out.commits.push_back(message(cfg,owner,voter,"SH_COMMIT",{{"batch",hash(label)},
                {"request_digest",hash(requests.dump())},{"seqs",out.seqs},{"claims",out.claims},
                {"record",ownRecord},{"record_digest",hash(ownRecord.dump())}},view));
        }
        out.prepared={{"sharper",true},{"proposal",out.assignment},{"prepares",out.accepts}};
        out.certificate=out.prepared;out.certificate["commits"]=out.commits;
        out.certificate["seqs"]=out.seqs;out.certificate["claims"]=out.claims;
        return out;
    }
    static json evidence(Replica& replica,const Batch& batch,const json& cfg,int voter) {
        // This helper synthesizes the named voter's evidence, rather than
        // copying another replica's cast COMMIT into its VIEW_CHANGE.
        json out={{"reservations",json::array()},{"initiations",json::array()},{"commits",json::array()}};
        if(batch.proposal.at("body").at("shard")==replica.shard) out["initiations"].push_back(batch.proposal);
        out["reservations"]=json::array({json{{"proposal",batch.assignment},{"accept",accept(cfg,batch,1,voter)}}});
        return out;
    }
    static json viewChange(Replica& replica,const json& cfg,int voter,int view,const json& extra,json prepared=json::array(),int owner=1) {
        return message(cfg,owner,voter,"VIEW_CHANGE",{{"stable",{{"seq",0},{"state",replica.genesis()},{"proof",json::array()}}},
            {"prepared",prepared},{"sharper_recovery",extra}},view);
    }
    static json emptyRecoveryEvidence() {
        return {{"reservations",json::array()},{"initiations",json::array()},{"commits",json::array()}};
    }
    static std::pair<json,std::map<int,json>> verifiedRecovery(Replica& replica,const json& vcs,int view,const std::string& context) {
        for(const auto& vc:vcs)
            require(replica.validVC(vc,view),context+": invalid VIEW_CHANGE from replica "+vc.at("body").at("from").dump());
        return replica.recovery(vcs,view);
    }
    static json newView(Replica& replica,const json& cfg,const json& vcs,int view,int owner=1) {
        auto recovered=verifiedRecovery(replica,vcs,view,replica.dir+" NEW_VIEW");json proposals=json::array();
        for(const auto& [seq,value]:recovered.second)
            proposals.push_back(message(cfg,owner,view%4,"PREPREPARE",{{"seq",seq},{"digest",hash(value.dump())},{"value",value}},view));
        return message(cfg,owner,view%4,"NEW_VIEW",{{"changes",vcs},{"proposals",proposals}},view);
    }
    static void installAcceptVector(Replica& replica,const Batch& batch) {
        auto& remembered=replica.shBatches.at(Replica::shKey(batch.proposal));
        for(const auto& vote:batch.accepts)
            remembered.accepts[vote.at("body").at("shard").get<int>()][vote.at("body").at("from").get<int>()]=vote;
    }
    static json clientRequest(const json& cfg,const std::string& label,const std::vector<int>& participants) {
        json accesses=json::array();
        for(int owner:participants) accesses.push_back({{"shard",owner},{"key","account:"+std::to_string(owner)+":shared"},
                                                       {"value",uint64_t(11+owner)}});
        json tx={{"id","unit:"+label+":tx"},{"key",accesses[0].at("key")},{"value",accesses[0].at("value")},
                 {"participants",participants},{"accesses",accesses}};
        json body={{"type","CLIENT"},{"run",cfg.at("run_id")},{"id","unit:"+label+":request"},
                   {"target",participants.front()},{"reply",{{"host","127.0.0.1"},{"port",30000}}},
                   {"txs",json::array({tx})}};
        return sign(body,readKey(cfg.at("client_private_key"),true));
    }
    static json fifoRequest(const json& cfg,const std::string& label,const std::vector<int>& participants,int count=1) {
        auto body=clientRequest(cfg,label,participants).at("body");auto prototype=body.at("txs")[0];
        body["txs"]=json::array();
        for(int i=0;i<count;++i) {
            auto tx=prototype;tx["id"]="unit:"+label+":tx:"+std::to_string(i);body["txs"].push_back(tx);
        }
        return sign(body,readKey(cfg.at("client_private_key"),true));
    }
    static void fifoBatchingChecks(const json& cfg,const std::string& directory,json& checks) {
        auto check=[&](bool condition,const std::string& name) {require(condition,name);checks.push_back(name);};
        auto nodeDirectory=[&](const std::string& name) {
            auto path=directory+"/"+name;std::filesystem::create_directories(path);return path;
        };
        auto batchingCfg=cfg;batchingCfg["consensus"]["batch_wait_ms"]=10;
        batchingCfg["consensus"]["cross_shard_batch_wait_ms"]=600;
        batchingCfg["consensus"]["cross_shard_batch_size"]=8;
        auto admitted=[&](Replica& replica,const json& request) {
            require(replica.members.clientMessage(request) && replica.sharperValidRequest(request,1),
                    "FIFO fixture request must have a real valid client signature and read/write participant set");
            replica.handle(request);
            require(replica.pending.count(request.at("body").at("id").get<std::string>()),
                    "FIFO fixture request must be admitted through the production authenticated CLIENT path");
        };
        auto rid=[](const json& request) {return request.at("body").at("id").get<std::string>();};
        {
            Replica replica(batchingCfg,1,0,nodeDirectory("fifo-separated-groups"));
            auto first=fifoRequest(cfg,"zz-fifo-A-first",{1,2});
            auto middle=fifoRequest(cfg,"aa-fifo-B-middle",{1,3});
            auto last=fifoRequest(cfg,"mm-fifo-A-last",{1,2});
            admitted(replica,first);admitted(replica,middle);admitted(replica,last);
            auto selected=replica.shSelectPendingPrefix();
            check(selected.requests==json::array({first}) && selected.count==1 && selected.prefixClosed,
                  "A B A client arrival sequence never merges its separated A requests");
            check(selected.group==json::array({1,2}) && rid(middle)<rid(first),
                  "a lexicographically earlier later request cannot replace the FIFO head's participant group");
            replica.batchStart=Clock::now()+std::chrono::hours(1);replica.sharperPropose();
            check(replica.shBatches.empty() && replica.shInitiating.empty(),
                  "a closed participant prefix still respects the original minimum batch wait");
            replica.batchStart=Clock::now()-std::chrono::milliseconds(20);replica.sharperPropose();
            check(!replica.shInitiating.empty() && replica.shBatches.at(replica.shInitiating).proposal.at("requests")==json::array({first}),
                  "a closed different-group prefix proposes after the minimum wait without waiting 600 milliseconds");
            check(replica.pending.count(rid(middle)) && replica.pending.count(rid(last)) &&
                  !replica.shSelected.count(rid(middle)) && !replica.shSelected.count(rid(last)),
                  "closed-prefix proposal leaves every later signed request queued in arrival order");
        }
        {
            Replica replica(batchingCfg,1,0,nodeDirectory("fifo-contiguous-groups"));
            auto first=fifoRequest(cfg,"zz-contiguous-A-first",{1,2});
            auto second=fifoRequest(cfg,"mm-contiguous-A-second",{1,2});
            auto different=fifoRequest(cfg,"aa-contiguous-B-third",{1,3});
            admitted(replica,first);admitted(replica,second);admitted(replica,different);
            auto selected=replica.shSelectPendingPrefix();
            check(selected.requests==json::array({first,second}) && selected.count==2 && selected.prefixClosed,
                  "A A B arrival sequence combines exactly the contiguous A prefix");
            for(const auto& request:selected.requests)
                require(replica.members.clientMessage(request),"FIFO selection must preserve the complete original client signature");
            check(selected.group==json::array({1,2}),"contiguous batching preserves the head's participant set and signed envelopes");
        }
        {
            Replica replica(batchingCfg,1,0,nodeDirectory("fifo-capacity-barrier"));
            auto first=fifoRequest(cfg,"capacity-a-first-six",{1,2},6);
            auto blocked=fifoRequest(cfg,"capacity-b-blocked-three",{1,2},3);
            auto small=fifoRequest(cfg,"capacity-c-fitting-two",{1,2},2);
            admitted(replica,first);admitted(replica,blocked);admitted(replica,small);
            auto selected=replica.shSelectPendingPrefix();
            check(selected.requests==json::array({first}) && selected.count==6 && selected.prefixClosed,
                  "an indivisible capacity barrier cannot be skipped to take a later fitting request");
            check(replica.pending.size()==3 && !replica.shSelected.count(rid(blocked)) && !replica.shSelected.count(rid(small)),
                  "capacity batching retains both the blocked request and the later small request");
            replica.batchStart=Clock::now()-std::chrono::milliseconds(20);replica.sharperPropose();
            check(replica.shBatches.at(replica.shInitiating).proposal.at("requests")==json::array({first}),
                  "capacity-closed prefix proposes its six complete transactions without filling from later arrivals");
        }
        {
            Replica replica(batchingCfg,1,0,nodeDirectory("fifo-open-prefix-wait"));
            auto only=fifoRequest(cfg,"open-partial-prefix",{1,2});admitted(replica,only);
            auto selected=replica.shSelectPendingPrefix();
            check(selected.requests==json::array({only}) && selected.count==1 && !selected.prefixClosed,
                  "an open partial FIFO prefix is distinguished from a closed participant prefix");
            replica.batchStart=Clock::now()-std::chrono::milliseconds(20);replica.sharperPropose();
            check(replica.shBatches.empty() && replica.shInitiating.empty() && replica.pending.count(rid(only)),
                  "open partial cross-shard prefix genuinely waits for its configured 600-millisecond batching deadline");
            replica.batchStart=Clock::now()-std::chrono::milliseconds(601);replica.sharperPropose();
            check(replica.shBatches.at(replica.shInitiating).proposal.at("requests")==json::array({only}),
                  "open partial prefix proposes intact once its own batching deadline expires");
        }
        {
            Replica replica(batchingCfg,1,0,nodeDirectory("fifo-full-prefix-ready"));
            auto full=fifoRequest(cfg,"full-prefix-eight",{1,2},8);admitted(replica,full);
            auto selected=replica.shSelectPendingPrefix();
            check(selected.requests==json::array({full}) && selected.count==8,
                  "a full FIFO prefix retains all eight transactions of its complete signed request");
            replica.batchStart=Clock::now()-std::chrono::milliseconds(20);replica.sharperPropose();
            check(replica.shBatches.at(replica.shInitiating).proposal.at("requests")==json::array({full}),
                  "a full contiguous prefix proposes without inheriting the partial-prefix batching wait");
        }
        {
            Replica replica(batchingCfg,1,0,nodeDirectory("fifo-duplicate-and-erase"));
            auto first=fifoRequest(cfg,"zz-order-first",{1,2});
            auto second=fifoRequest(cfg,"mm-order-second",{1,2});
            auto third=fifoRequest(cfg,"aa-order-third",{1,2});
            admitted(replica,first);admitted(replica,second);admitted(replica,third);
            auto before=replica.shSelectPendingPrefix();auto firstIndex=replica.shArrivalIndex.at(rid(first));
            auto nextArrival=replica.shNextArrival;replica.handle(first);replica.handle(first);
            check(before.requests==json::array({first,second,third}) && replica.shSelectPendingPrefix().requests==before.requests &&
                  replica.pending.size()==3 && replica.shArrivalIndex.at(rid(first))==firstIndex && replica.shNextArrival==nextArrival,
                  "duplicate CLIENT delivery never changes FIFO position or allocates another arrival entry");
            replica.erasePending(rid(second));
            check(replica.shSelectPendingPrefix().requests==json::array({first,third}) &&
                  replica.shArrivalIndex.count(rid(first)) && replica.shArrivalIndex.count(rid(third)) && !replica.shArrivalIndex.count(rid(second)),
                  "erasePending removes only its own FIFO request and index");
            auto newcomer=fifoRequest(cfg,"00-new-arrival",{1,2});admitted(replica,newcomer);
            check(replica.shSelectPendingPrefix().requests==json::array({first,third,newcomer}) &&
                  replica.shArrivalIndex.at(rid(newcomer))>replica.shArrivalIndex.at(rid(third)),
                  "a new lexicographically smallest ID remains at the arrival FIFO tail after deletion");
        }
        {
            Replica replica(batchingCfg,1,0,nodeDirectory("fifo-snapshot-rebuild"));
            auto finished=fifoRequest(cfg,"zz-rebuild-finished",{1,2});
            auto second=fifoRequest(cfg,"mm-rebuild-second",{1,2});
            auto third=fifoRequest(cfg,"aa-rebuild-third",{1,2});
            admitted(replica,finished);admitted(replica,second);admitted(replica,third);
            auto secondIndex=replica.shArrivalIndex.at(rid(second)),thirdIndex=replica.shArrivalIndex.at(rid(third));
            // Snapshot/certified completion can leave a prior pending entry
            // until the recovery path re-admits it against known results.
            const auto& tx=finished.at("body").at("txs")[0];
            json result={{"id",tx.at("id")},{"kind","executed"},{"seq",1},{"digest",hash("rebuild-certified-result")}};
            replica.state["requests"][rid(finished)]={{"txs_hash",hash(finished.at("body").at("txs").dump())},{"results",json::array({result})}};
            replica.completedCstResults[rid(finished)]=json::array({result});
            replica.rebuildPendingIndex();
            check(replica.shSelectPendingPrefix().requests==json::array({second,third}) &&
                  replica.shArrivalIndex.at(rid(second))==secondIndex && replica.shArrivalIndex.at(rid(third))==thirdIndex,
                  "snapshot pending-index rebuild preserves the unfinished request arrival order");
            check(!replica.pending.count(rid(finished)) && !replica.shArrivalIndex.count(rid(finished)) &&
                  replica.shArrivalOrder.size()==2 && replica.shArrivalIndex.size()==2,
                  "snapshot rebuild prunes completed pending residue from both FIFO indices");
            auto newcomer=fifoRequest(cfg,"00-rebuild-newcomer",{1,2});admitted(replica,newcomer);
            check(replica.shSelectPendingPrefix().requests==json::array({second,third,newcomer}),
                  "new arrivals remain behind recovered unfinished FIFO requests");
        }
        {
            Replica replica(batchingCfg,1,0,nodeDirectory("fifo-overlapping-client-alias"));
            auto original=fifoRequest(cfg,"owner-original",{1,2});admitted(replica,original);
            auto aliasBody=original.at("body");aliasBody["id"]="unit:00-owner-alias:request";
            auto alias=sign(aliasBody,readKey(cfg.at("client_private_key"),true));
            check(replica.members.clientMessage(alias) && replica.sharperValidRequest(alias,1),
                  "overlapping alias fixture remains an authentic complete signed CLIENT envelope");
            replica.handle(alias);replica.handle(alias);
            check(replica.pending.size()==1 && replica.deferredRequests.size()==1 &&
                  replica.deferredRequests.count(rid(alias)) && !replica.shArrivalIndex.count(rid(alias)) &&
                  replica.shSelectPendingPrefix().requests==json::array({original}),
                  "overlapping request aliases are deferred without creating duplicate FIFO consensus candidates");
            replica.batchStart=Clock::now()-std::chrono::milliseconds(601);replica.sharperPropose();
            auto batchKey=replica.shInitiating;replica.handle(original);replica.sharperPropose();
            check(replica.shBatches.size()==1 && replica.shLocal==batchKey && replica.shBatches.at(batchKey).proposal.at("requests")==json::array({original}) &&
                  replica.shBatches.at(batchKey).commit.is_null(),
                  "repeated owner or deferred alias delivery cannot start another consensus for the same transaction");
        }
    }
    static void originReservationAndDeadlineChecks(const json& cfg,const std::string& directory,json& checks) {
        auto check=[&](bool condition,const std::string& name) {require(condition,name);checks.push_back(name);};
        auto nodeDirectory=[&](const std::string& name) {
            auto path=directory+"/"+name;std::filesystem::create_directories(path);return path;
        };
        {
            // Origin 2 creates a batch for [2,3]. It must bind its proposed
            // origin_seq before the event loop admits a foreign [1,2] batch.
            Replica origin(cfg,2,0,nodeDirectory("atomic-origin-reservation"));
            auto request=clientRequest(cfg,"atomic-origin-reservation",{2,3});
            check(origin.sharperValidRequest(request,2),"atomic origin fixture has an authentic eligible client request");
            origin.sharperClient(request);origin.batchStart=Clock::now()-std::chrono::hours(1);
            origin.sharperPropose();
            auto ownKey=origin.shInitiating;
            check(!ownKey.empty() && origin.shBatches.count(ownKey),"origin creates and remembers its own SUPER_PROPOSE");
            const auto& own=origin.shBatches.at(ownKey);
            check(origin.shLocal==ownKey && origin.slots.count(origin.applied+1) &&
                  !origin.slots.at(origin.applied+1).proposal.is_null() && !own.assignment.is_null(),
                  "origin atomically reserves origin_seq before its first propose call returns");
            check(origin.slots.at(1).proposal.at("body").at("value").at("sharper_propose").at("body").at("batch")==ownKey &&
                  own.proposal.at("body").at("origin_seq")==1 && own.assignment.at("body").at("seq")==1,
                  "origin SUPER_PROPOSE and the immediate local assignment bind the identical sequence");
            check(!own.accept.is_null() && own.accept.at("body").at("local_seq")==1 && own.commit.is_null(),
                  "atomic reservation casts only a local ACCEPT without inventing a global COMMIT");
            std::string foreignLabel;
            for(int i=0;i<1000000;++i) {
                auto candidate="foreign-before-own:"+std::to_string(i);
                if(hash(candidate)<ownKey) {foreignLabel=candidate;break;}
            }
            check(!foreignLabel.empty(),"foreign fixture has a strictly earlier batch dictionary key");
            auto foreign=batch(origin,cfg,foreignLabel);auto foreignKey=Replica::shKey(foreign.proposal);
            check(origin.shValidPropose(foreign.proposal,foreign.proposal.at("requests")),
                  "foreign competing SUPER_PROPOSE has authentic signatures and participant accesses");
            origin.sharperHandle(foreign.proposal);
            for(int voter=0;voter<3;++voter) origin.sharperHandle(accept(cfg,foreign,1,voter));
            check(origin.shPrefixReady(origin.shBatches.at(foreignKey),2),
                  "foreign competitor becomes ready after three authentic lower-participant ACCEPTs");
            origin.sharperPropose();
            check(origin.shLocal==ownKey && origin.slots.at(1).proposal.at("body").at("value").at("sharper_propose").at("body").at("batch")==ownKey &&
                  origin.shBatches.at(foreignKey).assignment.is_null() && origin.shBatches.at(foreignKey).commit.is_null(),
                  "a ready foreign batch cannot steal an already advertised origin_seq even with an earlier key");
        }
        {
            // A foreign batch reaches idle shard 2 after its previous progress
            // deadline expired. Waiting for a missing lower-participant prefix
            // is remote work; becoming locally ready starts a fresh deadline.
            Replica idle(cfg,2,0,nodeDirectory("fresh-foreign-deadline"));
            auto foreign=batch(idle,cfg,"fresh-foreign-deadline");auto key=Replica::shKey(foreign.proposal);
            auto old=Clock::now()-std::chrono::milliseconds(idle.timeoutMs*4);idle.lastProgress=old;
            check(idle.sharperHandle(foreign.proposal),"idle replica admits an authentic new foreign SUPER_PROPOSE");
            check(idle.lastProgress==old && !idle.shPrefixReady(idle.shBatches.at(key),2) &&
                  !idle.sharperWaiting(Clock::now()),
                  "missing remote prefix neither revives the local deadline nor triggers a local view change");
            for(int voter=0;voter<2;++voter) idle.sharperHandle(accept(cfg,foreign,1,voter));
            check(idle.lastProgress==old,"incomplete foreign prefix does not postpone the local timeout");
            auto beforeReady=Clock::now();idle.sharperHandle(accept(cfg,foreign,1,2));
            auto activated=idle.lastProgress;
            check(activated>=beforeReady && idle.shPrefixReady(idle.shBatches.at(key),2) &&
                  idle.sharperWaiting(Clock::now()),
                  "newly ready foreign work starts a fresh local progress deadline");
            idle.sharperHandle(foreign.proposal);idle.sharperHandle(accept(cfg,foreign,1,2));
            check(idle.lastProgress==activated,"duplicate foreign proposal and ACCEPT cannot refresh the active deadline");
            check(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-activated).count()<idle.timeoutMs,
                  "fresh foreign activation cannot immediately inherit an expired local view-change timeout");
        }
    }
    static void newViewInstallationChecks(const json& cfg,const std::string& directory,json& checks) {
        auto check=[&](bool condition,const std::string& name) {require(condition,name);checks.push_back(name);};
        auto nodeDirectory=[&](const std::string& name) {
            auto path=directory+"/"+name;std::filesystem::create_directories(path);return path;
        };
        {
            // Only replica 0 accepted A. Its own signed VIEW_CHANGE is included
            // in the new primary's three-message certificate; the other two
            // replicas report no reservation. A therefore has neither f+1
            // recovery support nor a global prepared/committed certificate.
            Replica lagging(cfg,1,0,nodeDirectory("unsupported-reservation"));
            auto abandoned=batch(lagging,cfg,"unsupported-reservation");
            auto key=Replica::shKey(abandoned.proposal);
            lagging.sharperAcceptAssignment(abandoned.assignment);
            check(lagging.shLocal==key && !lagging.shBatches.at(key).accept.is_null(),
                  "fixture has one authenticated local ACCEPT reservation");
            check(!lagging.slots.at(1).prepared && lagging.shBatches.at(key).commit.is_null(),
                  "fixture has neither a global prepared quorum nor a cast COMMIT");
            auto ownEvidence=lagging.sharperRecoveryEvidence();
            check(ownEvidence.at("reservations").size()==1 && ownEvidence.at("commits").empty(),
                  "unsupported reservation evidence contains exactly one local vote");
            json vcs=json::array({viewChange(lagging,cfg,0,1,ownEvidence),
                viewChange(lagging,cfg,1,1,emptyRecoveryEvidence()),
                viewChange(lagging,cfg,2,1,emptyRecoveryEvidence())});
            auto recovered=verifiedRecovery(lagging,vcs,1,"unsupported-reservation");
            check(recovered.second.empty(),"legal NEW_VIEW excludes a singly supported unprepared reservation");
            auto nv=newView(lagging,cfg,vcs,1);
            check(lagging.members.replicaMessage(nv),"replacement NEW_VIEW has an authentic primary signature");
            lagging.acceptNewView(nv);
            check(lagging.view==1 && !lagging.changing,"authenticated replacement NEW_VIEW is installed");
            check(lagging.shLocal.empty(),"NEW_VIEW releases an unselected unprepared local reservation");
            check(lagging.shBatches.at(key).assignment.is_null() && lagging.shBatches.at(key).accept.is_null(),
                  "NEW_VIEW removes stale assignment and local ACCEPT for the unselected batch");
            auto rid=abandoned.proposal.at("requests")[0].at("body").at("id").get<std::string>();
            check(lagging.pending.count(rid) && lagging.shPending.count(key),
                  "releasing an unprepared reservation retains the complete signed request for retry");
            check(!lagging.isForward() && lagging.shBatches.at(key).commit.is_null(),
                  "nonprimary releases a weak reservation without manufacturing another consensus vote");
        }
        {
            // A promoted origin-2 primary must reclaim its previously advertised
            // head in the NEW_VIEW installation itself, before a foreign batch
            // can be admitted on the next event-loop iteration.
            Replica promoted(cfg,2,1,nodeDirectory("atomic-new-view-origin-reservation"));
            auto request=clientRequest(cfg,"atomic-new-view-origin-reservation",{2,3});
            auto requests=json::array({request});auto ownKey=hash("atomic-new-view-origin-reservation");
            auto proposal=message(cfg,2,0,"SH_SUPER_PROPOSE",{{"batch",ownKey},{"origin_seq",1},
                {"participants",json::array({2,3})},{"request_digest",hash(requests.dump())}});
            proposal["requests"]=requests;
            auto oldClaim=message(cfg,2,0,"SH_LOCAL_CLAIM",{{"batch",ownKey},{"origin_seq",1},{"seq",1},
                {"ballot",uint64_t(1)},{"request_digest",hash(requests.dump())},
                {"proposal_digest",hash(Replica::shHeader(proposal).dump())}});
            json value={{"requests",requests},{"sharper_propose",Replica::shHeader(proposal)},{"sharper_claim",oldClaim}};
            auto assignment=message(cfg,2,0,"PREPREPARE",{{"seq",1},{"digest",hash(value.dump())},{"value",value}});
            promoted.sharperAcceptAssignment(assignment);
            auto ownEvidence=promoted.sharperRecoveryEvidence();
            check(promoted.shLocal==ownKey && ownEvidence.at("reservations").size()==1 && ownEvidence.at("commits").empty(),
                  "promoted primary starts with one unprepared origin reservation and no COMMIT");
            json vcs=json::array({viewChange(promoted,cfg,1,1,ownEvidence,json::array(),2),
                viewChange(promoted,cfg,2,1,emptyRecoveryEvidence(),json::array(),2),
                viewChange(promoted,cfg,3,1,emptyRecoveryEvidence(),json::array(),2)});
            check(verifiedRecovery(promoted,vcs,1,"atomic-new-view-origin-reservation").second.empty(),
                  "promoted primary's weak origin reservation is absent from canonical recovery");
            promoted.acceptNewView(newView(promoted,cfg,vcs,1,2));
            const auto& reclaimed=promoted.shBatches.at(ownKey);auto immediateAssignment=reclaimed.assignment;
            check(promoted.isForward() && promoted.shLocal==ownKey && !immediateAssignment.is_null() &&
                  immediateAssignment.at("body").at("view")==1 && promoted.slots.at(1).proposal==immediateAssignment,
                  "NEW_VIEW atomically reclaims the promoted origin's unselected advertised head");
            check(Replica::shHeader(reclaimed.proposal)==Replica::shHeader(proposal) &&
                  immediateAssignment.at("body").at("value").at("sharper_propose")==Replica::shHeader(proposal) &&
                  immediateAssignment.at("body").at("value").at("requests")==requests,
                  "atomic NEW_VIEW retry preserves the identical batch header and signed requests");
            check(!reclaimed.accept.is_null() && reclaimed.accept.at("body").at("view")==1 &&
                  reclaimed.accept.at("body").at("claim_digest")!=ownEvidence.at("reservations")[0].at("accept").at("body").at("claim_digest") &&
                  reclaimed.commit.is_null(),
                  "atomic NEW_VIEW retry casts a fresh ACCEPT without inventing a global COMMIT");
            std::string foreignLabel;
            for(int i=0;i<1000000;++i) {
                auto candidate="foreign-after-new-view:"+std::to_string(i);
                if(hash(candidate)<ownKey) {foreignLabel=candidate;break;}
            }
            check(!foreignLabel.empty(),"NEW_VIEW competitor has a strictly earlier batch dictionary key");
            auto foreign=batch(promoted,cfg,foreignLabel);auto foreignKey=Replica::shKey(foreign.proposal);
            promoted.sharperHandle(foreign.proposal);
            for(int voter=0;voter<3;++voter) promoted.sharperHandle(accept(cfg,foreign,1,voter));
            check(promoted.shPrefixReady(promoted.shBatches.at(foreignKey),2),"NEW_VIEW competitor has an authentic complete lower-participant prefix");
            promoted.sharperPropose();
            check(promoted.shLocal==ownKey && promoted.slots.at(1).proposal==immediateAssignment &&
                  promoted.shBatches.at(foreignKey).assignment.is_null() && promoted.shBatches.at(foreignKey).commit.is_null(),
                  "foreign work cannot steal the origin head immediately after NEW_VIEW installation");
        }
        {
            // An authenticated global prepared certificate outranks partial
            // reservations, and must remain the selected canonical value.
            Replica prepared(cfg,1,3,nodeDirectory("protected-prepared"));
            auto agreed=batch(prepared,cfg,"protected-prepared");auto key=Replica::shKey(agreed.proposal);
            prepared.sharperAcceptAssignment(agreed.assignment);installAcceptVector(prepared,agreed);
            prepared.slots.at(1).prepared=true;prepared.preparedHistory[1]=agreed.prepared;
            json vcs=json::array({viewChange(prepared,cfg,1,1,evidence(prepared,agreed,cfg,1),json::array({agreed.prepared})),
                viewChange(prepared,cfg,2,1,emptyRecoveryEvidence()),
                viewChange(prepared,cfg,3,1,prepared.sharperRecoveryEvidence(),json::array({agreed.prepared}))});
            auto recovered=verifiedRecovery(prepared,vcs,1,"protected-prepared");
            check(recovered.second.at(1)==agreed.value,"NEW_VIEW selects the authenticated global prepared value");
            prepared.acceptNewView(newView(prepared,cfg,vcs,1));
            check(prepared.shLocal==key && prepared.slots.at(1).proposal.at("body").at("value")==agreed.value,
                  "NEW_VIEW retains the selected globally prepared assignment");
            check(prepared.validPrepared(prepared.preparedHistory.at(1)),
                  "installed prepared value still has valid participant ACCEPT quorums");
            check(prepared.shBatches.at(key).assignment.at("body").at("view")==1 &&
                  prepared.shBatches.at(key).assignment.at("body").at("value").at("sharper_claim")==agreed.claim1,
                  "recovered proposal preserves the original prepared claim and canonical value");
        }
        {
            // Participant 2 has not heard either the original proposal or its
            // lower-participant ACCEPTs. A full prepared vector in an included
            // VC alone must supply its authenticated prefix during recovery.
            Replica cold(cfg,2,1,nodeDirectory("cold-participant-prepared-recovery"));
            auto agreed=batch(cold,cfg,"cold-participant-prepared-recovery");auto key=Replica::shKey(agreed.proposal);
            json localValue={{"requests",agreed.proposal.at("requests")},
                {"sharper_propose",Replica::shHeader(agreed.proposal)},{"sharper_claim",agreed.claim2}};
            auto assignment=message(cfg,2,0,"PREPREPARE",{{"seq",1},{"digest",hash(localValue.dump())},{"value",localValue}});
            json globalPrepared={{"sharper",true},{"proposal",assignment},{"prepares",agreed.accepts}};
            check(cold.validPrepared(globalPrepared),"cold participant fixture has an authentic full cross-shard prepared vector");
            check(cold.shBatches.empty() && cold.shLocal.empty() && cold.slots.empty(),
                  "cold participant has no local assignment or prior lower-participant ACCEPT prefix");
            json vcs=json::array({viewChange(cold,cfg,1,1,emptyRecoveryEvidence(),json::array({globalPrepared}),2),
                viewChange(cold,cfg,2,1,emptyRecoveryEvidence(),json::array(),2),
                viewChange(cold,cfg,3,1,emptyRecoveryEvidence(),json::array(),2)});
            check(verifiedRecovery(cold,vcs,1,"cold-participant-prepared-recovery").second.at(1)==localValue,
                  "cold participant's NEW_VIEW selects the full prepared canonical value");
            cold.acceptNewView(newView(cold,cfg,vcs,1,2));
            const auto& recovered=cold.shBatches.at(key);
            check(cold.shLocal==key && !recovered.assignment.is_null() && cold.slots.at(1).proposal.at("body").at("value")==localValue,
                  "cold participant installs the recovered assignment using the VC's complete ACCEPT vector");
            check(cold.shPrefixReady(recovered,2) && recovered.accepts.at(1).size()>=3,
                  "selected prepared recovery imports three authenticated lower-participant ACCEPTs");
            check(cold.validPrepared(cold.preparedHistory.at(1)) && !recovered.commit.is_null(),
                  "cold participant preserves the global prepared value and can cast its matching COMMIT");
        }
        {
            // Replica 0 already cast COMMIT, but its VIEW_CHANGE is not among
            // the three messages selected by primary 1. Two other matching
            // local ACCEPT reservations suffice to recover A. Installation
            // must retain replica 0's vote rather than permitting a new value.
            Replica voted(cfg,1,0,nodeDirectory("protected-cast-commit"));
            auto agreed=batch(voted,cfg,"protected-cast-commit");auto key=Replica::shKey(agreed.proposal);
            voted.sharperAcceptAssignment(agreed.assignment);installAcceptVector(voted,agreed);
            voted.sharperAdvance(1);auto cast=voted.shBatches.at(key).commit;
            check(!cast.is_null() && voted.shValidCommit(cast,agreed.proposal,agreed.proposal.at("requests"),agreed.seqs,agreed.claims),
                  "fixture already cast one authentic COMMIT for the prepared value");
            json incomplete=json::array({viewChange(voted,cfg,1,1,evidence(voted,agreed,cfg,1)),
                viewChange(voted,cfg,2,1,emptyRecoveryEvidence()),
                viewChange(voted,cfg,3,1,emptyRecoveryEvidence())});
            check(verifiedRecovery(voted,incomplete,1,"protected-cast-commit unsafe canonical omission").second.empty(),
                  "three individually valid view changes can omit locally proven global preparation");
            auto priorAssignment=voted.slots.at(1).proposal;auto priorPrepared=voted.preparedHistory.at(1);
            bool rejectedUnsafeView=false;
            try {voted.acceptNewView(newView(voted,cfg,incomplete,1));}
            catch(const std::exception&) {rejectedUnsafeView=true;}
            check(rejectedUnsafeView,"NEW_VIEW rejects canonical omission of an authenticated locally cast prepared COMMIT");
            check(voted.view==0 && voted.targetView==0 && voted.shLocal==key &&
                  voted.slots.at(1).proposal==priorAssignment && voted.preparedHistory.at(1)==priorPrepared &&
                  voted.shBatches.at(key).commit==cast && voted.lastNewView.is_null(),
                  "unsafe NEW_VIEW rejection occurs before any slot, view, or COMMIT is partially replaced");
            json vcs=json::array({viewChange(voted,cfg,1,1,evidence(voted,agreed,cfg,1)),
                viewChange(voted,cfg,2,1,evidence(voted,agreed,cfg,2)),
                viewChange(voted,cfg,3,1,emptyRecoveryEvidence())});
            check(verifiedRecovery(voted,vcs,1,"protected-cast-commit matching reservations").second.at(1)==agreed.value,
                  "two matching reservations recover a value even when the COMMIT voter is omitted");
            voted.acceptNewView(newView(voted,cfg,vcs,1));
            check(voted.shBatches.at(key).commit==cast && voted.shBatches.at(key).commits.at(1).at(0)==cast,
                  "NEW_VIEW never cancels or rewrites an already cast COMMIT omitted from its certificate");
            auto competitor=batch(voted,cfg,"competitor-to-cast-commit",1,1);
            auto competitorKey=Replica::shKey(competitor.proposal);voted.shRemember(competitor.proposal);
            voted.sharperAcceptAssignment(competitor.assignment);voted.sharperAdvance(1);
            check(voted.shLocal==key && voted.slots.at(1).proposal.at("body").at("value")==agreed.value &&
                  voted.shBatches.at(competitorKey).assignment.is_null() && voted.shBatches.at(competitorKey).commit.is_null(),
                  "a conflicting batch cannot reserve or receive a second COMMIT for the protected sequence");
            check(voted.shBatches.at(key).commit==cast,"conflicting retry leaves the original COMMIT unchanged");
        }
        {
            // A full cross-shard certificate carried in a genuine VIEW_CHANGE
            // must still apply, retaining its original committed value and all
            // authenticated votes across the local NEW_VIEW installation.
            Replica committed(cfg,1,3,nodeDirectory("protected-certificate"));
            auto agreed=batch(committed,cfg,"protected-certificate");auto key=Replica::shKey(agreed.proposal);
            committed.sharperAcceptAssignment(agreed.assignment);installAcceptVector(committed,agreed);
            auto& remembered=committed.shBatches.at(key);
            for(const auto& vote:agreed.commits)
                remembered.commits[vote.at("body").at("shard").get<int>()][vote.at("body").at("from").get<int>()]=vote;
            auto ownBody=agreed.commits[0].at("body");ownBody["from"]=3;
            auto cast=sign(ownBody,replicaKey(cfg,1,3));remembered.commit=cast;remembered.commits[1][3]=cast;
            committed.certificates[1]=agreed.certificate;committed.preparedHistory[1]=agreed.certificate;
            json vcs=json::array({viewChange(committed,cfg,1,1,evidence(committed,agreed,cfg,1),json::array({agreed.certificate})),
                viewChange(committed,cfg,2,1,emptyRecoveryEvidence()),
                viewChange(committed,cfg,3,1,committed.sharperRecoveryEvidence(),json::array({agreed.certificate}))});
            check(verifiedRecovery(committed,vcs,1,"protected-certificate").second.at(1)==agreed.value,
                  "NEW_VIEW selects the complete committed certificate value");
            committed.acceptNewView(newView(committed,cfg,vcs,1));
            check(committed.applied==1 && committed.state.at("executed")==1 && remembered.applied,
                  "NEW_VIEW installation applies the preserved cross-shard commit certificate");
            check(committed.validCertificate(committed.certificates.at(1)) &&
                  committed.certificates.at(1).at("proposal").at("body").at("value")==agreed.value,
                  "applied recovered sequence retains a valid certificate for the identical canonical value");
            check(remembered.commit==cast,"applying a recovered certificate retains the replica's original COMMIT vote");
        }
    }
    static json run(const json& cfg,const std::string& directory) {
        Replica replica(cfg,1,3,directory);auto a=batch(replica,cfg,"A");json checks=json::array();
        auto check=[&](bool condition,const std::string& name) {require(condition,name);checks.push_back(name);};
        check(replica.sharperValidPrepared(a.prepared),"valid global prepared proof");
        check(replica.sharperValidCertificate(a.certificate),"valid global commit certificate");
        Replica::ShBatch scheduled;scheduled.proposal=a.proposal;
        check(replica.shPrefixReady(scheduled,1),"lowest participant may reserve the first slot");
        check(!replica.shPrefixReady(scheduled,2),"next participant waits for prior ACCEPT quorum");
        for(int voter=0;voter<2;++voter) scheduled.accepts[1][voter]=accept(cfg,a,1,voter);
        check(!replica.shPrefixReady(scheduled,2),"two prior ACCEPT votes do not reserve the next slot");
        auto alternateClaimBody=a.claim1.at("body");alternateClaimBody["ballot"]=uint64_t(2);
        auto alternateClaim=sign(alternateClaimBody,replicaKey(cfg,1,0));
        auto alternateAcceptBody=accept(cfg,a,1,2).at("body");
        alternateAcceptBody["claim"]=alternateClaim;alternateAcceptBody["claim_digest"]=hash(alternateClaim.dump());
        scheduled.accepts[1][2]=sign(alternateAcceptBody,replicaKey(cfg,1,2));
        check(replica.shValidAccept(scheduled.accepts[1][2],a.proposal),"alternate claim vote is individually authentic");
        check(!replica.shPrefixReady(scheduled,2),"different prior claims cannot form scheduling quorum");
        scheduled.accepts[1][2]=accept(cfg,a,1,2);
        check(replica.shPrefixReady(scheduled,2),"three matching prior ACCEPT votes allow the next slot");
        auto validEvidence=evidence(replica,a,cfg,2);
        auto vc=viewChange(replica,cfg,2,1,validEvidence);
        check(replica.validVC(vc,1),"valid pending reservation in view change");

        auto badSeq=validEvidence;auto ppBody=badSeq["reservations"][0]["proposal"]["body"];
        ppBody["seq"]=2;badSeq["reservations"][0]["proposal"]=sign(ppBody,replicaKey(cfg,1,0));
        check(!replica.validVC(viewChange(replica,cfg,2,1,badSeq),1),"reservation PP and claim sequence mismatch rejected");
        auto outside=batch(replica,cfg,"outside-window",replica.window+1);
        check(!replica.validVC(viewChange(replica,cfg,2,1,evidence(replica,outside,cfg,2)),1),"reservation outside local window rejected");
        auto wrongSigner=validEvidence;
        wrongSigner["reservations"][0]["accept"]=accept(cfg,a,1,1);
        check(!replica.validVC(viewChange(replica,cfg,2,1,wrongSigner),1),"reservation signer must equal VC voter");
        auto forgedAccept=validEvidence;forgedAccept["reservations"][0]["accept"]["signature"]=std::string(128,'0');
        check(!replica.validVC(viewChange(replica,cfg,2,1,forgedAccept),1),"forged reservation signature rejected");
        auto malformedCommit=validEvidence;
        malformedCommit["commits"]=json::array({message(cfg,1,2,"SH_COMMIT",{{"batch",hash("A")}})});
        check(!replica.validVC(viewChange(replica,cfg,2,1,malformedCommit),1),"signed malformed recovery COMMIT rejected");
        auto validCommit=validEvidence;validCommit["commits"]=json::array({a.commits[2]});
        check(replica.validVC(viewChange(replica,cfg,2,1,validCommit),1),"well formed recovery COMMIT accepted");
        auto wrongBatch=validCommit;auto commitBody=wrongBatch["commits"][0]["body"];
        commitBody["batch"]=hash("unknown");wrongBatch["commits"][0]=sign(commitBody,replicaKey(cfg,1,2));
        check(!replica.validVC(viewChange(replica,cfg,2,1,wrongBatch),1),"recovery COMMIT must bind a known proposal");
        auto wrongDigest=validCommit;commitBody=wrongDigest["commits"][0]["body"];
        commitBody["record_digest"]=hash("forged-record");wrongDigest["commits"][0]=sign(commitBody,replicaKey(cfg,1,2));
        check(!replica.validVC(viewChange(replica,cfg,2,1,wrongDigest),1),"recovery COMMIT record digest checked");
        auto notArray=validEvidence;notArray["commits"]=json::object();
        check(!replica.validVC(viewChange(replica,cfg,2,1,notArray),1),"recovery evidence arrays required");
        auto tooMany=validEvidence;tooMany["commits"]=json::array();
        for(int i=0;i<=replica.window;++i) tooMany["commits"].push_back(a.commits[2]);
        check(!replica.validVC(viewChange(replica,cfg,2,1,tooMany),1),"recovery evidence size bounded");

        auto extraValue=a.value;extraValue["unbound_extension"]=true;
        check(!replica.sharperValidValue(extraValue),"claim cannot certify extra unbound value fields");
        auto badClaim=a.value;auto claimBody=badClaim["sharper_claim"]["body"];
        claimBody["proposal_digest"]=hash("another-signed-origin-header");
        badClaim["sharper_claim"]=sign(claimBody,replicaKey(cfg,1,0));
        check(!replica.sharperValidValue(badClaim),"claim binds original signed origin header");

        auto b=batch(replica,cfg,"B",1,1);
        auto byzantine=evidence(replica,b,cfg,1);byzantine["reservations"][0]["accept"]=accept(cfg,b,1,1,1);
        auto honest2=viewChange(replica,cfg,2,2,evidence(replica,a,cfg,2),json::array({a.prepared}));
        auto honest3=viewChange(replica,cfg,3,2,evidence(replica,a,cfg,3),json::array({a.prepared}));
        auto byzantineVC=viewChange(replica,cfg,1,2,byzantine);
        check(replica.validVC(byzantineVC,2),"individually authentic higher view reservation");
        bool protectedPrepared=false;
        try {
            auto recovered=replica.recovery(json::array({byzantineVC,honest2,honest3}),2);
            protectedPrepared=recovered.second.at(1)==a.value;
        } catch(const std::exception&) {protectedPrepared=true;}
        check(protectedPrepared,"one higher view reservation cannot replace global prepared value");
        auto empty2=viewChange(replica,cfg,2,2,replica.sharperRecoveryEvidence());
        auto empty3=viewChange(replica,cfg,3,2,replica.sharperRecoveryEvidence());
        auto unsupported=replica.recovery(json::array({byzantineVC,empty2,empty3}),2);
        check(unsupported.second.empty(),"one Byzantine reservation cannot invent a recovered lock");
        auto partial2=viewChange(replica,cfg,2,2,evidence(replica,a,cfg,2));
        auto partial3=viewChange(replica,cfg,3,2,evidence(replica,a,cfg,3));
        auto partial=replica.recovery(json::array({byzantineVC,partial2,partial3}),2);
        check(partial.second.at(1)==a.value,"two matching reservations preserve the prior local assignment");
        auto noConflict=viewChange(replica,cfg,1,2,evidence(replica,a,cfg,1));
        auto recovered=replica.recovery(json::array({noConflict,honest2,honest3}),2);
        check(recovered.second.at(1)==a.value,"valid recovery preserves agreed canonical value");
        fifoBatchingChecks(cfg,directory,checks);
        originReservationAndDeadlineChecks(cfg,directory,checks);
        newViewInstallationChecks(cfg,directory,checks);
        return {{"checks",checks},{"network_started",false}};
    }
};

int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: test_sharper_recovery CONFIG OUTPUT_DIRECTORY");
        std::cout<<SharPerProtocolTest::run(readJson(argv[1]),argv[2]).dump()<<'\n';
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
