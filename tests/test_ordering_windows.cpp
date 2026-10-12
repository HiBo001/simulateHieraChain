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
        return {{"checks",checks},{"network_started",false},{"m_values",json::array({1,2,4,12})}};
    }
};

int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: test_ordering_windows CONFIG OUTPUT_DIRECTORY");
        std::cout<<OrderingWindowsTest::run(readJson(argv[1]),argv[2]).dump()<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
