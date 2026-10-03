#pragma once
#include "common.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>

namespace arbor {

// A canonical, authenticated index of the complete replicated JSON state.
// Top-level objects are indexed by entry; other fields have one value leaf.
// A marker leaf also binds each field's name and object/value distinction, so
// an empty object and a missing field have different roots. Nested entry
// values use JSON's canonical dump, just as transaction signatures do.
//
// A treap ordered by the encoded path and SHA-256(path) priority has a unique
// shape independent of insertion order. Its expected update cost is O(log N).
// All hashes have separate domains and fixed-size child digests; names use
// length prefixes, preventing path ambiguities. This replaces repeated full
// state serialization, without omitting transaction/dedup/checkpoint metadata.
class StateDigest {
    using Digest=std::array<unsigned char,SHA256_DIGEST_LENGTH>;
    struct Node {
        std::string path;
        Digest priority, value, digest;
        std::unique_ptr<Node> left, right;
        Node(std::string key, Digest rank, Digest leaf)
            :path(std::move(key)),priority(rank),value(leaf) {}
    };
    std::unique_ptr<Node> tree;
    // Object fields keep their indexed keys so a rare complete field removal
    // or type change can also remove all of its former entry leaves.
    std::map<std::string,std::set<std::string>> objects;
    std::set<std::string> fields, dirtyFields;
    std::map<std::string,std::set<std::string>> dirtyEntries;
    std::string cachedRoot;
    uint64_t rehashCount=0, leafWriteCount=0;
    size_t leaves=0;

    static void length(std::string& bytes,size_t value) {
        uint64_t n=value;
        for(int shift=56;shift>=0;shift-=8) bytes.push_back(static_cast<char>(n>>shift));
    }
    static void component(std::string& bytes,const std::string& value) {
        length(bytes,value.size()); bytes.append(value);
    }
    static std::string fieldPath(const std::string& field) {
        std::string path(1,'F');component(path,field);return path;
    }
    static std::string entryPath(const std::string& field,const std::string& key) {
        std::string path(1,'E');component(path,field);component(path,key);return path;
    }
    static Digest sha(const std::string& bytes) {
        Digest result{};
        SHA256(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size(),result.data());
        return result;
    }
    static void append(std::string& bytes,const Digest& digest) {
        bytes.append(reinterpret_cast<const char*>(digest.data()),digest.size());
    }
    static const Digest& empty() {
        static const Digest value=sha("arbor-state-merkle-v1/empty");return value;
    }
    static const Digest& digest(const std::unique_ptr<Node>& node) {
        return node?node->digest:empty();
    }
    static bool precedes(const Node& a,const Node& b) {
        int comparison=std::memcmp(a.priority.data(),b.priority.data(),a.priority.size());
        return comparison<0 || (comparison==0 && a.path<b.path);
    }
    static Digest leafDigest(const std::string& path,const std::string& value) {
        std::string bytes="arbor-state-merkle-v1/leaf";
        component(bytes,path);component(bytes,value);return sha(bytes);
    }
    static Digest rank(const std::string& path) {
        std::string bytes="arbor-state-merkle-v1/priority";
        component(bytes,path);return sha(bytes);
    }
    void refreshNode(Node& node) {
        std::string bytes="arbor-state-merkle-v1/node";
        bytes.reserve(bytes.size()+3*SHA256_DIGEST_LENGTH);
        append(bytes,node.value);append(bytes,digest(node.left));append(bytes,digest(node.right));
        node.digest=sha(bytes);++rehashCount;
    }
    void rotateLeft(std::unique_ptr<Node>& node) {
        auto parent=std::move(node->right);
        node->right=std::move(parent->left);refreshNode(*node);
        parent->left=std::move(node);node=std::move(parent);
    }
    void rotateRight(std::unique_ptr<Node>& node) {
        auto parent=std::move(node->left);
        node->left=std::move(parent->right);refreshNode(*node);
        parent->right=std::move(node);node=std::move(parent);
    }
    bool put(std::unique_ptr<Node>& node,const std::string& path,const Digest& value) {
        if(!node) {
            node=std::make_unique<Node>(path,rank(path),value);
            refreshNode(*node);++leaves;++leafWriteCount;return true;
        }
        if(path==node->path) {
            if(node->value==value) return false;
            node->value=value;++leafWriteCount;
        } else if(path<node->path) {
            if(!put(node->left,path,value)) return false;
            if(precedes(*node->left,*node)) rotateRight(node);
        } else {
            if(!put(node->right,path,value)) return false;
            if(precedes(*node->right,*node)) rotateLeft(node);
        }
        refreshNode(*node);return true;
    }
    std::unique_ptr<Node> merge(std::unique_ptr<Node> left,std::unique_ptr<Node> right) {
        if(!left) return right;
        if(!right) return left;
        if(precedes(*left,*right)) {
            left->right=merge(std::move(left->right),std::move(right));refreshNode(*left);return left;
        }
        right->left=merge(std::move(left),std::move(right->left));refreshNode(*right);return right;
    }
    bool erase(std::unique_ptr<Node>& node,const std::string& path) {
        if(!node) return false;
        if(path==node->path) {
            node=merge(std::move(node->left),std::move(node->right));
            --leaves;++leafWriteCount;return true;
        }
        bool changed=path<node->path?erase(node->left,path):erase(node->right,path);
        if(changed) refreshNode(*node);
        return changed;
    }
    void put(const std::string& path,const std::string& value) {
        put(tree,path,leafDigest(path,value));
    }
    void eraseField(const std::string& name) {
        auto object=objects.find(name);
        if(object!=objects.end()) {
            for(const auto& key:object->second) erase(tree,entryPath(name,key));
            objects.erase(object);
        }
        erase(tree,fieldPath(name));fields.erase(name);
    }
    void setField(const std::string& name,const json& value) {
        if(!value.is_object()) {
            if(objects.count(name)) eraseField(name);
            fields.insert(name);put(fieldPath(name),"value:"+value.dump());return;
        }
        fields.insert(name);put(fieldPath(name),"object");
        auto& keys=objects[name];
        for(auto it=keys.begin();it!=keys.end();) {
            if(!value.contains(*it)) {erase(tree,entryPath(name,*it));it=keys.erase(it);}
            else ++it;
        }
        for(auto it=value.begin();it!=value.end();++it) {
            keys.insert(it.key());put(entryPath(name,it.key()),it.value().dump());
        }
    }
    void refreshRoot() {
        std::string bytes="arbor-state-merkle-v1/root";append(bytes,digest(tree));
        auto rootDigest=sha(bytes);cachedRoot=hex(rootDigest.data(),rootDigest.size());
    }

public:
    StateDigest() { refreshRoot(); }
    void rebuild(const json& state) {
        if(!state.is_object()) throw std::runtime_error("replicated state must be an object");
        tree.reset();objects.clear();fields.clear();dirtyFields.clear();dirtyEntries.clear();
        leaves=0;rehashCount=0;leafWriteCount=0;
        for(auto it=state.begin();it!=state.end();++it) setField(it.key(),it.value());
        refreshRoot();
    }
    void markField(const std::string& field) { dirtyFields.insert(field); }
    void markEntry(const std::string& field,const std::string& key) { dirtyEntries[field].insert(key); }
    const std::string& refresh(const json& state) {
        if(!state.is_object()) throw std::runtime_error("replicated state must be an object");
        auto oldWrites=leafWriteCount;
        for(const auto& name:dirtyFields) {
            auto value=state.find(name);
            if(value==state.end()) eraseField(name);else setField(name,*value);
        }
        for(const auto& [name,keys]:dirtyEntries) {
            if(dirtyFields.count(name)) continue;
            auto value=state.find(name);
            if(value==state.end()) {eraseField(name);continue;}
            if(!value->is_object() || !objects.count(name)) {setField(name,*value);continue;}
            auto& indexed=objects.at(name);
            for(const auto& key:keys) {
                auto item=value->find(key);
                if(item==value->end()) {erase(tree,entryPath(name,key));indexed.erase(key);}
                else {put(entryPath(name,key),item->dump());indexed.insert(key);}
            }
        }
        dirtyFields.clear();dirtyEntries.clear();
        if(leafWriteCount!=oldWrites) refreshRoot();
        return cachedRoot;
    }
    const std::string& root() const {return cachedRoot;}
    size_t leafCount() const {return leaves;}
    uint64_t nodeRehashes() const {return rehashCount;}
    uint64_t leafWrites() const {return leafWriteCount;}
    static std::string fromState(const json& state) {
        StateDigest index;index.rebuild(state);return index.root();
    }
};
}
