    // Saguaro is conventional replicated two-phase commit. Cross-shard proofs
    // attest semantic state after a PBFT slot; signer subsets never enter state.
    std::map<std::string,std::map<int,json>> sagAttestations;
    std::map<std::string,json> sagProofs, sagPrepareQueue, sagFinishQueue;
    std::map<std::string,std::map<int,json>> sagReceivedVotes, sagReceivedAcks;
    std::set<std::string> sagFinished, sagCompletionNeeded;
    std::map<std::string,Clock::time_point> sagLastForward;
    mutable std::set<std::string> sagCheckedProofs;
    std::set<std::string> sagActive, sagSelectedRequests, sagParticipantActive;
    std::map<std::string,std::string> sagActiveTx;
    std::map<std::string,std::string> sagUndecidedTx;
    using SagAccessKey=std::pair<int,std::string>;
    mutable std::map<std::string,std::set<SagAccessKey>> sagRequestKeys;
    std::map<SagAccessKey,std::set<std::string>> sagActiveKeys;
    std::map<std::string,std::set<SagAccessKey>> sagBatchKeys;
    std::map<std::string,Clock::time_point> sagRetryAfter;
    uint64_t sagAborted=0;
    uint64_t sagWorkTimerStarts=0, sagAdmissionDeferrals=0;
    Clock::time_point sagLastRetry=Clock::now();

    static json saguaroGenesis(json result) {
        for(const char* field:{"sag_coordinations","sag_preparations","sag_locks","sag_known","sag_known_requests","sag_retry_counts"})
            result[field]=json::object();
        return result;
    }
    void saguaroInitialize() {}
    bool sagUnappliedPbft() const {
        if(certificates.upper_bound(applied)!=certificates.end()) return true;
        for(auto it=slots.upper_bound(applied);it!=slots.end();++it)
            if(!it->second.proposal.is_null()) return true;
        return false;
    }
    void sagActivateWork(bool wasWaiting,bool inserted,const char* phase) {
        auto now=Clock::now();
        // A newly actionable phase gets its own deadline after idle/network
        // waiting. Duplicate retries and a stuck PBFT slot never renew it.
        if(inserted && !wasWaiting && !changing && !sagUnappliedPbft() && saguaroWaiting(now)) {
            lastProgress=now;++sagWorkTimerStarts;
            log("saguaro_work_activated",{{"phase",phase}});
        }
    }
    const std::set<SagAccessKey>& sagKeys(const json& request) const {
        std::string rid=request.at("body").at("id");
        auto prior=sagRequestKeys.find(rid);if(prior!=sagRequestKeys.end()) return prior->second;
        std::set<SagAccessKey> keys;
        for(const auto& tx:request.at("body").at("txs")) for(int owner:tx.at("participants"))
            keys.emplace(owner,accessFor(tx,owner).at("key").get<std::string>());
        return sagRequestKeys.emplace(rid,std::move(keys)).first->second;
    }
    bool sagAdmissible(const std::string& rid,const json& request,Clock::time_point now) const {
        if(sagSelectedRequests.count(rid)) return false;
        auto retry=sagRetryAfter.find(rid);
        if(retry!=sagRetryAfter.end() && now<retry->second) return false;
        for(const auto& key:sagKeys(request)) if(sagActiveKeys.count(key)) return false;
        return true;
    }
    void sagReserveKeys(const std::string& batch,const json& requests) {
        auto& keys=sagBatchKeys[batch];
        for(const auto& request:requests) for(const auto& key:sagKeys(request)) keys.insert(key);
        // Different replicas may learn all ACKs at different times. Keep all
        // local owners rather than overwrite a prior batch during recovery.
        for(const auto& key:keys) sagActiveKeys[key].insert(batch);
    }
    void sagReleaseKeys(const std::string& batch) {
        auto batchKeys=sagBatchKeys.find(batch);if(batchKeys==sagBatchKeys.end()) return;
        for(const auto& key:batchKeys->second) {
            auto owners=sagActiveKeys.find(key);if(owners==sagActiveKeys.end()) continue;
            owners->second.erase(batch);if(owners->second.empty()) sagActiveKeys.erase(owners);
        }
        sagBatchKeys.erase(batchKeys);
    }
    int sagRetryDelay(const std::string& rid,unsigned attempt) const {
        unsigned shift=std::min(4u,attempt?attempt-1:0u);
        auto jitter=std::stoul(hash(rid+":"+std::to_string(attempt)).substr(0,8),nullptr,16)%50;
        return std::min(timeoutMs/2,int((50u<<shift)+jitter));
    }
    static std::string sagProofKey(const json& record) {
        return record.at("phase").get<std::string>()+"|"+record.at("batch").get<std::string>()+"|"+hash(record.dump());
    }
    bool sagBatch(const std::string& batch,int coordinator) const {
        try {return batchOrigin(batch)==coordinator;} catch(...) {return false;}
    }
    json sagPs(const std::set<int>& ps) const {
        json out=json::array();for(int id:ps) out.push_back(id);return out;
    }
    bool sagRequests(const json& requests,int origin) const {
        if(!requests.is_array() || requests.empty()) return false;
        std::set<std::string> rids,tids;size_t count=0;
        for(const auto& request:requests) {
            if(!validRequestForShard(request,origin) || !rids.insert(request.at("body").at("id")).second) return false;
            for(const auto& tx:request.at("body").at("txs")) {
                if(!tids.insert(tx.at("id")).second) return false;
                ++count;
            }
        }
        return count<=size_t(crossShardBatchSize);
    }
    json sagInit(const std::string& batch,const json& requests) const {
        return {{"phase","INIT"},{"batch",batch},{"coordinator",shard},{"origin",shard},
                {"participants",sagPs(participants(requests))},{"requests",requests}};
    }
    bool sagValidRecord(const json& record) const {
        try {
            const std::string phase=record.at("phase"),batch=record.at("batch");
            int coordinator=record.at("coordinator"),origin=record.at("origin");
            if(!sagBatch(batch,coordinator) || !members.parent.count(origin) ||
               !record.at("participants").is_array() || record.at("participants").size()<2) return false;
            std::set<int> ps;
            for(const auto& x:record.at("participants")) if(!members.leaves.count(x.get<int>()) || !ps.insert(x.get<int>()).second) return false;
            if(members.lca(record.at("participants"))!=coordinator || sagPs(ps)!=record.at("participants")) return false;
            if(phase=="INIT") {
                return origin==coordinator && sagRequests(record.at("requests"),coordinator) &&
                    participants(record.at("requests"))==ps;
            }
            if(!record.at("init_digest").is_string() || record.at("init_digest").get<std::string>().size()!=64) return false;
            if(phase=="VOTE") {
                if(!ps.count(origin) || (record.at("vote")!="YES" && record.at("vote")!="NO")) return false;
                return record.at("reads").is_object() && record.at("writes").is_array();
            }
            if(phase=="DECISION") {
                return origin==coordinator && (record.at("decision")=="COMMIT" || record.at("decision")=="ABORT") && record.at("records").is_array();
            }
            if(phase=="ACK") return ps.count(origin) && (record.at("decision")=="COMMIT" || record.at("decision")=="ABORT") && record.at("results").is_array();
            return false;
        } catch(...) {return false;}
    }
    bool sagValidProof(const json& proof,const std::string& phase) const {
        try {
            const auto fingerprint=hash(proof.dump());
            if(sagCheckedProofs.count(fingerprint)) return proof.at("record").at("phase")==phase;
            const auto& record=proof.at("record");
            if(record.at("phase")!=phase || !sagValidRecord(record) || !proof.at("votes").is_array()) return false;
            std::set<int> voters;const auto recordDigest=hash(record.dump());
            for(const auto& vote:proof.at("votes")) {
                const auto& body=vote.at("body");int signer=body.at("from");
                if(body.at("type")!="SAG_ATTEST" || body.at("shard")!=record.at("origin") ||
                   signer<0 || signer>=4 || !voters.insert(signer).second ||
                   body.at("phase")!=phase || body.at("batch")!=record.at("batch") ||
                   body.at("record_digest")!=recordDigest || !members.replicaMessage(vote)) return false;
            }
            if(voters.size()<3) return false;
            if(sagCheckedProofs.size()<100000) sagCheckedProofs.insert(fingerprint);
            return true;
        } catch(...) {return false;}
    }
    json sagLocalRecord(const std::string& phase,const std::string& batch) const {
        if((phase=="INIT" || phase=="DECISION") && state.at("sag_coordinations").contains(batch)) {
            const auto& entry=state.at("sag_coordinations").at(batch);
            if(phase=="INIT") return entry.at("init");
            if(entry.contains("decision")) return entry.at("decision");
        }
        if((phase=="VOTE" || phase=="ACK") && state.at("sag_preparations").contains(batch)) {
            const auto& entry=state.at("sag_preparations").at(batch);
            if(phase=="VOTE") return entry.at("vote");
            if(entry.contains("ack")) return entry.at("ack");
        }
        return nullptr;
    }
    void sagAttest(const std::string& phase,const std::string& batch) {
        auto record=sagLocalRecord(phase,batch);if(record.is_null()) return;
        auto envelope=make("SAG_ATTEST",{{"phase",phase},{"batch",batch},{"record_digest",hash(record.dump())}});
        // The attached record is authenticated by the signed digest and is
        // stripped from individual votes before constructing the quorum proof.
        envelope["record"]=record;sendTo(shard,view%4,envelope);
    }
    void sagForward(const json& proof,bool force=false) {
        if(!isForward()) return;
        const auto& record=proof.at("record");auto key=sagProofKey(record);
        auto now=Clock::now();auto prior=sagLastForward.find(key);
        if(!force && prior!=sagLastForward.end() && std::chrono::duration_cast<std::chrono::milliseconds>(now-prior->second).count()<1000) return;
        sagLastForward[key]=now;
        const std::string phase=record.at("phase");
        if(phase=="INIT" || phase=="DECISION") {
            for(int leaf:record.at("participants")) sendShard(leaf,make("SAG_QC",{{"target",leaf},{"proof",proof}}));
        } else {
            int coordinator=record.at("coordinator");
            sendShard(coordinator,make("SAG_QC",{{"target",coordinator},{"proof",proof}}));
        }
        log("saguaro_forwarded",{{"phase",phase},{"batch",record.at("batch")},{"signers",proof.at("votes").size()}});
    }
    bool sagConflict(const json& requests,const std::string& owner="") const {
        for(const auto& request:requests) for(const auto& tx:request.at("body").at("txs")) {
            if(!touches(tx,shard)) continue;
            auto key=tx.at("participants").size()==1?tx.at("key").get<std::string>():accessFor(tx,shard).at("key").get<std::string>();
            if(state.at("sag_locks").contains(key) && state.at("sag_locks").at(key)!=owner) return true;
        }
        return false;
    }
    json sagVote(const json& init) {
        const auto& requests=init.at("requests");const std::string batch=init.at("batch");
        json record={{"phase","VOTE"},{"batch",batch},{"coordinator",init.at("coordinator")},{"origin",shard},
            {"participants",init.at("participants")},{"init_digest",hash(init.dump())},
            {"vote",sagConflict(requests,batch)?"NO":"YES"},{"reads",json::object()},{"writes",json::array()}};
        if(record.at("vote")=="NO") return record;
        for(const auto& request:requests) for(const auto& tx:request.at("body").at("txs")) {
            if(!touches(tx,shard)) continue;
            auto access=accessFor(tx,shard);std::string id=tx.at("id"),key=access.at("key");
            bool duplicate=state.at("cst_seen").contains(id);
            if(duplicate && state.at("cst_seen").at(id).at("tx_digest")!=hash(tx.dump())) {
                record["vote"]="NO";record["reads"]=json::object();record["writes"]=json::array();return record;
            }
            if(!duplicate && !record["reads"].contains(key)) record["reads"][key]=state.at("kv").contains(key)?state.at("kv").at(key):initialAccount();
            record["writes"].push_back({{"id",id},{"tx_digest",hash(tx.dump())},{"key",key},{"value",access.at("value")},
                {"fib",duplicate?expectedFib:fibonacci()},{"duplicate",duplicate}});
        }
        return record;
    }
    bool sagValidVoteFor(const json& record,const json& init,int owner) const {
        try {
            if(record.at("phase")!="VOTE" || record.at("origin")!=owner || record.at("batch")!=init.at("batch") ||
               record.at("coordinator")!=init.at("coordinator") || record.at("participants")!=init.at("participants") ||
               record.at("init_digest")!=hash(init.dump())) return false;
            if(record.at("vote")=="NO") return record.at("reads").empty() && record.at("writes").empty();
            std::set<std::string> keys;size_t index=0;
            for(const auto& request:init.at("requests")) for(const auto& tx:request.at("body").at("txs")) if(touches(tx,owner)) {
                if(index>=record.at("writes").size()) return false;
                const auto& write=record.at("writes")[index++];auto access=accessFor(tx,owner);std::string key=access.at("key");
                if(write.at("id")!=tx.at("id") || write.at("tx_digest")!=hash(tx.dump()) || write.at("key")!=key ||
                   write.at("value")!=access.at("value") || write.at("fib")!=expectedFib || !write.at("duplicate").is_boolean()) return false;
                if(!write.at("duplicate").get<bool>()) keys.insert(key);
            }
            if(index!=record.at("writes").size() || record.at("reads").size()!=keys.size()) return false;
            for(const auto& key:keys) {
                const auto& account=record.at("reads").at(key);
                if(!account.at("version").is_number_integer() || !account.at("value").is_number_integer() || !account.at("digest").is_string()) return false;
            }
            return true;
        } catch(...) {return false;}
    }
    bool sagValidDecisionFor(const json& record,const json& init) const {
        try {
            if(record.at("phase")!="DECISION" || record.at("origin")!=init.at("coordinator") ||
               record.at("batch")!=init.at("batch") || record.at("coordinator")!=init.at("coordinator") ||
               record.at("participants")!=init.at("participants") || record.at("init_digest")!=hash(init.dump())) return false;
            auto ps=participants(init.at("requests"));std::set<int> found;bool yes=true;
            std::map<std::string,bool> duplicate;
            for(const auto& vote:record.at("records")) {
                int owner=vote.at("origin");if(!ps.count(owner) || !found.insert(owner).second || !sagValidVoteFor(vote,init,owner)) return false;
                if(vote.at("vote")!="YES") yes=false;
                else for(const auto& write:vote.at("writes")) {
                    std::string id=write.at("id");bool flag=write.at("duplicate");
                    auto prior=duplicate.find(id);if(prior!=duplicate.end() && prior->second!=flag) yes=false;
                    duplicate[id]=flag;
                }
            }
            return found==ps && record.at("decision")== (yes?"COMMIT":"ABORT");
        } catch(...) {return false;}
    }
    bool saguaroValidValue(const json& value) const {
        try {
            if(!value.at("requests").is_array()) return false;
            if(!value.contains("sag_action")) {
                if(value.size()!=1) return false;
                size_t count=0;std::set<std::string> rids,tids;
                for(const auto& request:value.at("requests")) {
                    if(!members.leaves.count(shard) || !validRequest(request) || !rids.insert(request.at("body").at("id")).second || sagConflict(json::array({request}))) return false;
                    for(const auto& tx:request.at("body").at("txs")) {if(!tids.insert(tx.at("id")).second) return false;++count;}
                }
                return count<=size_t(batchSize);
            }
            const std::string action=value.at("sag_action"),batch=value.at("batch");
            if(action=="INIT") {
                if(value.size()!=3 || !isCoordinator() || !sagBatch(batch,shard) || state.at("sag_coordinations").contains(batch) || !sagRequests(value.at("requests"),shard)) return false;
                for(const auto& request:value.at("requests")) {
                    std::string rid=request.at("body").at("id");
                    if(state.at("sag_known_requests").contains(rid) && state.at("sag_known_requests").at(rid)!=hash(request.at("body").at("txs").dump())) return false;
                    for(const auto& tx:request.at("body").at("txs")) {
                    std::string id=tx.at("id");if(state.at("sag_known").contains(id) && state.at("sag_known").at(id)!=hash(tx.dump())) return false;
                    if(state.at("seen").contains(id) && state.at("seen").at(id).at("tx_digest")!=hash(tx.dump())) return false;
                    // PBFT validity depends on replicated INIT/DECIDE state,
                    // never on asynchronously received participant ACKs.
                    if(!state.at("seen").contains(id) && sagUndecidedTx.count(id)) return false;
                    }
                }
                return true;
            }
            if(!value.at("requests").empty()) return false;
            if(action=="PREPARE") {
                if(value.size()!=4) return false;
                const auto& proof=value.at("init");if(!sagValidProof(proof,"INIT")) return false;
                const auto& init=proof.at("record");
                return members.leaves.count(shard) && init.at("batch")==batch && participants(init.at("requests")).count(shard) && !state.at("sag_preparations").contains(batch);
            }
            if(action=="DECIDE") {
                if(value.size()!=5 || !isCoordinator() || !state.at("sag_coordinations").contains(batch) || state.at("sag_coordinations").at(batch).contains("decision")) return false;
                const auto& init=state.at("sag_coordinations").at(batch).at("init");auto ps=participants(init.at("requests"));
                std::set<int> found;json records=json::array();
                for(const auto& proof:value.at("votes")) {
                    if(!sagValidProof(proof,"VOTE")) return false;
                    const auto& vote=proof.at("record");int owner=vote.at("origin");
                    if(!ps.count(owner) || !found.insert(owner).second || !sagValidVoteFor(vote,init,owner)) return false;
                    records.push_back(vote);
                }
                if(found!=ps) return false;
                const auto& decision=value.at("decision");
                return decision.at("records")==records && sagValidRecord(decision) && sagValidDecisionFor(decision,init);
            }
            if(action=="FINISH") {
                if(value.size()!=4 || !members.leaves.count(shard) || !state.at("sag_preparations").contains(batch) || state.at("sag_preparations").at(batch).contains("ack") || !sagValidProof(value.at("decision"),"DECISION")) return false;
                const auto& prep=state.at("sag_preparations").at(batch);const auto& decision=value.at("decision").at("record");
                if(!sagValidDecisionFor(decision,prep.at("init"))) return false;
                for(const auto& vote:decision.at("records")) if(vote.at("origin")==shard) return vote==prep.at("vote");
                return false;
            }
            return false;
        } catch(...) {return false;}
    }
    json sagApplyCommit(const json& prep,const json& decision,const std::string& batch,int n) {
        std::map<int,json> working;std::map<std::string,bool> duplicate;
        for(const auto& vote:decision.at("records")) {
            working[vote.at("origin").get<int>()]=vote.at("reads");
            for(const auto& write:vote.at("writes")) duplicate[write.at("id")]=write.at("duplicate").get<bool>();
        }
        json results=json::array();
        for(const auto& request:prep.at("init").at("requests")) for(const auto& tx:request.at("body").at("txs")) {
            std::string id=tx.at("id");
            if(duplicate.at(id)) {
                if(touches(tx,shard)) results.push_back({{"id",id},{"duplicate",true}});
                continue;
            }
            std::map<int,json> old,next;
            for(int owner:tx.at("participants")) old[owner]=working.at(owner).at(accessFor(tx,owner).at("key").get<std::string>());
            for(const auto& [owner,previous]:old) {
                uint64_t result=accessFor(tx,owner).at("value").get<uint64_t>()+expectedFib;std::string dependency=previous.dump();
                for(const auto& [remote,account]:old) if(remote!=owner) {result+=account.at("value").get<uint64_t>();dependency+=account.dump();}
                next[owner]={{"version",previous.at("version").get<uint64_t>()+1},{"value",result},{"fib",expectedFib},
                    {"digest",hash(dependency+tx.dump()+std::to_string(owner))}};
            }
            for(const auto& [owner,account]:next) working[owner][accessFor(tx,owner).at("key").get<std::string>()]=account;
            if(!touches(tx,shard)) continue;
            std::string localKey=accessFor(tx,shard).at("key");
            json result={{"id",id},{"seq",n},{"kind","executed"},{"digest",next.at(shard).at("digest")}};
            results.push_back({{"id",id},{"key",localKey},{"state",next.at(shard)}});
            state["cst_seen"][id]={{"tx_digest",hash(tx.dump())},{"coordinator_batch",batch},{"committed",true}};stateDigest.markEntry("cst_seen",id);
            state["seen"][id]={{"tx_digest",hash(tx.dump())},{"result",result}};stateDigest.markEntry("seen",id);
            state["executed"]=state["executed"].get<uint64_t>()+1;
            state["leaf_ordered_cst"]=state["leaf_ordered_cst"].get<uint64_t>()+1;
        }
        for(auto it=working.at(shard).begin();it!=working.at(shard).end();++it) {
            state["kv"][it.key()]=it.value();stateDigest.markEntry("kv",it.key());kvDirty=true;
        }
        return results;
    }
    void saguaroApply(const json& value,const json& certificate,int n) {
        (void)certificate;
        if(!value.contains("sag_action")) {applyClientRequests(value.at("requests"),n);return;}
        const std::string action=value.at("sag_action"),batch=value.at("batch");
        if(action=="INIT") {
            auto init=sagInit(batch,value.at("requests"));
            state["sag_coordinations"][batch]={{"init",init},{"seq",n}};
            stateDigest.markEntry("sag_coordinations",batch);sagActive.insert(batch);
            sagReserveKeys(batch,init.at("requests"));
            for(const auto& request:init.at("requests")) {
                std::string rid=request.at("body").at("id");sagSelectedRequests.insert(rid);
                state["sag_known_requests"][rid]=hash(request.at("body").at("txs").dump());stateDigest.markEntry("sag_known_requests",rid);
                for(const auto& tx:request.at("body").at("txs")) if(!state["seen"].contains(tx.at("id").get<std::string>())) {
                    std::string id=tx.at("id");sagActiveTx[id]=batch;sagUndecidedTx[id]=batch;
                }
            }
            for(const auto& request:init.at("requests")) for(const auto& tx:request.at("body").at("txs")) {
                std::string id=tx.at("id");
                if(!state["sag_known"].contains(id)) {
                    state["sag_known"][id]=hash(tx.dump());stateDigest.markEntry("sag_known",id);
                    state["ordered_cst"]=state["ordered_cst"].get<uint64_t>()+1;
                }
            }
        } else if(action=="PREPARE") {
            const auto& init=value.at("init").at("record");auto vote=sagVote(init);
            state["sag_preparations"][batch]={{"init",init},{"vote",vote},{"seq",n}};
            stateDigest.markEntry("sag_preparations",batch);sagParticipantActive.insert(batch);
            if(vote.at("vote")=="YES") for(auto it=vote.at("reads").begin();it!=vote.at("reads").end();++it) {
                state["sag_locks"][it.key()]=batch;stateDigest.markEntry("sag_locks",it.key());
            }
            sagPrepareQueue.erase(batch);
        } else if(action=="DECIDE") {
            const auto& decision=value.at("decision");auto& coord=state["sag_coordinations"][batch];
            coord["decision"]=decision;stateDigest.markEntry("sag_coordinations",batch);
            for(const auto& request:coord.at("init").at("requests")) {
                std::string rid=request.at("body").at("id");
                if(decision.at("decision")=="ABORT") {
                    state["sag_retry_counts"][rid]=state["sag_retry_counts"].value(rid,0u)+1;
                    stateDigest.markEntry("sag_retry_counts",rid);
                }
                for(const auto& tx:request.at("body").at("txs")) {
                    auto owner=sagUndecidedTx.find(tx.at("id").get<std::string>());
                    if(owner!=sagUndecidedTx.end() && owner->second==batch) sagUndecidedTx.erase(owner);
                }
            }
            if(decision.at("decision")=="COMMIT") for(const auto& request:coord.at("init").at("requests")) {
                std::string rid=request.at("body").at("id");json results=json::array();
                for(const auto& tx:request.at("body").at("txs")) {
                    std::string id=tx.at("id");
                    if(state["seen"].contains(id)) {results.push_back(state["seen"].at(id).at("result"));continue;}
                    json result={{"id",id},{"seq",coord.at("seq")},{"kind","ordered_only"},{"digest",hash(tx.dump())}};
                    state["seen"][id]={{"tx_digest",hash(tx.dump())},{"result",result},{"sag_batch",batch}};
                    stateDigest.markEntry("seen",id);changedTx.insert(id);results.push_back(result);
                }
                state["requests"][rid]={{"txs_hash",hash(request.at("body").at("txs").dump())},{"results",results}};
                stateDigest.markEntry("requests",rid);erasePending(rid);
            }
        } else if(action=="FINISH") {
            const auto& decision=value.at("decision").at("record");auto& prep=state["sag_preparations"][batch];
            json results=json::array();if(decision.at("decision")=="COMMIT") results=sagApplyCommit(prep,decision,batch,n);
            for(auto it=prep.at("vote").at("reads").begin();it!=prep.at("vote").at("reads").end();++it) {
                if(state["sag_locks"].contains(it.key()) && state["sag_locks"].at(it.key())==batch) {
                    state["sag_locks"].erase(it.key());stateDigest.markEntry("sag_locks",it.key());
                }
            }
            prep["ack"]={{"phase","ACK"},{"batch",batch},{"coordinator",decision.at("coordinator")},{"origin",shard},
                {"participants",decision.at("participants")},{"init_digest",decision.at("init_digest")},
                {"decision",decision.at("decision")},{"results",results}};
            stateDigest.markEntry("sag_preparations",batch);sagFinishQueue.erase(batch);sagParticipantActive.erase(batch);
        }
        log("saguaro_applied",{{"action",action},{"batch",batch},{"seq",n}});
    }
    void saguaroAfterApply(const json& value,const json& certificate,int n) {
        (void)certificate;(void)n;
        if(!value.contains("sag_action")) return;
        std::string action=value.at("sag_action"),batch=value.at("batch");
        sagAttest(action=="INIT"?"INIT":action=="PREPARE"?"VOTE":action=="DECIDE"?"DECISION":"ACK",batch);
        if(action=="DECIDE") sagComplete(batch);
    }
    bool sagValidAckFor(const json& record,const json& coord) const {
        try {
            const auto& init=coord.at("init");const auto& decision=coord.at("decision");int owner=record.at("origin");
            if(!participants(init.at("requests")).count(owner) || record.at("participants")!=init.at("participants") ||
               record.at("batch")!=init.at("batch") || record.at("coordinator")!=init.at("coordinator") ||
               record.at("init_digest")!=decision.at("init_digest") || record.at("decision")!=decision.at("decision")) return false;
            if(decision.at("decision")=="ABORT") return record.at("results").empty();
            std::map<std::string,bool> flags;
            for(const auto& vote:decision.at("records")) for(const auto& write:vote.at("writes")) flags[write.at("id")]=write.at("duplicate").get<bool>();
            size_t index=0;
            for(const auto& request:init.at("requests")) for(const auto& tx:request.at("body").at("txs")) if(touches(tx,owner)) {
                if(index>=record.at("results").size()) return false;
                const auto& result=record.at("results")[index++];std::string id=tx.at("id");
                if(result.at("id")!=id) return false;
                if(flags.at(id)) {if(result!=json{{"id",id},{"duplicate",true}}) return false;continue;}
                if(result.at("key")!=accessFor(tx,owner).at("key") || result.size()!=3) return false;
                const auto& account=result.at("state");
                if(account.size()!=4 || !account.at("version").is_number_integer() || account.at("version").get<int64_t>()<=0 ||
                   !account.at("value").is_number_unsigned() || account.at("fib")!=expectedFib ||
                   !account.at("digest").is_string() || account.at("digest").get<std::string>().size()!=64) return false;
            }
            // Three replica signatures certify the participant's actual
            // execution. The coordinator checks binding and result shape;
            // it does not redo the application on behalf of every leaf.
            return index==record.at("results").size();
        } catch(...) {return false;}
    }
    void sagComplete(const std::string& batch) {
        if(sagFinished.count(batch)) {sagCompletionNeeded.erase(batch);return;}
        if(!state["sag_coordinations"].contains(batch)) return;
        const auto& coord=state["sag_coordinations"].at(batch);if(!coord.contains("decision")) return;
        const auto& decision=coord.at("decision");auto ps=participants(coord.at("init").at("requests"));
        if(!sagReceivedAcks.count(batch)) return;
        json acknowledgments=json::array();
        for(int owner:ps) {
            if(!sagReceivedAcks.at(batch).count(owner)) return;
            const auto& record=sagReceivedAcks.at(batch).at(owner).at("record");
            if(!sagValidAckFor(record,coord)) return;
            acknowledgments.push_back(record);
        }
        sagFinished.insert(batch);sagCompletionNeeded.erase(batch);
        sagActive.erase(batch);sagReleaseKeys(batch);
        // Receiving the first complete, validated ACK set is real progress.
        // It may make key-blocked requests runnable after network waiting.
        if(!changing && !sagUnappliedPbft()) lastProgress=Clock::now();
        for(const auto& request:coord.at("init").at("requests")) {
            sagSelectedRequests.erase(request.at("body").at("id"));
            for(const auto& tx:request.at("body").at("txs")) {
                std::string id=tx.at("id");if(sagActiveTx.count(id) && sagActiveTx.at(id)==batch) sagActiveTx.erase(id);changedTx.insert(id);
            }
        }
        if(decision.at("decision")=="ABORT") {
            sagAborted++;log("saguaro_aborted",{{"batch",batch}});
            auto now=Clock::now();int shortest=timeoutMs,longest=0;
            for(const auto& request:coord.at("init").at("requests")) {
                std::string rid=request.at("body").at("id");
                int delay=sagRetryDelay(rid,state["sag_retry_counts"].value(rid,1u));
                sagRetryAfter[rid]=now+std::chrono::milliseconds(delay);
                shortest=std::min(shortest,delay);longest=std::max(longest,delay);
            }
            log("saguaro_retry_scheduled",{{"batch",batch},{"requests",coord.at("init").at("requests").size()},
                {"min_delay_ms",shortest},{"max_delay_ms",longest}});
            // Original CLIENT envelopes remain pending until a successful
            // decision, preserving IDs and the entire client request batch.
            for(const auto& request:coord.at("init").at("requests")) saguaroClient(request);
            return;
        }
        const auto completionDigest=hash(acknowledgments.dump());
        for(const auto& request:coord.at("init").at("requests")) {
            std::string rid=request.at("body").at("id");sagRetryAfter.erase(rid);sagRequestKeys.erase(rid);
            json results=json::array();
            for(const auto& tx:request.at("body").at("txs")) {
                std::string id=tx.at("id");json result={{"id",id},{"seq",coord.at("seq")},{"kind","executed"},
                    {"digest",hash(completionDigest+id)}};
                if(!completedTxResults.count(id)) {completedTxResults[id]=result;completedCstTransactions++;}
                else {result=completedTxResults.at(id);result["kind"]="duplicate";}
                results.push_back(result);changedTx.insert(id);
            }
            reply(request,results);erasePending(request.at("body").at("id"));
        }
        completedCstBatches.insert(batch);log("saguaro_completed",{{"batch",batch},{"participants",ps.size()}});
    }
    void saguaroClient(const json& request) {
        if(!validRequest(request)) {rejected++;return;}
        const auto& body=request.at("body");std::string rid=body.at("id");auto txhash=hash(body.at("txs").dump());
        if((state["requests"].contains(rid) && state["requests"].at(rid).at("txs_hash")!=txhash) ||
           (state["sag_known_requests"].contains(rid) && state["sag_known_requests"].at(rid)!=txhash)) {replyConflict(request,"request_id_conflict");rejected++;return;}
        for(const auto* requests:{&pending,&deferredRequests}) {
            auto prior=requests->find(rid);
            if(prior!=requests->end()) {
                if(prior->second.at("body").at("txs")!=body.at("txs")) {replyConflict(request,"request_id_conflict");rejected++;}
                else if(!isForward()) sendTo(shard,view%4,request);
                return;
            }
        }
        if(replyCompletedTransactions(request)) return;
        if(pending.size()+deferredRequests.size()>=10000) {rejected++;return;}
        bool blocked=false;
        for(const auto& tx:body.at("txs")) {
            std::string id=tx.at("id");auto digest=hash(tx.dump());
            if(state["sag_known"].contains(id) && state["sag_known"].at(id)!=digest) {replyConflict(request);rejected++;return;}
            if(state["seen"].contains(id) && state["seen"].at(id).at("tx_digest")!=digest) {replyConflict(request);rejected++;return;}
            auto owner=pendingTx.find(id);
            if(owner!=pendingTx.end() && owner->second.first!=digest) {replyConflict(request);rejected++;return;}
            if(owner!=pendingTx.end() || awaitingResult(id) || sagActiveTx.count(id)) blocked=true;
            if(awaitingResult(id) && state["seen"].at(id).contains("sag_batch")) sagCompletionNeeded.insert(state["seen"].at(id).at("sag_batch"));
        }
        if(blocked) deferRequest(request);
        else {
            bool wasWaiting=saguaroWaiting(Clock::now());
            if(pending.empty()) batchStart=Clock::now();
            pending[rid]=request;
            for(const auto& tx:body.at("txs")) pendingTx[tx.at("id").get<std::string>()]={hash(tx.dump()),rid};
            if(pending.size()==1) batchStart=Clock::now();
            sagActivateWork(wasWaiting,true,"CLIENT");
        }
        if(!isForward()) sendTo(shard,view%4,request);
    }
    bool saguaroHandle(const json& envelope) {
        const auto& body=envelope.at("body");std::string type=body.at("type");
        if(type.rfind("SAG_",0)!=0) return false;
        try {
            if(type=="SAG_ATTEST") {
                const auto& record=envelope.at("record");std::string phase=record.at("phase"),batch=record.at("batch");
                auto local=sagLocalRecord(phase,batch);
                if(body.at("shard")!=shard || record.at("origin")!=shard || body.at("phase")!=phase ||
                   body.at("batch")!=batch || body.at("record_digest")!=hash(record.dump()) || !sagValidRecord(record) ||
                   (!local.is_null() && record!=local)) {rejected++;return true;}
                auto key=sagProofKey(record);
                if(sagAttestations.size()>=100000 && !sagAttestations.count(key)) {rejected++;return true;}
                auto vote=envelope;vote.erase("record");sagAttestations[key][body.at("from").get<int>()]=std::move(vote);
                if(!local.is_null() && sagAttestations[key].size()>=3) {
                    json votes=json::array();for(const auto& [replica,vote]:sagAttestations[key]) {(void)replica;votes.push_back(vote);}
                    json proof={{"record",record},{"votes",votes}};sagProofs[key]=proof;sagForward(proof);
                }
            } else if(type=="SAG_QC") {
                if(body.at("target")!=shard) {rejected++;return true;}
                const auto& proof=body.at("proof");const auto& record=proof.at("record");
                const std::string phase=record.at("phase"),batch=record.at("batch");int origin=record.at("origin");
                if(body.at("shard")!=origin || !sagValidProof(proof,phase)) {rejected++;return true;}
                if(phase=="INIT") {
                    if(!members.leaves.count(shard) || !participants(record.at("requests")).count(shard)) {rejected++;return true;}
                    if(state["sag_preparations"].contains(batch)) {
                        if(state["sag_preparations"].at(batch).at("init")!=record) {rejected++;return true;}
                        sagAttest(state["sag_preparations"].at(batch).contains("ack")?"ACK":"VOTE",batch);
                    } else {
                        bool wasWaiting=saguaroWaiting(Clock::now());
                        bool inserted=sagPrepareQueue.emplace(batch,proof).second;
                        sagActivateWork(wasWaiting,inserted,"INIT");
                    }
                } else if(phase=="VOTE") {
                    if(record.at("coordinator")!=shard || !state["sag_coordinations"].contains(batch) || !sagValidVoteFor(record,state["sag_coordinations"].at(batch).at("init"),origin)) {rejected++;return true;}
                    bool wasWaiting=saguaroWaiting(Clock::now());
                    bool inserted=!sagReceivedVotes[batch].count(origin);
                    sagReceivedVotes[batch][origin]=proof;
                    if(state["sag_coordinations"].at(batch).contains("decision")) sagAttest("DECISION",batch);
                    else sagActivateWork(wasWaiting,inserted,"VOTE");
                } else if(phase=="DECISION") {
                    if(!members.leaves.count(shard) || !state["sag_preparations"].contains(batch) || !sagValidDecisionFor(record,state["sag_preparations"].at(batch).at("init"))) {rejected++;return true;}
                    if(state["sag_preparations"].at(batch).contains("ack")) sagAttest("ACK",batch);
                    else {
                        bool wasWaiting=saguaroWaiting(Clock::now());
                        bool inserted=sagFinishQueue.emplace(batch,proof).second;
                        sagActivateWork(wasWaiting,inserted,"DECISION");
                    }
                } else if(phase=="ACK") {
                    if(record.at("coordinator")!=shard || !state["sag_coordinations"].contains(batch) || record.at("init_digest")!=hash(state["sag_coordinations"].at(batch).at("init").dump())) {rejected++;return true;}
                    const auto& coord=state["sag_coordinations"].at(batch);
                    if(coord.contains("decision") && !sagValidAckFor(record,coord)) {rejected++;return true;}
                    sagReceivedAcks[batch][origin]=proof;sagComplete(batch);
                } else rejected++;
            } else if(type=="SAG_QUERY") {
                if(body.at("target")!=shard || !body.at("batch").is_string()) {rejected++;return true;}
                std::string phase=body.at("phase"),batch=body.at("batch");auto record=sagLocalRecord(phase,batch);
                if(record.is_null() || (body.at("shard")!=record.at("coordinator") &&
                   std::find(record.at("participants").begin(),record.at("participants").end(),body.at("shard"))==record.at("participants").end())) {rejected++;return true;}
                sagAttest(phase,batch);
                auto key=sagProofKey(record);if(sagProofs.count(key)) sagForward(sagProofs.at(key),true);
            } else rejected++;
        } catch(...) {rejected++;}
        return true;
    }
    void saguaroPropose() {
        if(changing || !isForward() || applied+1>stableSeq+window || certificates.count(applied+1) ||
           (slots.count(applied+1) && !slots.at(applied+1).proposal.is_null())) return;
        json value={{"requests",json::array()}};
        if(members.leaves.count(shard)) {
            for(auto it=sagFinishQueue.begin();it!=sagFinishQueue.end();) {
                if(state["sag_preparations"].contains(it->first) && state["sag_preparations"].at(it->first).contains("ack")) {it=sagFinishQueue.erase(it);continue;}
                value={{"requests",json::array()},{"sag_action","FINISH"},{"batch",it->first},{"decision",it->second}};break;
            }
            if(!value.contains("sag_action")) for(auto it=sagPrepareQueue.begin();it!=sagPrepareQueue.end();) {
                if(state["sag_preparations"].contains(it->first)) {it=sagPrepareQueue.erase(it);continue;}
                value={{"requests",json::array()},{"sag_action","PREPARE"},{"batch",it->first},{"init",it->second}};break;
            }
            if(!value.contains("sag_action")) {
                if(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-batchStart).count()<members.config.at("consensus").at("batch_wait_ms").get<int>()) return;
                size_t count=0;
                for(const auto& [rid,request]:pending) {
                    (void)rid;if(sagConflict(json::array({request}))) continue;
                    if(count+request.at("body").at("txs").size()>size_t(batchSize)) break;
                    value["requests"].push_back(request);count+=request.at("body").at("txs").size();
                }
                if(value.at("requests").empty()) return;
            }
        } else {
            std::string ready;
            for(const auto& batch:sagActive) {
                const auto& coord=state["sag_coordinations"].at(batch);
                if(!coord.contains("decision") && sagReceivedVotes.count(batch) &&
                   sagReceivedVotes.at(batch).size()==participants(coord.at("init").at("requests")).size()) {ready=batch;break;}
            }
            if(!ready.empty()) {
                const auto& coord=state["sag_coordinations"].at(ready);
                auto ps=participants(coord.at("init").at("requests"));json proofs=json::array(),records=json::array();
                for(int owner:ps) {
                    auto proof=sagReceivedVotes.at(ready).at(owner);proofs.push_back(proof);records.push_back(proof.at("record"));
                }
                bool yes=true;std::map<std::string,bool> flags;
                for(const auto& record:records) {
                    if(record.at("vote")!="YES") yes=false;
                    else for(const auto& write:record.at("writes")) {
                        std::string id=write.at("id");bool flag=write.at("duplicate");
                        if(flags.count(id) && flags.at(id)!=flag) yes=false;
                        flags[id]=flag;
                    }
                }
                json decision={{"phase","DECISION"},{"batch",ready},{"coordinator",shard},{"origin",shard},
                    {"participants",coord.at("init").at("participants")},{"init_digest",hash(coord.at("init").dump())},
                    {"decision",yes?"COMMIT":"ABORT"},{"records",records}};
                value={{"requests",json::array()},{"sag_action","DECIDE"},{"batch",ready},{"votes",proofs},{"decision",decision}};
            } else {
                constexpr int maxInflight=8;
                if(sagActive.size()>=size_t(maxInflight) || pending.empty()) return;
                json requests=json::array();size_t count=0;std::string group;bool blocked=false;
                auto now=Clock::now();
                for(const auto& [rid,request]:pending) {
                    if(sagSelectedRequests.count(rid)) continue;
                    if(!sagAdmissible(rid,request,now)) {
                        ++sagAdmissionDeferrals;blocked=true;continue;
                    }
                    auto candidate=participantGroup(request);
                    if((!group.empty() && candidate!=group) || count+request.at("body").at("txs").size()>size_t(crossShardBatchSize)) {blocked=count>0;break;}
                    requests.push_back(request);count+=request.at("body").at("txs").size();group=candidate;
                }
                if(requests.empty() || (count<size_t(crossShardBatchSize) && !blocked &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-batchStart).count()<crossShardBatchWaitMs)) return;
                value={{"requests",requests},{"sag_action","INIT"},{"batch",std::to_string(shard)+":"+std::to_string(applied+1)}};
            }
        }
        if(!saguaroValidValue(value)) {log("saguaro_invalid_local_value",{{"value",value}});return;}
        auto proposal=make("PREPREPARE",{{"seq",applied+1},{"value",value},{"digest",hash(value.dump())}});
        broadcast(proposal);if(!value.contains("sag_action") || value.at("sag_action")=="INIT") batchStart=Clock::now();
    }
    bool saguaroWaiting(Clock::time_point now) const {
        if((!sagActive.empty() || !sagParticipantActive.empty() || !sagCompletionNeeded.empty()) &&
           std::chrono::duration_cast<std::chrono::milliseconds>(now-lastForwardHeartbeat).count()>timeoutMs) return true;
        if(changing || certificates.count(applied+1) || (slots.count(applied+1) && !slots.at(applied+1).proposal.is_null())) return true;
        if(members.leaves.count(shard)) {
            if(!sagFinishQueue.empty() || !sagPrepareQueue.empty()) return true;
            for(const auto& [rid,request]:pending) {(void)rid;if(!sagConflict(json::array({request}))) return true;}
            return false;
        }
        for(const auto& batch:sagActive) {
            const auto& coord=state.at("sag_coordinations").at(batch);
            if(!coord.contains("decision") && sagReceivedVotes.count(batch) &&
               sagReceivedVotes.at(batch).size()==participants(coord.at("init").at("requests")).size()) return true;
        }
        if(sagActive.size()>=size_t(8)) return false;
        for(const auto& [rid,request]:pending) if(sagAdmissible(rid,request,now)) return true;
        return false;
    }
    void sagQuery(const std::string& phase,const std::string& batch,int destination) {
        if(isForward()) sendShard(destination,make("SAG_QUERY",{{"target",destination},{"phase",phase},{"batch",batch}}));
    }
    void saguaroTick(Clock::time_point now) {
        if(std::chrono::duration_cast<std::chrono::milliseconds>(now-sagLastRetry).count()<1000) return;
        sagLastRetry=now;
        for(const auto& batch:sagActive) {
            const auto& coord=state["sag_coordinations"].at(batch);std::string phase=coord.contains("decision")?"DECISION":"INIT";
            sagAttest(phase,batch);auto record=sagLocalRecord(phase,batch);auto key=sagProofKey(record);
            if(sagProofs.count(key)) sagForward(sagProofs.at(key),true);
            if(coord.contains("decision")) for(int owner:coord.at("init").at("participants")) sagQuery("ACK",batch,owner);
        }
        for(const auto& batch:sagParticipantActive) {
            const auto& prep=state["sag_preparations"].at(batch);
            sagAttest("VOTE",batch);auto key=sagProofKey(prep.at("vote"));if(sagProofs.count(key)) sagForward(sagProofs.at(key),true);
            sagQuery("DECISION",batch,prep.at("init").at("coordinator"));
        }
        for(const auto& batch:sagCompletionNeeded) if(state["sag_coordinations"].contains(batch)) {
            const auto& coord=state["sag_coordinations"].at(batch);
            if(!coord.contains("decision")) continue;
            for(int owner:coord.at("init").at("participants")) sagQuery("ACK",batch,owner);
        }
    }
    void saguaroRestore() {
        sagPrepareQueue.clear();sagFinishQueue.clear();sagActive.clear();sagSelectedRequests.clear();sagParticipantActive.clear();sagActiveTx.clear();
        sagUndecidedTx.clear();sagRequestKeys.clear();sagActiveKeys.clear();sagBatchKeys.clear();sagRetryAfter.clear();
        auto now=Clock::now();
        // Retry attempts are replicated at DECIDE, not counted again from
        // duplicate ACK delivery. Reconstruct only scheduling deadlines here.
        for(auto it=state["sag_retry_counts"].begin();it!=state["sag_retry_counts"].end();++it)
            if(!state["requests"].contains(it.key()))
                sagRetryAfter[it.key()]=now+std::chrono::milliseconds(sagRetryDelay(it.key(),it.value().get<unsigned>()));
        for(auto it=state["sag_coordinations"].begin();it!=state["sag_coordinations"].end();++it) if(!sagFinished.count(it.key())) {
            sagActive.insert(it.key());
            sagReserveKeys(it.key(),it.value().at("init").at("requests"));
            for(const auto& request:it.value().at("init").at("requests")) {
                sagSelectedRequests.insert(request.at("body").at("id"));
                for(const auto& tx:request.at("body").at("txs")) if(!state["seen"].contains(tx.at("id").get<std::string>())) {
                    std::string id=tx.at("id");sagActiveTx[id]=it.key();
                    if(!it.value().contains("decision")) sagUndecidedTx[id]=it.key();
                }
            }
            sagAttest(it.value().contains("decision")?"DECISION":"INIT",it.key());
            sagCompletionNeeded.insert(it.key());
        }
        for(auto it=state["sag_preparations"].begin();it!=state["sag_preparations"].end();++it) {
            if(!it.value().contains("ack")) sagParticipantActive.insert(it.key());
            sagAttest(it.value().contains("ack")?"ACK":"VOTE",it.key());
        }
        sagLastRetry=Clock::time_point{};
        if(!changing && !sagUnappliedPbft()) lastProgress=Clock::now();
    }
    void saguaroStatus(json& status) const {
        auto coordinatorActive=sagActive;coordinatorActive.insert(sagCompletionNeeded.begin(),sagCompletionNeeded.end());
        size_t active=sagParticipantActive.size()+coordinatorActive.size();
        status["method"]="saguaro";
        status["sag_active_batches"]=active;
        status["sag_pending_prepares"]=sagPrepareQueue.size();
        status["sag_pending_decisions"]=sagFinishQueue.size();
        status["sag_pending_completions"]=sagCompletionNeeded.size();
        status["sag_held_locks"]=state.at("sag_locks").size();
        status["sag_aborted_batches"]=sagAborted;
        status["sag_completed_batches"]=completedCstBatches.size();
        status["sag_active_access_keys"]=sagActiveKeys.size();
        status["sag_admission_deferrals"]=sagAdmissionDeferrals;
        status["sag_work_timer_starts"]=sagWorkTimerStarts;
        size_t waitingRetries=0;auto now=Clock::now();
        for(const auto& [rid,deadline]:sagRetryAfter) {(void)rid;if(now<deadline) ++waitingRetries;}
        status["sag_retry_waiting_requests"]=waitingRetries;
    }
