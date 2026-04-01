#pragma once
// Simple SHA256 implementation for token authentication
// Based on FIPS 180-4

#include <cstdint>
#include <cstring>
#include <string>
#include <array>
#include <vector>
#include <ctime>
#include <unistd.h>

namespace sha256 {

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t ep0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t ep1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t sig0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t sig1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline std::array<uint8_t, 32> hash(const uint8_t* data, size_t len) {
    // Initial hash values
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    // Pre-processing: adding padding bits
    size_t new_len = len + 1 + 8;
    while (new_len % 64 != 0) new_len++;

    std::vector<uint8_t> msg(new_len, 0);
    memcpy(msg.data(), data, len);
    msg[len] = 0x80;

    // Append original length in bits as big-endian 64-bit
    uint64_t bit_len = len * 8;
    for (int i = 0; i < 8; i++) {
        msg[new_len - 1 - i] = (bit_len >> (i * 8)) & 0xff;
    }

    // Process each 64-byte chunk
    for (size_t chunk = 0; chunk < new_len / 64; chunk++) {
        uint32_t w[64];

        // Copy chunk into first 16 words
        for (int i = 0; i < 16; i++) {
            w[i] = (msg[chunk*64 + i*4] << 24) |
                   (msg[chunk*64 + i*4 + 1] << 16) |
                   (msg[chunk*64 + i*4 + 2] << 8) |
                   msg[chunk*64 + i*4 + 3];
        }

        // Extend the first 16 words into the remaining 48 words
        for (int i = 16; i < 64; i++) {
            w[i] = sig1(w[i-2]) + w[i-7] + sig0(w[i-15]) + w[i-16];
        }

        // Compression
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + ep1(e) + ch(e, f, g) + k[i] + w[i];
            uint32_t t2 = ep0(a) + maj(a, b, c);
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    // Produce the final hash value (big-endian)
    std::array<uint8_t, 32> result;
    for (int i = 0; i < 8; i++) {
        result[i*4] = (h[i] >> 24) & 0xff;
        result[i*4 + 1] = (h[i] >> 16) & 0xff;
        result[i*4 + 2] = (h[i] >> 8) & 0xff;
        result[i*4 + 3] = h[i] & 0xff;
    }

    return result;
}

inline std::array<uint8_t, 32> hash(const std::string& s) {
    return hash(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

inline std::string to_hex(const std::array<uint8_t, 32>& h) {
    static const char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (int i = 0; i < 32; i++) {
        result[i*2] = hex[h[i] >> 4];
        result[i*2 + 1] = hex[h[i] & 0xf];
    }
    return result;
}

inline std::string hash_hex(const std::string& s) {
    return to_hex(hash(s));
}

// Generate token: SHA256(secret + ":" + timestamp_str)
// timestamp = Unix time / 300 (5-minute window)
inline std::string generate_token(const std::string& secret, uint64_t timestamp) {
    std::string input = secret + ":" + std::to_string(timestamp);
    return hash_hex(input);
}

// Validate token: check current and adjacent timestamps
inline bool validate_token(const std::string& secret, const std::string& token, uint64_t current_time) {
    uint64_t ts = current_time / 300;
    // Check current window and ±1 window (total 15 minutes tolerance)
    for (int delta = -1; delta <= 1; delta++) {
        if (generate_token(secret, ts + delta) == token) {
            return true;
        }
    }
    return false;
}

// Generate random hex string of specified length
inline std::string random_hex(int len) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(len);

    // Use /dev/urandom for random bytes
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        for (int i = 0; i < len / 2; i++) {
            int c = fgetc(f);
            result += hex[(c >> 4) & 0xf];
            result += hex[c & 0xf];
        }
        fclose(f);
    } else {
        // Fallback: use time and pid
        uint64_t seed = time(nullptr) ^ getpid();
        for (int i = 0; i < len / 2; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            int c = (seed >> 32) & 0xff;
            result += hex[(c >> 4) & 0xf];
            result += hex[c & 0xf];
        }
    }
    return result;
}

} // namespace sha256