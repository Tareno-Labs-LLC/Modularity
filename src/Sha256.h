#pragma once

// Small, self-contained SHA-256 (FIPS 180-4). Header-only, no dependencies, incremental API.
// Used for the runtime bundle's per-file and whole-bundle integrity hashes. Not for passwords.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace Modularity {

class Sha256 {
public:
    Sha256() { init(); }

    void init() {
        mLen = 0;
        mBufferLen = 0;
        static const uint32_t kInit[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        std::memcpy(mState, kInit, sizeof(mState));
    }

    void update(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        mLen += len;
        while (len > 0) {
            const size_t take = (64 - mBufferLen < len) ? (64 - mBufferLen) : len;
            std::memcpy(mBuffer + mBufferLen, p, take);
            mBufferLen += take;
            p += take;
            len -= take;
            if (mBufferLen == 64) {
                transform(mBuffer);
                mBufferLen = 0;
            }
        }
    }

    void finish(uint8_t out[32]) {
        const uint64_t bitLen = mLen * 8ull;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0x00;
        while (mBufferLen != 56) {
            update(&pad, 1);
        }
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) {
            lenBytes[i] = static_cast<uint8_t>((bitLen >> (56 - 8 * i)) & 0xff);
        }
        update(lenBytes, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>((mState[i] >> 24) & 0xff);
            out[i * 4 + 1] = static_cast<uint8_t>((mState[i] >> 16) & 0xff);
            out[i * 4 + 2] = static_cast<uint8_t>((mState[i] >> 8) & 0xff);
            out[i * 4 + 3] = static_cast<uint8_t>(mState[i] & 0xff);
        }
    }

    static void hash(const void* data, size_t len, uint8_t out[32]) {
        Sha256 h;
        h.update(data, len);
        h.finish(out);
    }

    static std::string toHex(const uint8_t digest[32]) {
        static const char* kHex = "0123456789abcdef";
        std::string s(64, '0');
        for (int i = 0; i < 32; ++i) {
            s[i * 2] = kHex[(digest[i] >> 4) & 0xf];
            s[i * 2 + 1] = kHex[digest[i] & 0xf];
        }
        return s;
    }

private:
    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void transform(const uint8_t block[64]) {
        static const uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = mState[0], b = mState[1], c = mState[2], d = mState[3];
        uint32_t e = mState[4], f = mState[5], g = mState[6], h = mState[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        mState[0] += a; mState[1] += b; mState[2] += c; mState[3] += d;
        mState[4] += e; mState[5] += f; mState[6] += g; mState[7] += h;
    }

    uint32_t mState[8];
    uint64_t mLen = 0;
    uint8_t mBuffer[64];
    size_t mBufferLen = 0;
};

} // namespace Modularity
