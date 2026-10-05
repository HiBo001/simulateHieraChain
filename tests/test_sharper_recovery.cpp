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
        auto out=replica.sharperRecoveryEvidence();
        out["reservations"]=json::array({json{{"proposal",batch.assignment},{"accept",accept(cfg,batch,1,voter)}}});
        return out;
    }
    static json viewChange(Replica& replica,const json& cfg,int voter,int view,const json& extra,json prepared=json::array()) {
        return message(cfg,1,voter,"VIEW_CHANGE",{{"stable",{{"seq",0},{"state",replica.genesis()},{"proof",json::array()}}},
            {"prepared",prepared},{"sharper_recovery",extra}},view);
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
