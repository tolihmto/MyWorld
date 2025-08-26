#include "terrain.hpp"
#include "config.hpp"
#include <algorithm>
#include <cmath>

namespace terrain {
namespace {
    inline int idx(int i, int j) { return i * (cfg::GRID + 1) + j; }

    uint32_t hash2i(int x, int y, uint32_t seed){
        uint32_t h = seed;
        h ^= 0x9E3779B9u + (uint32_t)x + (h<<6) + (h>>2);
        h ^= 0x85EBCA6Bu + (uint32_t)y + (h<<6) + (h>>2);
        h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
        return h;
    }
    float rnd01(int x, int y, uint32_t seed){
        return (hash2i(x, y, seed) & 0xFFFFFF) / 16777215.f; // [0,1]
    }
    float smoothstep(float t){ return t*t*(3.f - 2.f*t); }
    float lerp(float a, float b, float t){ return a + (b - a) * t; }
    float valueNoise2D(float x, float y, uint32_t seed){
        int xi = (int)std::floor(x);
        int yi = (int)std::floor(y);
        float xf = x - xi; float yf = y - yi;
        float v00 = rnd01(xi,   yi,   seed);
        float v10 = rnd01(xi+1, yi,   seed);
        float v01 = rnd01(xi,   yi+1, seed);
        float v11 = rnd01(xi+1, yi+1, seed);
        float u = smoothstep(xf);
        float v = smoothstep(yf);
        float x1 = lerp(v00, v10, u);
        float x2 = lerp(v01, v11, u);
        return lerp(x1, x2, v); // [0,1]
    }
    float fbm(float x, float y, uint32_t seed, int octaves, float lacunarity, float gain){
        float amp = 0.5f;
        float freq = 1.f;
        float sum = 0.f;
        float norm = 0.f;
        for (int o = 0; o < octaves; ++o) {
            sum  += amp * valueNoise2D(x * freq, y * freq, seed + o * 1013u);
            norm += amp;
            freq *= lacunarity;
            amp  *= gain;
        }
        return (norm > 0.f) ? (sum / norm) : 0.f; // [0,1]
    }
}

void generateMap(std::vector<int>& heights, uint32_t seed){
    const float baseScale = cfg::NOISE_BASE_SCALE; // how many large features across the grid
    heights.resize((cfg::GRID + 1) * (cfg::GRID + 1));
    // 3-octave setup
    const float lacunarity   = 2.0f;  // frequency multiplier per octave
    const float persistence  = 0.5f;  // amplitude multiplier per octave
    const float freq1 = std::pow(lacunarity, 0); const float amp1 = std::pow(persistence, 0); // 1, 1.0
    const float freq2 = std::pow(lacunarity, 1); const float amp2 = std::pow(persistence, 1); // 2, 0.5
    const float freq3 = std::pow(lacunarity, 2); const float amp3 = std::pow(persistence, 2); // 4, 0.25
    const float norm = (amp1 + amp2 + amp3);

    for (int i = 0; i <= cfg::GRID; ++i) {
        for (int j = 0; j <= cfg::GRID; ++j) {
            float x = (float)i / (float)cfg::GRID * baseScale;
            float y = (float)j / (float)cfg::GRID * baseScale;

            // Domain warp (léger) pour disperser les pics
            const float ws = cfg::NOISE_WARP_SCALE; // basse fréquence
            float wx = (valueNoise2D(x * ws, y * ws, seed + 777u) - 0.5f) * 2.f * cfg::NOISE_WARP_STRENGTH;
            float wy = (valueNoise2D((x + 13.37f) * ws, (y - 9.21f) * ws, seed + 1553u) - 0.5f) * 2.f * cfg::NOISE_WARP_STRENGTH;
            float xw = x + wx;
            float yw = y + wy;

            // Sum 3 octaves (standard FBM)
            float o1 = valueNoise2D(xw * freq1, yw * freq1, seed);
            float o2 = valueNoise2D(xw * freq2, yw * freq2, seed + 1013u);
            float o3 = valueNoise2D(xw * freq3, yw * freq3, seed + 2026u);
            float n_fbm = (amp1 * o1 + amp2 * o2 + amp3 * o3) / norm; // [0,1]

            // Ridged transform par octave (pics marqués)
            auto ridge = [](float v){
                float r = 1.f - std::fabs(2.f * v - 1.f); // crêtes
                return r * r; // affûter
            };
            float r1 = ridge(o1);
            float r2 = ridge(o2);
            float r3 = ridge(o3);
            float n_ridged = (amp1 * r1 + amp2 * r2 + amp3 * r3) / norm; // [0,1]

            // Mélange FBM vs ridged
            float w = std::clamp(cfg::NOISE_RIDGED_WEIGHT, 0.0f, 1.0f);
            float n = (1.f - w) * n_fbm + w * n_ridged;

            // Island mask: plus on s'éloigne du centre, plus on baisse
            float gx = (float)i / (float)cfg::GRID; // [0,1]
            float gy = (float)j / (float)cfg::GRID; // [0,1]
            float dx = gx - 0.5f;
            float dy = gy - 0.5f;
            float dist = std::sqrt(dx*dx + dy*dy) / 0.5f; // 0 au centre, ~1 au bord du cercle inscrit
            float t = 0.f;
            if (cfg::ISLAND_RADIUS < 1.f) {
                t = std::clamp((dist - cfg::ISLAND_RADIUS) / (1.f - cfg::ISLAND_RADIUS), 0.0f, 1.0f);
            }
            float mask = std::pow(t, cfg::ISLAND_POWER); // 0 au centre, ->1 vers bords
            float islandFactor = 1.f - mask;
            n *= islandFactor;

            // Accentuer/aplanir via gamma configurable
            n = std::clamp(n, 0.0f, 1.0f);
            n = std::pow(n, cfg::NOISE_EXP);

            float h0 = (float)cfg::MIN_ELEV + n * (float)(cfg::MAX_ELEV - cfg::MIN_ELEV);
            float h = h0 * cfg::HEIGHT_SCALE - cfg::SEA_OFFSET;
            int hi = (int)std::round(h);

            // Mountain chain mask (low-frequency ridged band), warped for continuity
            {
                float cx = x * cfg::MNT_MASK_FREQ;
                float cy = y * cfg::MNT_MASK_FREQ;
                // Domain warp
                float wx = (valueNoise2D(cx * 0.5f, cy * 0.5f, seed + 9001u) - 0.5f) * 2.f * cfg::MNT_MASK_WARP;
                float wy = (valueNoise2D((cx + 5.3f) * 0.5f, (cy - 2.7f) * 0.5f, seed + 1723u) - 0.5f) * 2.f * cfg::MNT_MASK_WARP;
                float nm = valueNoise2D(cx + wx, cy + wy, seed + 1337u); // [0,1]
                float mr = 1.f - std::fabs(2.f * nm - 1.f); // ridged band
                mr = std::clamp(mr, 0.0f, 1.0f);
                if (mr > cfg::MNT_MASK_THRESH) {
                    float tmask = (mr - cfg::MNT_MASK_THRESH) / std::max(1e-4f, 1.f - cfg::MNT_MASK_THRESH);
                    hi += (int)std::round(tmask * cfg::MNT_MASK_STRENGTH);
                }
            }
            // Rare high mountain spikes (only on land)
            if (hi > 0) {
                float r = rnd01(i, j, seed + 0xBEEF1234u);
                if (r < cfg::RARE_PEAK_PROB) {
                    hi += (int)std::round(cfg::RARE_PEAK_BOOST);
                }
            }
            heights[idx(i, j)] = std::clamp(hi, cfg::MIN_ELEV, cfg::MAX_ELEV);
        }
    }
}

} // namespace terrain
