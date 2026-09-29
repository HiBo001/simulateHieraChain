#pragma once
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <chrono>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace arbor {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Key = std::shared_ptr<EVP_PKEY>;
inline double millis(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
}
inline std::string hex(const unsigned char* p, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}
inline std::vector<unsigned char> unhex(const std::string& s) {
    if (s.size() % 2) throw std::runtime_error("invalid hex");
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("invalid hex");
    };
    std::vector<unsigned char> v;
    for (size_t i = 0; i < s.size(); i += 2) v.push_back((digit(s[i]) << 4) | digit(s[i+1]));
    return v;
}
inline std::string hash(const std::string& s) {
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), out);
    return hex(out, sizeof(out));
}
inline Key readKey(const std::string& path, bool secret) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open key: " + path);
    EVP_PKEY* k = secret ? PEM_read_PrivateKey(f, nullptr, nullptr, nullptr)
                        : PEM_read_PUBKEY(f, nullptr, nullptr, nullptr);
    fclose(f);
    if (!k) throw std::runtime_error("invalid Ed25519 key: " + path);
    return Key(k, EVP_PKEY_free);
}
inline json sign(json body, const Key& k) {
    auto ctx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    auto bytes = body.dump();
    unsigned char sig[128]; size_t n = sizeof(sig);
    if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, k.get()) != 1 ||
        EVP_DigestSign(ctx.get(), sig, &n, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()) != 1)
        throw std::runtime_error("Ed25519 signing failed");
    return {{"body", body}, {"signature", hex(sig, n)}};
}
inline bool verify(const json& env, const Key& k) {
    try {
        auto bytes = env.at("body").dump();
        auto sig = unhex(env.at("signature").get<std::string>());
        auto ctx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        return EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, k.get()) == 1 &&
            EVP_DigestVerify(ctx.get(), sig.data(), sig.size(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()) == 1;
    } catch (...) { return false; }
}
inline json readJson(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path);
    json j; f >> j; return j;
}
inline void writeJson(const std::string& path, const json& j) {
    { std::ofstream f(path + ".tmp"); f << j.dump(2) << '\n'; if (!f) throw std::runtime_error("write failed: " + path); }
    if (rename((path + ".tmp").c_str(), path.c_str())) throw std::runtime_error("rename failed: " + path);
}
}
