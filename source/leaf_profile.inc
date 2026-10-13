    // Local diagnostics only: never part of replicated state or StateDigest.
    mutable uint64_t leafSchedulerElapsedNs=0,leafSelectionElapsedNs=0,leafFrontierValidationElapsedNs=0,
        leafQcValidationElapsedNs=0,leafTxnExecutionElapsedNs=0;
    mutable unsigned leafSchedulerDepth=0,leafSelectionDepth=0,leafFrontierValidationDepth=0,
        leafQcValidationDepth=0,leafTxnExecutionDepth=0;
    mutable uint64_t leafQcValidationCalls=0,leafQcCacheHits=0,leafQcFullValidations=0;
    uint64_t leafBusinessSlots=0,leafControlSlots=0,leafFrontierProofs=0,leafFrontierBytes=0,
        leafBusinessOrders=0,leafBundledSlots=0;
    mutable int leafSelectionBlockedCoordinator=-1;
    mutable std::vector<int> leafProfileAncestorCache;
    enum LeafWaitKind { LeafIdle,LeafRoot,LeafChild,LeafWitness,LeafLocalSlot,LeafOther,LeafWaitKinds };
    LeafWaitKind leafWaitKind=LeafIdle;
    Clock::time_point leafWaitSince=Clock::now();
    std::array<uint64_t,LeafWaitKinds> leafWaitNs{};
    struct LeafElapsedScope {
        uint64_t& total;unsigned& depth;bool outer=false;Clock::time_point begin;
        LeafElapsedScope(const Replica& replica,uint64_t& counter,unsigned& nesting):total(counter),depth(nesting) {
            if(!replica.members.leaves.count(replica.shard)) return;
            outer=depth++==0;if(outer) begin=Clock::now();
        }
        ~LeafElapsedScope() {
            // Coordinators leave nesting at zero. Nested scopes account once.
            if(!depth) return;
            if(outer) total+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count();
            --depth;
        }
    };
    const std::vector<int>& leafProfileAncestors() const {
        if(leafProfileAncestorCache.empty()) {
            const auto ancestors=members.ancestors(shard);
            leafProfileAncestorCache.assign(ancestors.begin(),ancestors.end());
            auto depth=[&](int co) {int n=0;for(int p=members.parent.at(co);p!=-1;p=members.parent.at(p)) ++n;return n;};
            std::sort(leafProfileAncestorCache.begin(),leafProfileAncestorCache.end(),[&](int a,int b) {
                return std::make_pair(depth(a),a)<std::make_pair(depth(b),b);
            });
        }
        return leafProfileAncestorCache;
    }
    int leafProfileMissingCoordinator(int block,bool manifests) const {
        const auto row=roundCertificates.find(block);
        for(int co:leafProfileAncestors())
            if(row==roundCertificates.end() || !row->second.count(co)) return co;
        if(manifests) for(int co:leafProfileAncestors()) {
            const auto& value=row->second.at(co).at("proposal").at("body").at("value");
            const auto& manifest=value.at("cst_window_manifest").at(std::to_string(shard));
            for(const auto& entry:manifest.at("orders")) {
                const auto cached=roundCertificates.find(entry.at("block").get<int>());
                if(cached==roundCertificates.end() || !cached->second.count(co)) return co;
            }
        }
        return -1;
    }
    LeafWaitKind leafProfileWaitState() const {
        if(!members.leaves.count(shard)) return LeafIdle;
        if(!stagedRecords.empty()) return LeafWitness;
        const auto slot=slots.find(applied+1);
        if(certificates.count(applied+1) || (slot!=slots.end() && !slot->second.proposal.is_null())) return LeafLocalSlot;
        if(pendingCst.empty()) return LeafIdle;
        if(changing) return LeafOther;
        if(!cstSelection.is_null()) return LeafLocalSlot;
        if(leafSelectionBlockedCoordinator>=0)
            return members.parent.at(leafSelectionBlockedCoordinator)==-1?LeafRoot:LeafChild;
        return LeafOther;
    }
    void leafProfileAccountWait(LeafWaitKind next,Clock::time_point now) {
        if(now>=leafWaitSince)
            leafWaitNs[leafWaitKind]+=std::chrono::duration_cast<std::chrono::nanoseconds>(now-leafWaitSince).count();
        leafWaitSince=now;leafWaitKind=next;
    }
    void leafProfileAppliedSlot(const json& value) {
        if(!members.leaves.count(shard)) return;
        if(value.contains("cst_orders")) {
            ++leafBusinessSlots;leafBusinessOrders+=value.at("cst_orders").size();
            if(value.at("cst_orders").size()>1) ++leafBundledSlots;
        }
        if(value.contains("cst_stream_progress")) ++leafControlSlots;
        if(value.contains("cst_frontier")) {
            leafFrontierProofs+=value.at("cst_frontier").size();
            leafFrontierBytes+=value.at("cst_frontier").dump().size();
        }
    }
    void leafProfileStatus(json& status) const {
        if(!members.leaves.count(shard)) return;
        auto waits=leafWaitNs;const auto now=Clock::now();
        if(now>=leafWaitSince) waits[leafWaitKind]+=std::chrono::duration_cast<std::chrono::nanoseconds>(now-leafWaitSince).count();
        status["leaf_profile"]={{"schema_version",1},{"wait_root_ns",waits[LeafRoot]},
            {"wait_child_ns",waits[LeafChild]},{"wait_witness_ns",waits[LeafWitness]},
            {"wait_local_slot_ns",waits[LeafLocalSlot]},{"wait_other_ns",waits[LeafOther]},
            {"scheduler_elapsed_ns",leafSchedulerElapsedNs},{"selection_elapsed_ns",leafSelectionElapsedNs},
            {"frontier_validation_elapsed_ns",leafFrontierValidationElapsedNs},{"qc_validation_elapsed_ns",leafQcValidationElapsedNs},
            {"qc_validation_calls",leafQcValidationCalls},{"qc_cache_hits",leafQcCacheHits},{"qc_full_validations",leafQcFullValidations},
            {"execution_elapsed_ns",executionNs},{"txn_execution_elapsed_ns",leafTxnExecutionElapsedNs},
            {"business_slots",leafBusinessSlots},{"business_orders",leafBusinessOrders},{"bundled_slots",leafBundledSlots},{"control_slots",leafControlSlots},{"frontier_proofs",leafFrontierProofs},
            {"frontier_bytes",leafFrontierBytes},{"current_wait",int(leafWaitKind)}};
    }
