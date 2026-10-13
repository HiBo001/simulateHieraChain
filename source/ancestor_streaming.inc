    // root_first retains the existing root_1, parent_1, root_2, parent_2
    // stream. lower_first consumes a configured number of blocks from each
    // lower ancestor before the next root block; every position is mandatory.
    // Only the leaf's own ancestors participate. The durable cursor advances
    // through leaf PBFT, including certified skips of non-applicable blocks.
    // The order is run configuration: snapshots must use the same order.
    static constexpr size_t STREAM_FRONTIER_LIMIT=32;
    bool ancestorStreamingEnabled() const {
        return windowsEnabled() && members.config.at("consensus").value("ancestor_streaming",false);
    }
    std::string streamAncestorOrder() const {
        return members.config.at("consensus").value("ancestor_order",std::string("root_first"));
    }
    uint64_t streamLowerBlocksPerRoot() const {
        return members.config.at("consensus").value("ancestor_lower_blocks_per_root",uint64_t(1));
    }
    uint64_t streamLeafBatchBlocks() const {
        return members.config.at("consensus").value("ancestor_leaf_batch_blocks",uint64_t(1));
    }
    bool streamLeafBatchEnabled() const {
        return ancestorStreamingEnabled() && streamLeafBatchBlocks()>1;
    }
    std::vector<int> streamAncestors() const {
        const auto ancestors=members.ancestors(shard);
        std::vector<int> result(ancestors.begin(),ancestors.end());
        const bool lowerFirst=streamAncestorOrder()=="lower_first";
        std::sort(result.begin(),result.end(),[&](int a,int b) {
            const int da=orderingDepth(a),db=orderingDepth(b);
            if(da!=db) return lowerFirst?da>db:da<db;
            return a<b;
        });
        return result;
    }
    uint64_t streamCursor() const { return state.value("cst_stream_cursor",uint64_t(0)); }
    std::pair<int,uint64_t> slotAtPosition(uint64_t position) const {
        const auto ancestors=streamAncestors();
        if(!position || ancestors.empty()) throw std::runtime_error("invalid ancestor stream position");
        const uint64_t offset=position-1;
        if(streamAncestorOrder()!="lower_first")
            return {ancestors[offset%ancestors.size()],offset/ancestors.size()+1};
        const uint64_t lowerCount=ancestors.size()-1,ratio=streamLowerBlocksPerRoot();
        if(!ratio || lowerCount>(UINT64_MAX-1)/ratio)
            throw std::runtime_error("ancestor stream cycle width overflow");
        const uint64_t width=lowerCount*ratio+1,cycle=offset/width,slot=offset%width;
        if(slot==width-1) return {ancestors.back(),cycle+1};
        // With no lower ancestor every position is the root branch above.
        if(streamLeafBatchEnabled())
            return {ancestors[slot/ratio],cycle*ratio+slot%ratio+1};
        return {ancestors[slot%lowerCount],cycle*ratio+slot/lowerCount+1};
    }
    uint64_t streamPosition(int coordinator,uint64_t block) const {
        const auto ancestors=streamAncestors();
        auto it=std::find(ancestors.begin(),ancestors.end(),coordinator);
        if(!block || it==ancestors.end()) throw std::runtime_error("invalid ancestor stream position");
        uint64_t cycle=block-1,width=ancestors.size(),slot=it-ancestors.begin();
        if(streamAncestorOrder()=="lower_first") {
            const uint64_t lowerCount=ancestors.size()-1,ratio=streamLowerBlocksPerRoot();
            if(!ratio || lowerCount>(UINT64_MAX-1)/ratio)
                throw std::runtime_error("ancestor stream cycle width overflow");
            width=lowerCount*ratio+1;
            if(it+1==ancestors.end()) slot=width-1;
            else {
                cycle=(block-1)/ratio;
                slot=streamLeafBatchEnabled()?slot*ratio+(block-1)%ratio:
                    ((block-1)%ratio)*lowerCount+slot;
            }
        }
        if(cycle>(UINT64_MAX-slot-1)/width) throw std::runtime_error("ancestor stream position overflow");
        return cycle*width+slot+1;
    }
    uint64_t streamBatchEndPosition(int coordinator,uint64_t block) const {
        const auto ancestors=streamAncestors();
        if(ancestors.empty()) throw std::runtime_error("invalid ancestor stream batch");
        if(coordinator==ancestors.back()) return streamPosition(coordinator,block);
        const uint64_t ratio=streamLowerBlocksPerRoot();
        if(!ratio || !block || (block-1)/ratio>=UINT64_MAX/ratio)
            throw std::runtime_error("ancestor stream batch end overflow");
        return streamPosition(coordinator,((block-1)/ratio+1)*ratio);
    }
    bool validStreamBatchFrontier(const json& frontier,const json& orders) const {
        LeafElapsedScope schedulerProfile(*this,leafSchedulerElapsedNs,leafSchedulerDepth);
        LeafElapsedScope frontierProfile(*this,leafFrontierValidationElapsedNs,leafFrontierValidationDepth);
        try {
            if(!streamLeafBatchEnabled() || !members.leaves.count(shard) || !frontier.is_array() ||
               frontier.empty() || frontier.size()>STREAM_FRONTIER_LIMIT || !orders.is_array() ||
               orders.size()>streamLeafBatchBlocks()) return false;
            std::map<int,int> projected;
            std::set<std::string> batchKeys;
            size_t applicableCount=0;
            int businessCoordinator=-1;
            uint64_t batchEnd=0,lastPosition=0;
            for(size_t i=0;i<frontier.size();++i) {
                if(streamCursor()>UINT64_MAX-i-1) return false;
                const uint64_t position=streamCursor()+i+1;
                const auto [co,block]=slotAtPosition(position);
                if(block>INT_MAX) return false;
                const auto& cert=frontier.at(i);const auto& body=cert.at("proposal").at("body");
                const auto& value=body.at("value");
                if(body.at("shard")!=co || value.at("cst_round")!=block || !validOrderClose(cert)) return false;
                if(!projected.count(co)) projected[co]=localProjection(co);
                const bool applicable=orderParticipants(cert).count(shard)>0;
                if(applicable) {
                    if(applicableCount>=orders.size() || !validCoordinatorCertificate(cert) ||
                       !validCoordinatorCertificate(orders.at(applicableCount)) ||
                       cert.at("proposal")!=orders.at(applicableCount).at("proposal") ||
                       state.at("cst_batches").contains(cstKey(cert)) ||
                       !batchKeys.insert(cstKey(cert)).second || projected.at(co)==INT_MAX) return false;
                    if(businessCoordinator<0) {
                        businessCoordinator=co;batchEnd=streamBatchEndPosition(co,block);
                    } else if(co!=businessCoordinator) return false;
                    ++projected.at(co);++applicableCount;
                }
                if(businessCoordinator>=0 && (co!=businessCoordinator || position>batchEnd)) return false;
                if(projection(value,shard)!=projected.at(co)) return false;
                lastPosition=position;
            }
            if(applicableCount!=orders.size()) return false;
            // A lower-layer business cannot consume the first half of its
            // fixed pair. Its second certificate is required even if that
            // block is empty or does not involve this leaf. Root is one slot.
            return orders.empty() || lastPosition==batchEnd;
        } catch(...) {return false;}
    }
    bool validStreamFrontier(const json& frontier,const json* order=nullptr) const {
        if(streamLeafBatchEnabled())
            return validStreamBatchFrontier(frontier,order?json::array({*order}):json::array());
        LeafElapsedScope schedulerProfile(*this,leafSchedulerElapsedNs,leafSchedulerDepth);
        LeafElapsedScope frontierProfile(*this,leafFrontierValidationElapsedNs,leafFrontierValidationDepth);
        try {
            if(!ancestorStreamingEnabled() || !members.leaves.count(shard) || !frontier.is_array() ||
               frontier.empty() || frontier.size()>STREAM_FRONTIER_LIMIT) return false;
            if(!order && frontier.size()!=STREAM_FRONTIER_LIMIT) return false;
            if(order && !validCoordinatorCertificate(*order)) return false;
            for(size_t i=0;i<frontier.size();++i) {
                if(streamCursor()>UINT64_MAX-i-1) return false;
                const uint64_t position=streamCursor()+i+1;
                const auto [co,block]=slotAtPosition(position);
                if(block>INT_MAX) return false;
                const auto& cert=frontier.at(i);const auto& body=cert.at("proposal").at("body");
                const auto& value=body.at("value");
                if(body.at("shard")!=co || value.at("cst_round")!=block || !validOrderClose(cert)) return false;
                const bool applicable=orderParticipants(cert).count(shard)>0;
                const bool last=order && i+1==frontier.size();
                if(last) {
                    if(!applicable || !validCoordinatorCertificate(cert) ||
                       cert.at("proposal")!=order->at("proposal") ||
                       state.at("cst_batches").contains(cstKey(cert)) ||
                       projection(value,shard)!=localProjection(co)+1) return false;
                } else if(applicable || projection(value,shard)!=localProjection(co)) return false;
            }
            return true;
        } catch(...) {return false;}
    }
    const json& nextStreamingCstOrder() const {
        ++cstSelectionLookups;
        if(!cstSelectionDirty) return cstSelection;
        ++cstSelectionRebuilds;cstSelection=nullptr;leafSelectionBlockedCoordinator=-1;
        if(!members.leaves.count(shard) || pendingCst.empty()) {cstSelectionDirty=false;return cstSelection;}
        if(streamAncestors().empty()) {cstSelectionDirty=false;return cstSelection;}
        json frontier=json::array(),orders=json::array();
        int businessCoordinator=-1;
        uint64_t batchEnd=0;
        for(size_t i=0;i<STREAM_FRONTIER_LIMIT;++i) {
            if(streamCursor()>UINT64_MAX-i-1) break;
            const uint64_t position=streamCursor()+i+1;
            const auto [co,block]=slotAtPosition(position);
            if(block>INT_MAX) break;
            auto row=roundCertificates.find(int(block));
            // Never inspect a later slot if the fixed next slot is absent.
            if(row==roundCertificates.end() || !row->second.count(co)) {leafSelectionBlockedCoordinator=co;break;}
            const auto& cert=row->second.at(co);
            if(streamLeafBatchEnabled() && orders.empty() && !frontier.empty() &&
               streamBatchEndPosition(co,block)-streamCursor()>STREAM_FRONTIER_LIMIT) {
                // Keep the complete lower pair in the next proposal when a
                // long non-applicable prefix leaves only one proof slot. A
                // shorter all-empty control prefix is safe in bundle mode.
                if(validStreamBatchFrontier(frontier,orders))
                    cstSelection={{"certificate",nullptr},{"certificates",orders},{"frontier",frontier}};
                break;
            }
            frontier.push_back(cert);
            if(streamLeafBatchEnabled()) {
                if(orderParticipants(cert).count(shard)) {
                    if(businessCoordinator<0) {
                        businessCoordinator=co;batchEnd=streamBatchEndPosition(co,block);
                    }
                    orders.push_back(cert);
                }
                if(businessCoordinator>=0 && position==batchEnd) {
                    if(validStreamBatchFrontier(frontier,orders))
                        cstSelection={{"certificate",orders.back()},{"certificates",orders},{"frontier",frontier}};
                    break;
                }
                if(frontier.size()==STREAM_FRONTIER_LIMIT) {
                    if(orders.empty() && validStreamBatchFrontier(frontier,orders))
                        cstSelection={{"certificate",nullptr},{"certificates",orders},{"frontier",frontier}};
                    break;
                }
                continue;
            }
            if(orderParticipants(cert).count(shard)) {
                if(validStreamFrontier(frontier,&cert))
                    cstSelection={{"certificate",cert},{"frontier",frontier}};
                break;
            }
            if(projection(cert.at("proposal").at("body").at("value"),shard)!=localProjection(co)) break;
            // Bound proof size. A real leaf PBFT control slot durably consumes
            // this full prefix before selecting another prefix or business.
            if(frontier.size()==STREAM_FRONTIER_LIMIT && validStreamFrontier(frontier))
                cstSelection={{"certificate",nullptr},{"frontier",frontier}};
        }
        cstSelectionDirty=false;return cstSelection;
    }
    void applyStreamFrontier(const json& frontier) {
        const auto& body=frontier.back().at("proposal").at("body");
        state["cst_stream_cursor"]=streamPosition(body.at("shard"),body.at("value").at("cst_round"));
        stateDigest.markField("cst_stream_cursor");
        log("ancestor_stream_advanced",{{"cursor",streamCursor()},{"block",body.at("value").at("cst_round")},
                                        {"coordinator",body.at("shard")},{"proofs",frontier.size()}});
    }
    void queryStreamingRounds() {
        if(pendingCst.empty() && stagedRecords.empty()) return;
        if(streamAncestors().empty()) return;
        std::map<int,json> missing;
        for(size_t i=0;i<STREAM_FRONTIER_LIMIT;++i) {
            if(streamCursor()>UINT64_MAX-i-1) break;
            const auto [co,block]=slotAtPosition(streamCursor()+i+1);
            if(block>INT_MAX) break;
            auto row=roundCertificates.find(int(block));
            if(row==roundCertificates.end() || !row->second.count(co)) {
                if(!missing.count(co)) missing[co]=json::array();
                missing[co].push_back(block);
            }
        }
        for(const auto& [co,blocks]:missing) sendShard(co,make("CST_ROUND_QUERY",
            {{"target",co},{"rounds",blocks},{"leaf",shard},{"after_index",localProjection(co)}}));
    }
