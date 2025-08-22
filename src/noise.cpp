#include "noise.hpp"
#include <cmath>
#include <cstdint>

namespace {
    // 32-bit integer hash (xorshift-like) for deterministic value noise
    inline uint32_t hash2d(int x, int y, uint32_t seed) {
        uint32_t h = static_cast<uint32_t>(x) * 0x27d4eb2dU ^ static_cast<uint32_t>(y) * 0x165667b1U ^ seed * 0x9e3779b9U;
        h ^= h >> 15; h *= 0x85ebca6bU; h ^= h >> 13; h *= 0xc2b2ae35U; h ^= h >> 16;
        return h;
    }
    inline float valFromHash(uint32_t h) {
        // Map to [0,1)
        return (h & 0xFFFFFF) / 16777216.0f;
    }
    inline float smoothstep(float t) {
        return t * t * (3.f - 2.f * t);
    }
    inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

    // Value noise (continuous via bilinear interpolation of lattice values)
    float valueNoise2D(float x, float y, uint32_t seed) {
        int xi = static_cast<int>(std::floor(x));
        int yi = static_cast<int>(std::floor(y));
        float xf = x - static_cast<float>(xi);
        float yf = y - static_cast<float>(yi);
        float u = smoothstep(xf);
        float v = smoothstep(yf);
        float v00 = valFromHash(hash2d(xi + 0, yi + 0, seed));
        float v10 = valFromHash(hash2d(xi + 1, yi + 0, seed));
        float v01 = valFromHash(hash2d(xi + 0, yi + 1, seed));
        float v11 = valFromHash(hash2d(xi + 1, yi + 1, seed));
        float vx0 = lerp(v00, v10, u);
        float vx1 = lerp(v01, v11, u);
        return lerp(vx0, vx1, v);
    }
}

namespace noise {

float fbm(float x, float y, uint32_t seed, int octaves, float lacunarity, float gain) {
    float amp = 0.5f;
    float freq = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * (valueNoise2D(x * freq, y * freq, seed + static_cast<uint32_t>(o * 1315423911U)) * 2.f - 1.f);
        norm += amp;
        freq *= lacunarity;
        amp *= gain;
    }
    if (norm > 0.f) sum /= norm;
    // Clamp to [-1,1]
    if (sum < -1.f) sum = -1.f; else if (sum > 1.f) sum = 1.f;
    return sum;
}

}
