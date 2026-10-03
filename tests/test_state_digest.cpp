#include "state_digest.h"
#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using namespace arbor;

static void check(bool condition,const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}
static void assertMatches(const StateDigest& index,const json& state) {
    check(index.root()==StateDigest::fromState(state),"incremental root differs from full snapshot reconstruction");
}
static json initial() {
    return {{"seq",0},{"chain",hash("genesis")},{"executed",0},
        {"kv",json::object()},{"seen",json::object()},{"requests",json::object()},
        {"cst_finalized",json::object()},{"unknown_metadata",{{"a",json::array({1,2,3})}}}};
}

static void randomBatchMutations() {
    json state=initial();StateDigest index;index.rebuild(state);
    std::mt19937 random(913042);
    for(int batch=0;batch<150;++batch) {
        for(int step=0;step<40;++step) {
            std::string key="tx-"+std::to_string(random()%300);
            std::string field=std::vector<std::string>{"kv","seen","requests","cst_finalized"}[random()%4];
            if(random()%7==0) state[field].erase(key);
            else state[field][key]={{"version",random()%1000},{"values",json::array({random()%20,random()%20})}};
            index.markEntry(field,key);
            // Repeated writes to the same entry within one batch must use its
            // final value, including write/delete and delete/write sequences.
            if(random()%5==0) {
                state[field][key]={{"final",batch}};index.markEntry(field,key);
            }
        }
        state["seq"]=batch+1;state["chain"]=hash(state["chain"].get<std::string>()+std::to_string(batch));
        index.markField("seq");index.markField("chain");index.refresh(state);assertMatches(index,state);
    }
    std::cout<<"PASS randomized batched state updates, deletions and reconstruction\n";
}

static void insertionOrderAndRebuild() {
    json state=initial();std::vector<std::string> keys;
    for(int i=0;i<1000;++i) keys.push_back("transaction-"+std::to_string(i));
    StateDigest forward,reverse;forward.rebuild(state);reverse.rebuild(state);
    for(const auto& key:keys) {
        state["seen"][key]={{"digest",hash(key)}};
        forward.markEntry("seen",key);forward.refresh(state);
    }
    auto complete=state;state["seen"]=json::object();
    std::reverse(keys.begin(),keys.end());
    for(const auto& key:keys) {
        state["seen"][key]=complete["seen"][key];
        reverse.markEntry("seen",key);reverse.refresh(state);
    }
    check(forward.root()==reverse.root(),"equivalent states depend on insertion order");
    assertMatches(forward,complete);
    StateDigest restored;restored.rebuild(complete);check(restored.root()==forward.root(),"snapshot restore root differs");
    complete["seen"]["transaction-400"]["digest"]="modified";
    restored.markEntry("seen","transaction-400");restored.refresh(complete);assertMatches(restored,complete);
    check(restored.root()!=forward.root(),"post-restore mutation was not authenticated");
    std::cout<<"PASS canonical insertion order and snapshot restore followed by updates\n";
}

static void allStateAndPathBoundaries() {
    json state=initial();auto expected=StateDigest::fromState(state);
    for(const auto& field:std::vector<std::string>{"seq","chain","executed","kv","seen","requests","cst_finalized","unknown_metadata"}) {
        auto changed=state;
        if(changed[field].is_object()) changed[field]["tampered"]=true;else changed[field]="tampered";
        check(StateDigest::fromState(changed)!=expected,"state field omitted from authenticated root: "+field);
    }
    auto missing=state;missing.erase("kv");check(StateDigest::fromState(missing)!=expected,"empty map indistinguishable from missing map");
    auto scalar=state;scalar["kv"]=json::array();check(StateDigest::fromState(scalar)!=expected,"object/array type indistinguishable");
    check(StateDigest::fromState({{"a",{{"b/c",1}}}})!=StateDigest::fromState({{"a/b",{{"c",1}}}}),"ambiguous field/key path encoding");
    check(StateDigest::fromState({{"a",{{"",1}}}})!=StateDigest::fromState({{"a",1}}),"field marker collides with an empty entry key");
    check(StateDigest::fromState({{"a",{{"x",1},{"y",2}}}})!=StateDigest::fromState({{"a",{{"x",2},{"y",1}}}}),"entry value or name not bound");
    std::cout<<"PASS all state fields, metadata, values, names and type boundaries authenticated\n";
}

static void structuralChanges() {
    json state=initial();StateDigest index;index.rebuild(state);
    state["new"]=json::object();index.markField("new");index.refresh(state);assertMatches(index,state);
    state["new"][""]={{"nested",{{"payload",true}}}};index.markEntry("new","");index.refresh(state);assertMatches(index,state);
    state["new"]="scalar";index.markField("new");index.refresh(state);assertMatches(index,state);
    state["new"]=json::array({1,2,3});index.markField("new");index.refresh(state);assertMatches(index,state);
    state["new"]={{"first",1},{"second",2}};index.markField("new");index.markEntry("new","first");index.refresh(state);assertMatches(index,state);
    state.erase("new");index.markField("new");index.refresh(state);assertMatches(index,state);
    // Entry marks also correctly reconcile a new field or a changed field type.
    state["seq"]={{"counter",100}};index.markEntry("seq","counter");index.refresh(state);assertMatches(index,state);
    state["seq"]=99;index.markEntry("seq","counter");index.refresh(state);assertMatches(index,state);
    bool rejected=false;try {StateDigest::fromState(json::array({1}));} catch(const std::exception&) {rejected=true;}
    check(rejected,"non-object replicated state accepted");
    std::cout<<"PASS field creation/removal, nested values and complete type changes\n";
}

static void coalescingAndBoundedWork() {
    json state=initial();
    for(int i=0;i<12000;++i) state["seen"]["tx-"+std::to_string(i)]={{"value",i}};
    StateDigest index;index.rebuild(state);
    auto oldWrites=index.leafWrites(),oldHashes=index.nodeRehashes();
    for(int i=0;i<1000;++i) {
        state["seen"]["tx-6000"]={{"value",i}};index.markEntry("seen","tx-6000");
    }
    index.refresh(state);assertMatches(index,state);
    check(index.leafWrites()-oldWrites==1,"repeated batch mutations were not coalesced");
    check(index.nodeRehashes()-oldHashes<128,"one entry update recomputed growing historical state");
    auto root=index.root();oldHashes=index.nodeRehashes();oldWrites=index.leafWrites();
    index.markEntry("seen","tx-6000");index.markField("executed");index.refresh(state);
    check(index.root()==root && index.leafWrites()==oldWrites && index.nodeRehashes()==oldHashes,"unchanged values caused unnecessary tree rehashing");
    index.rebuild(json::object());check(index.leafCount()==0,"rebuild retained old leaves");
    check(index.root()==StateDigest::fromState(json::object()),"empty rebuilt state digest differs");
    std::cout<<"PASS repeated writes coalesced and one update bounded independently of state history\n";
}

int main() {
    try {
        randomBatchMutations();insertionOrderAndRebuild();allStateAndPathBoundaries();
        structuralChanges();coalescingAndBoundedWork();
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
    return 0;
}
