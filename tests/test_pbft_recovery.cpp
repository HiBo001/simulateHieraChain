// Exercise the shared PBFT recovery path with real Ed25519 messages, without
// starting a listener, worker, client, or cluster process.
#define ARBOR_SAGUARO 1
#define ARBOR_PBFT_RECOVERY_TESTS 1
#define main saguaro_node_program_main
#include "../source/main.cpp"
#undef main

struct PbftRecoveryTest {
    static void require(bool condition,const std::string& message) {
        if(!condition) throw std::runtime_error(message);
    }
    static Key replicaKey(const json& cfg,int shard,int replica) {
        for(const auto& node:cfg.at("nodes"))
            if(node.at("shard")==shard && node.at("replica")==replica)
                return readKey(node.at("private_key"),true);
        throw std::runtime_error("missing test replica key");
    }
    static json message(const json& cfg,int replica,const std::string& type,json fields,int view=0) {
        fields["type"]=type;fields["run"]=cfg.at("run_id");fields["shard"]=1;
        fields["from"]=replica;fields["view"]=view;
        return sign(fields,replicaKey(cfg,1,replica));
    }
    static json proposal(const json& cfg,int seq,int view=0) {
        json value={{"requests",json::array()}};
        return message(cfg,view%4,"PREPREPARE",{{"seq",seq},{"digest",hash(value.dump())},{"value",value}},view);
    }
    static json prepared(const json& cfg,int seq,int view=0) {
        auto pp=proposal(cfg,seq,view);json votes=json::array();
        for(int voter=0;voter<4 && votes.size()<2;++voter) if(voter!=view%4)
            votes.push_back(message(cfg,voter,"PREPARE",{{"seq",seq},{"digest",pp.at("body").at("digest")}},view));
        return {{"proposal",pp},{"prepares",votes}};
    }
    static json viewChange(Replica& replica,const json& cfg,int voter,int view,json proofs=json::array()) {
        return message(cfg,voter,"VIEW_CHANGE",{{"stable",{{"seq",0},{"state",replica.genesis()},{"proof",json::array()}}},
            {"prepared",proofs}},view);
    }
    static json newView(Replica& replica,const json& cfg,int view) {
        json vcs=json::array();vcs.push_back(viewChange(replica,cfg,view%4,view));
        for(int voter=0;voter<4 && vcs.size()<3;++voter) if(voter!=view%4)
            vcs.push_back(viewChange(replica,cfg,voter,view));
        return message(cfg,view%4,"NEW_VIEW",{{"changes",vcs},{"proposals",json::array()}},view);
    }
    static void viewChangeCacheChecks(const json& cfg,const std::string& directory,json& checks) {
        Replica replica(cfg,1,3,directory);
        auto check=[&](bool condition,const std::string& name) {require(condition,name);checks.push_back(name);};
        auto old=Clock::now()-std::chrono::hours(1);replica.lastProgress=old;
        auto original=viewChange(replica,cfg,0,1);
        auto before=replica.pbftVcValidations;
        replica.handle(original);
        check(replica.pbftVcValidations>before && replica.viewChanges.at(1).at(0)==original,
              "first authentic VIEW_CHANGE is deeply validated and stored");
        check(replica.view==0 && replica.targetView==0 && !replica.changing && replica.lastProgress==old,
              "one authenticated voter cannot rotate the view or renew the progress deadline");
        auto validated=replica.pbftVcValidations;auto replays=replica.pbftVcExactReplays;
        for(int i=0;i<20;++i) replica.handle(original);
        check(replica.pbftVcValidations==validated && replica.pbftVcExactReplays==replays+20,
              "exact stored VIEW_CHANGE replay avoids repeated deep validation");
        check(replica.viewChanges.at(1).size()==1 && replica.viewChanges.at(1).at(0)==original && replica.lastProgress==old,
              "exact VIEW_CHANGE retries preserve the first evidence and its deadline");

        auto tampered=original;tampered["body"]["prepared"]=json::array({prepared(cfg,1)});
        auto replayCount=replica.pbftVcExactReplays;
        replica.handle(tampered);
        check(replica.pbftVcExactReplays==replayCount && replica.viewChanges.at(1).at(0)==original,
              "a changed signed body cannot use the exact-envelope replay shortcut");

        auto numericTamper=original;numericTamper["body"]["stable"]["state"]["executed"]=0.0;
        check(numericTamper==original && numericTamper.dump()!=original.dump() && !replica.members.replicaMessage(numericTamper),
              "numeric representation fixture is structurally equal but changes the authenticated serialized body");
        auto rejectedBefore=replica.rejected;replica.handle(numericTamper);
        check(replica.pbftVcExactReplays==replayCount && replica.rejected>rejectedBefore &&
              replica.viewChanges.at(1).at(0).dump()==original.dump(),
              "integer-to-float body tampering cannot reuse the exact-envelope authentication cache");

        auto badSignature=original;badSignature["signature"]=std::string(128,'0');
        rejectedBefore=replica.rejected;replica.handle(badSignature);
        check(replica.pbftVcExactReplays==replayCount && replica.rejected>rejectedBefore &&
              replica.viewChanges.at(1).at(0).dump()==original.dump(),
              "a changed signature cannot reuse a previously authenticated VIEW_CHANGE fingerprint");

        auto withExtra=original;withExtra["unsigned_test_metadata"]="changed envelope";
        validated=replica.pbftVcValidations;replica.handle(withExtra);
        check(replica.pbftVcValidations>validated && replica.pbftVcExactReplays==replayCount &&
              replica.viewChanges.at(1).at(0).dump()==original.dump(),
              "changed unsigned envelope fields use normal authentication and preserve the first stored vote");

        auto invalidStable=original.at("body");invalidStable["stable"]["state"]["executed"]=1;
        auto signedBadStable=sign(invalidStable,replicaKey(cfg,1,0));
        validated=replica.pbftVcValidations;replica.handle(signedBadStable);
        check(replica.pbftVcValidations>validated && replica.pbftVcExactReplays==replayCount &&
              replica.viewChanges.at(1).at(0)==original,
              "re-signed invalid checkpoint evidence is validated and cannot replace cached VIEW_CHANGE");

        auto invalidPrepared=original.at("body");auto proof=prepared(cfg,1);
        proof["prepares"][1]=proof.at("prepares")[0];invalidPrepared["prepared"]=json::array({proof});
        auto signedBadPrepared=sign(invalidPrepared,replicaKey(cfg,1,0));
        validated=replica.pbftVcValidations;replica.handle(signedBadPrepared);
        check(replica.pbftVcValidations>validated && replica.pbftVcExactReplays==replayCount &&
              replica.viewChanges.at(1).at(0)==original,
              "re-signed duplicated PREPARE voters cannot bypass VIEW_CHANGE proof validation");

        auto alternate=viewChange(replica,cfg,0,1,json::array({prepared(cfg,2)}));
        validated=replica.pbftVcValidations;replica.handle(alternate);
        check(replica.pbftVcValidations>validated && replica.pbftVcExactReplays==replayCount &&
              replica.viewChanges.at(1).at(0)==original,
              "different authentic evidence from the same voter is checked but does not replace the first vote");
        check(replica.lastProgress==old && replica.slots.empty() && replica.preparedHistory.empty() &&
              replica.lastNewView.is_null() && replica.pbftNewViewBuilds==0,
              "invalid or alternative VIEW_CHANGE evidence cannot create work or renew progress");
    }
    static void immutableNewViewChecks(const json& cfg,const std::string& directory,json& checks) {
        Replica primary(cfg,1,1,directory);
        auto check=[&](bool condition,const std::string& name) {require(condition,name);checks.push_back(name);};
        auto agreed=prepared(cfg,1);
        check(primary.validPrepared(agreed),"new-view fixture has a genuine two-backup prepared certificate");
        primary.handle(viewChange(primary,cfg,0,1,json::array({agreed})));
        primary.handle(viewChange(primary,cfg,1,1));
        check(primary.lastNewView.is_null() && primary.pbftNewViewBuilds==0,
              "two distinct VIEW_CHANGE voters cannot create NEW_VIEW");
        primary.handle(viewChange(primary,cfg,2,1));
        auto candidate=primary.lastNewView;
        check(!candidate.is_null() && primary.pbftNewViewBuilds==1 && primary.members.replicaMessage(candidate),
              "three distinct authentic VIEW_CHANGE voters build one signed NEW_VIEW");
        std::set<int> voters;
        for(const auto& vc:candidate.at("body").at("changes")) {
            check(primary.validVC(vc,1),"NEW_VIEW carries authentic stable and prepared recovery evidence");
            voters.insert(vc.at("body").at("from").get<int>());
        }
        check(voters.size()==3 && voters.count(1) && candidate.at("body").at("changes").size()==3,
              "NEW_VIEW uses exactly three distinct voters including its own primary");
        auto recovery=primary.recovery(candidate.at("body").at("changes"),1);
        check(candidate.at("body").at("proposals").size()==1 && recovery.second.at(1)==agreed.at("proposal").at("body").at("value") &&
              candidate.at("body").at("proposals")[0].at("body").at("value")==recovery.second.at(1),
              "NEW_VIEW's signed proposal exactly matches canonical prepared recovery");
        check(primary.view==0 && primary.targetView==1 && primary.changing,
              "constructed NEW_VIEW remains pending until the primary installs its own message");

        auto progress=primary.lastProgress;auto builds=primary.pbftNewViewBuilds;
        auto fourth=viewChange(primary,cfg,3,1,json::array({prepared(cfg,2)}));
        check(primary.validVC(fourth,1),"fourth VIEW_CHANGE carries independently authentic future-slot evidence");
        primary.handle(fourth);
        check(primary.viewChanges.at(1).size()==4 && primary.lastNewView==candidate && primary.pbftNewViewBuilds==builds,
              "a fourth valid VIEW_CHANGE cannot rewrite the already signed NEW_VIEW candidate");
        auto validations=primary.pbftVcValidations;
        for(int i=0;i<20;++i) primary.maybeNewView(1);
        check(primary.lastNewView==candidate && primary.pbftNewViewBuilds==builds && primary.pbftVcValidations==validations &&
              primary.lastProgress==progress,
              "repeated quorum checks neither rebuild immutable NEW_VIEW nor refresh progress");

        auto retryStart=primary.pbftLastNewViewRetry;auto retries=primary.pbftNewViewRetries;auto queued=primary.inbox.size();
        primary.retryPbftNewView(retryStart+std::chrono::milliseconds(249));
        check(primary.pbftNewViewRetries==retries && primary.inbox.size()==queued,
              "pending NEW_VIEW retry is throttled until 250 milliseconds");
        primary.retryPbftNewView(retryStart+std::chrono::milliseconds(250));
        check(primary.pbftNewViewRetries==retries+1 && primary.inbox.size()==queued+1 && primary.inbox.back()==candidate,
              "pending NEW_VIEW retransmits the identical signed candidate before local installation");
        primary.retryPbftNewView(retryStart+std::chrono::milliseconds(250));
        check(primary.pbftNewViewRetries==retries+1 && primary.lastNewView==candidate && primary.lastProgress==progress,
              "duplicate retry tick does not send again or renew the view-change deadline");

        primary.handle(candidate);
        check(primary.view==1 && primary.targetView==1 && !primary.changing && primary.lastNewView==candidate &&
              primary.slots.at(1).proposal.at("body").at("value")==recovery.second.at(1),
              "primary installs the same immutable NEW_VIEW and recovers its prepared value");
        auto installedRetry=primary.pbftLastNewViewRetry;retries=primary.pbftNewViewRetries;queued=primary.inbox.size();
        primary.retryPbftNewView(installedRetry+std::chrono::milliseconds(250));
        check(primary.pbftNewViewRetries==retries+1 && primary.inbox.size()==queued+1 && primary.inbox.back()==candidate,
              "installed primary may retransmit the same NEW_VIEW to lagging replicas");

        primary.startViewChange(2);retries=primary.pbftNewViewRetries;queued=primary.inbox.size();progress=primary.lastProgress;
        primary.retryPbftNewView(installedRetry+std::chrono::seconds(1));
        check(primary.pbftNewViewRetries==retries && primary.inbox.size()==queued && primary.lastNewView==candidate &&
              primary.lastProgress==progress && primary.targetView==2,
              "superseded NEW_VIEW cannot be retransmitted in a higher target view");
    }
    static void staleMessageChecks(const json& cfg,const std::string& directory,json& checks) {
        Replica replica(cfg,1,2,directory);
        auto check=[&](bool condition,const std::string& name) {require(condition,name);checks.push_back(name);};
        replica.view=1;replica.targetView=2;replica.changing=true;
        auto old=Clock::now()-std::chrono::hours(1);replica.lastProgress=old;
        auto staleVc=viewChange(replica,cfg,0,1);auto staleNv=newView(replica,cfg,1);
        auto validations=replica.pbftVcValidations;
        replica.handle(staleVc);replica.handle(staleNv);
        check(replica.pbftVcValidations==validations && replica.viewChanges.empty() && replica.lastNewView.is_null() &&
              replica.view==1 && replica.targetView==2 && replica.changing && replica.lastProgress==old,
              "obsolete installed VIEW_CHANGE and NEW_VIEW are discarded without recovery work or deadline changes");
        auto staleBadVc=staleVc;staleBadVc["body"]["prepared"]=json::array({json{{"invalid","proof"}}});
        auto staleBadNv=staleNv;staleBadNv["body"]["changes"]=json::array({json{{"invalid","change"}}});
        replica.handle(staleBadVc);replica.handle(staleBadNv);
        check(replica.pbftVcValidations==validations && replica.viewChanges.empty() && replica.slots.empty() &&
              replica.preparedHistory.empty() && replica.lastProgress==old,
              "malformed obsolete evidence cannot enter recovery or modify replica state");

        replica.view=0;replica.handle(staleNv);
        check(replica.pbftVcValidations==validations && replica.view==0 && replica.targetView==2 &&
              replica.changing && replica.viewChanges.empty() && replica.lastNewView.is_null() && replica.lastProgress==old,
              "NEW_VIEW below the current target is discarded before validating its nested recovery evidence");
        // VIEW_CHANGE above the installed view is still authenticated under
        // the original PBFT collection rule, even below the current target.
        replica.handle(staleVc);
        check(replica.pbftVcValidations>validations && replica.viewChanges.at(1).at(0)==staleVc &&
              replica.view==0 && replica.targetView==2 && replica.changing && replica.lastProgress==old,
              "earlier-target VIEW_CHANGE above the installed view is authenticated without lowering the target");
        replica.viewChanges.clear();validations=replica.pbftVcValidations;

        replica.view=2;replica.targetView=2;replica.changing=false;
        auto installedVc=viewChange(replica,cfg,0,2);auto installedNv=newView(replica,cfg,2);
        replica.handle(installedVc);replica.handle(installedNv);
        check(replica.pbftVcValidations==validations && replica.viewChanges.empty() && replica.view==2 &&
              replica.targetView==2 && !replica.changing && replica.lastNewView.is_null() && replica.lastProgress==old,
              "already installed VIEW_CHANGE and NEW_VIEW retries leave all protocol state unchanged");
        replica.handle(viewChange(replica,cfg,0,67));
        check(replica.pbftVcValidations==validations && replica.viewChanges.empty() && replica.lastProgress==old,
              "VIEW_CHANGE outside the bounded view window is rejected before expensive recovery validation");
    }
    static void preparedDeadlineChecks(const json& cfg,const std::string& directory,json& checks) {
        Replica replica(cfg,1,0,directory);
        auto check=[&](bool condition,const std::string& name) {require(condition,name);checks.push_back(name);};
        auto head=proposal(cfg,1);replica.handle(head);
        check(replica.slots.count(1) && replica.slots.at(1).proposal==head,
              "prepared deadline fixture accepts a genuinely signed head proposal");
        auto old=Clock::now()-std::chrono::hours(1);replica.lastProgress=old;
        auto first=message(cfg,1,"PREPARE",{{"seq",1},{"digest",head.at("body").at("digest")}});
        auto second=message(cfg,2,"PREPARE",{{"seq",1},{"digest",head.at("body").at("digest")}});
        replica.handle(first);
        check(!replica.slots.at(1).prepared && replica.lastProgress==old,
              "one authentic PREPARE cannot refresh the head's progress deadline");
        auto beforeQuorum=Clock::now();replica.handle(second);auto preparedAt=replica.lastProgress;
        check(replica.slots.at(1).prepared && replica.validPrepared(replica.preparedHistory.at(1)) && preparedAt>=beforeQuorum,
              "first genuine head PREPARE quorum starts a fresh COMMIT-phase deadline");
        check(replica.slots.at(1).commitSent && !replica.slots.at(1).committed && replica.applied==0,
              "head preparation sends COMMIT without pretending execution has completed");
        replica.handle(first);replica.handle(second);replica.advance(1);replica.advance(1);
        check(replica.lastProgress==preparedAt && replica.applied==0,
              "duplicate PREPARE and repeated advance cannot renew an already prepared head deadline");

        auto future=proposal(cfg,2);replica.handle(future);replica.lastProgress=old;
        replica.handle(message(cfg,1,"PREPARE",{{"seq",2},{"digest",future.at("body").at("digest")}}));
        replica.handle(message(cfg,2,"PREPARE",{{"seq",2},{"digest",future.at("body").at("digest")}}));
        check(replica.slots.at(2).prepared && replica.validPrepared(replica.preparedHistory.at(2)) &&
              replica.lastProgress==old && replica.applied==0 && !replica.certificates.count(1),
              "future-slot preparation cannot postpone recovery for the blocked execution head");
        replica.advance(2);
        check(replica.lastProgress==old,"repeated future-slot advance cannot renew the head's deadline");

        auto phaseFolder=[&](const std::string& name) {
            auto path=directory+"/"+name;std::filesystem::create_directories(path);return path;
        };
        {
            Replica changing(cfg,1,0,phaseFolder("changing-view-prepared"));changing.handle(head);
            changing.slots.at(1).prepares[1]=first;changing.slots.at(1).prepares[2]=second;
            changing.changing=true;changing.targetView=1;changing.lastProgress=old;changing.advance(1);
            check(changing.slots.at(1).prepared && changing.lastProgress==old,
                  "head preparation cannot renew a deadline after view change has started");
        }
        {
            Replica obsolete(cfg,1,0,phaseFolder("old-view-prepared"));obsolete.handle(head);
            obsolete.slots.at(1).prepares[1]=first;obsolete.slots.at(1).prepares[2]=second;
            obsolete.view=1;obsolete.targetView=1;obsolete.lastProgress=old;obsolete.advance(1);
            check(obsolete.slots.at(1).prepared && obsolete.lastProgress==old,
                  "preparation of an old-view head cannot extend the installed view's deadline");
        }
    }
    static json run(const json& cfg,const std::string& directory) {
        json checks=json::array();
        auto folder=[&](const std::string& label) {auto path=directory+"/"+label;std::filesystem::create_directories(path);return path;};
        viewChangeCacheChecks(cfg,folder("vc-cache"),checks);
        immutableNewViewChecks(cfg,folder("immutable-new-view"),checks);
        staleMessageChecks(cfg,folder("stale-messages"),checks);
        preparedDeadlineChecks(cfg,folder("prepared-deadlines"),checks);
        return {{"checks",checks},{"network_started",false},{"method","saguaro"}};
    }
};

int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: test_pbft_recovery CONFIG OUTPUT_DIRECTORY");
        std::cout<<PbftRecoveryTest::run(readJson(argv[1]),argv[2]).dump()<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
