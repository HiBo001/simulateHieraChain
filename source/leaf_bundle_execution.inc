    // Local preview results belong to a committed leaf slot, never to the
    // replicated state. The caller clears its entries after the slot applies
    // or a stable-state installation invalidates the preview.
    std::map<std::string,json> leafCstOutcomeCache;

    json cstExecutionOutcome(const json& witness) const {
        LeafElapsedScope txnProfile(*this,leafTxnExecutionElapsedNs,leafTxnExecutionDepth);
        const auto key=witness.at("batch_key").get<std::string>();
        const auto mine=witnessRecord(witness,shard);
        if(recordPayload(mine)!=recordPayload(stagedRecords.at(key)))
            throw std::runtime_error("local execution record differs from certified dependency");
        std::map<int,json> working;std::map<std::string,bool> flags;
        for(const auto& proof:witness.at("proofs")) {
            const auto& record=proof.at("record");
            working[record.at("shard").get<int>()]=record.at("reads");
            for(const auto& write:record.at("writes"))
                flags[write.at("id")]=write.at("duplicate").get<bool>();
        }
        json localWrites=json::array(),seen=json::object();
        const auto& requests=mine.at("order_certificate").at("proposal").at("body").at("value").at("requests");
        for(const auto& request:requests) for(const auto& tx:request.at("body").at("txs")) {
            if(flags.at(tx.at("id").get<std::string>())) continue;
            std::map<int,json> old,next;
            for(auto participant:tx.at("participants")) {
                const int owner=participant;const auto access=accessFor(tx,owner);
                old[owner]=working.at(owner).at(access.at("key").get<std::string>());
            }
            for(const auto& [owner,previous]:old) {
                const auto access=accessFor(tx,owner);
                uint64_t result=access.at("value").get<uint64_t>()+expectedFib;
                std::string dependency=previous.dump();
                for(const auto& [remote,account]:old) if(remote!=owner) {
                    result+=account.at("value").get<uint64_t>();dependency+=account.dump();
                }
                next[owner]={{"version",previous.at("version").get<uint64_t>()+1},{"value",result},{"fib",expectedFib},
                    {"digest",hash(dependency+tx.dump()+std::to_string(owner))}};
            }
            for(const auto& [owner,account]:next)
                working[owner][accessFor(tx,owner).at("key").get<std::string>()]=account;
            if(!touches(tx,shard)) continue;
            const auto access=accessFor(tx,shard);
            localWrites.push_back({{"id",tx.at("id")},{"key",access.at("key")},{"state",next.at(shard)}});
            seen[tx.at("id").get<std::string>()]={{"tx_digest",hash(tx.dump())},{"coordinator_batch",key},{"committed",true}};
        }
        return {{"kv",working.at(shard)},{"seen",seen},{"local_writes",localWrites},
            {"finalized",{{"order_digest",mine.at("order_digest")},{"execution_digest",executionDigest(witness)},
                {"result_digest",hash(localWrites.dump())}}}};
    }

    // Apply only once every business order in the committed leaf slot has a
    // matching witness. Computing the outcome above changes no state, proof
    // cache, ACK queue, or log; a second order can stage against its overlay.
    void applyCstExecutionOutcome(const json& witness,const json& outcome) {
        LeafElapsedScope txnProfile(*this,leafTxnExecutionElapsedNs,leafTxnExecutionDepth);
        const auto key=witness.at("batch_key").get<std::string>();
        const auto mine=witnessRecord(witness,shard);
        if(recordPayload(mine)!=recordPayload(stagedRecords.at(key)))
            throw std::runtime_error("local execution record differs from certified dependency");
        const auto& localWrites=outcome.at("local_writes");
        for(auto it=outcome.at("seen").begin();it!=outcome.at("seen").end();++it) {
            state["cst_seen"][it.key()]=it.value();stateDigest.markEntry("cst_seen",it.key());
        }
        state["executed"]=state["executed"].get<uint64_t>()+localWrites.size();
        state["leaf_ordered_cst"]=state["leaf_ordered_cst"].get<uint64_t>()+localWrites.size();
        for(auto it=outcome.at("kv").begin();it!=outcome.at("kv").end();++it) {
            state["kv"][it.key()]=it.value();stateDigest.markEntry("kv",it.key());kvDirty=true;
        }
        state["cst_finalized"][key]=outcome.at("finalized");stateDigest.markEntry("cst_finalized",key);
        executionWitnesses[key]=witness;stagedRecords.erase(key);preparedProofs.erase(key);
        if(!ackProofs.count(key) || !ackProofs.at(key).count(shard)) pendingLocalAckQcs.insert(key);
        for(auto it=preparedVotes.begin();it!=preparedVotes.end();)
            it=it->first.rfind(key+"|",0)==0?preparedVotes.erase(it):std::next(it);
        log("cst_executed",{{"batch_key",key},{"transactions",localWrites.size()}});
    }
