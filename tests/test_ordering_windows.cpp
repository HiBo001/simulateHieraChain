// Window safety uses actual request signatures, PBFT ballots and certificates.
// Network threads are intentionally not started by this deterministic harness.
#define ARBOR_ORDERING_WINDOW_TESTS 1
#define main arbor_node_program_main
#include "../source/main.cpp"
#undef main

struct OrderingWindowsTest {
    static void require(bool okay,const std::string& message) {
        if(!okay) throw std::runtime_error(message);
    }
    static Key key(const json& cfg,int shard,int replica) {
        for(const auto& node:cfg.at("nodes"))
            if(node.at("shard")==shard && node.at("replica")==replica)
                return readKey(node.at("private_key"),true);
        throw std::runtime_error("missing replica key");
    }
    static json message(const json& cfg,int shard,int replica,const std::string& type,json fields,int view=0) {
        fields["type"]=type;fields["run"]=cfg.at("run_id");fields["shard"]=shard;
        fields["from"]=replica;fields["view"]=view;
        return sign(fields,key(cfg,shard,replica));
    }
    static json request(const json& cfg,const std::string& rid,const json& participants,int target,uint64_t value=1) {
        json accesses=json::array();
        for(int sid:participants) accesses.push_back({{"shard",sid},{"key","account:"+std::to_string(sid)+":"+rid},
                                                    {"value",value}});
        json tx={{"id",rid+":tx"},{"key",accesses[0].at("key")},{"value",value},
                 {"participants",participants},{"accesses",accesses}};
        return sign({{"type","CLIENT"},{"run",cfg.at("run_id")},{"id",rid},{"target",target},
                     {"reply",{{"host","127.0.0.1"},{"port",31000}}},{"txs",json::array({tx})}},
                    readKey(cfg.at("client_private_key"),true));
    }
    static json proposal(const json& cfg,int shard,int seq,const json& value,int view=0) {
        return message(cfg,shard,view%4,"PREPREPARE",{{"seq",seq},{"digest",hash(value.dump())},{"value",value}},view);
    }
    static json sharedRequest(const json& cfg,const std::string& rid,const json& participants,int target,uint64_t value) {
        auto body=request(cfg,rid,participants,target,value).at("body");
        for(auto& access:body["txs"][0]["accesses"])
            access["key"]="account:"+std::to_string(access.at("shard").get<int>())+":window-shared";
        body["txs"][0]["key"]=body["txs"][0]["accesses"][0]["key"];
        return sign(body,readKey(cfg.at("client_private_key"),true));
    }
    static json certificate(const json& cfg,const json& pp) {
        const auto& b=pp.at("body");int shard=b.at("shard"),seq=b.at("seq"),view=b.at("view");
        json prepares=json::array(),commits=json::array();
        for(int voter=0;voter<4;++voter) {
            if(voter!=view%4 && prepares.size()<2)
                prepares.push_back(message(cfg,shard,voter,"PREPARE",{{"seq",seq},{"digest",b.at("digest")}},view));
            if(commits.size()<3)
                commits.push_back(message(cfg,shard,voter,"COMMIT",{{"seq",seq},{"digest",b.at("digest")}},view));
        }
        return {{"proposal",pp},{"prepares",prepares},{"commits",commits}};
    }
    static void drain(Replica& replica) {
        while(!replica.inbox.empty()) {
            json next=replica.inbox.front();replica.inbox.pop();replica.handle(next);
        }
    }
    static void votes(Replica& replica,const json& cfg,const json& pp) {
        const auto& b=pp.at("body");int seq=b.at("seq"),view=b.at("view");
        for(int voter=0;voter<4;++voter) if(voter!=view%4)
            replica.handle(message(cfg,replica.shard,voter,"PREPARE",{{"seq",seq},{"digest",b.at("digest")}},view));
        drain(replica);
        for(int voter=0;voter<4;++voter)
            replica.handle(message(cfg,replica.shard,voter,"COMMIT",{{"seq",seq},{"digest",b.at("digest")}},view));
        drain(replica);
    }
    static json block(Replica& coordinator,const json& cfg,json requests=json::array()) {
        int n=coordinator.applied+1,b=coordinator.state.at("cst_round").get<int>()+1;
        const auto ps=coordinator.participants(requests);json watermarks=json::object();
        for(int leaf:coordinator.members.descendants(coordinator.shard))
            watermarks[std::to_string(leaf)]=coordinator.state.at("participant_indices").value(std::to_string(leaf),0)+(ps.count(leaf)?1:0);
        json value={{"requests",requests},{"cst_round",b},{"cst_watermarks",watermarks}};
        if(!requests.empty()) value["cst_order_index"]=coordinator.state.at("cst_order_index").get<int>()+1;
        if(b%coordinator.orderingWindowBlocks()==0)
            value["cst_window_manifest"]=coordinator.buildWindowManifest(value,n);
        require(coordinator.validValue(value),"locally built window value must pass normal PBFT validation");
        json pp=proposal(cfg,coordinator.shard,n,value);coordinator.handle(pp);votes(coordinator,cfg,pp);
        require(coordinator.applied==n,"three authentic COMMIT votes must apply a coordinator block");
        return certificate(cfg,pp);
    }
    static json popProposal(Replica& replica) {
        while(!replica.inbox.empty()) {
            json next=replica.inbox.front();replica.inbox.pop();
            if(next.at("body").at("type")=="PREPREPARE") return next;
            replica.handle(next);
        }
        return nullptr;
    }
    static json scheduled(Replica& replica) {
        replica.batchStart=Clock::now()-std::chrono::seconds(1);
        replica.propose();return popProposal(replica);
    }
    static void remember(Replica& leaf,const json& cert) {
        require(leaf.rememberRound(cert),"authenticated coordinator certificate must be cached");
    }
    static json witness(Replica& leaf,const json& cfg,const json& order) {
        const std::string batch=Replica::cstKey(order);json proofs=json::array();
        for(int owner:leaf.orderParticipants(order)) {
            json record={{"shard",owner},{"batch_key",batch},{"order_digest",order.at("proposal").at("body").at("digest")},
                         {"order_certificate",order},{"reads",json::object()},{"writes",json::array()}};
            for(const auto& req:order.at("proposal").at("body").at("value").at("requests"))
                for(const auto& tx:req.at("body").at("txs")) if(leaf.touches(tx,owner)) {
                    auto access=leaf.accessFor(tx,owner);const std::string local=access.at("key");
                    record["reads"][local]=leaf.state.at("kv").contains(local)?leaf.state.at("kv").at(local):leaf.initialAccount();
                    record["writes"].push_back({{"id",tx.at("id")},{"tx_digest",hash(tx.dump())},{"key",local},
                        {"value",access.at("value")},{"fib",leaf.expectedFib},{"duplicate",false}});
                }
            json ballots=json::array();
            for(int voter:{0,1,2}) ballots.push_back(message(cfg,owner,voter,"CST_PREPARED",
                {{"batch_key",batch},{"record_digest",hash(leaf.recordPayload(record).dump())}}));
            proofs.push_back({{"record",record},{"votes",ballots}});
        }
        return {{"batch_key",batch},{"proofs",proofs}};
    }
    static json stable(const json& cfg,Replica& replica) {
        json proof=json::array();const auto digest=StateDigest::fromState(replica.state);
        for(int voter:{0,1,2}) proof.push_back(message(cfg,replica.shard,voter,"CHECKPOINT",
            {{"seq",replica.applied},{"digest",digest}}));
        return {{"seq",replica.applied},{"state",replica.state},{"proof",proof}};
    }
    static json stagedWitness(const json& cfg,const json& order,const std::vector<Replica*>& leaves) {
        const auto batch=Replica::cstKey(order);json proofs=json::array();
        const auto& first=*leaves.at(0);
        for(int owner:first.orderParticipants(order)) {
            const Replica* source=nullptr;
            for(const auto* candidate:leaves) if(candidate->shard==owner) source=candidate;
            require(source && source->stagedRecords.count(batch),"every participant must stage its actual ordered execution record");
            const auto record=source->stagedRecords.at(batch);json ballots=json::array();
            for(int voter:{0,1,2}) ballots.push_back(message(cfg,owner,voter,"CST_PREPARED",
                {{"batch_key",batch},{"record_digest",hash(source->recordPayload(record).dump())}}));
            proofs.push_back({{"record",record},{"votes",ballots}});
        }
        return {{"batch_key",batch},{"proofs",proofs}};
    }
    static void markExecuted(Replica& leaf,const json& cert) {
        // A selection-only fixture advances the same replicated projections as
        // finalizeCst. Separate tests below exercise the real execution path.
        const auto& b=cert.at("proposal").at("body");int co=b.at("shard");
        leaf.state["cst_batches"][Replica::cstKey(cert)]=b.at("digest");
        leaf.state["cst_indices"][std::to_string(co)]=leaf.projection(b.at("value"),leaf.shard);
        leaf.state["cst_round"]=std::max(leaf.state.at("cst_round").get<int>(),b.at("value").at("cst_round").get<int>());
        leaf.pendingCst.erase(Replica::cstKey(cert));leaf.cstSelectionDirty=true;
    }
    static json run(const json& input,const std::string& directory) {
        json checks=json::array();int caseNumber=0;
        auto check=[&](bool okay,const std::string& name) {require(okay,name);checks.push_back(name);};
        auto cfg=input;
        auto node=[&](int shard,int replica=0) {
            const std::string path=directory+"/case-"+std::to_string(++caseNumber);
            std::filesystem::create_directories(path);
            return std::make_unique<Replica>(cfg,shard,replica,path);
        };
        const json lowerPs=json::array({1,2}),rootPs=json::array({1,3});
        for(int m:{1,2,4}) {
            cfg=input;cfg["consensus"]["ordering_window_blocks"]=m;
            auto lower=node(5),root=node(7),leaf=node(1);
            check(leaf->windowsEnabled() && leaf->windowOf(m)==1 && leaf->windowOf(m+1)==2,
                  "m="+std::to_string(m)+" maps exactly m real PBFT blocks to each window");
            json lowerOrder=block(*lower,cfg,json::array({request(cfg,"lower:m"+std::to_string(m),lowerPs,5)}));
            json lowerClose=lowerOrder;
            for(int b=2;b<=m;++b) lowerClose=block(*lower,cfg);
            for(int b=1;b<m;++b) block(*root,cfg);
            json rootOrder=block(*root,cfg,json::array({request(cfg,"root:m"+std::to_string(m),rootPs,7)}));
            remember(*leaf,lowerOrder);remember(*leaf,lowerClose);
            check(leaf->nextCstOrder().is_null(),"m="+std::to_string(m)+" lower order waits for the root closing certificate");
            remember(*leaf,rootOrder);
            const auto selected=leaf->nextCstOrder();
            check(!selected.is_null() && Replica::cstKey(selected.at("certificate"))==Replica::cstKey(rootOrder),
                  "m="+std::to_string(m)+" higher-layer coordinator 7 precedes lower-layer coordinator 5");
            check(!leaf->validLeafOrder(lowerOrder,selected.at("frontier")),
                  "m="+std::to_string(m)+" signed leaf proposals cannot reorder a lower-layer batch ahead of the root");
            markExecuted(*leaf,rootOrder);
            const auto next=leaf->nextCstOrder();
            check(!next.is_null() && Replica::cstKey(next.at("certificate"))==Replica::cstKey(lowerOrder),
                  "m="+std::to_string(m)+" lower batch follows the root even when its block number is smaller");
            check(leaf->validOrderClose(rootOrder) && leaf->validOrderClose(lowerClose),
                  "m="+std::to_string(m)+" window closing certificates carry authentic PBFT quorum evidence");
        }
        {
            cfg=input;cfg["consensus"]["ordering_window_blocks"]=2;
            auto lower=node(5),root=node(7),leaf=node(1);
            auto lowerOrder=block(*lower,cfg,json::array({request(cfg,"missing:lower",lowerPs,5)}));
            auto lowerClose=block(*lower,cfg);
            auto rootOrder=block(*root,cfg,json::array({request(cfg,"missing:root",rootPs,7)}));
            auto rootClose=block(*root,cfg);
            remember(*leaf,lowerOrder);remember(*leaf,lowerClose);remember(*leaf,rootClose);
            check(leaf->nextCstOrder().is_null(),"a closing certificate cannot substitute for a missing manifest business order");
            check(!leaf->validLeafOrder(lowerOrder,json::array({lowerClose,rootClose})),
                  "the entire window must be complete before any destined batch can be proposed");
            auto forged=rootOrder;forged["proposal"]["body"]["value"]["requests"][0]["body"]["txs"][0]["value"]=uint64_t(999);
            check(!leaf->rememberRound(forged),"tampered business payloads cannot satisfy a signed manifest entry");
            remember(*leaf,rootOrder);
            check(!leaf->nextCstOrder().is_null() && leaf->nextCstOrder().at("certificate")==rootOrder,
                  "late arrival of the exact certified business order releases the root batch");
            const size_t pending=leaf->pendingCst.size(),certs=leaf->roundCertificates.size();
            for(int replay=0;replay<5;++replay) {remember(*leaf,rootOrder);remember(*leaf,rootClose);}
            check(leaf->pendingCst.size()==pending && leaf->roundCertificates.size()==certs && leaf->state.at("executed")==0,
                  "repeated closing and business certificates do not create duplicate requests or execute transactions");
            auto altered=rootClose.at("proposal").at("body").at("value");
            altered["cst_window_manifest"]["1"]["orders"][0]["request_digest"]=std::string(64,'0');
            check(!root->validLocalWindowValue(altered),"a primary cannot certify a closing manifest detached from its committed history");
            auto malformed=rootClose.at("proposal").at("body").at("value");
            malformed["cst_window_manifest"]["1"]["end"]=2;
            check(!leaf->validOrderClose(certificate(cfg,proposal(cfg,7,2,malformed))),
                  "valid signatures do not rescue inconsistent manifest counts");
            auto nonclosing=rootOrder.at("proposal").at("body").at("value");
            nonclosing["cst_window_manifest"]=rootClose.at("proposal").at("body").at("value").at("cst_window_manifest");
            check(!leaf->validOrderClose(certificate(cfg,proposal(cfg,7,1,nonclosing))),
                  "non-closing blocks cannot claim a window closing manifest");
        }
        {
            cfg=input;cfg["consensus"]["ordering_window_blocks"]=2;
            auto lower=node(5),root=node(7),leaf=node(1);
            auto old=block(*lower,cfg,json::array({request(cfg,"older:lower",lowerPs,5)}));
            auto oldClose=block(*lower,cfg);block(*root,cfg);auto oldRootClose=block(*root,cfg);
            auto newer=block(*root,cfg,json::array({request(cfg,"newer:root",rootPs,7)}));
            auto newerRootClose=block(*root,cfg);block(*lower,cfg);auto newerLowerClose=block(*lower,cfg);
            remember(*leaf,newer);remember(*leaf,newerRootClose);remember(*leaf,newerLowerClose);
            check(leaf->nextCstOrder().is_null(),"a future window cannot skip an older unexecuted projected order");
            remember(*leaf,old);remember(*leaf,oldClose);remember(*leaf,oldRootClose);
            check(!leaf->nextCstOrder().is_null() && leaf->nextCstOrder().at("certificate")==old,
                  "ascending windows take precedence over coordinator depth in a later window");
            markExecuted(*leaf,old);
            check(!leaf->nextCstOrder().is_null() && leaf->nextCstOrder().at("certificate")==newer,
                  "cumulative projections permit the next complete window only after earlier business execution");
        }
        {
            cfg=input;cfg["consensus"]["ordering_window_blocks"]=2;
            auto lower=node(5),root=node(7),leaf=node(1);
            auto lowerOrder=block(*lower,cfg,json::array({sharedRequest(cfg,"execution:lower",lowerPs,5,2)}));
            auto lowerClose=block(*lower,cfg);block(*root,cfg);
            auto rootOrder=block(*root,cfg,json::array({sharedRequest(cfg,"execution:root",rootPs,7,1)}));
            remember(*leaf,lowerOrder);remember(*leaf,lowerClose);remember(*leaf,rootOrder);
            auto pp=scheduled(*leaf);require(!pp.is_null(),"complete window must yield a real leaf proposal");
            check(pp.at("body").at("value").at("cst_orders")[0]==rootOrder,
                  "real leaf PBFT proposes the certified root batch before its earlier-arriving lower batch");
            leaf->handle(pp);votes(*leaf,cfg,pp);
            check(leaf->applied==0 && leaf->certificates.count(1) && leaf->stagedRecords.size()==1,
                  "a committed leaf PBFT slot still waits for all participant execution proofs");
            check(scheduled(*leaf).is_null(),"a leaf keeps exactly one batch in flight while its read/write proofs are missing");
            const json committed=leaf->certificates.at(1);leaf->startViewChange(4);
            json changes=json::array();
            for(int voter:{0,1,2}) changes.push_back(message(cfg,1,voter,"VIEW_CHANGE",
                {{"stable",{{"seq",0},{"state",leaf->genesis()},{"proof",json::array()}}},
                 {"prepared",json::array({committed})}},4));
            const auto newProposal=proposal(cfg,1,1,pp.at("body").at("value"),4);
            leaf->handle(message(cfg,1,0,"NEW_VIEW",{{"changes",changes},{"proposals",json::array({newProposal})}},4));
            check(leaf->view==4 && !leaf->changing && leaf->applied==0 && leaf->certificates.count(1),
                  "view change preserves the committed window order without applying an incomplete witness");
            require(leaf->importWitness(witness(*leaf,cfg,rootOrder)),"authentic participant execution proofs must import");
            leaf->applyReady();
            check(leaf->applied==1 && leaf->state.at("executed")==1 && leaf->state.at("cst_batches").contains(Replica::cstKey(rootOrder)),
                  "the recovered leaf batch executes exactly once after its certified witness arrives");
            remember(*leaf,rootOrder);remember(*leaf,rootOrder);leaf->applyReady();
            check(leaf->applied==1 && leaf->state.at("executed")==1 && leaf->pendingCst.size()==1,
                  "post-execution certificate replays cannot execute or enqueue the finished batch again");
            auto next=scheduled(*leaf);require(!next.is_null(),"lower batch must become selectable after root execution");
            check(next.at("body").at("value").at("cst_orders")[0]==lowerOrder,
                  "completion of the root releases the lower-layer batch in the same window");
            leaf->handle(next);votes(*leaf,cfg,next);
            require(leaf->importWitness(witness(*leaf,cfg,lowerOrder)),"lower witness must import");leaf->applyReady();
            check(leaf->applied==2 && leaf->state.at("executed")==2 && leaf->state.at("cst_round")==2 && leaf->pendingCst.empty(),
                  "executing an earlier lower block cannot move the replicated leaf window backwards");
            check(leaf->state.at("kv").at("account:1:window-shared").at("version")==2 &&
                  leaf->state.at("kv").at("account:1:window-shared").at("value")==uint64_t(2)+leaf->expectedFib,
                  "conflicting keys reflect root execution first and lower execution second");
        }
        {
            cfg=input;cfg["consensus"]["ordering_window_blocks"]=2;
            auto lower=node(5),root=node(7),leaf=node(1);
            auto lowerOrder=block(*lower,cfg,json::array({request(cfg,"deadline:lower",lowerPs,5)}));
            auto lowerClose=block(*lower,cfg);block(*root,cfg);
            remember(*leaf,lowerOrder);remember(*leaf,lowerClose);
            const auto now=Clock::now(),old=now-std::chrono::seconds(100);
            leaf->lastProgress=old;
            check(!leaf->activateWindowWork(now) && leaf->lastProgress==old && leaf->windowWorkTimerStarts==0,
                  "waiting for an absent ancestor closing proof cannot start or refresh a local PBFT deadline");
            auto rootClose=block(*root,cfg);remember(*leaf,rootClose);
            check(leaf->activateWindowWork(now) && leaf->lastProgress==now && leaf->windowWorkTimerStarts==1 &&
                  leaf->windowReadyHeadKey==Replica::cstKey(lowerOrder),
                  "a complete head first becoming eligible after long idle receives one fresh PBFT deadline");
            for(int repeat=0;repeat<3;++repeat) remember(*leaf,rootClose);
            check(!leaf->activateWindowWork(now+std::chrono::seconds(10)) && leaf->lastProgress==now &&
                  leaf->windowWorkTimerStarts==1,
                  "replayed closing proofs and repeated ready-head polls cannot prolong an existing deadline");
            block(*lower,cfg);auto lowerFutureClose=block(*lower,cfg);
            auto rootFutureOrder=block(*root,cfg,json::array({request(cfg,"deadline:future-root",rootPs,7)}));
            auto rootFutureClose=block(*root,cfg);
            remember(*leaf,lowerFutureClose);remember(*leaf,rootFutureOrder);remember(*leaf,rootFutureClose);
            check(!leaf->activateWindowWork(now+std::chrono::seconds(11)) && leaf->lastProgress==now,
                  "later complete windows and unrelated empty closing proofs do not refresh the current execution head");
            const auto selected=leaf->nextCstOrder();require(!selected.is_null(),"deadline fixture must retain its first complete head");
            auto active=proposal(cfg,1,1,{{"requests",json::array()},{"cst_orders",json::array({lowerOrder})},
                                          {"cst_frontier",selected.at("frontier")}});
            leaf->slots[1].proposal=active;
            check(!leaf->activateWindowWork(now+std::chrono::seconds(12)) && leaf->lastProgress==now,
                  "an active leaf PBFT proposal prevents readiness bookkeeping from extending its timeout");
            leaf->slots.clear();leaf->certificates[1]=certificate(cfg,active);
            check(!leaf->activateWindowWork(now+std::chrono::seconds(13)) && leaf->lastProgress==now,
                  "a committed execution head cannot receive a fresh deadline from duplicate readiness proofs");
            leaf->certificates.clear();leaf->stagedRecords[Replica::cstKey(lowerOrder)]=json::object();
            check(!leaf->activateWindowWork(now+std::chrono::seconds(14)) && leaf->lastProgress==now,
                  "waiting for participant execution records cannot be converted into repeated fresh PBFT deadlines");
            leaf->stagedRecords.clear();leaf->changing=true;
            check(!leaf->activateWindowWork(now+std::chrono::seconds(15)) && leaf->lastProgress==now,
                  "readiness activation cannot reset an ongoing certified view-change deadline");
            leaf->changing=false;markExecuted(*leaf,lowerOrder);
            check(leaf->activateWindowWork(now+std::chrono::seconds(16)) &&
                  leaf->lastProgress==now+std::chrono::seconds(16) && leaf->windowWorkTimerStarts==2 &&
                  leaf->windowReadyHeadKey==Replica::cstKey(rootFutureOrder),
                  "a genuinely different complete execution head can start its own deadline after the earlier head finishes");
            check(!leaf->activateWindowWork(now+std::chrono::seconds(17)) && leaf->windowWorkTimerStarts==2,
                  "the second execution head also starts exactly one timer");
            check(!root->activateWindowWork(now) && root->windowWorkTimerStarts==0,
                  "the leaf readiness timer does not change coordinator timeout behavior");
            cfg["consensus"]["ordering_window_blocks"]=0;auto legacy=node(1);
            check(!legacy->activateWindowWork(now) && legacy->windowWorkTimerStarts==0,
                  "disabled window mode retains the legacy PBFT deadline behavior");
        }
        {
            cfg=input;cfg["consensus"]["ordering_window_blocks"]=12;
            auto lower=node(5),root=node(7),leaf=node(1);
            std::vector<json> business;
            for(int b=1;b<=8;++b) business.push_back(block(*lower,cfg,
                json::array({request(cfg,"flow:"+std::to_string(b),lowerPs,5)})));
            lower->handle(request(cfg,"flow:waiting",lowerPs,5));
            check(lower->outstandingOrders.size()==8 && lower->pending.size()==1,
                  "the regression fixture fills the coordinator's eight-business-batch flow-control limit");
            json lowerClose;
            for(int b=9;b<=12;++b) {
                lower->windowLastBlock=Clock::now()-std::chrono::milliseconds(lower->windowEmptyIntervalMs()+1);
                auto pp=scheduled(*lower);
                require(!pp.is_null(),"flow-controlled coordinator must still propose empty closing blocks");
                require(pp.at("body").at("value").at("requests").empty(),"empty close must not admit a ninth blocked business order");
                lower->handle(pp);votes(*lower,cfg,pp);lowerClose=certificate(cfg,pp);
            }
            json rootClose;for(int b=1;b<=12;++b) rootClose=block(*root,cfg);
            for(const auto& cert:business) remember(*leaf,cert);
            remember(*leaf,lowerClose);remember(*leaf,rootClose);
            check(lower->applied==12 && lower->state.at("cst_order_index")==8 && lower->outstandingOrders.size()==8,
                  "m greater than eight closes with real empty PBFT blocks despite outstanding business backpressure");
            check(!leaf->nextCstOrder().is_null() && leaf->nextCstOrder().at("certificate")==business.front(),
                  "a large complete window releases its first retained business batch without a flow-control deadlock");
            check(rootClose.at("proposal").at("body").at("value").at("cst_window_manifest").at("1").at("orders").empty(),
                  "an idle root's real PBFT empty close proves that no root orders are missing");
        }
        {
            cfg=input;cfg["consensus"]["ordering_window_blocks"]=2;
            auto original=node(5),restored=node(5,3);
            auto first=block(*original,cfg,json::array({request(cfg,"restore:one",lowerPs,5)}));block(*original,cfg);
            const auto checkpoint=stable(cfg,*original);
            require(restored->validStable(checkpoint),"snapshot fixture must carry three valid checkpoint votes");
            restored->installStable(checkpoint);
            check(restored->state.at("cst_rounds")==original->state.at("cst_rounds") && restored->applied==2,
                  "certified checkpoint catch-up preserves all committed window manifest history");
            auto value=first.at("proposal").at("body").at("value");
            value["requests"]=json::array({request(cfg,"restore:two",lowerPs,5)});
            value["cst_round"]=3;value["cst_order_index"]=2;
            for(auto it=value["cst_watermarks"].begin();it!=value["cst_watermarks"].end();++it)
                it.value()=restored->state.at("participant_indices").value(it.key(),0)+(it.key()=="1"||it.key()=="2"?1:0);
            check(restored->validValue(value),"a recovered backup validates the next window against restored cumulative projections");
            const auto now=Clock::now();restored->windowLastBlock=now;
            check(!restored->windowEmptyDue(now+std::chrono::milliseconds(restored->windowEmptyIntervalMs()-1)) &&
                   restored->windowEmptyDue(now+std::chrono::milliseconds(restored->windowEmptyIntervalMs())),
                  "the empty-block timer respects the configured interval rather than creating busy-loop blocks");
            auto pause=sign({{"type","WINDOW_PAUSE"},{"run",cfg.at("run_id")},{"target",5},{"final_block",3}},
                readKey(cfg.at("client_private_key"),true));
            check(restored->handleWindowPause(pause) && restored->windowPauseBlock==4,
                  "an authenticated end-of-experiment pause rounds up to a real closing boundary");
            auto badPause=pause;badPause["body"]["final_block"]=0;
            check(!restored->handleWindowPause(badPause) && restored->windowPauseBlock==4,
                  "a forged pause cannot suppress required closing blocks");
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_streaming",false}});
            cfg["consensus"].erase("ancestor_order");
            cfg["consensus"].erase("ancestor_lower_blocks_per_root");
            cfg["shards"]=json::array();
            for(const auto& [id,parent]:std::vector<std::pair<int,int>>{{1,5},{2,5},{3,6},{4,6},{5,7},{6,7},{7,9},{8,9},{9,-1}})
                cfg["shards"].push_back({{"id",id},{"parent",parent<0?json(nullptr):json(parent)}});
            auto lower=node(5),middle=node(7),root=node(9),leaf=node(1),backup=node(1,1),unrelated=node(6);
            check(leaf->windowsEnabled() && !leaf->ancestorStreamingEnabled() &&
                  leaf->members.ancestors(1)==std::set<int>({5,7,9}),
                  "four-layer m=1 barrier uses only the leaf's root, middle and parent ancestors");
            auto lowerOrder=block(*lower,cfg,json::array({sharedRequest(cfg,"layered:four:lower",json::array({1,2}),5,3)}));
            auto middleOrder=block(*middle,cfg,json::array({sharedRequest(cfg,"layered:four:middle",json::array({1,2,3}),7,2)}));
            auto rootOrder=block(*root,cfg,json::array({sharedRequest(cfg,"layered:four:root",json::array({1,2,8}),9,1)}));
            remember(*leaf,rootOrder);
            check(leaf->nextCstOrder().is_null(),
                  "four-layer m=1 root block one waits for same-number middle and parent certificates");
            remember(*leaf,lowerOrder);
            check(leaf->nextCstOrder().is_null(),
                  "four-layer m=1 root and parent cannot replace the missing middle block one");
            remember(*leaf,middleOrder);
            const auto complete=leaf->nextCstOrder();
            check(!complete.is_null() && complete.at("certificate")==rootOrder && complete.at("frontier").size()==3,
                  "four-layer m=1 complete own-ancestor frontier releases Root1 without a sibling coordinator certificate");
            json rootValue={{"requests",json::array()},{"cst_orders",json::array({rootOrder})},
                            {"cst_frontier",complete.at("frontier")}};
            remember(*backup,rootOrder);remember(*backup,middleOrder);remember(*backup,lowerOrder);
            check(backup->validValue(rootValue),
                  "four-layer m=1 backup authenticates the complete own-ancestor frontier with genuine PBFT quorums");
            check(!leaf->validLeafOrder(middleOrder,complete.at("frontier")) &&
                  !leaf->validLeafOrder(lowerOrder,complete.at("frontier")),
                  "four-layer m=1 middle and parent cannot overtake the unexecuted root block one");
            auto sibling=block(*unrelated,cfg);
            check(!leaf->rememberRound(sibling),
                  "four-layer m=1 rejects a genuine certificate from non-ancestor coordinator six");
            auto forged=rootValue;
            for(auto& close:forged["cst_frontier"])
                if(close.at("proposal").at("body").at("shard")==7) close=sibling;
            check(!backup->validValue(forged),
                  "four-layer m=1 a sibling quorum cannot substitute for the middle ancestor's closing proof");
            int executed=0;
            for(const auto& expected:std::vector<json>{rootOrder,middleOrder,lowerOrder}) {
                const int coordinator=expected.at("proposal").at("body").at("shard");
                const auto selected=leaf->nextCstOrder();
                check(!selected.is_null() && selected.at("certificate")==expected,
                      "four-layer m=1 selects Root1 then Middle1 then Parent1: coordinator "+std::to_string(coordinator));
                auto pp=scheduled(*leaf);require(!pp.is_null(),"four-layer m=1 complete frontier must yield a leaf proposal");
                check(pp.at("body").at("value").at("cst_orders").size()==1 &&
                      pp.at("body").at("value").at("cst_orders")[0]==expected,
                      "four-layer m=1 submits exactly one coordinator block per leaf PBFT: coordinator "+std::to_string(coordinator));
                leaf->handle(pp);votes(*leaf,cfg,pp);
                check(leaf->applied==executed && leaf->certificates.count(executed+1) && scheduled(*leaf).is_null(),
                      "four-layer m=1 cannot submit the next layer before the current block's witness executes: coordinator "+std::to_string(coordinator));
                require(leaf->importWitness(witness(*leaf,cfg,expected)),"four-layer m=1 authenticated witness must import");
                leaf->applyReady();++executed;
                check(leaf->applied==executed && leaf->state.at("executed")==executed && leaf->state.at("cst_round")==1,
                      "four-layer m=1 applies each layer once at the same logical block number: coordinator "+std::to_string(coordinator));
            }
            check(leaf->state.at("kv").at("account:1:window-shared").at("version")==3 &&
                  leaf->state.at("kv").at("account:1:window-shared").at("value")==uint64_t(3)+leaf->expectedFib &&
                  leaf->pendingCst.empty() && leaf->leafBusinessSlots==3 && leaf->leafControlSlots==0,
                  "four-layer m=1 conflicting account records Root, Middle, Parent execution in three separate business slots");
        }
        // Streaming deliberately differs from the old whole-window barrier.
        for(int m:{1,4}) {
            cfg=input;cfg["consensus"]["ordering_window_blocks"]=m;
            cfg["consensus"]["ancestor_streaming"]=true;
            auto lower=node(5),root=node(7),leaf=node(1),backup=node(1,1),restored=node(1,3),unrelated=node(6);
            auto rootFirst=block(*root,cfg,json::array({request(cfg,"stream:root:first:m"+std::to_string(m),rootPs,7)}));
            auto profiled=node(1,2);
            profiled->pendingCst.emplace(Replica::cstKey(rootFirst),json{{"body",{{"certificate",rootFirst}}}});
            profiled->nextCstOrder();
            check(profiled->leafProfileWaitState()==Replica::LeafRoot,
                  "profiling attributes the missing fixed root head to Root wait without validating a new proof");
            const auto profileState=profiled->state;const auto profileDigest=profiled->cachedStateDigest;
            auto profileTime=Clock::now();profiled->leafWaitSince=profileTime;
            profiled->leafProfileAccountWait(Replica::LeafRoot,profileTime);
            profiled->leafProfileAccountWait(Replica::LeafChild,profileTime+std::chrono::milliseconds(10));
            profiled->leafProfileAccountWait(Replica::LeafIdle,profileTime+std::chrono::milliseconds(30));
            check(profiled->leafWaitNs[Replica::LeafRoot]==10000000 && profiled->leafWaitNs[Replica::LeafChild]==20000000,
                  "Root and Child wall wait timers charge disjoint intervals once");
            check(profiled->state==profileState && profiled->cachedStateDigest==profileDigest,
                  "diagnostic counters never change replicated state or its digest");
            remember(*leaf,rootFirst);
            const auto first=leaf->nextCstOrder();
            check(!first.is_null() && first.at("certificate")==rootFirst && first.at("frontier").size()==1,
                  "stream m="+std::to_string(m)+" executes the root head without the same-number child or closing window");
            json rootValue={{"requests",json::array()},{"cst_orders",json::array({rootFirst})},{"cst_frontier",first.at("frontier")}};
            check(backup->validValue(rootValue),"a backup validates a self-contained streaming prefix without local arrival history");
            auto forged=rootValue;forged["cst_orders"][0]["commits"]=json::array();
            check(!backup->validValue(forged),"a genuine prefix cannot authenticate a forged candidate quorum");
            auto wrong=block(*unrelated,cfg);
            check(!leaf->rememberRound(wrong),"streaming rejects a sibling coordinator certificate");
            auto extra=rootValue;extra["cst_frontier"].push_back(wrong);
            check(!backup->validValue(extra),"streaming rejects an extra sibling or future proof after the selected head");
            auto pp=proposal(cfg,1,1,rootValue);leaf->handle(pp);votes(*leaf,cfg,pp);
            check(leaf->leafProfileWaitState()==Replica::LeafWitness,
                  "a committed slot awaiting cross-leaf dependencies is Witness wait rather than ancestor wait");
            check(leaf->applied==0 && leaf->streamCursor()==0,
                  "a committed business slot cannot advance the stream before execution dependencies complete");
            check(leaf->importWitness(witness(*leaf,cfg,rootFirst)),"streaming retains real authenticated read-write dependencies");
            leaf->applyReady();
            check(leaf->applied==1 && leaf->streamCursor()==1 && leaf->state.at("executed")==1,
                  "stream cursor advances with the actually executed PBFT business slot");
            check(leaf->cachedStateDigest==StateDigest::fromState(leaf->state),"the streaming cursor is included in the incremental replicated digest");
            check(leaf->leafBusinessSlots==1 && leaf->leafControlSlots==0 && leaf->leafFrontierProofs==1 &&
                  leaf->leafTxnExecutionElapsedNs>0 && leaf->leafQcValidationCalls>0 && leaf->leafSchedulerDepth==0,
                  "business execution and proof counters are cumulative local diagnostics with balanced nested timers");
            auto rootNext=block(*root,cfg,json::array({request(cfg,"stream:root:next:m"+std::to_string(m),rootPs,7)}));
            remember(*leaf,rootNext);
            check(leaf->nextCstOrder().is_null(),"R2 cannot bypass missing C1 after R1 has executed");
            check(leaf->leafProfileWaitState()==Replica::LeafChild,
                  "R2 waiting behind absent C1 charges Child wait without skipping C1");
            auto childFirst=block(*lower,cfg,json::array({request(cfg,"stream:child:first:m"+std::to_string(m),lowerPs,5)}));
            remember(*leaf,childFirst);const auto child=leaf->nextCstOrder();
            check(!child.is_null() && child.at("certificate")==childFirst,
                  "late C1 is selected before already-arrived R2");
            json badFrontier=json::array({rootNext});
            check(!leaf->validLeafOrder(rootNext,badFrontier),"a Byzantine primary cannot propose R2 without C1 evidence");
            json childValue={{"requests",json::array()},{"cst_orders",json::array({childFirst})},{"cst_frontier",child.at("frontier")}};
            pp=proposal(cfg,1,2,childValue);leaf->handle(pp);votes(*leaf,cfg,pp);
            require(leaf->importWitness(witness(*leaf,cfg,childFirst)),"child execution witness must import");leaf->applyReady();
            check(leaf->streamCursor()==2 && leaf->state.at("executed")==2,"C1 executes after R1 and durably completes that block number");
            const auto checkpoint=stable(cfg,*leaf);
            require(restored->validStable(checkpoint),"stream checkpoint signatures must validate");restored->installStable(checkpoint);
            remember(*restored,rootNext);
            check(restored->streamCursor()==2 && !restored->nextCstOrder().is_null() &&
                  restored->nextCstOrder().at("certificate")==rootNext,
                  "checkpoint recovery restores the fixed cursor without replaying R1 or waiting for C2");
            restored->startViewChange(1);
            check(restored->streamCursor()==2,"view-change initiation preserves the durable streaming cursor");
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",4},{"ancestor_streaming",true}});
            auto lower=node(5),root=node(7),leaf=node(1),restored=node(1,3);
            for(int blockNumber=1;blockNumber<=16;++blockNumber) {
                remember(*leaf,block(*root,cfg));remember(*leaf,block(*lower,cfg));
            }
            auto business=block(*root,cfg,json::array({request(cfg,"stream:after:empty:prefix",rootPs,7)}));
            remember(*leaf,business);const auto progress=leaf->nextCstOrder();
            check(!progress.is_null() && progress.at("certificate").is_null() && progress.at("frontier").size()==32,
                  "long empty prefixes use bounded certified progress instead of an unbounded proposal");
            json progressValue={{"requests",json::array()},{"cst_stream_progress",true},{"cst_frontier",progress.at("frontier")}};
            check(leaf->validValue(progressValue),"bounded skip-only progress requires genuine contiguous ancestor certificates");
            auto missing=progressValue;missing["cst_frontier"].erase(missing["cst_frontier"].begin()+1);
            check(!leaf->validValue(missing),"empty-prefix progress cannot omit a child position");
            auto reordered=progressValue;std::swap(reordered["cst_frontier"][0],reordered["cst_frontier"][1]);
            check(!leaf->validValue(reordered),"empty-prefix progress cannot reorder root and child proofs");
            auto pp=proposal(cfg,1,1,progressValue);leaf->handle(pp);votes(*leaf,cfg,pp);
            check(leaf->leafControlSlots==1 && leaf->leafBusinessSlots==0 && leaf->leafFrontierProofs==32 &&
                  leaf->leafTxnExecutionElapsedNs<leaf->executionNs,
                  "a skip-only control slot adds proofs and state work without a business slot");
            check(leaf->applied==1 && leaf->streamCursor()==32 && leaf->state.at("executed")==0,
                  "skip-only PBFT advances the cursor without creating fake executed transactions");
            const auto checkpoint=stable(cfg,*leaf);restored->installStable(checkpoint);
            remember(*restored,business);
            check(restored->streamCursor()==32 && restored->nextCstOrder().at("certificate")==business,
                  "checkpoint recovery retains skipped empty positions before R17");
            auto replay=progressValue;
            check(!restored->validValue(replay),"an old authenticated progress prefix cannot rewind the cursor");
        }
        for(const auto& bad:json::array({"nearest",1,nullptr})) {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_streaming",true},{"ancestor_order",bad}});
            bool rejected=false;try {auto invalid=node(1);} catch(const std::exception&) {rejected=true;}
            check(rejected,"native configuration rejects invalid ancestor_order="+bad.dump());
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_order","lower_first"}});
            bool rejected=false;try {auto invalid=node(1);} catch(const std::exception&) {rejected=true;}
            check(rejected,"native lower_first requires streaming rather than silently changing whole-window order");
        }
        for(int m:{1,4}) {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",m},{"ancestor_streaming",true},
                                               {"ancestor_order","lower_first"}});
            auto lower=node(5),root=node(7),leaf=node(1),backup=node(1,1),restored=node(1,3),sibling=node(6);
            check(leaf->streamAncestors()==std::vector<int>({5,7}) && leaf->streamAncestorOrder()=="lower_first",
                  "lower-first m="+std::to_string(m)+" orders the child before root at each logical block");
            auto childFirst=block(*lower,cfg,json::array({sharedRequest(cfg,"lower-first:child1:m"+std::to_string(m),lowerPs,5,1)}));
            auto rootFirst=block(*root,cfg,json::array({sharedRequest(cfg,"lower-first:root1:m"+std::to_string(m),rootPs,7,2)}));
            auto childNext=block(*lower,cfg,json::array({sharedRequest(cfg,"lower-first:child2:m"+std::to_string(m),lowerPs,5,3)}));
            remember(*leaf,childFirst);const auto first=leaf->nextCstOrder();
            check(!first.is_null() && first.at("certificate")==childFirst && first.at("frontier").size()==1,
                  "lower-first m="+std::to_string(m)+" C1 executes without R1 or a window close");
            json childValue={{"requests",json::array()},{"cst_orders",json::array({childFirst})},{"cst_frontier",first.at("frontier")}};
            check(backup->validValue(childValue),"lower-first backups validate C1 from real quorum evidence without local arrival history");
            auto forged=childValue;forged["cst_orders"][0]["commits"]=json::array();
            check(!backup->validValue(forged),"lower-first rejects forged candidate quorum despite a genuine prefix");
            auto wrong=block(*sibling,cfg);
            check(!leaf->rememberRound(wrong),"lower-first rejects a signed sibling coordinator block");
            auto extra=childValue;extra["cst_frontier"].push_back(wrong);
            check(!backup->validValue(extra),"lower-first rejects sibling or future proofs after the selected C1");
            auto pp=proposal(cfg,1,1,childValue);leaf->handle(pp);votes(*leaf,cfg,pp);
            check(leaf->applied==0 && leaf->streamCursor()==0,
                  "lower-first committed C1 still waits for authenticated participant execution dependencies");
            require(leaf->importWitness(witness(*leaf,cfg,childFirst)),"lower-first C1 execution witness must import");leaf->applyReady();
            check(leaf->applied==1 && leaf->streamCursor()==1 && leaf->state.at("executed")==1,
                  "lower-first cursor advances only after actual C1 execution");
            remember(*leaf,childNext);
            check(leaf->nextCstOrder().is_null(),"lower-first already-arrived C2 cannot bypass missing R1 or starve root work");
            check(leaf->leafProfileWaitState()==Replica::LeafRoot,
                  "lower-first profiling attributes missing R1 after C1 to Root wait");
            check(!leaf->validLeafOrder(childNext,json::array({childNext})),
                  "lower-first a Byzantine primary cannot skip R1 using a real C2 quorum");
            remember(*leaf,rootFirst);const auto second=leaf->nextCstOrder();
            check(!second.is_null() && second.at("certificate")==rootFirst,
                  "lower-first late R1 precedes the ready C2");
            json rootValue={{"requests",json::array()},{"cst_orders",json::array({rootFirst})},{"cst_frontier",second.at("frontier")}};
            pp=proposal(cfg,1,2,rootValue);leaf->handle(pp);votes(*leaf,cfg,pp);
            require(leaf->importWitness(witness(*leaf,cfg,rootFirst)),"lower-first R1 witness must import");leaf->applyReady();
            check(leaf->streamCursor()==2 && leaf->state.at("executed")==2 &&
                  leaf->state.at("kv").at("account:1:window-shared").at("version")==2 &&
                  leaf->state.at("kv").at("account:1:window-shared").at("value")==uint64_t(2)+leaf->expectedFib,
                  "lower-first conflicting account reflects certified C1 then R1 execution");
            check(leaf->cachedStateDigest==StateDigest::fromState(leaf->state),
                  "lower-first incremental digest includes the unchanged durable cursor encoding");
            const auto checkpoint=stable(cfg,*leaf);
            require(restored->validStable(checkpoint),"lower-first checkpoint signatures must validate");restored->installStable(checkpoint);
            remember(*restored,childNext);
            check(restored->streamCursor()==2 && !restored->nextCstOrder().is_null() &&
                  restored->nextCstOrder().at("certificate")==childNext,
                  "lower-first same-config checkpoint resumes at C2 without replaying C1 or waiting for R2");
            check(!restored->validValue(childValue),"lower-first replayed C1 proof cannot rewind the recovered cursor");
            restored->startViewChange(1);
            check(restored->streamCursor()==2,"lower-first view-change initiation retains the durable execution cursor");
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",4},{"ancestor_streaming",true},
                                               {"ancestor_order","lower_first"}});
            auto lower=node(5),root=node(7),leaf=node(1),restored=node(1,3);
            for(int blockNumber=1;blockNumber<=16;++blockNumber) {
                remember(*leaf,block(*root,cfg));remember(*leaf,block(*lower,cfg));
            }
            auto business=block(*lower,cfg,json::array({request(cfg,"lower-first:after:empty:prefix",lowerPs,5)}));
            remember(*leaf,business);const auto progress=leaf->nextCstOrder();
            check(!progress.is_null() && progress.at("certificate").is_null() && progress.at("frontier").size()==32 &&
                  progress.at("frontier")[0].at("proposal").at("body").at("shard")==5 &&
                  progress.at("frontier")[31].at("proposal").at("body").at("shard")==7,
                  "lower-first bounds certified empty progress to 32 positions in child-root order");
            json progressValue={{"requests",json::array()},{"cst_stream_progress",true},{"cst_frontier",progress.at("frontier")}};
            check(leaf->validValue(progressValue),"lower-first skip-only progress validates a complete real-quorum prefix");
            auto wrongOrder=progressValue;std::swap(wrongOrder["cst_frontier"][0],wrongOrder["cst_frontier"][1]);
            check(!leaf->validValue(wrongOrder),"lower-first rejects root-first reordering of a signed empty prefix");
            auto omitted=progressValue;omitted["cst_frontier"].erase(omitted["cst_frontier"].begin()+1);
            check(!leaf->validValue(omitted),"lower-first rejects an omitted root position in skip-only progress");
            auto pp=proposal(cfg,1,1,progressValue);leaf->handle(pp);votes(*leaf,cfg,pp);
            check(leaf->applied==1 && leaf->streamCursor()==32 && leaf->state.at("executed")==0 && leaf->leafControlSlots==1,
                  "lower-first real control PBFT consumes 32 positions without synthetic transactions");
            const auto checkpoint=stable(cfg,*leaf);restored->installStable(checkpoint);
            remember(*restored,business);
            check(restored->streamCursor()==32 && !restored->nextCstOrder().is_null() &&
                  restored->nextCstOrder().at("certificate")==business,
                  "lower-first cursor-32 checkpoint resumes at C17 before unavailable R17");
            check(!restored->validValue(progressValue),"lower-first replayed skip-only prefix cannot rewind a cursor-32 checkpoint");
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_streaming",true},
                                               {"ancestor_order","lower_first"},{"ancestor_lower_blocks_per_root",1}});
            cfg["shards"]=json::array();
            for(const auto& [id,parent]:std::vector<std::pair<int,int>>{{1,5},{2,5},{3,6},{4,6},{5,7},{6,7},{7,9},{8,9},{9,-1}})
                cfg["shards"].push_back({{"id",id},{"parent",parent<0?json(nullptr):json(parent)}});
            auto lower=node(5),middle=node(7),root=node(9),leaf=node(1),backup=node(1,1),restored=node(1,3),sibling=node(6);
            check(leaf->streamAncestors()==std::vector<int>({5,7,9}),
                  "four-layer lower-first visits nearest parent then middle then root");
            auto lowerOrder=block(*lower,cfg,json::array({sharedRequest(cfg,"lower-first:four:lower",json::array({1,2}),5,1)}));
            auto middleOrder=block(*middle,cfg,json::array({sharedRequest(cfg,"lower-first:four:middle",json::array({1,2,3}),7,2)}));
            auto rootOrder=block(*root,cfg,json::array({sharedRequest(cfg,"lower-first:four:root",json::array({1,2,8}),9,3)}));
            auto next=block(*lower,cfg,json::array({request(cfg,"lower-first:four:next",json::array({1,2}),5)}));
            remember(*backup,rootOrder);remember(*backup,middleOrder);
            check(backup->nextCstOrder().is_null(),"four-layer lower-first root and middle cannot skip a missing nearest-parent position");
            // Only C1 has arrived at the primary; M1 and R1 are still absent.
            remember(*leaf,lowerOrder);
            const auto immediate=leaf->nextCstOrder();
            check(!immediate.is_null() && immediate.at("certificate")==lowerOrder && immediate.at("frontier").size()==1,
                  "four-layer m=1 C1 is immediately selectable without M1 or R1 certificates");
            json immediateValue={{"requests",json::array()},{"cst_orders",json::array({lowerOrder})},
                                 {"cst_frontier",immediate.at("frontier")}};
            check(backup->validValue(immediateValue),
                  "four-layer m=1 backup validates the single C1 quorum without a complete same-number ancestor frontier");
            check(!leaf->rememberRound(block(*sibling,cfg)),
                  "four-layer lower-first does not accept or wait for a sibling coordinator at the same layer");
            int executed=0;
            for(const auto& expected:std::vector<json>{lowerOrder,middleOrder,rootOrder}) {
                const auto selected=leaf->nextCstOrder();
                check(!selected.is_null() && selected.at("certificate")==expected,
                      "four-layer lower-first selects certified coordinator "+std::to_string(expected.at("proposal").at("body").at("shard").get<int>()));
                auto pp=scheduled(*leaf);require(!pp.is_null(),"four-layer lower-first must yield a leaf proposal");
                check(pp.at("body").at("value").at("cst_orders").size()==1 &&
                      pp.at("body").at("value").at("cst_orders")[0]==expected &&
                      pp.at("body").at("value").at("cst_frontier").size()==1,
                      "four-layer m=1 immediately submits one coordinator block in one leaf PBFT: coordinator "+
                      std::to_string(expected.at("proposal").at("body").at("shard").get<int>()));
                leaf->handle(pp);votes(*leaf,cfg,pp);
                check(leaf->applied==executed && leaf->streamCursor()==uint64_t(executed) && scheduled(*leaf).is_null(),
                      "four-layer m=1 committed block waits for execution witness before the next layer: coordinator "+
                      std::to_string(expected.at("proposal").at("body").at("shard").get<int>()));
                require(leaf->importWitness(witness(*leaf,cfg,expected)),"four-layer lower-first witness must import");leaf->applyReady();
                ++executed;
                if(executed==1) {
                    // R1 and next-cycle C2 cannot stand in for this cycle's M1.
                    remember(*leaf,next);remember(*leaf,rootOrder);
                    check(leaf->nextCstOrder().is_null() && scheduled(*leaf).is_null(),
                          "four-layer m=1 C2 and R1 already arrived still wait for missing M1 after executing C1");
                    check(!leaf->validLeafOrder(rootOrder,json::array({rootOrder})) &&
                          !leaf->validLeafOrder(next,json::array({next})),
                          "four-layer m=1 genuine R1 or C2 quorum cannot bypass the required M1 slot");
                    remember(*leaf,middleOrder);
                }
            }
            check(leaf->streamCursor()==3 && leaf->state.at("executed")==3 &&
                  leaf->state.at("kv").at("account:1:window-shared").at("version")==3 &&
                  leaf->state.at("kv").at("account:1:window-shared").at("value")==uint64_t(3)+leaf->expectedFib &&
                  leaf->leafBusinessSlots==3 && leaf->leafControlSlots==0,
                  "four-layer conflicting account executes lower, middle, root exactly once under real leaf PBFT");
            const auto checkpoint=stable(cfg,*leaf);restored->installStable(checkpoint);
            remember(*restored,next);
            check(restored->streamCursor()==3 && !restored->nextCstOrder().is_null() &&
                  restored->nextCstOrder().at("certificate")==next,
                  "four-layer lower-first checkpoint resumes at nearest-parent block two");
        }
        for(const auto& bad:json::array({0,-1,33,1.5,true,"10",nullptr,uint64_t(UINT64_MAX)})) {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_streaming",true},
                {"ancestor_order","lower_first"},{"ancestor_lower_blocks_per_root",bad}});
            bool rejected=false;try {auto invalid=node(1);} catch(const std::exception&) {rejected=true;}
            check(rejected,"native configuration rejects invalid lower/root ratio="+bad.dump());
        }
        for(bool streaming:{false,true}) {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_streaming",streaming},
                {"ancestor_order","root_first"},{"ancestor_lower_blocks_per_root",10}});
            bool rejected=false;try {auto invalid=node(1);} catch(const std::exception&) {rejected=true;}
            check(rejected,"native ratio ten requires lower-first streaming, streaming="+std::to_string(streaming));
        }
        for(const auto& order:std::vector<std::string>{"root_first","lower_first"}) {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_streaming",true},
                {"ancestor_order",order},{"ancestor_lower_blocks_per_root",1}});
            auto leaf=node(1);const auto ancestors=leaf->streamAncestors();bool compatible=true;
            for(uint64_t position=1;position<=100;++position) {
                const auto [co,b]=leaf->slotAtPosition(position);
                compatible &= co==ancestors[(position-1)%ancestors.size()] && b==1+(position-1)/ancestors.size()
                              && leaf->streamPosition(co,b)==position;
            }
            check(compatible,"explicit ratio one preserves every original "+order+" position and inverse mapping");
            bool rejected=false;try {leaf->slotAtPosition(0);} catch(const std::exception&) {rejected=true;}
            check(rejected,"zero stream position is rejected before unsigned underflow for "+order);
            rejected=false;try {leaf->streamPosition(5,UINT64_MAX);} catch(const std::exception&) {rejected=true;}
            check(rejected,"inverse stream position rejects uint64 overflow for "+order);
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_streaming",true},
                {"ancestor_order","lower_first"},{"ancestor_lower_blocks_per_root",10}});
            auto lower=node(5),root=node(7),leaf=node(1),backup=node(1,1),restored=node(1,3),afterRoot=node(1,2);
            bool mapped=true;
            for(uint64_t position=1;position<=100;++position) {
                const auto [co,b]=leaf->slotAtPosition(position);const uint64_t cycle=(position-1)/11,offset=(position-1)%11;
                mapped &= co==(offset==10?7:5) && b==(offset==10?cycle+1:cycle*10+offset+1)
                           && leaf->streamPosition(co,b)==position;
            }
            check(mapped,"ratio ten maps C1 through C10 then R1, C11 through C20 then R2 exactly");
            std::vector<json> children;
            for(int b=1;b<=11;++b) children.push_back(block(*lower,cfg,json::array({
                sharedRequest(cfg,"ratio10:child:"+std::to_string(b),lowerPs,5,b==11?12:b)})));
            const auto rootOrder=block(*root,cfg,json::array({sharedRequest(cfg,"ratio10:root:1",rootPs,7,11)}));
            remember(*leaf,children.front());const auto first=leaf->nextCstOrder();
            check(!first.is_null() && first.at("certificate")==children.front() && first.at("frontier").size()==1,
                  "ratio ten C1 executes with no Root block available");
            json firstValue={{"requests",json::array()},{"cst_orders",json::array({children.front()})},
                             {"cst_frontier",first.at("frontier")}};
            check(backup->validValue(firstValue),"ratio ten backup validates the actual child QC independently of arrival history");
            auto forged=firstValue;forged["cst_orders"][0]["commits"]=json::array();
            check(!backup->validValue(forged),"ratio ten rejects forged business quorum with an authentic prefix");
            check(!backup->validLeafOrder(children[1],json::array({children[1]})),
                  "ratio ten cannot skip applicable C1 using authentic C2 evidence");
            auto pp=proposal(cfg,1,1,firstValue);leaf->handle(pp);votes(*leaf,cfg,pp);
            check(leaf->applied==0 && leaf->streamCursor()==0,
                  "ratio ten committed child cannot advance the cursor until authenticated execution dependencies arrive");
            require(leaf->importWitness(witness(*leaf,cfg,children.front())),"ratio ten C1 witness must import");leaf->applyReady();
            auto execute=[&](Replica& target,const json& expected) {
                const auto selected=target.nextCstOrder();
                require(!selected.is_null() && selected.at("certificate")==expected,"ratio ten selects the mandatory next business block");
                auto nextProposal=scheduled(target);
                if(target.me!=target.view%4) {
                    check(nextProposal.is_null(),"ratio ten backup does not propose after checkpoint at cursor "+std::to_string(target.streamCursor()));
                    json value={{"requests",json::array()},{"cst_orders",json::array({expected})},
                                {"cst_frontier",selected.at("frontier")}};
                    nextProposal=proposal(cfg,target.shard,target.applied+1,value,target.view);
                }
                require(!nextProposal.is_null(),"ratio ten ready business produces a primary-signed leaf PBFT proposal");
                target.handle(nextProposal);votes(target,cfg,nextProposal);
                require(target.importWitness(witness(target,cfg,expected)),"ratio ten authenticated execution witness must import");
                target.applyReady();
            };
            for(int b=2;b<=5;++b) {remember(*leaf,children[b-1]);execute(*leaf,children[b-1]);}
            const auto midpoint=stable(cfg,*leaf);require(restored->validStable(midpoint),"ratio ten C5 checkpoint authenticates");
            restored->installStable(midpoint);
            check(restored->streamCursor()==5 && restored->state.at("executed")==5 &&
                  restored->cachedStateDigest==StateDigest::fromState(restored->state),
                  "ratio ten checkpoint at C5 restores cursor, projection and conflicting account digest");
            remember(*restored,children[10]);
            for(int b=6;b<=9;++b) {remember(*restored,children[b-1]);execute(*restored,children[b-1]);}
            check(restored->nextCstOrder().is_null() && restored->leafProfileWaitState()==Replica::LeafChild,
                  "ratio ten received C11 cannot bypass missing C10");
            check(!restored->validLeafOrder(rootOrder,json::array({rootOrder})),
                  "ratio ten Root1 cannot bypass missing C10 even with an authentic root quorum");
            remember(*restored,children[9]);execute(*restored,children[9]);
            check(restored->streamCursor()==10 && restored->state.at("executed")==10 &&
                  restored->nextCstOrder().is_null() && restored->leafProfileWaitState()==Replica::LeafRoot,
                  "ratio ten executes all ten child blocks without Root1, then blocks C11 on missing Root1");
            check(!restored->validLeafOrder(children[10],json::array({children[10]})),
                  "ratio ten authentic C11 cannot skip the mandatory Root1 position");
            remember(*restored,rootOrder);execute(*restored,rootOrder);
            check(restored->streamCursor()==11 && restored->state.at("executed")==11 &&
                  restored->state.at("cst_round")==10 && restored->localProjection(7)==1,
                  "ratio ten Root1 remains valid after Child10 raised the leaf shared maximum round to ten");
            const auto boundary=stable(cfg,*restored);require(afterRoot->validStable(boundary),"ratio ten Root1 checkpoint authenticates");
            afterRoot->installStable(boundary);remember(*afterRoot,children[10]);
            check(afterRoot->streamCursor()==11 && !afterRoot->nextCstOrder().is_null() &&
                  afterRoot->nextCstOrder().at("certificate")==children[10],
                  "ratio ten checkpoint after Root1 resumes at C11 without waiting for Root2");
            execute(*afterRoot,children[10]);
            check(afterRoot->streamCursor()==12 && afterRoot->state.at("executed")==12 &&
                  afterRoot->state.at("kv").at("account:1:window-shared").at("version")==12 &&
                  afterRoot->state.at("kv").at("account:1:window-shared").at("value")==uint64_t(12)+afterRoot->expectedFib &&
                  afterRoot->cachedStateDigest==StateDigest::fromState(afterRoot->state),
                  "ratio ten authentic PBFT execution serializes all twelve conflicting writes across the Root1 boundary");
            check(!afterRoot->validValue(firstValue),"ratio ten old child proof cannot rewind a recovered weighted cursor");
            afterRoot->state["cst_stream_cursor"]=UINT64_MAX;afterRoot->cstSelectionDirty=true;
            check(!afterRoot->validStreamFrontier(json::array({children.front()}),&children.front()),
                  "ratio ten frontier rejects uint64 cursor overflow rather than wrapping to C1");
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",4},{"ancestor_streaming",true},
                {"ancestor_order","lower_first"},{"ancestor_lower_blocks_per_root",10}});
            auto lower=node(5),root=node(7),leaf=node(1),restored=node(1,3);
            for(int b=1;b<=30;++b) remember(*leaf,block(*lower,cfg));
            for(int b=1;b<=3;++b) remember(*leaf,block(*root,cfg));
            auto business=block(*lower,cfg,json::array({request(cfg,"ratio10:after:control",lowerPs,5)}));
            remember(*leaf,business);const auto progress=leaf->nextCstOrder();
            check(!progress.is_null() && progress.at("certificate").is_null() && progress.at("frontier").size()==32 &&
                  progress.at("frontier")[10].at("proposal").at("body").at("shard")==7 &&
                  progress.at("frontier")[21].at("proposal").at("body").at("shard")==7 &&
                  progress.at("frontier")[31].at("proposal").at("body").at("value").at("cst_round")==30,
                  "ratio ten 32-proof skip control spans three cycles and ends at C30 before Root3");
            json value={{"requests",json::array()},{"cst_stream_progress",true},{"cst_frontier",progress.at("frontier")}};
            check(leaf->validValue(value),"ratio ten bounded progress authenticates every weighted ancestor position");
            auto reordered=value;std::swap(reordered["cst_frontier"][9],reordered["cst_frontier"][10]);
            check(!leaf->validValue(reordered),"ratio ten skip control rejects Root1 before Child10");
            auto pp=proposal(cfg,1,1,value);leaf->handle(pp);votes(*leaf,cfg,pp);
            check(leaf->streamCursor()==32 && leaf->leafControlSlots==1 && leaf->state.at("executed")==0,
                  "ratio ten actual control PBFT consumes the 32-proof prefix without fake business execution");
            restored->installStable(stable(cfg,*leaf));remember(*restored,business);
            check(restored->nextCstOrder().is_null(),"ratio ten cursor32 checkpoint cannot skip missing Root3 to C31");
            auto rootThird=root->roundCertificates.at(3).at(7);remember(*restored,rootThird);
            const auto selected=restored->nextCstOrder();
            check(!selected.is_null() && selected.at("certificate")==business && selected.at("frontier").size()==2 &&
                  selected.at("frontier")[0]==rootThird,
                  "ratio ten checkpoint continuation certifies Root3 then selects C31 in the fourth cycle");
            check(!restored->validValue(value),"ratio ten old weighted skip prefix cannot rewind the recovered cursor");
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",4},{"ancestor_streaming",true},
                {"ancestor_order","lower_first"},{"ancestor_lower_blocks_per_root",10}});
            cfg["shards"]=json::array();
            for(const auto& [id,parent]:std::vector<std::pair<int,int>>{{1,5},{2,5},{3,6},{4,6},{5,7},{6,7},{7,9},{8,9},{9,-1}})
                cfg["shards"].push_back({{"id",id},{"parent",parent<0?json(nullptr):json(parent)}});
            auto leaf=node(1),rootOnly=node(8);bool mapped=true;
            for(uint64_t position=1;position<=100;++position) {
                const auto [co,b]=leaf->slotAtPosition(position);const uint64_t cycle=(position-1)/21,offset=(position-1)%21;
                mapped &= co==(offset==20?9:(offset%2?7:5)) && b==(offset==20?cycle+1:cycle*10+offset/2+1)
                           && leaf->streamPosition(co,b)==position;
                const auto [rootCo,rootBlock]=rootOnly->slotAtPosition(position);
                mapped &= rootCo==9 && rootBlock==position && rootOnly->streamPosition(rootCo,rootBlock)==position;
            }
            check(mapped,"four-layer ratio ten visits C1 M1 through C10 M10 then Root1, and root-only leaves never divide by zero");
            const auto [co,b]=rootOnly->slotAtPosition(UINT64_MAX);
            check(co==9 && b==UINT64_MAX && rootOnly->streamPosition(co,b)==UINT64_MAX,
                  "unbalanced root-only weighted mapping accepts the final uint64 position without overflow");
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_streaming",true},
                {"ancestor_order","lower_first"},{"ancestor_lower_blocks_per_root",2},{"ancestor_leaf_batch_blocks",2}});
            auto lower=node(5),root=node(7),leaf=node(1),peer=node(2),backup=node(1,1),synced=node(1,2),checkpointed=node(1,3);
            const auto child1=block(*lower,cfg,json::array({sharedRequest(cfg,"bundle:child:1",lowerPs,5,1)}));
            const auto child2=block(*lower,cfg,json::array({sharedRequest(cfg,"bundle:child:2",lowerPs,5,2)}));
            const auto child3=block(*lower,cfg,json::array({sharedRequest(cfg,"bundle:child:3",lowerPs,5,3)}));
            const auto child4=block(*lower,cfg,json::array({sharedRequest(cfg,"bundle:child:4",lowerPs,5,4)}));
            const auto root1=block(*root,cfg,json::array({sharedRequest(cfg,"bundle:root:1",rootPs,7,3)}));
            const auto initialState=leaf->state;const auto initialDigest=leaf->cachedStateDigest;
            remember(*leaf,child1);
            check(leaf->nextCstOrder().is_null() && scheduled(*leaf).is_null(),
                  "bundle waits for the second lower certificate instead of proposing half of a pair");
            remember(*leaf,child2);remember(*peer,child1);remember(*peer,child2);
            const auto selected=leaf->nextCstOrder();
            check(!selected.is_null() && selected.at("certificates")==json::array({child1,child2}) &&
                  selected.at("frontier")==json::array({child1,child2}) && !leaf->roundCertificates.at(1).count(7),
                  "bundle selects two consecutive lower business blocks with no Root certificate required");
            const auto pp=scheduled(*leaf);require(!pp.is_null(),"complete lower pair must produce one PBFT proposal");
            const auto value=pp.at("body").at("value");
            check(value.at("cst_orders").size()==2 && value.at("cst_frontier").size()==2 && backup->validValue(value),
                  "bundle primary and backup independently validate two genuine coordinator QCs in one leaf PBFT");
            auto forged=value;std::swap(forged["cst_orders"][0],forged["cst_orders"][1]);
            check(!backup->validValue(forged),"bundle rejects reversed business order despite authentic coordinator certificates");
            forged=value;forged["cst_orders"][1]["commits"]=json::array();
            check(!backup->validValue(forged),"bundle rejects a forged second business quorum");
            forged=value;forged["cst_orders"].erase(1);forged["cst_frontier"].erase(1);
            check(!backup->validValue(forged),"bundle rejects a half-pair frontier that omits the second mandatory position");
            leaf->handle(pp);votes(*leaf,cfg,pp);
            const auto peerPP=proposal(cfg,2,1,value);peer->handle(peerPP);votes(*peer,cfg,peerPP);
            check(leaf->applied==0 && peer->applied==0 && leaf->stagedRecords.count(Replica::cstKey(child1)) &&
                  !leaf->stagedRecords.count(Replica::cstKey(child2)),
                  "bundle committed PBFT stages its first business and leaves the second unstaged before witness one");
            const auto witness1=stagedWitness(cfg,child1,{leaf.get(),peer.get()});
            require(leaf->importWitness(witness1) && peer->importWitness(witness1),"both leaves must authenticate witness one");
            leaf->applyReady();peer->applyReady();
            check(leaf->state==initialState && leaf->cachedStateDigest==initialDigest && leaf->applied==0 &&
                  leaf->streamCursor()==0 && leaf->state.at("cst_batches").empty() && leaf->pendingLocalAckQcs.empty(),
                  "bundle witness one previews execution without mutating state cursor sequence digest or ACK queue");
            const auto& stagedSecond=leaf->stagedRecords.at(Replica::cstKey(child2));
            check(stagedSecond.at("reads").at("account:1:window-shared").at("version")==1 &&
                  stagedSecond.at("reads").at("account:1:window-shared").at("value")==2 &&
                  peer->stagedRecords.at(Replica::cstKey(child2)).at("reads").at("account:2:window-shared").at("version")==1,
                  "bundle second prepared records read the first business preview on both participants");
            check(leaf->leafCstOutcomeCache.size()==1 && peer->state.at("executed")==0 && peer->streamCursor()==0,
                  "bundle caches the first pure outcome while every participant awaits the second witness");
            const auto witness2=stagedWitness(cfg,child2,{leaf.get(),peer.get()});
            const auto witnesses=json::array({witness1,witness2});
            const auto leafCertificate=certificate(cfg,pp);
            const auto genesisCheckpoint=json{{"seq",0},{"state",synced->genesis()},{"proof",json::array()}};
            auto syncFields=json{{"stable",genesisCheckpoint},{"certificates",json::array({leafCertificate})},
                {"execution_witnesses",json::array({{{"seq",1},{"witnesses",witnesses}}})}};
            for(int variant=0;variant<3;++variant) {
                auto invalid=syncFields;
                auto& list=invalid["execution_witnesses"][0]["witnesses"];
                if(variant==0) std::swap(list[0],list[1]);
                if(variant==1) list.erase(1);
                if(variant==2) list.push_back(witness2);
                const auto rejectedBefore=synced->rejected;
                synced->handle(message(cfg,1,0,"SYNC",invalid));
                check(synced->rejected==rejectedBefore+1 && synced->applied==0 &&
                      synced->state==initialState && synced->certificates.empty() && synced->preparedProofs.empty(),
                      "bundle SYNC rejects invalid ordered witness array atomically: variant "+std::to_string(variant));
            }
            require(leaf->importWitness(witness2) && peer->importWitness(witness2),"both leaves must authenticate witness two");
            leaf->applyReady();peer->applyReady();
            check(leaf->applied==1 && peer->applied==1 && leaf->streamCursor()==2 && peer->streamCursor()==2 &&
                  leaf->state.at("executed")==2 && peer->state.at("executed")==2 && leaf->state.at("cst_batches").size()==2,
                  "bundle applies two transactions atomically in one sequence slot and advances the complete pair cursor");
            check(leaf->state.at("kv").at("account:1:window-shared").at("version")==2 &&
                  leaf->state.at("kv").at("account:1:window-shared").at("value")==5 &&
                  peer->state.at("kv").at("account:2:window-shared").at("version")==2 &&
                  peer->state.at("kv").at("account:2:window-shared").at("value")==5,
                  "bundle genuine participant witnesses serialize conflicting writes to version two and value five");
            check(leaf->leafBusinessSlots==1 && leaf->leafBusinessOrders==2 && leaf->leafBundledSlots==1 &&
                  leaf->pendingLocalAckQcs.size()==2 && leaf->slotWitnesses.at(1)==witnesses && leaf->leafCstOutcomeCache.empty(),
                  "bundle records one business PBFT two orders two ACK obligations and both execution witnesses");
            synced->handle(message(cfg,1,0,"SYNC",syncFields));
            check(synced->applied==1 && synced->streamCursor()==2 && synced->state==leaf->state &&
                  synced->cachedStateDigest==leaf->cachedStateDigest && synced->slotWitnesses.at(1)==witnesses,
                  "bundle SYNC replays both witnesses through the same ordered overlay and produces the identical state digest");
            checkpointed->leafCstOutcomeCache[Replica::cstKey(child1)]=json::object();
            checkpointed->installStable(stable(cfg,*leaf));
            check(checkpointed->applied==1 && checkpointed->streamCursor()==2 &&
                  checkpointed->leafCstOutcomeCache.empty() && checkpointed->cachedStateDigest==leaf->cachedStateDigest,
                  "bundle certified checkpoint clears stale local previews and restores the full pair state");
            remember(*leaf,child3);remember(*leaf,child4);
            check(leaf->nextCstOrder().is_null() && !leaf->validStreamBatchFrontier(json::array({child3,child4}),json::array({child3,child4})),
                  "bundle next lower pair cannot overtake the still missing Root one position");
            remember(*leaf,root1);const auto rootSelected=leaf->nextCstOrder();
            check(!rootSelected.is_null() && rootSelected.at("certificates")==json::array({root1}) &&
                  rootSelected.at("frontier")==json::array({root1}),
                  "bundle Root remains a single business block between consecutive lower pairs");
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_streaming",true},
                {"ancestor_order","lower_first"},{"ancestor_lower_blocks_per_root",2},{"ancestor_leaf_batch_blocks",2}});
            cfg["shards"]=json::array();
            for(const auto& [id,parent]:std::vector<std::pair<int,int>>{{1,5},{2,5},{3,6},{4,6},{5,7},{6,7},{7,9},{8,9},{9,-1}})
                cfg["shards"].push_back({{"id",id},{"parent",parent<0?json(nullptr):json(parent)}});
            auto leaf=node(1),rootOnly=node(8);bool mapped=true;
            for(uint64_t position=1;position<=100;++position) {
                const uint64_t cycle=(position-1)/5,offset=(position-1)%5;
                const auto [co,b]=leaf->slotAtPosition(position);
                mapped &= co==(offset==4?9:(offset<2?5:7)) && b==(offset==4?cycle+1:cycle*2+offset%2+1) &&
                    leaf->streamPosition(co,b)==position;
                const auto [rootCo,rootBlock]=rootOnly->slotAtPosition(position);
                mapped &= rootCo==9 && rootBlock==position && rootOnly->streamPosition(rootCo,rootBlock)==position;
            }
            check(mapped,"four-layer bundle mapping visits Child1 Child2 Middle1 Middle2 Root1 and preserves root-only leaves");
        }
        {
            cfg=input;cfg["consensus"].update({{"ordering_window_blocks",1},{"ancestor_streaming",true},
                {"ancestor_order","lower_first"},{"ancestor_lower_blocks_per_root",2},{"ancestor_leaf_batch_blocks",2}});
            auto lower=node(5),root=node(7),leaf=node(1);
            const auto child1=block(*lower,cfg,json::array({request(cfg,"bundle:prefix:initial1",lowerPs,5)}));
            const auto child2=block(*lower,cfg,json::array({request(cfg,"bundle:prefix:initial2",lowerPs,5)}));
            // Selection fixture starts at a consumed pair; genuine atomic
            // execution of that pair is exercised immediately above.
            markExecuted(*leaf,child1);markExecuted(*leaf,child2);
            leaf->applyStreamFrontier(json::array({child1,child2}));
            for(int b=3;b<=22;++b) remember(*leaf,block(*lower,cfg));
            for(int b=1;b<=11;++b) remember(*leaf,block(*root,cfg));
            const auto business1=block(*lower,cfg,json::array({request(cfg,"bundle:prefix:business1",lowerPs,5)}));
            const auto business2=block(*lower,cfg,json::array({request(cfg,"bundle:prefix:business2",lowerPs,5)}));
            remember(*leaf,business1);remember(*leaf,business2);
            const auto progress=leaf->nextCstOrder();
            check(!progress.is_null() && progress.at("certificate").is_null() &&
                  progress.at("certificates").empty() && progress.at("frontier").size()==31 &&
                  progress.at("frontier").back().at("proposal").at("body").at("shard")==7,
                  "bundle 31 empty-prefix proofs produce a control slot before a pair would exceed the 32-proof bound");
            const auto controlPP=scheduled(*leaf);require(!controlPP.is_null(),"bounded prefix must produce real control PBFT");
            check(controlPP.at("body").at("value").value("cst_stream_progress",false) &&
                  !controlPP.at("body").at("value").contains("cst_orders") && leaf->validValue(controlPP.at("body").at("value")),
                  "bundle shorter empty-prefix control is independently valid and carries no fake business");
            leaf->handle(controlPP);votes(*leaf,cfg,controlPP);
            check(leaf->applied==1 && leaf->streamCursor()==33 && leaf->leafControlSlots==1 && leaf->state.at("executed")==0,
                  "bundle real control PBFT consumes exactly 31 empty positions without splitting or executing the pending pair");
            const auto selected=leaf->nextCstOrder();
            check(!selected.is_null() && selected.at("certificates")==json::array({business1,business2}) &&
                  selected.at("frontier").size()==2 &&
                  leaf->validStreamBatchFrontier(selected.at("frontier"),selected.at("certificates")),
                  "bundle after bounded prefix selects the entire next pair in two proofs");
        }
        return {{"checks",checks},{"network_started",false},{"m_values",json::array({1,2,4,12})},
                {"streaming_m_values",json::array({1,4})},{"ancestor_orders",json::array({"root_first","lower_first"})}};
    }
};

int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: test_ordering_windows CONFIG OUTPUT_DIRECTORY");
        std::cout<<OrderingWindowsTest::run(readJson(argv[1]),argv[2]).dump()<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
