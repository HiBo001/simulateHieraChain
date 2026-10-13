// In-memory protocol checks use real client signatures and 3-replica proofs.
// Network::start is never called, so these checks inject no faults into a run.
#define ARBOR_SAGUARO 1
#define ARBOR_BASELINE_POLICY_TESTS 1
#define main baseline_node_program_main
#include "../source/main.cpp"
#undef main

struct BaselinePolicyTest {
#ifdef ARBOR_AHL
    static constexpr bool domainPolicy=false;
#else
    static constexpr bool domainPolicy=true;
#endif
    static void require(bool condition,const std::string& message) {
        if(!condition) throw std::runtime_error(message);
    }
    static Key replicaKey(const json& cfg,int shard,int replica) {
        for(const auto& node:cfg.at("nodes"))
            if(node.at("shard")==shard && node.at("replica")==replica)
                return readKey(node.at("private_key"),true);
        throw std::runtime_error("missing replica test key");
    }
    static std::string folder(const std::string& root,const std::string& name) {
        auto path=root+"/"+name;std::filesystem::create_directories(path);return path;
    }
    static json request(const json& cfg,const std::string& label,const std::vector<int>& ps,
                        const std::string& keyLabel="",int target=5) {
        json accesses=json::array();const auto& suffix=keyLabel.empty()?label:keyLabel;
        for(int owner:ps) accesses.push_back({{"shard",owner},{"key","account:"+std::to_string(owner)+":"+suffix},
                                            {"value",uint64_t(11+owner)}});
        json tx={{"id","unit:"+label+":tx"},{"key",accesses[0].at("key")},{"value",accesses[0].at("value")},
                 {"participants",ps},{"accesses",accesses}};
        json body={{"type","CLIENT"},{"run",cfg.at("run_id")},{"id","unit:"+label+":request"},
                   {"target",target},{"reply",{{"host","127.0.0.1"},{"port",30000}}},{"txs",json::array({tx})}};
        return sign(body,readKey(cfg.at("client_private_key"),true));
    }
    static json initValue(const json& request,int sequence) {
        return {{"requests",json::array({request})},{"sag_action","INIT"},{"batch","5:"+std::to_string(sequence)}};
    }
    static json proof(const json& cfg,const json& record) {
        json votes=json::array();int owner=record.at("origin");
        for(int signer=0;signer<3;++signer) {
            json body={{"type","SAG_ATTEST"},{"run",cfg.at("run_id")},{"shard",owner},{"from",signer},{"view",0},
                       {"phase",record.at("phase")},{"batch",record.at("batch")},{"record_digest",hash(record.dump())}};
            votes.push_back(sign(body,replicaKey(cfg,owner,signer)));
        }
        return {{"record",record},{"votes",votes}};
    }
    static json prepareValue(const json& cfg,const json& init) {
        return {{"requests",json::array()},{"sag_action","PREPARE"},{"batch",init.at("batch")},{"init",proof(cfg,init)}};
    }
    static json decideValue(const json& cfg,const json& init,const std::vector<json>& records) {
        json proofs=json::array(),votes=json::array();bool yes=true;
        for(const auto& record:records) {proofs.push_back(proof(cfg,record));votes.push_back(record);if(record.at("vote")!="YES") yes=false;}
        json decision={{"phase","DECISION"},{"batch",init.at("batch")},{"coordinator",5},{"origin",5},
                       {"participants",init.at("participants")},{"init_digest",hash(init.dump())},
                       {"decision",yes?"COMMIT":"ABORT"},{"records",votes}};
        return {{"requests",json::array()},{"sag_action","DECIDE"},{"batch",init.at("batch")},
                {"votes",proofs},{"decision",decision}};
    }
    static json finishValue(const json& cfg,const json& decision) {
        return {{"requests",json::array()},{"sag_action","FINISH"},{"batch",decision.at("batch")},
                {"decision",proof(cfg,decision)}};
    }
    static void apply(Replica& replica,const json& value,int seq,const std::string& context) {
        require(replica.saguaroValidValue(value),context+": genuine value must pass production PBFT validity");
        replica.saguaroApply(value,json::object(),seq);
    }
    static void coordinatorChecks(const json& cfg,const std::string& directory,json& checks) {
        auto check=[&](bool condition,const std::string& name){require(condition,name);checks.push_back(name);};
        Replica coordinator(cfg,5,0,folder(directory,"coordinator"));
        auto first=request(cfg,"coord-first",{1,2}),same=request(cfg,"coord-same",{1,2});
        auto one=request(cfg,"coord-one",{1,3}),two=request(cfg,"coord-two",{1,2,3});
        auto firstValue=initValue(first,1);apply(coordinator,firstValue,1,"first coordinator INIT");
        const auto init=coordinator.state.at("sag_coordinations").at("5:1").at("init");
        check(coordinator.members.clientMessage(first) && coordinator.sagValidProof(proof(cfg,init),"INIT"),
              "fixtures carry authentic client signatures and three distinct replica signatures");
        check(coordinator.saguaroValidValue(initValue(same,2))==!domainPolicy,
              "same two participants with disjoint keys follow the method-specific coordinator policy");
        check(coordinator.sagAdmissible(same.at("body").at("id"),same,Clock::now())==!domainPolicy,
              "coordinator admission uses the same participant policy as PBFT validity");
        check(coordinator.saguaroValidValue(initValue(one,3)),
              "one shared participant permits independent disjoint-key coordinator work");
        check(coordinator.saguaroValidValue(initValue(two,4))==!domainPolicy,
              "two shared participants are detected even when participant sets have different sizes");
        check(coordinator.sagDomainAdmissible(json::array({first}),false,"5:1"),
              "coordinator domain comparison exempts its own existing batch");
        check(!coordinator.saguaroValidValue(firstValue),"duplicate INIT for the same coordinator batch is rejected");

        Replica restored(cfg,5,3,folder(directory,"coordinator-restored"));restored.state=coordinator.state;
        restored.saguaroRestore();
        check(restored.saguaroValidValue(initValue(same,2))==!domainPolicy,
              "restored replicated INIT state preserves method-specific coordinator admission");
        coordinator.sagReceivedAcks.clear();coordinator.sagReceivedVotes.clear();
        coordinator.sagCompletionNeeded.insert("unrelated-local-completion");
        check(coordinator.saguaroValidValue(initValue(same,2))==restored.saguaroValidValue(initValue(same,2)),
              "asynchronous ACK and completion indexes cannot change coordinator PBFT validity");

        std::vector<json> records;
        for(int owner:{1,2}) {
            Replica leaf(cfg,owner,0,folder(directory,"vote-"+std::to_string(owner)));
            records.push_back(leaf.sagVote(init));
        }
        auto decision=decideValue(cfg,init,records);apply(coordinator,decision,2,"coordinator DECIDE");
        check(coordinator.saguaroValidValue(initValue(same,3)),
              "coordinator participant gate releases at DECIDE before participant ACKs arrive");
        if(domainPolicy) check(coordinator.sagAdmissible(same.at("body").at("id"),same,Clock::now()),
              "Saguaro admits disjoint keys after DECIDE while the old batch still awaits ACKs");
        check(coordinator.sagActive.count("5:1") && !coordinator.sagFinished.count("5:1") &&
              coordinator.completedCstTransactions==0 && coordinator.completedTxResults.empty(),
              "DECIDE alone neither completes the batch nor credits executed client transactions");
        restored.state=coordinator.state;restored.saguaroRestore();
        check(restored.saguaroValidValue(initValue(same,3)),
              "recovery does not resurrect a participant gate already released by replicated DECIDE");
        check(!coordinator.saguaroValidValue(decision),"a decided coordinator batch cannot accept a second decision");
    }
    static void participantChecks(const json& cfg,const std::string& directory,json& checks) {
        auto check=[&](bool condition,const std::string& name){require(condition,name);checks.push_back(name);};
        Replica coordinator(cfg,5,0,folder(directory,"coord"));Replica leaf(cfg,1,0,folder(directory,"leaf"));
        auto first=request(cfg,"leaf-first",{1,2}),same=request(cfg,"leaf-same",{1,2});
        auto one=request(cfg,"leaf-one",{1,3}),two=request(cfg,"leaf-two",{1,2,3});
        auto init=coordinator.sagInit("5:1",json::array({first}));
        auto sameInit=coordinator.sagInit("5:2",json::array({same}));
        auto prepare=prepareValue(cfg,init);apply(leaf,prepare,1,"participant PREPARE");
        check(leaf.state.at("kv").empty() && leaf.state.at("executed")==0 && !leaf.state.at("sag_locks").empty(),
              "participant PREPARE takes key locks without changing application state or executed count");
        check(leaf.saguaroValidValue(prepareValue(cfg,sameInit))==!domainPolicy,
              "same two participants with disjoint keys follow the method-specific participant policy");
        check(leaf.saguaroValidValue(prepareValue(cfg,coordinator.sagInit("5:3",json::array({one})))),
              "one shared participant permits independent disjoint-key PREPARE");
        check(leaf.saguaroValidValue(prepareValue(cfg,coordinator.sagInit("5:4",json::array({two}))))==!domainPolicy,
              "participant gate detects two-domain overlap across differently sized requests");
        check(!leaf.saguaroValidValue(prepare),"duplicate participant PREPARE is rejected without voting twice");
        check(leaf.sagDomainAdmissible(json::array({first}),true,"5:1"),
              "participant domain comparison exempts its own existing batch");

        Replica restored(cfg,1,3,folder(directory,"leaf-restored"));restored.state=leaf.state;restored.saguaroRestore();
        leaf.sagPrepareQueue.clear();leaf.sagFinishQueue.clear();leaf.sagReceivedAcks.clear();
        check(restored.saguaroValidValue(prepareValue(cfg,sameInit))==leaf.saguaroValidValue(prepareValue(cfg,sameInit)),
              "participant PBFT validity is unchanged by local delivery queues and restored state");
        auto shared=request(cfg,"leaf-shared",{1,2},"leaf-first");
        auto sharedInit=coordinator.sagInit("5:5",json::array({shared}));
        check(leaf.sagVote(sharedInit).at("vote")=="NO" && leaf.sagConflict(json::array({shared})),
              "key-level 2PL rejects an overlapping write while an earlier participant owns its lock");
        auto owningVote=leaf.sagVote(init);
        check(owningVote.at("vote")=="YES", "a batch's own existing key locks are not treated as another transaction's conflict");

        Replica second(cfg,2,0,folder(directory,"second-leaf"));
        auto decision=decideValue(cfg,init,{leaf.state.at("sag_preparations").at("5:1").at("vote"),second.sagVote(init)});
        auto finish=finishValue(cfg,decision.at("decision"));
        auto forged=finish;forged["decision"]["votes"][2]=forged["decision"]["votes"][1];
        check(!leaf.saguaroValidValue(forged),"duplicate replica voters cannot certify participant execution");
        forged=finish;forged["decision"]["record"]["decision"]="ABORT";
        check(!leaf.saguaroValidValue(forged),"a changed decision body cannot reuse authenticated COMMIT signatures");
        apply(leaf,finish,2,"participant FINISH");
        check(leaf.state.at("executed")==1 && leaf.state.at("kv").size()==1 && leaf.state.at("sag_locks").empty(),
              "only certified participant FINISH commits application state and releases key locks");
        check(leaf.saguaroValidValue(prepareValue(cfg,sameInit)),
              "participant domain gate releases after FINISH even before coordinator receives ACK");
        check(leaf.state.at("sag_preparations").at("5:1").contains("ack") && !leaf.saguaroValidValue(finish),
              "replicated ACK tombstone rejects repeated FINISH and prevents duplicate application");
        restored.state=leaf.state;restored.saguaroRestore();
        check(restored.saguaroValidValue(prepareValue(cfg,sameInit)) && !restored.saguaroValidValue(finish),
              "restoration preserves FINISH release and the exactly-once execution tombstone");
    }
    static void keyAdmissionChecks(const json& cfg,const std::string& directory,json& checks) {
        Replica coordinator(cfg,5,0,folder(directory,"key-admission"));
        auto first=request(cfg,"key-first",{1,2}),shared=request(cfg,"key-second",{1,2},"key-first");
        apply(coordinator,initValue(first,1),1,"key admission INIT");
        require(!coordinator.sagAdmissible(shared.at("body").at("id"),shared,Clock::now()),
                "shared-key coordinator work must wait for the existing batch");
        checks.push_back("shared-key coordinator admission remains blocked under both method policies");
    }
    static void timeoutAbortChecks(const json& cfg,const std::string& directory,json& checks) {
        auto check=[&](bool condition,const std::string& name){require(condition,name);checks.push_back(name);};
        Replica coordinator(cfg,5,0,folder(directory,"coord"));
        auto req=request(cfg,"timeout",{1,2});auto initial=initValue(req,1);
        apply(coordinator,initial,1,"timeout fixture INIT");
        auto init=coordinator.state.at("sag_coordinations").at("5:1").at("init");
        json record={{"phase","DECISION"},{"batch","5:1"},{"coordinator",5},{"origin",5},
                     {"participants",init.at("participants")},{"init_digest",hash(init.dump())},
                     {"decision","ABORT"},{"records",json::array()},{"timeout_abort",true},{"init",init}};
        json abort={{"requests",json::array()},{"sag_action","DECIDE"},{"batch","5:1"},
                    {"votes",json::array()},{"decision",record}};
        check(coordinator.saguaroValidValue(abort)==domainPolicy,
              "only Saguaro permits a PBFT timeout ABORT without all participant votes");
        auto invalid=abort;invalid["decision"]["decision"]="COMMIT";
        check(!coordinator.saguaroValidValue(invalid),
              "timeout marker can never turn a missing-vote decision into COMMIT");
        invalid=abort;invalid["decision"].erase("timeout_abort");invalid["decision"].erase("init");
        invalid["decision"]["decision"]="COMMIT";
        check(!coordinator.saguaroValidValue(invalid),
              "ordinary COMMIT still requires every participant's authentic vote");
        invalid=abort;invalid["decision"]["init"]["requests"][0]["signature"]=std::string(128,'0');
        invalid["decision"]["init_digest"]=hash(invalid["decision"]["init"].dump());
        check(!coordinator.saguaroValidValue(invalid),
              "timeout ABORT cannot bind an INIT with a forged client signature");
        if(!domainPolicy) {
            Replica leaf(cfg,1,0,folder(directory,"ahl-unprepared"));
            check(!leaf.saguaroValidValue(finishValue(cfg,record)),
                  "AHL cannot inherit Saguaro's unprepared-participant timeout FINISH path");
            return;
        }

        apply(coordinator,abort,2,"timeout coordinator DECIDE");
        check(coordinator.completedCstTransactions==0 && coordinator.completedTxResults.empty(),
              "timeout ABORT never credits a transaction as executed");
        Replica prepared(cfg,1,0,folder(directory,"prepared"));
        Replica unprepared(cfg,2,0,folder(directory,"unprepared"));
        auto prepare=prepareValue(cfg,init);apply(prepared,prepare,1,"timeout participant PREPARE");
        auto finish=finishValue(cfg,record);
        auto malformedFinish=finish;malformedFinish["decision"]["votes"][2]=malformedFinish["decision"]["votes"][0];
        check(!unprepared.saguaroValidValue(malformedFinish),
              "an unprepared participant still requires three distinct authentic ABORT signers");
        auto wrong=record;wrong["init"]=coordinator.sagInit("5:1",json::array({request(cfg,"other-init",{1,2})}));
        check(!unprepared.saguaroValidValue(finishValue(cfg,wrong)),
              "signed timeout ABORT with mismatched INIT digest is rejected");
        auto fakeCommit=record;fakeCommit["decision"]="COMMIT";
        check(!unprepared.saguaroValidValue(finishValue(cfg,fakeCommit)),
              "even three signatures cannot authorize an unprepared timeout COMMIT");

        apply(prepared,finish,2,"prepared participant timeout FINISH");
        apply(unprepared,finish,1,"unprepared participant timeout FINISH");
        for(auto* leaf:{&prepared,&unprepared}) {
            check(leaf->state.at("kv").empty() && leaf->state.at("executed")==0 && leaf->state.at("sag_locks").empty(),
                  std::string(leaf==&prepared?"prepared":"unprepared")+" timeout FINISH releases all locks without applying writes");
            auto ack=leaf->state.at("sag_preparations").at("5:1").at("ack");
            check(ack.at("decision")=="ABORT" && ack.at("results").empty() &&
                  coordinator.sagValidAckFor(ack,coordinator.state.at("sag_coordinations").at("5:1")),
                  std::string(leaf==&prepared?"prepared":"unprepared")+" participant persists a correctly bound empty ABORT ACK");
            check(!leaf->saguaroValidValue(prepare) && !leaf->saguaroValidValue(finish),
                  std::string(leaf==&prepared?"prepared":"unprepared")+" ABORT tombstone prevents late PREPARE and duplicate FINISH");
        }
        Replica restored(cfg,2,3,folder(directory,"restored-unprepared"));restored.state=unprepared.state;
        restored.saguaroRestore();
        check(!restored.saguaroValidValue(prepare) && !restored.saguaroValidValue(finish) && restored.sagParticipantActive.empty(),
              "restoring an unprepared ABORT tombstone cannot reactivate work or execute a late INIT");

        Replica fresh1(cfg,1,0,folder(directory,"late-vote-one"));Replica fresh2(cfg,2,0,folder(directory,"late-vote-two"));
        auto lateCommit=decideValue(cfg,init,{fresh1.sagVote(init),fresh2.sagVote(init)});
        check(!coordinator.saguaroValidValue(lateCommit),
              "a fully voted late COMMIT cannot overwrite the coordinator's replicated timeout ABORT");
    }
    static json run(const json& cfg,const std::string& directory) {
        json checks=json::array();
        coordinatorChecks(cfg,folder(directory,"coordinator-checks"),checks);
        participantChecks(cfg,folder(directory,"participant-checks"),checks);
        keyAdmissionChecks(cfg,folder(directory,"key-checks"),checks);
        timeoutAbortChecks(cfg,folder(directory,"timeout-checks"),checks);
        return {{"checks",checks},{"network_started",false},{"method",domainPolicy?"saguaro":"ahl"},
                {"domain_policy",domainPolicy}};
    }
    static json crossNcaChecks(const json& cfg,const std::string& directory) {
        require(domainPolicy,"cross-NCA fixture is only valid for Saguaro");
        json checks=json::array();
        auto check=[&](bool condition,const std::string& name){require(condition,name);checks.push_back(name);};
        Replica child(cfg,5,0,folder(directory,"child-coordinator"));
        Replica root(cfg,7,0,folder(directory,"root-coordinator"));
        Replica leaf1(cfg,1,0,folder(directory,"leaf1")),leaf2(cfg,2,0,folder(directory,"leaf2"));
        auto reqA=request(cfg,"inversion-a",{1,2});
        auto reqB=request(cfg,"inversion-b",{1,2,3},"",7);
        auto a=initValue(reqA,1),b=initValue(reqB,1);b["batch"]="7:1";
        apply(child,a,1,"child coordinator inversion INIT");apply(root,b,1,"root coordinator inversion INIT");
        auto initA=child.state.at("sag_coordinations").at("5:1").at("init");
        auto initB=root.state.at("sag_coordinations").at("7:1").at("init");
        auto prepareA=prepareValue(cfg,initA),prepareB=prepareValue(cfg,initB);
        apply(leaf1,prepareA,1,"leaf1 accepts child first");apply(leaf2,prepareB,1,"leaf2 accepts root first");
        check(!leaf1.saguaroValidValue(prepareB) && !leaf2.saguaroValidValue(prepareA),
              "opposite PREPARE arrival at two leaves models a real cross-NCA domain-order inversion");
        check(leaf1.sagVote(initB).at("vote")=="YES" && leaf2.sagVote(initA).at("vote")=="YES",
              "cross-NCA inversion arises from participant ordering even with disjoint application keys");
        auto start=Clock::now();child.sagDecisionStarted["5:1"]=start;
        int delay=child.timeoutMs/2+int((unsigned(child.shard)*131u)%unsigned(std::max(1,child.timeoutMs/4)));
        check(!child.sagDecisionExpired("5:1",start+std::chrono::milliseconds(delay-1)) &&
              child.sagDecisionExpired("5:1",start+std::chrono::milliseconds(delay)),
              "coordinator transaction deadline is half the view timeout plus domain-specific jitter");
        child.sagDecisionStarted["5:1"]=Clock::now()-std::chrono::milliseconds(delay+1);child.applied=1;
        child.lastForwardHeartbeat=Clock::now();child.saguaroPropose();json proposal=nullptr;
        while(!child.inbox.empty()) {
            auto envelope=child.inbox.front();child.inbox.pop();
            if(envelope.at("body").at("type")=="PREPREPARE") proposal=envelope;
        }
        check(!proposal.is_null() && child.members.replicaMessage(proposal),
              "expired inverted transaction produces a genuinely signed local PBFT proposal");
        auto abort=proposal.at("body").at("value");
        check(abort.at("sag_action")=="DECIDE" && abort.at("votes").empty() &&
              abort.at("decision").at("decision")=="ABORT" && abort.at("decision").at("timeout_abort")==true,
              "missing cross-NCA votes schedule ABORT rather than inventing a COMMIT");
        apply(child,abort,2,"cross-NCA timeout DECIDE");
        auto finish=finishValue(cfg,abort.at("decision"));
        apply(leaf1,finish,2,"prepared cross-NCA ABORT FINISH");
        apply(leaf2,finish,2,"unprepared cross-NCA ABORT FINISH");
        check(leaf1.saguaroValidValue(prepareB) && !leaf1.saguaroValidValue(prepareA) && !leaf2.saguaroValidValue(prepareA),
              "certified ABORT unblocks the other coordinator and prevents the abandoned batch's late PREPARE");
        check(leaf1.state.at("executed")==0 && leaf2.state.at("executed")==0 && leaf1.state.at("kv").empty() && leaf2.state.at("kv").empty(),
              "breaking cross-NCA inversion does not execute either transaction prematurely");
        apply(leaf1,prepareB,3,"formerly blocked root PREPARE");
        check(leaf1.state.at("sag_preparations").at("7:1").at("vote").at("vote")=="YES",
              "formerly blocked root transaction resumes with a valid YES vote after ABORT release");
        return {{"checks",checks},{"network_started",false},{"method","saguaro"},{"domain_policy",true}};
    }
};

int main(int argc,char** argv) {
    try {
        if(argc!=3 && argc!=4) throw std::runtime_error("usage: test_baseline_policies CONFIG OUTPUT_DIRECTORY [cross-nca]");
        auto report=argc==4?BaselinePolicyTest::crossNcaChecks(readJson(argv[1]),argv[2]):BaselinePolicyTest::run(readJson(argv[1]),argv[2]);
        std::cout<<report.dump()<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
