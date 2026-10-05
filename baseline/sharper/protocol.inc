    // SharPer's flattened Byzantine agreement (SUPER_PROPOSE variant).
    // Cross-shard ACCEPT and COMMIT are the consensus votes themselves.
    // No ancestor, 2PC decision, or nested local PBFT round is involved.
    struct ShBatch {
        json proposal, assignment, accept, commit, executed;
        std::map<int,std::map<int,json>> accepts,commits,done;
        bool applied=false,complete=false;
    };
    std::map<std::string,ShBatch> shBatches;
    std::map<std::string,json> shPending;
    std::set<std::string> shSelected;
    std::string shLocal,shInitiating;
    uint64_t shAttempt=0,shCommitted=0;
    mutable std::set<std::string> shChecked;
    Clock::time_point shLastRetry=Clock::now();

    static json sharperGenesis(json result) {
        result["sharper_batches"]=json::object();
        result["sharper_initiated"]=0;
        return result;
    }
    bool shSigned(const json& e) const {
        auto key=hash(e.dump());if(shChecked.count(key)) return true;
        if(!members.replicaMessage(e)) return false;
        if(shChecked.size()>=12000) shChecked.clear();
        shChecked.insert(key);return true;
    }
    static json shHeader(const json& e) {return {{"body",e.at("body")},{"signature",e.at("signature")}};}
    static std::string shKey(const json& p) {return p.at("body").at("batch");}
    static json shPs(const std::set<int>& ps) {
        json out=json::array();for(int owner:ps) out.push_back(owner);return out;
    }
    bool sharperCrossValue(const json& value) const {return value.is_object() && value.contains("sharper_propose");}
    bool sharperCrossProof(const json& p) const {
        try {return sharperCrossValue(p.at("proposal").at("body").at("value"));} catch(...) {return false;}
    }
    bool sharperValidRequest(const json& e,int target) const {
        try {
            if(!members.clientMessage(e) || !members.leaves.count(target)) return false;
            const auto& b=e.at("body");
            if(b.at("type")!="CLIENT" || b.at("target")!=target || !b.at("id").is_string() ||
               b.at("id").get<std::string>().empty() || b.at("id").get<std::string>().size()>200 ||
               !b.at("txs").is_array() || b.at("txs").empty()) return false;
            if(b.at("txs").size()>size_t(b.at("txs")[0].at("participants").size()>1?crossShardBatchSize:batchSize)) return false;
            in_addr address{};const auto& reply=b.at("reply");
            if(inet_pton(AF_INET,reply.at("host").get<std::string>().c_str(),&address)!=1 ||
               reply.at("port").get<int>()<=0 || reply.at("port").get<int>()>65535) return false;
            std::set<std::string> ids;json group;
            for(const auto& tx:b.at("txs")) {
                std::string id=tx.at("id"),key=tx.at("key");
                if(id.empty() || id.size()>200 || key.empty() || key.size()>128 || !ids.insert(id).second ||
                   !tx.at("value").is_number_unsigned() || !tx.at("participants").is_array()) return false;
                std::set<int> ps;
                for(int owner:tx.at("participants")) if(!members.leaves.count(owner) || !ps.insert(owner).second) return false;
                if(ps.empty() || *ps.begin()!=target || tx.at("participants")!=shPs(ps)) return false;
                if(group.is_null()) group=tx.at("participants");else if(group!=tx.at("participants")) return false;
                if(ps.size()==1) {if(tx.contains("accesses")) return false;}
                else {
                    if(b.at("txs").size()>size_t(crossShardBatchSize) || !tx.at("accesses").is_array() || tx.at("accesses").size()!=ps.size()) return false;
                    std::set<int> found;
                    for(const auto& access:tx.at("accesses")) {
                        int owner=access.at("shard");std::string localKey=access.at("key");
                        if(!ps.count(owner) || !found.insert(owner).second || localKey.empty() || localKey.size()>128 || !access.at("value").is_number_unsigned()) return false;
                    }
                }
            }
            return true;
        } catch(...) {return false;}
    }
    bool shValidPropose(const json& p,const json& requests) const {
        try {
            const auto& b=p.at("body");int origin=b.at("shard"),v=b.at("view");
            if(!shSigned(shHeader(p)) || b.at("type")!="SH_SUPER_PROPOSE" || v<0 || b.at("from")!=v%4 ||
               !members.leaves.count(origin) || b.at("origin_seq").get<int>()<=0 ||
               !b.at("batch").is_string() || b.at("batch").get<std::string>().size()!=64 ||
               b.at("request_digest")!=hash(requests.dump()) || !requests.is_array() || requests.empty()) return false;
            std::set<std::string> rids,tids;std::set<int> ps;size_t count=0;
            for(const auto& request:requests) {
                if(!sharperValidRequest(request,origin) || !rids.insert(request.at("body").at("id")).second) return false;
                for(const auto& tx:request.at("body").at("txs")) {
                    auto owners=tx.at("participants").get<std::vector<int>>();std::set<int> group(owners.begin(),owners.end());
                    if(group.size()<2 || (!ps.empty() && ps!=group) || !tids.insert(tx.at("id")).second) return false;
                    ps=group;++count;
                }
            }
            return count<=size_t(crossShardBatchSize) && b.at("participants")==shPs(ps) && *ps.begin()==origin;
        } catch(...) {return false;}
    }
    bool shValidClaim(const json& claim,const json& p) const {
        try {
            const auto& b=claim.at("body");const auto& pb=p.at("body");int owner=b.at("shard"),v=b.at("view");
            return shSigned(claim) && b.at("type")=="SH_LOCAL_CLAIM" && v>=0 && b.at("from")==v%4 &&
                b.at("seq").get<int>()>0 && b.at("ballot").get<uint64_t>()>0 &&
                touches(json{{"participants",pb.at("participants")}},owner) && b.at("batch")==pb.at("batch") &&
                b.at("request_digest")==pb.at("request_digest") && b.at("origin_seq")==pb.at("origin_seq") &&
                b.at("proposal_digest")==hash(shHeader(p).dump()) &&
                (owner!=pb.at("shard").get<int>() || b.at("seq")==pb.at("origin_seq"));
        } catch(...) {return false;}
    }
    bool shValidAccept(const json& e,const json& p) const {
        try {
            const auto& b=e.at("body");const auto& pb=p.at("body");const auto& claim=b.at("claim");
            return shSigned(e) && b.at("type")=="SH_ACCEPT" && b.at("batch")==pb.at("batch") &&
                b.at("request_digest")==pb.at("request_digest") && b.at("origin_seq")==pb.at("origin_seq") &&
                shValidClaim(claim,p) && b.at("shard")==claim.at("body").at("shard") &&
                b.at("local_seq")==claim.at("body").at("seq") && b.at("claim_digest")==hash(claim.dump());
        } catch(...) {return false;}
    }
    bool shAcceptVector(const json& p,const json& votes,json& seqs,json& claims) const {
        try {
            std::map<int,std::set<int>> signers;std::map<int,std::string> chosen;
            seqs=json::object();claims=json::object();
            if(!votes.is_array() || votes.size()>4*p.at("body").at("participants").size()) return false;
            for(const auto& vote:votes) {
                if(!shValidAccept(vote,p)) return false;
                const auto& b=vote.at("body");int owner=b.at("shard"),r=b.at("from");std::string digest=b.at("claim_digest");
                if(!signers[owner].insert(r).second || (chosen.count(owner) && chosen.at(owner)!=digest)) return false;
                chosen[owner]=digest;seqs[std::to_string(owner)]=b.at("local_seq");claims[std::to_string(owner)]=digest;
            }
            for(int owner:p.at("body").at("participants")) if(signers[owner].size()<3) return false;
            return signers.size()==p.at("body").at("participants").size();
        } catch(...) {return false;}
    }
    bool shValidRecord(const json& record,const json& p,const json& requests,int owner,int seq) const {
        try {
            if(record.at("shard")!=owner || record.at("seq")!=seq || record.at("batch")!=shKey(p) ||
               record.at("request_digest")!=p.at("body").at("request_digest") || !record.at("reads").is_object() || !record.at("writes").is_array()) return false;
            std::set<std::string> keys;size_t i=0;
            for(const auto& request:requests) for(const auto& tx:request.at("body").at("txs")) {
                if(i>=record.at("writes").size()) return false;
                const auto& write=record.at("writes")[i++];const auto access=accessFor(tx,owner);std::string key=access.at("key");
                if(write.at("id")!=tx.at("id") || write.at("tx_digest")!=hash(tx.dump()) || write.at("key")!=key ||
                   write.at("value")!=access.at("value") || !write.at("duplicate").is_boolean()) return false;
                if(!write.at("duplicate").get<bool>()) keys.insert(key);
            }
            if(i!=record.at("writes").size() || record.at("reads").size()!=keys.size()) return false;
            for(const auto& key:keys) {
                const auto& a=record.at("reads").at(key);
                if(!a.at("version").is_number_integer() || !a.at("value").is_number_integer() || !a.at("digest").is_string()) return false;
            }
            return true;
        } catch(...) {return false;}
    }
    bool shValidCommit(const json& e,const json& p,const json& requests,const json& seqs,const json& claims) const {
        try {
            const auto& b=e.at("body");int owner=b.at("shard");
            if(!seqs.is_object() || !claims.is_object() || seqs.size()!=p.at("body").at("participants").size() || claims.size()!=seqs.size()) return false;
            for(int participant:p.at("body").at("participants")) {
                auto key=std::to_string(participant);
                if(!seqs.contains(key) || !seqs.at(key).is_number_integer() || seqs.at(key).get<int>()<=0 ||
                   !claims.contains(key) || !claims.at(key).is_string() || claims.at(key).get<std::string>().size()!=64) return false;
            }
            return shSigned(e) && b.at("type")=="SH_COMMIT" && b.at("batch")==shKey(p) &&
                b.at("request_digest")==p.at("body").at("request_digest") && b.at("seqs")==seqs && b.at("claims")==claims &&
                seqs.contains(std::to_string(owner)) && b.at("record_digest")==hash(b.at("record").dump()) &&
                shValidRecord(b.at("record"),p,requests,owner,seqs.at(std::to_string(owner)));
        } catch(...) {return false;}
    }
    bool shCommitRecords(const json& p,const json& requests,const json& votes,const json& seqs,const json& claims,json& records) const {
        try {
            records=json::object();std::map<int,std::set<int>> signers;std::map<std::string,bool> duplicates;
            if(!votes.is_array() || votes.size()>4*p.at("body").at("participants").size()) return false;
            for(const auto& vote:votes) {
                if(!shValidCommit(vote,p,requests,seqs,claims)) return false;
                const auto& b=vote.at("body");int owner=b.at("shard");auto k=std::to_string(owner);
                if(!signers[owner].insert(b.at("from").get<int>()).second || (records.contains(k) && records.at(k)!=b.at("record"))) return false;
                records[k]=b.at("record");
            }
            for(int owner:p.at("body").at("participants")) {
                if(signers[owner].size()<3) return false;
                for(const auto& write:records.at(std::to_string(owner)).at("writes")) {
                    std::string id=write.at("id");bool duplicate=write.at("duplicate");
                    if(duplicates.count(id) && duplicates.at(id)!=duplicate) return false;
                    duplicates[id]=duplicate;
                }
            }
            return signers.size()==p.at("body").at("participants").size();
        } catch(...) {return false;}
    }
    bool sharperValidValue(const json& value) const {
        try {
            const auto& requests=value.at("requests");
            if(!requests.is_array()) return false;
            if(sharperCrossValue(value)) {
                const auto& p=value.at("sharper_propose");const auto& claim=value.at("sharper_claim");
                if(!shValidPropose(p,requests) || !shValidClaim(claim,p) || claim.at("body").at("shard")!=shard) return false;
                // The vote's claim digest binds this entire canonical value:
                // the original signed header, complete requests, and claim.
                if(value!=json{{"requests",requests},{"sharper_propose",shHeader(p)},{"sharper_claim",claim}}) return false;
            } else {
                if(value.size()!=1) return false;
                size_t count=0;std::set<std::string> rids,tids;
                for(const auto& request:requests) {
                    if(!validRequest(request) || !rids.insert(request.at("body").at("id")).second) return false;
                    auto rid=request.at("body").at("id").get<std::string>();
                    if(state.at("requests").contains(rid) && state.at("requests").at(rid).at("txs_hash")!=hash(request.at("body").at("txs").dump())) return false;
                    for(const auto& tx:request.at("body").at("txs")) {
                        if(tx.at("participants").size()!=1 || !tids.insert(tx.at("id")).second) return false;
                        auto id=tx.at("id").get<std::string>();
                        if(state.at("seen").contains(id) && state.at("seen").at(id).at("tx_digest")!=hash(tx.dump())) return false;
                        ++count;
                    }
                }
                if(count>size_t(batchSize)) return false;
            }
            return true;
        } catch(...) {return false;}
    }
    bool sharperValidPrepared(const json& proof) const {
        try {
            const auto& pp=proof.at("proposal");const auto& b=pp.at("body");const auto& value=b.at("value");
            if(!validSignedProposal(pp) || !sharperValidValue(value) || b.at("seq")!=value.at("sharper_claim").at("body").at("seq")) return false;
            json seqs,claims;
            return shAcceptVector(value.at("sharper_propose"),proof.at("prepares"),seqs,claims) &&
                seqs.at(std::to_string(shard))==b.at("seq") && claims.at(std::to_string(shard))==hash(value.at("sharper_claim").dump());
        } catch(...) {return false;}
    }
    bool sharperValidCertificate(const json& proof) const {
        try {
            auto fingerprint=hash(proof.dump());if(shChecked.count("cert:"+fingerprint)) return true;
            if(!sharperValidPrepared(proof)) return false;
            const auto& value=proof.at("proposal").at("body").at("value");json seqs,claims,records;
            if(!shAcceptVector(value.at("sharper_propose"),proof.at("prepares"),seqs,claims) ||
               !shCommitRecords(value.at("sharper_propose"),value.at("requests"),proof.at("commits"),seqs,claims,records)) return false;
            if(shChecked.size()>=12000) shChecked.clear();shChecked.insert("cert:"+fingerprint);return true;
        } catch(...) {return false;}
    }
    json shRequestProposal(const json& value) const {
        auto p=value.at("sharper_propose");p["requests"]=value.at("requests");return p;
    }
    void shSend(const json& p,const json& message) {
        for(int owner:p.at("body").at("participants")) sendShard(owner,message);
    }
    json shVotes(const std::map<int,std::map<int,json>>& groups,const json& p,const char* field=nullptr) const {
        (void)field;json out=json::array();
        for(int owner:p.at("body").at("participants")) {
            auto it=groups.find(owner);if(it==groups.end()) return json::array();
            std::map<std::string,json> grouped;
            for(const auto& [r,vote]:it->second) {
                (void)r;std::string d=vote.at("body").contains("record_digest")?
                    vote.at("body").at("seqs").dump()+vote.at("body").at("claims").dump()+vote.at("body").at("record_digest").get<std::string>():vote.at("body").at("claim_digest").get<std::string>();
                if(!grouped.count(d)) grouped[d]=json::array();grouped[d].push_back(vote);
            }
            json selected=json::array();for(const auto& [d,votes]:grouped) {(void)d;if(votes.size()>=3) {selected=votes;break;}}
            if(selected.empty()) return json::array();
            for(size_t i=0;i<3;++i) out.push_back(selected[i]);
        }
        return out;
    }
    json sharperPreparedProof(const json& assignment) const {
        const auto key=shKey(assignment.at("body").at("value").at("sharper_propose"));auto it=shBatches.find(key);
        return {{"sharper",true},{"proposal",assignment},{"prepares",it==shBatches.end()?json::array():shVotes(it->second.accepts,it->second.proposal)}};
    }
    json shPrefixVotes(const ShBatch& batch,int owner) const {
        json out=json::array();
        for(int previous:batch.proposal.at("body").at("participants")) {
            if(previous>=owner) break;
            auto it=batch.accepts.find(previous);if(it==batch.accepts.end()) return json::array();
            std::map<std::string,json> groups;
            for(const auto& [r,vote]:it->second) {
                (void)r;auto d=vote.at("body").at("claim_digest").get<std::string>();
                if(!groups.count(d)) groups[d]=json::array();groups[d].push_back(vote);
            }
            bool found=false;
            for(const auto& [d,votes]:groups) if(votes.size()>=3) {
                (void)d;for(int i=0;i<3;++i) out.push_back(votes[i]);found=true;break;
            }
            if(!found) return json::array();
        }
        return out;
    }
    bool shPrefixReady(const ShBatch& batch,int owner) const {
        size_t previous=0;for(int id:batch.proposal.at("body").at("participants")) if(id<owner) ++previous;
        return shPrefixVotes(batch,owner).size()==previous*3;
    }
    json shRecord(const json& p,int seq) const {
        json record={{"shard",shard},{"seq",seq},{"batch",shKey(p)},{"request_digest",p.at("body").at("request_digest")},
                     {"reads",json::object()},{"writes",json::array()}};
        for(const auto& request:p.at("requests")) for(const auto& tx:request.at("body").at("txs")) {
            std::string id=tx.at("id");const auto access=accessFor(tx,shard);std::string key=access.at("key");
            bool duplicate=state.at("seen").contains(id);
            if(duplicate && state.at("seen").at(id).at("tx_digest")!=hash(tx.dump())) throw std::runtime_error("SharPer transaction ID conflicts with committed transaction");
            if(!duplicate && !record["reads"].contains(key)) record["reads"][key]=state.at("kv").contains(key)?state.at("kv").at(key):initialAccount();
            record["writes"].push_back({{"id",id},{"tx_digest",hash(tx.dump())},{"key",key},{"value",access.at("value")},{"duplicate",duplicate}});
        }
        return record;
    }
    void shRemember(const json& p) {
        bool idle=pending.empty() && shLocal.empty() && shInitiating.empty();
        auto key=shKey(p);auto [it,inserted]=shBatches.emplace(key,ShBatch{});
        if(inserted) {it->second.proposal=p;shPending[key]=p;}
        if(p.at("body").at("shard")==shard && !it->second.complete) {
            shInitiating=key;
            for(const auto& request:p.at("requests")) {
                auto rid=request.at("body").at("id").get<std::string>();shSelected.insert(rid);
                if(!pending.count(rid) && !state.at("requests").contains(rid)) {
                    pending[rid]=request;
                    for(const auto& tx:request.at("body").at("txs")) if(!state.at("seen").contains(tx.at("id").get<std::string>()))
                        pendingTx.emplace(tx.at("id"),std::make_pair(hash(tx.dump()),rid));
                }
            }
            if(inserted && idle) {lastProgress=Clock::now();batchStart=lastProgress;}
        }
    }
    void sharperAcceptAssignment(const json& pp,bool recovered=false) {
        const auto& b=pp.at("body");int n=b.at("seq");const auto& value=b.at("value");
        if(changing || b.at("view")!=view || n<=applied || n>stableSeq+window || !validSignedProposal(pp) || !sharperValidValue(value)) return;
        auto p=shRequestProposal(value);auto key=shKey(p);
        if(n!=applied+1) {catchupTarget=std::max(catchupTarget,n-1);return;}
        const auto& claim=value.at("sharper_claim");
        if(claim.at("body").at("seq")!=n || (!recovered && claim.at("body").at("view")!=view)) return;
        if(!shLocal.empty() && shLocal!=key) return;
        auto previous=slots.find(n);
        if(previous!=slots.end() && !previous->second.proposal.is_null() && previous->second.proposal.at("body").at("value")!=value) return;
        shRemember(p);auto& batch=shBatches.at(key);
        if(pp.contains("sharper_prefix")) for(const auto& vote:pp.at("sharper_prefix")) {
            if(!shValidAccept(vote,p) || vote.at("body").at("shard").get<int>()>=shard) return;
            batch.accepts[vote.at("body").at("shard").get<int>()][vote.at("body").at("from").get<int>()]=vote;
        }
        if(!shPrefixReady(batch,shard)) return;
        for(const auto& request:p.at("requests")) {
            auto rid=request.at("body").at("id").get<std::string>();
            if(state.at("requests").contains(rid) && state.at("requests").at(rid).at("txs_hash")!=hash(request.at("body").at("txs").dump())) return;
            for(const auto& tx:request.at("body").at("txs")) {
                auto id=tx.at("id").get<std::string>();
                if(state.at("seen").contains(id) && state.at("seen").at(id).at("tx_digest")!=hash(tx.dump())) return;
            }
        }
        if(!batch.assignment.is_null() && batch.assignment.at("body").at("value")!=value) return;
        bool fresh=batch.assignment.is_null();batch.assignment=pp;shLocal=key;shPending.erase(key);slots[n].proposal=pp;
        if(fresh) {lastProgress=Clock::now();log("sharper_assigned",{{"seq",n},{"batch",key}});}
        if(batch.accept.is_null()) batch.accept=make("SH_ACCEPT",{{"batch",key},{"request_digest",p.at("body").at("request_digest")},
            {"origin_seq",p.at("body").at("origin_seq")},{"local_seq",n},{"claim",claim},{"claim_digest",hash(claim.dump())}});
        batch.accepts[shard][me]=batch.accept;shSend(p,batch.accept);sharperAdvance(n);
    }
    void sharperAdvance(int n) {
        auto si=slots.find(n);if(si==slots.end() || si->second.proposal.is_null()) return;
        auto key=shKey(si->second.proposal.at("body").at("value").at("sharper_propose"));auto bi=shBatches.find(key);
        if(bi==shBatches.end() || bi->second.applied) return;
        auto& batch=bi->second;auto proof=sharperPreparedProof(batch.assignment);json seqs,claims;
        if(!shAcceptVector(batch.proposal,proof.at("prepares"),seqs,claims) ||
           claims.at(std::to_string(shard))!=hash(batch.assignment.at("body").at("value").at("sharper_claim").dump())) return;
        auto& slot=si->second;
        if(!slot.prepared) {slot.prepared=true;preparedHistory[n]=proof;log("sharper_accepted",{{"seq",n},{"batch",key}});lastProgress=Clock::now();}
        if(batch.commit.is_null() && n==applied+1) {
            auto record=shRecord(batch.proposal,n);
            batch.commit=make("SH_COMMIT",{{"batch",key},{"request_digest",batch.proposal.at("body").at("request_digest")},
                {"seqs",seqs},{"claims",claims},{"record",record},{"record_digest",hash(record.dump())}});
            slot.commitSent=true;batch.commits[shard][me]=batch.commit;shSend(batch.proposal,batch.commit);
        }
        auto commits=shVotes(batch.commits,batch.proposal);json records;
        if(!shCommitRecords(batch.proposal,batch.proposal.at("requests"),commits,seqs,claims,records)) return;
        proof["commits"]=commits;proof["seqs"]=seqs;proof["claims"]=claims;
        if(!sharperValidCertificate(proof)) throw std::runtime_error("invalid SharPer commit proof");
        if(records.at(std::to_string(shard))!=shRecord(batch.proposal,n)) throw std::runtime_error("SharPer local read state changed before execution");
        if(!slot.committed) {slot.committed=true;certificates[n]=proof;preparedHistory[n]=proof;applyReady();}
    }
    void sharperApply(const json& value,const json& certificate,int n) {
        if(!sharperCrossValue(value)) {applyClientRequests(value.at("requests"),n);return;}
        auto p=shRequestProposal(value);auto key=shKey(p);json seqs,claims,records;
        if(!shAcceptVector(p,certificate.at("prepares"),seqs,claims) ||
           !shCommitRecords(p,value.at("requests"),certificate.at("commits"),seqs,claims,records)) throw std::runtime_error("invalid SharPer execution certificate");
        if(records.at(std::to_string(shard))!=shRecord(p,n)) throw std::runtime_error("SharPer checkpoint prefix differs from committed reads");
        std::map<int,json> working;std::map<std::string,bool> duplicate;
        for(int owner:p.at("body").at("participants")) {
            const auto& record=records.at(std::to_string(owner));working[owner]=record.at("reads");
            for(const auto& write:record.at("writes")) duplicate[write.at("id")]=write.at("duplicate").get<bool>();
        }
        json globalResults=json::array();
        for(const auto& request:value.at("requests")) {
            json localResults=json::array();auto rid=request.at("body").at("id").get<std::string>();
            for(const auto& tx:request.at("body").at("txs")) {
                std::string id=tx.at("id");
                if(duplicate.at(id)) {
                    auto result=state.at("seen").at(id).at("result");result["kind"]="duplicate";localResults.push_back(result);
                    globalResults.push_back({{"id",id},{"kind","duplicate"}});continue;
                }
                std::map<int,json> old,next;const auto fib=fibonacci();
                for(int owner:tx.at("participants")) old[owner]=working.at(owner).at(accessFor(tx,owner).at("key").get<std::string>());
                json states=json::object();
                for(const auto& [owner,previous]:old) {
                    uint64_t result=accessFor(tx,owner).at("value").get<uint64_t>()+fib;std::string dependency=previous.dump();
                    for(const auto& [remote,account]:old) if(remote!=owner) {result+=account.at("value").get<uint64_t>();dependency+=account.dump();}
                    next[owner]={{"version",previous.at("version").get<uint64_t>()+1},{"value",result},{"fib",fib},{"digest",hash(dependency+tx.dump()+std::to_string(owner))}};
                    states[std::to_string(owner)]=next[owner];
                }
                for(const auto& [owner,account]:next) working[owner][accessFor(tx,owner).at("key").get<std::string>()]=account;
                json result={{"id",id},{"seq",n},{"kind","executed"},{"digest",next.at(shard).at("digest")}};
                state["seen"][id]={{"tx_digest",hash(tx.dump())},{"result",result},{"sharper_batch",key}};stateDigest.markEntry("seen",id);
                state["cst_seen"][id]={{"tx_digest",hash(tx.dump())},{"committed",true},{"sharper_batch",key}};stateDigest.markEntry("cst_seen",id);
                state["executed"]=state["executed"].get<uint64_t>()+1;
                state["leaf_ordered_cst"]=state["leaf_ordered_cst"].get<uint64_t>()+1;
                localResults.push_back(result);globalResults.push_back({{"id",id},{"kind","executed"},{"digest",hash(states.dump())}});changedTx.insert(id);
            }
            state["requests"][rid]={{"txs_hash",hash(request.at("body").at("txs").dump())},{"results",localResults}};stateDigest.markEntry("requests",rid);
        }
        for(auto it=working.at(shard).begin();it!=working.at(shard).end();++it) {state["kv"][it.key()]=it.value();stateDigest.markEntry("kv",it.key());kvDirty=true;}
        state["sharper_batches"][key]={{"proposal",p},{"seq",n},{"seqs",seqs},{"claims",claims},{"results",globalResults}};stateDigest.markEntry("sharper_batches",key);
        if(p.at("body").at("shard")==shard) {state["sharper_initiated"]=state["sharper_initiated"].get<uint64_t>()+globalResults.size();stateDigest.markField("sharper_initiated");}
    }
    void shSendExecuted(const std::string& key) {
        const auto& stored=state.at("sharper_batches").at(key);auto& batch=shBatches[key];batch.proposal=stored.at("proposal");batch.applied=true;
        if(batch.executed.is_null()) batch.executed=make("SH_EXECUTED",{{"batch",key},{"request_digest",batch.proposal.at("body").at("request_digest")},
            {"seqs",stored.at("seqs")},{"claims",stored.at("claims")},{"result_digest",hash(stored.at("results").dump())}});
        int origin=batch.proposal.at("body").at("shard");sendShard(origin,batch.executed);
        if(origin==shard) {batch.done[shard][me]=batch.executed;shFinish(key);}
    }
    void sharperAfterApply(const json& value,const json& certificate,int n) {
        (void)certificate;(void)n;
        if(!sharperCrossValue(value)) return;
        auto key=shKey(value.at("sharper_propose"));shLocal.clear();shPending.erase(key);++shCommitted;
        shSendExecuted(key);
        // A small authenticated hint lets a lagging local replica use the
        // shared certificate/checkpoint recovery path; no extra agreement.
        broadcast(make("SH_APPLIED",{{"seq",n},{"batch",key}}));
        log("sharper_executed",{{"batch",key},{"seq",n}});
    }
    bool shValidDone(const json& e,const json& stored) const {
        try {
            const auto& b=e.at("body");const auto& p=stored.at("proposal");
            return shSigned(e) && b.at("type")=="SH_EXECUTED" && b.at("batch")==shKey(p) &&
                touches(json{{"participants",p.at("body").at("participants")}},b.at("shard").get<int>()) &&
                b.at("request_digest")==p.at("body").at("request_digest") && b.at("seqs")==stored.at("seqs") &&
                b.at("claims")==stored.at("claims") && b.at("result_digest")==hash(stored.at("results").dump());
        } catch(...) {return false;}
    }
    void shFinish(const std::string& key) {
        if(!state.at("sharper_batches").contains(key)) return;
        auto& batch=shBatches.at(key);if(batch.complete) return;
        const auto& stored=state.at("sharper_batches").at(key);const auto& p=stored.at("proposal");
        if(p.at("body").at("shard")!=shard) return;
        for(int owner:p.at("body").at("participants")) {
            size_t count=0;for(const auto& [r,e]:batch.done[owner]) {(void)r;if(shValidDone(e,stored)) ++count;}
            if(count<3) return;
        }
        batch.complete=true;if(shInitiating==key) shInitiating.clear();
        size_t i=0;
        for(const auto& request:p.at("requests")) {
            json results=json::array();
            for(const auto& tx:request.at("body").at("txs")) {
                auto result=stored.at("results")[i++];auto id=tx.at("id").get<std::string>();
                if(!completedTxResults.count(id) || result.at("kind")=="executed") completedTxResults[id]=result;
                results.push_back(result);changedTx.insert(tx.at("id"));
            }
            auto rid=request.at("body").at("id").get<std::string>();completedCstResults[rid]=results;
            reply(request,results);erasePending(rid);shSelected.erase(rid);
        }
        log("sharper_complete",{{"batch",key}});
    }
    bool sharperReplyCompleted(const json& req) {
        auto rid=req.at("body").at("id").get<std::string>();
        if(completedCstResults.count(rid)) {reply(req,completedCstResults.at(rid));++duplicates;return true;}
        json results=json::array();
        bool original=state.at("requests").contains(rid);
        for(const auto& tx:req.at("body").at("txs")) {
            std::string id=tx.at("id");
            if(state.at("seen").contains(id) && state.at("seen").at(id).at("tx_digest")!=hash(tx.dump())) {replyConflict(req);++rejected;return true;}
            if(!state.at("seen").contains(id) || awaitingResult(id)) return false;
            auto result=completedTxResults.count(id)?completedTxResults.at(id):state.at("seen").at(id).at("result");
            if(!original) result["kind"]="duplicate";results.push_back(result);
        }
        reply(req,results);duplicates+=results.size();return true;
    }
    void sharperClient(const json& request) {
        if(!validRequest(request)) {++rejected;return;}
        const auto& b=request.at("body");std::string rid=b.at("id");
        for(const auto& tx:b.at("txs")) {
            std::string id=tx.at("id"),digest=hash(tx.dump());
            if((state.at("seen").contains(id) && state.at("seen").at(id).at("tx_digest")!=digest) ||
               (pendingTx.count(id) && pendingTx.at(id).first!=digest)) {replyConflict(request);++rejected;return;}
        }
        if(state.at("requests").contains(rid) && state.at("requests").at(rid).at("txs_hash")!=hash(b.at("txs").dump())) {replyConflict(request,"request_id_conflict");++rejected;return;}
        for(const auto* queue:{&pending,&deferredRequests}) {
            auto it=queue->find(rid);if(it!=queue->end()) {
                if(it->second.at("body").at("txs")!=b.at("txs")) {replyConflict(request,"request_id_conflict");++rejected;}
                else ++duplicates;return;
            }
        }
        if(sharperReplyCompleted(request)) return;
        if(pending.size()+deferredRequests.size()>=10000) {++rejected;return;}
        for(const auto& tx:b.at("txs")) {
            std::string id=tx.at("id");
            if(pendingTx.count(id) || awaitingResult(id)) {deferRequest(request);if(!isForward()) sendTo(shard,view%4,request);return;}
        }
        bool idle=pending.empty() && shLocal.empty();pending[rid]=request;
        for(const auto& tx:b.at("txs")) if(!state.at("seen").contains(tx.at("id").get<std::string>()))
            pendingTx.emplace(tx.at("id"),std::make_pair(hash(tx.dump()),rid));
        if(idle) {batchStart=Clock::now();lastProgress=batchStart;}
        if(!isForward()) sendTo(shard,view%4,request);
    }
    void sharperPropose() {
        if(!isForward() || !members.leaves.count(shard) || applied+1>stableSeq+window || certificates.count(applied+1)) return;
        for(const auto& [key,p]:shPending) {
            if(state.at("sharper_batches").contains(key)) continue;
            if(!shLocal.empty() && shLocal!=key) continue;
            if(!shPrefixReady(shBatches.at(key),shard)) continue;
            if(slots.count(applied+1) && !slots.at(applied+1).proposal.is_null()) continue;
            if(p.at("body").at("shard")==shard && p.at("body").at("origin_seq")!=applied+1) continue;
            json claim=make("SH_LOCAL_CLAIM",{{"seq",applied+1},{"ballot",++shAttempt},{"batch",key},
                {"request_digest",p.at("body").at("request_digest")},{"origin_seq",p.at("body").at("origin_seq")},{"proposal_digest",hash(shHeader(p).dump())}});
            json value={{"requests",p.at("requests")},{"sharper_propose",shHeader(p)},{"sharper_claim",claim}};
            auto pp=make("PREPREPARE",{{"seq",applied+1},{"digest",hash(value.dump())},{"value",value}});
            pp["sharper_prefix"]=shPrefixVotes(shBatches.at(key),shard);
            sharperAcceptAssignment(pp);broadcast(pp);return;
        }
        if(!shLocal.empty() || (slots.count(applied+1) && !slots.at(applied+1).proposal.is_null()) || !shInitiating.empty()) return;
        int waited=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-batchStart).count();
        if(waited<members.config.at("consensus").at("batch_wait_ms").get<int>()) return;
        json requests=json::array(),group;int count=0;
        for(const auto& [rid,request]:pending) {
            if(shSelected.count(rid)) continue;
            bool blocked=false;for(const auto& tx:request.at("body").at("txs")) if(awaitingResult(tx.at("id"))) blocked=true;
            if(blocked) continue;
            auto ps=request.at("body").at("txs")[0].at("participants");int limit=ps.size()>1?crossShardBatchSize:batchSize;
            if(group.is_null()) group=ps;if(ps!=group) continue;
            if(count+int(request.at("body").at("txs").size())>limit) continue;
            requests.push_back(request);count+=request.at("body").at("txs").size();
        }
        if(requests.empty()) return;
        if(group.size()>1) {
            if(waited<crossShardBatchWaitMs && count<crossShardBatchSize) return;
            auto requestDigest=hash(requests.dump());
            auto key=hash(std::to_string(shard)+":"+std::to_string(applied+1)+":"+std::to_string(view)+":"+std::to_string(++shAttempt)+":"+requestDigest);
            auto p=make("SH_SUPER_PROPOSE",{{"batch",key},{"origin_seq",applied+1},{"participants",group},{"request_digest",requestDigest}});p["requests"]=requests;
            shRemember(p);shSend(p,p);lastProgress=Clock::now();
        } else {
            json value={{"requests",requests}};
            broadcast(make("PREPREPARE",{{"seq",applied+1},{"digest",hash(value.dump())},{"value",value}}));
        }
        batchStart=Clock::now();
    }
    bool sharperHandle(const json& e) {
        const auto& b=e.at("body");std::string type=b.at("type");
        if(type=="PREPARE" || type=="COMMIT") {
            int seq=b.at("seq");auto it=slots.find(seq);
            if(it!=slots.end() && !it->second.proposal.is_null() && sharperCrossValue(it->second.proposal.at("body").at("value"))) return true;
            return false;
        }
        if(type.rfind("SH_",0)!=0) return false;
        if(type=="SH_APPLIED") {
            int n=b.at("seq");
            if(b.at("shard")==shard && n>applied && n<=stableSeq+window) catchupTarget=std::max(catchupTarget,n);
            return true;
        }
        if(type=="SH_SUPER_PROPOSE") {
            if(!shValidPropose(e,e.at("requests")) || !touches(json{{"participants",b.at("participants")}},shard)) {++rejected;return true;}
            auto key=shKey(e);
            if(state.at("sharper_batches").contains(key)) {shSendExecuted(key);return true;}
            shRemember(e);return true;
        }
        if(type=="SH_QUERY_DONE") {
            auto key=b.at("batch").get<std::string>();
            if(state.at("sharper_batches").contains(key)) {
                const auto& p=state.at("sharper_batches").at(key).at("proposal");
                if(b.at("shard")==p.at("body").at("shard")) shSendExecuted(key);
            }
            return true;
        }
        auto key=b.at("batch").get<std::string>();auto it=shBatches.find(key);
        if(it==shBatches.end()) return true;
        auto& batch=it->second;
        if(type=="SH_ACCEPT") {
            if(!shValidAccept(e,batch.proposal)) {++rejected;return true;}
            bool wasReady=shPrefixReady(batch,shard);
            batch.accepts[b.at("shard").get<int>()][b.at("from").get<int>()]=e;
            if(!wasReady && shPrefixReady(batch,shard) && shLocal.empty()) lastProgress=Clock::now();
        } else if(type=="SH_COMMIT") {
            // Votes may arrive before the matching ACCEPTs. Authenticate their
            // payload now; require the complete matching vector before applying.
            if(!shSigned(e) || !shValidCommit(e,batch.proposal,batch.proposal.at("requests"),b.at("seqs"),b.at("claims"))) {++rejected;return true;}
            batch.commits[b.at("shard").get<int>()][b.at("from").get<int>()]=e;
        } else if(type=="SH_EXECUTED") {
            if(!touches(json{{"participants",batch.proposal.at("body").at("participants")}},b.at("shard").get<int>())) {++rejected;return true;}
            batch.done[b.at("shard").get<int>()][b.at("from").get<int>()]=e;shFinish(key);return true;
        } else {++rejected;return true;}
        if(!batch.assignment.is_null()) sharperAdvance(batch.assignment.at("body").at("seq"));return true;
    }
    json sharperRecoveryEvidence() const {
        json result={{"reservations",json::array()},{"initiations",json::array()},{"commits",json::array()}};
        for(const auto& [key,batch]:shBatches) {
            if(batch.applied) continue;
            if(!batch.assignment.is_null() && !batch.accept.is_null()) result["reservations"].push_back({{"proposal",batch.assignment},{"accept",batch.accept}});
            if(batch.proposal.at("body").at("shard")==shard) result["initiations"].push_back(batch.proposal);
            if(!batch.commit.is_null()) result["commits"].push_back(batch.commit);
        }
        return result;
    }
    bool sharperValidRecovery(const json& vc) const {
        try {
            const auto& evidence=vc.at("body").at("sharper_recovery");int voter=vc.at("body").at("from"),v=vc.at("body").at("view");
            for(const char* field:{"reservations","initiations","commits"}) if(!evidence.at(field).is_array() || evidence.at(field).size()>size_t(window)) return false;
            std::map<std::string,json> proposals,assignments;
            for(const auto& reservation:evidence.at("reservations")) {
                const auto& pp=reservation.at("proposal");const auto& value=pp.at("body").at("value");const auto& accept=reservation.at("accept");
                int seq=pp.at("body").at("seq"),h=vc.at("body").at("stable").at("seq");
                if(!validSignedProposal(pp) || !sharperValidValue(value) || pp.at("body").at("view").get<int>()>=v ||
                   seq<=h || seq>h+window || seq!=value.at("sharper_claim").at("body").at("seq") ||
                   !shValidAccept(accept,value.at("sharper_propose")) || accept.at("body").at("shard")!=shard || accept.at("body").at("from")!=voter ||
                   accept.at("body").at("claim_digest")!=hash(value.at("sharper_claim").dump())) return false;
                auto key=shKey(value.at("sharper_propose"));proposals[key]=shRequestProposal(value);assignments[key]=pp;
            }
            for(const auto& p:evidence.at("initiations")) {
                if(!shValidPropose(p,p.at("requests")) || p.at("body").at("shard")!=shard) return false;
                if(proposals.count(shKey(p)) && shHeader(proposals.at(shKey(p)))!=shHeader(p)) return false;
                proposals[shKey(p)]=p;
            }
            for(const auto& commit:evidence.at("commits")) {
                auto key=commit.at("body").at("batch").get<std::string>();
                if(!proposals.count(key) || !assignments.count(key) || commit.at("body").at("shard")!=shard || commit.at("body").at("from")!=voter ||
                   !shValidCommit(commit,proposals.at(key),proposals.at(key).at("requests"),commit.at("body").at("seqs"),commit.at("body").at("claims")) ||
                   commit.at("body").at("seqs").at(std::to_string(shard))!=assignments.at(key).at("body").at("seq") ||
                   commit.at("body").at("claims").at(std::to_string(shard))!=hash(assignments.at(key).at("body").at("value").at("sharper_claim").dump())) return false;
            }
            return true;
        } catch(...) {return false;}
    }
    void sharperAugmentRecovery(std::map<int,json>& selected,const json& vcs,int stable) const {
        std::map<int,std::map<std::string,json>> candidates;
        std::map<int,std::map<std::string,std::set<int>>> supporters;
        for(const auto& vc:vcs) for(const auto& reservation:vc.at("body").at("sharper_recovery").at("reservations")) {
            const auto& pp=reservation.at("proposal");const auto& b=pp.at("body");int seq=b.at("seq");
            if(seq<=stable) continue;
            std::string digest=b.at("digest");supporters[seq][digest].insert(vc.at("body").at("from").get<int>());
            auto& candidate=candidates[seq][digest];
            if(candidate.is_null() || b.at("view").get<int>()>candidate.at("body").at("view").get<int>()) candidate=pp;
        }
        // Full global prepared/committed evidence always outranks a partial
        // reservation. A lone Byzantine VC cannot invent a local lock.
        for(const auto& [seq,groups]:candidates) {
            if(selected.count(seq)) continue;
            for(const auto& [digest,pp]:groups) if(supporters.at(seq).at(digest).size()>=2) {selected[seq]={{"proposal",pp}};break;}
        }
    }
    void sharperInstallRecovery(const json& vcs) {
        for(const auto& vc:vcs) {
            const auto& evidence=vc.at("body").at("sharper_recovery");
            for(const auto& p:evidence.at("initiations")) shRemember(p);
            for(const auto& reservation:evidence.at("reservations")) {
                auto p=shRequestProposal(reservation.at("proposal").at("body").at("value"));auto key=shKey(p);
                shRemember(p);const auto& accept=reservation.at("accept");shBatches.at(key).accepts[shard][accept.at("body").at("from").get<int>()]=accept;
            }
            for(const auto& commit:evidence.at("commits")) {
                auto key=commit.at("body").at("batch").get<std::string>();auto it=shBatches.find(key);if(it==shBatches.end()) continue;
                it->second.commits[shard][commit.at("body").at("from").get<int>()]=commit;
                if(commit.at("body").at("from")==me) it->second.commit=commit;
            }
        }
    }
    void sharperRestore() {
        // A checkpoint never erases already-cast COMMIT votes in this run.
        // Restart in the same run is forbidden by the shared engine.
        if(!shLocal.empty() && state.at("sharper_batches").contains(shLocal)) shLocal.clear();
        for(auto it=state.at("sharper_batches").begin();it!=state.at("sharper_batches").end();++it) {
            auto& batch=shBatches[it.key()];batch.proposal=it.value().at("proposal");batch.applied=true;shPending.erase(it.key());
            shSendExecuted(it.key());
        }
    }
    bool sharperWaiting(Clock::time_point now) const {
        bool localWork=false;
        if(shLocal.empty()) {
            for(const auto& [key,p]:shPending) {(void)p;if(shPrefixReady(shBatches.at(key),shard)) {localWork=true;break;}}
            if(shInitiating.empty() && !pending.empty()) localWork=true;
        }
        // Missing remote votes cannot be fixed by rotating a healthy local
        // primary. Detect its missing heartbeat instead of a network deadline.
        bool primaryMissing=(!shLocal.empty() || !shInitiating.empty() || !shPending.empty() || !pending.empty()) &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now-lastForwardHeartbeat).count()>timeoutMs;
        return localWork || primaryMissing;
    }
    void sharperTick(Clock::time_point now) {
        if(std::chrono::duration_cast<std::chrono::milliseconds>(now-shLastRetry).count()<750) return;
        for(auto& [key,batch]:shBatches) {
            if(batch.complete) continue;
            if(batch.applied) {
                if(batch.proposal.at("body").at("shard")==shard) {
                    auto query=make("SH_QUERY_DONE",{{"batch",key}});shSend(batch.proposal,query);shSendExecuted(key);
                }
                continue;
            }
            if(isForward() && batch.proposal.at("body").at("shard")==shard) shSend(batch.proposal,batch.proposal);
            if(!batch.assignment.is_null()) {
                if(isForward() && batch.assignment.at("body").at("view")==view) broadcast(batch.assignment);
                if(!batch.accept.is_null()) shSend(batch.proposal,batch.accept);
                if(!batch.commit.is_null()) shSend(batch.proposal,batch.commit);
            }
        }
        shLastRetry=now;
    }
    void sharperStatus(json& s) const {
        size_t active=0,pendingCount=0,waiting=0;
        for(const auto& [key,batch]:shBatches) {
            if(!batch.applied && !batch.assignment.is_null()) ++active;
            if(!batch.applied && batch.assignment.is_null()) ++pendingCount;
            if(batch.applied && batch.proposal.at("body").at("shard")==shard && !batch.complete) ++waiting;
        }
        s["method"]="sharper";s["sharper_active_batches"]=active;s["sharper_pending_batches"]=pendingCount;
        s["sharper_waiting_execution"]=waiting;s["sharper_initiated_transactions"]=state.at("sharper_initiated");
        s["sharper_committed_batches"]=shCommitted;
    }
