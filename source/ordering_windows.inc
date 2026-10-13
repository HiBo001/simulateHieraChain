    // This file is included inside Replica and only in the Arbor binary.
    // A run ID binds the immutable membership/configuration epoch. Windows
    // use coordinator logical blocks, including PBFT-certified empty blocks.
    Clock::time_point windowLastBlock=Clock::now();
    int windowPauseBlock=-1;
    std::string windowReadyHeadKey;
    int windowReadyHeadApplied=-1;
    uint64_t windowWorkTimerStarts=0;
    bool windowsEnabled() const {
        return members.multiLayer() && orderingWindowBlocks()>0;
    }
    int orderingWindowBlocks() const {
        return members.config.at("consensus").value("ordering_window_blocks",0);
    }
    int windowOf(int block) const {
        const int m=orderingWindowBlocks();
        return block>0 && m>0 ? 1+(block-1)/m : 0;
    }
    int orderingDepth(int co) const {
        int depth=0;
        for(int p=members.parent.at(co);p!=-1;p=members.parent.at(p)) ++depth;
        return depth;
    }
    int windowEmptyIntervalMs() const {
        return members.config.at("consensus").value("empty_block_interval_ms",50);
    }
    bool windowEmptyDue(Clock::time_point now) const {
        if(!windowsEnabled() || !isCoordinator()) return false;
        const int block=state.at("cst_round").get<int>();
        if(windowPauseBlock>=0 && block>=windowPauseBlock && block%orderingWindowBlocks()==0) return false;
        return std::chrono::duration_cast<std::chrono::milliseconds>(now-windowLastBlock).count()>=windowEmptyIntervalMs();
    }
#include "ancestor_streaming.inc"
    json windowManifestEntry(const json& value,int seq,int leaf) const {
        return {{"block",value.at("cst_round")},{"seq",seq},{"index",value.at("cst_order_index")},
                {"projection",projection(value,leaf)}, {"request_digest",hash(value.at("requests").dump())}};
    }
    json buildWindowManifest(const json& current,int seq) const {
        const int last=current.at("cst_round"),m=orderingWindowBlocks(),first=last-m+1;
        if(last<=0 || last%m!=0) throw std::runtime_error("manifest requires a closing window block");
        json manifest=json::object();
        for(int leaf:members.descendants(shard)) {
            int start=0;
            if(first>1) {
                const auto& previous=state.at("cst_rounds").at(std::to_string(first-1));
                start=projection(previous.at("value"),leaf);
            }
            json orders=json::array();
            for(int block=first;block<=last;++block) {
                const json* value=&current;int position=seq;
                if(block!=last) {
                    const auto& old=state.at("cst_rounds").at(std::to_string(block));
                    value=&old.at("value");position=old.at("seq");
                }
                if(participants(value->at("requests")).count(leaf)) orders.push_back(windowManifestEntry(*value,position,leaf));
            }
            manifest[std::to_string(leaf)]={{"start",start},{"end",projection(current,leaf)},{"orders",orders}};
        }
        return manifest;
    }
    bool validWindowManifestShape(const json& value,int origin,int seq) const {
        try {
            const int block=value.at("cst_round"),m=orderingWindowBlocks();
            const bool closes=block%m==0;
            if(!closes) return !value.contains("cst_window_manifest");
            const auto& manifest=value.at("cst_window_manifest");
            const auto descendants=members.descendants(origin);
            if(!manifest.is_object() || manifest.size()!=descendants.size()) return false;
            const auto currentParticipants=participants(value.at("requests"));
            std::map<int,json> referenced;
            for(int leaf:descendants) {
                const auto& row=manifest.at(std::to_string(leaf));
                if(!row.is_object() || row.size()!=3 || !row.at("start").is_number_integer() ||
                   !row.at("end").is_number_integer() || !row.at("orders").is_array()) return false;
                const int start=row.at("start"),end=row.at("end");
                if(start<0 || end<start || end!=projection(value,leaf) ||
                   row.at("orders").size()>size_t(m) || row.at("orders").size()!=size_t(end-start)) return false;
                int oldBlock=block-m,oldSeq=0,oldIndex=0,index=start;
                bool currentFound=false;
                for(const auto& entry:row.at("orders")) {
                    if(!entry.is_object() || entry.size()!=5 || !entry.at("block").is_number_integer() ||
                       !entry.at("seq").is_number_integer() || !entry.at("index").is_number_integer() ||
                       !entry.at("projection").is_number_integer() || !entry.at("request_digest").is_string()) return false;
                    const int n=entry.at("block"),s=entry.at("seq"),i=entry.at("index");
                    const auto digest=entry.at("request_digest").get<std::string>();
                    if(n<=oldBlock || n>block || s<=oldSeq || s>seq || i<=oldIndex ||
                       entry.at("projection")!=++index || digest.size()!=64 ||
                       !std::all_of(digest.begin(),digest.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) return false;
                    // The same coordinator block has one request payload and
                    // PBFT position across all of its participating leaves.
                    json identity={{"seq",s},{"index",i},{"request_digest",digest}};
                    if(referenced.count(n) && referenced.at(n)!=identity) return false;
                    referenced[n]=identity;oldBlock=n;oldSeq=s;oldIndex=i;
                    if(n==block) {
                        if(s!=seq || !currentParticipants.count(leaf) || !value.contains("cst_order_index") ||
                           i!=value.at("cst_order_index") || digest!=hash(value.at("requests").dump())) return false;
                        currentFound=true;
                    }
                }
                if(currentFound!=(currentParticipants.count(leaf)>0)) return false;
            }
            return true;
        } catch(...) {return false;}
    }
    bool validLocalWindowValue(const json& value) const {
        try {
            if(!value.contains("cst_round")) return !value.contains("cst_window_manifest");
            if(!validWindowManifestShape(value,shard,applied+1)) return false;
            if(value.at("cst_round").get<int>()%orderingWindowBlocks()==0)
                return value.at("cst_window_manifest")==buildWindowManifest(value,applied+1);
            return true;
        } catch(...) {return false;}
    }
    bool windowEntryMatches(const json& entry,const json& cert,int origin,int leaf) const {
        try {
            if(!validCoordinatorCertificate(cert)) return false;
            const auto& b=cert.at("proposal").at("body");const auto& value=b.at("value");
            return b.at("shard")==origin && b.at("seq")==entry.at("seq") &&
                   value.at("cst_round")==entry.at("block") && value.at("cst_order_index")==entry.at("index") &&
                   projection(value,leaf)==entry.at("projection") && orderParticipants(cert).count(leaf) &&
                   hash(value.at("requests").dump())==entry.at("request_digest");
        } catch(...) {return false;}
    }
    bool validWindowLeafOrder(const json& cert,const json& frontier) const {
        LeafElapsedScope schedulerProfile(*this,leafSchedulerElapsedNs,leafSchedulerDepth);
        LeafElapsedScope frontierProfile(*this,leafFrontierValidationElapsedNs,leafFrontierValidationDepth);
        try {
            if(!members.leaves.count(shard) || !validCoordinatorCertificate(cert) || !orderParticipants(cert).count(shard) ||
               state.at("cst_batches").contains(cstKey(cert))) return false;
            const auto& candidate=cert.at("proposal").at("body");
            const int w=windowOf(candidate.at("value").at("cst_round"));
            if(w<windowOf(state.at("cst_round")) || !frontier.is_array() ||
               frontier.size()!=members.ancestors(shard).size()) return false;
            std::map<int,const json*> closes;
            std::map<std::tuple<int,int,int>,const json*> orders;
            for(const auto& close:frontier) {
                const auto& cb=close.at("proposal").at("body");const int co=cb.at("shard");
                if(!members.ancestors(shard).count(co) || !validOrderClose(close) ||
                   cb.at("value").at("cst_round")!=w*orderingWindowBlocks() || !closes.emplace(co,&close).second) return false;
                const auto& row=cb.at("value").at("cst_window_manifest").at(std::to_string(shard));
                const int projected=localProjection(co),start=row.at("start"),end=row.at("end");
                // A window cannot overtake an earlier, not yet executed order.
                if(projected<start || projected>end) return false;
                for(const auto& entry:row.at("orders")) {
                    const int block=entry.at("block");auto cached=roundCertificates.find(block);
                    if(cached==roundCertificates.end() || !cached->second.count(co)) return false;
                    const json& order=cached->second.at(co);
                    if(!windowEntryMatches(entry,order,co,shard)) return false;
                    const auto key=cstKey(order);const bool done=state.at("cst_batches").contains(key);
                    if(done && state.at("cst_batches").at(key)!=order.at("proposal").at("body").at("digest")) return false;
                    if(done!=(entry.at("projection").get<int>()<=projected)) return false;
                    orders.emplace(std::make_tuple(orderingDepth(co),co,block),&order);
                }
            }
            // Completeness was checked for the entire incoming window before
            // choosing any order. Root depth is zero regardless of shard IDs.
            bool unfinished=false;
            const json* first=nullptr;
            for(const auto& [position,order]:orders) {
                (void)position;const bool done=state.at("cst_batches").contains(cstKey(*order));
                if(done && unfinished) return false;
                if(!done) {unfinished=true;if(!first) first=order;}
            }
            return first && cstKey(*first)==cstKey(cert) && first->at("proposal").at("body").at("digest")==candidate.at("digest");
        } catch(...) {return false;}
    }
    const json& nextWindowCstOrder() const {
        ++cstSelectionLookups;
        if(!cstSelectionDirty) return cstSelection;
        ++cstSelectionRebuilds;cstSelection=nullptr;leafSelectionBlockedCoordinator=-1;
        std::map<std::tuple<int,int,int,int>,const json*> candidates;
        for(const auto& [key,message]:pendingCst) {
            if(state.at("cst_batches").contains(key)) continue;
            const auto& cert=message.at("body").at("certificate");const auto& b=cert.at("proposal").at("body");
            int co=b.at("shard"),block=b.at("value").at("cst_round");
            candidates.emplace(std::make_tuple(windowOf(block),orderingDepth(co),co,block),&cert);
        }
        for(const auto& [position,cert]:candidates) {
            json frontier=json::array();const int closing=std::get<0>(position)*orderingWindowBlocks();
            auto row=roundCertificates.find(closing);if(row==roundCertificates.end()) continue;
            bool complete=true;
            for(int co:members.ancestors(shard)) {
                if(!row->second.count(co)) {complete=false;break;}
                frontier.push_back(row->second.at(co));
            }
            if(complete && validWindowLeafOrder(*cert,frontier)) {
                cstSelection={{"certificate",*cert},{"frontier",frontier}};break;
            }
        }
        if(cstSelection.is_null() && !candidates.empty())
            leafSelectionBlockedCoordinator=leafProfileMissingCoordinator(std::get<0>(candidates.begin()->first)*orderingWindowBlocks(),true);
        cstSelectionDirty=false;return cstSelection;
    }
    bool activateWindowWork(Clock::time_point now) {
        if(!windowsEnabled() || !members.leaves.count(shard) || changing || !stagedRecords.empty()) return false;
        const int head=applied+1;
        if(certificates.count(head) || (slots.count(head) && !slots.at(head).proposal.is_null())) return false;
        const auto& next=nextCstOrder();
        if(next.is_null()) return false;
        const auto key=next.at("certificate").is_null()
            ? "stream-progress:"+std::to_string(streamCursor()+next.at("frontier").size())
            : cstKey(next.at("certificate"));
        if(windowReadyHeadApplied==applied && windowReadyHeadKey==key) return false;
        // Waiting for the next authenticated ancestor prefix/window is not a
        // stalled local PBFT instance. Start that instance's deadline
        // when its deterministically next order or bounded progress first becomes
        // admissible. Replayed proofs, later empty blocks and active proposals
        // must never refresh the deadline for the same execution head.
        windowReadyHeadKey=key;windowReadyHeadApplied=applied;
        lastProgress=now;++windowWorkTimerStarts;
        log("ordering_window_work_activated",{{"batch",key},{"after",applied}});
        return true;
    }
    bool handleWindowPause(const json& e) {
        try {
            const auto& b=e.at("body");
            if(!windowsEnabled() || !isCoordinator() || b.at("type")!="WINDOW_PAUSE" ||
               !members.clientMessage(e) || b.at("target")!=shard ||
               !b.at("final_block").is_number_integer()) return false;
            const int final=b.at("final_block"),current=state.at("cst_round"),m=orderingWindowBlocks();
            if(final<0 || final>INT_MAX-m || current>INT_MAX-m) return false;
            const int boundary=std::max(final,current);
            windowPauseBlock=boundary==0?0:((boundary-1)/m+1)*m;
            log("ordering_window_pause",{{"final_block",windowPauseBlock}});return true;
        } catch(...) {return false;}
    }
    void orderingWindowStatus(json& status) const {
        status["ordering_window_blocks"]=orderingWindowBlocks();
        status["window_empty_block_interval_ms"]=windowEmptyIntervalMs();
        status["ordering_windows_enabled"]=windowsEnabled();
        status["window_pause_block"]=windowPauseBlock;
        status["window_work_timer_starts"]=windowWorkTimerStarts;
        status["ancestor_streaming_enabled"]=ancestorStreamingEnabled();
        if(ancestorStreamingEnabled() && members.leaves.count(shard)) status["ancestor_stream_cursor"]=streamCursor();
        const int block=state.at("cst_round");
        status["ordering_window"]=windowOf(block);
        status["window_paused"]=windowsEnabled() && isCoordinator() && windowPauseBlock>=0 &&
            block>=windowPauseBlock && block%orderingWindowBlocks()==0 &&
            (!slots.count(applied+1) || slots.at(applied+1).proposal.is_null());
        status["window_business_blocks"]=isCoordinator()?state.at("cst_order_index").get<int>():0;
        status["window_empty_blocks"]=windowsEnabled() && isCoordinator()?block-state.at("cst_order_index").get<int>():0;
    }
    void queryWindowRounds() {
        if(!windowsEnabled() || !isForward()) return;
        if(isCoordinator()) {
            json missing=json::array();
            for(auto it=state.at("cst_rounds").begin();it!=state.at("cst_rounds").end();++it) {
                const int block=std::stoi(it.key());
                if(!roundCertificates.count(block) || !roundCertificates.at(block).count(shard)) missing.push_back(block);
                if(missing.size()>=32) break;
            }
            if(!missing.empty()) broadcast(make("CST_ROUND_QUERY",{{"target",shard},{"rounds",missing},{"leaf",-1},{"after_index",-1}}));
            return;
        }
        if(ancestorStreamingEnabled()) {queryStreamingRounds();return;}
        if(pendingCst.empty() && stagedRecords.empty()) return;
        std::set<int> windows;
        for(const auto& [key,message]:pendingCst) {
            (void)key;windows.insert(windowOf(message.at("body").at("certificate").at("proposal").at("body").at("value").at("cst_round")));
            if(windows.size()>=8) break;
        }
        for(int co:members.ancestors(shard)) {
            json wanted=json::array();std::set<int> selected;
            auto add=[&](int block) {
                if(wanted.size()<32 && selected.insert(block).second) wanted.push_back(block);
            };
            for(int w:windows) {
                const int closing=w*orderingWindowBlocks();auto row=roundCertificates.find(closing);
                if(row==roundCertificates.end() || !row->second.count(co)) add(closing);
            }
            for(int w:windows) {
                const int closing=w*orderingWindowBlocks();auto row=roundCertificates.find(closing);
                if(row==roundCertificates.end() || !row->second.count(co)) continue;
                const auto& manifest=row->second.at(co).at("proposal").at("body").at("value").at("cst_window_manifest").at(std::to_string(shard));
                for(const auto& entry:manifest.at("orders")) {
                    const int block=entry.at("block");auto cached=roundCertificates.find(block);
                    if(cached==roundCertificates.end() || !cached->second.count(co)) add(block);
                }
            }
            // Even an apparently complete future window can have missing
            // historical projected orders; the coordinator supplies them.
            sendShard(co,make("CST_ROUND_QUERY",{{"target",co},{"rounds",wanted},{"leaf",shard},{"after_index",localProjection(co)}}));
        }
    }
    bool handleWindowQuery(const json& e) {
        const auto& b=e.at("body");
        if(!windowsEnabled()) return false;
        if(b.at("type")=="CST_ROUND_REQUEST") return true; // Timers, not demand, seal windows.
        if(b.at("type")!="CST_ROUND_QUERY") return false;
        try {
            if(!isCoordinator() || b.at("target")!=shard || !b.at("rounds").is_array() || b.at("rounds").size()>32) {++rejected;return true;}
            const int source=b.at("shard"),sender=b.at("from"),leaf=b.at("leaf"),after=b.at("after_index");
            const bool local=source==shard;
            if(!local && (!isForward() || leaf!=source || !members.descendants(shard).count(leaf) || after<0)) return true;
            std::vector<int> wanted;std::set<int> selected;
            auto add=[&](int block) {if(wanted.size()<32 && selected.insert(block).second) wanted.push_back(block);};
            for(const auto& r:b.at("rounds")) {
                if(!r.is_number_integer() || r.get<int>()<=0) {++rejected;return true;}
                add(r.get<int>());
            }
            if(!local) for(const auto& [block,closes]:roundCertificates) {
                if(closes.count(shard) && orderParticipants(closes.at(shard)).count(leaf) &&
                   projection(closes.at(shard).at("proposal").at("body").at("value"),leaf)>after) {
                    add(block);if(!ancestorStreamingEnabled()) add(windowOf(block)*orderingWindowBlocks());
                }
                if(wanted.size()>=32) break;
            }
            json proofs=json::array(),missing=json::array();
            for(int block:wanted) {
                if(roundCertificates.count(block) && roundCertificates.at(block).count(shard)) proofs.push_back(roundCertificates.at(block).at(shard));
                else if(state.at("cst_rounds").contains(std::to_string(block))) missing.push_back(block);
            }
            if(!proofs.empty()) {
                auto response=make("CST_ROUND_PROOFS",{{"target",source},{"certificates",proofs}});
                if(!local) sendShard(source,response); // Every backup needs the same completeness proof.
                else if(sender!=me) sendTo(source,sender,response);
            }
            if(!local && !missing.empty()) broadcast(make("CST_ROUND_QUERY",{{"target",shard},{"rounds",missing},{"leaf",-1},{"after_index",-1}}));
        } catch(...) {++rejected;}
        return true;
    }
