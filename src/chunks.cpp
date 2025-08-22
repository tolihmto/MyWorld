#include "chunks.hpp"
#include "noise.hpp"
#include <algorithm>
#include <cmath>

static inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

const Chunk& ChunkManager::getChunk(int cx, int cy) {
    ChunkKey key{cx, cy};
    auto it = _cache.find(key);
    if (it != _cache.end()) {
        // Move to front in LRU
        _lru.erase(it->second.it);
        _lru.push_front(key);
        it->second.it = _lru.begin();
        return it->second.ch;
    }
    // Miss: create and generate
    Entry e{};
    generateChunk(e.ch, cx, cy);
    _lru.push_front(key);
    e.it = _lru.begin();
    _cache.emplace(key, std::move(e));
    // Evict if needed
    while (_cache.size() > static_cast<size_t>(cfg::MAX_CACHED_CHUNKS)) {
        const ChunkKey& oldKey = _lru.back();
        _cache.erase(oldKey);
        _lru.pop_back();
    }
    return _cache.find(key)->second.ch;
}

void ChunkManager::generateChunk(Chunk& out, int cx, int cy) {
    const int S = cfg::CHUNK_SIZE;
    // World index of the chunk origin (top-left corner) in tile space
    const int I0 = cx * S;
    const int J0 = cy * S;

    if (_mode == Mode::Empty) {
        std::fill(out.heights.begin(), out.heights.end(), 0);
        return;
    }

    // Procedural: seamless value-noise FBM in world coordinates
    // Keep similar scale to the previous 300x300 map generation
    const float baseScale = cfg::NOISE_BASE_SCALE;
    // Continents: use larger features and slightly lower sea level for more oceans
    float worldFreq = baseScale / (float)cfg::GRID; // features per ~300 tiles
    float seaOffset = cfg::SEA_OFFSET;
    if (_continents) {
        worldFreq *= 0.5f;   // bigger features (twice larger)
        seaOffset  += 0.8f;  // push sea level up => more water
    }

    for (int i = 0; i <= S; ++i) {
        for (int j = 0; j <= S; ++j) {
            int I = I0 + i; // world tile coords
            int J = J0 + j;
            float x = I * worldFreq;
            float y = J * worldFreq;
            float n = noise::fbm(x, y, _seed, 5, 2.0f, 0.5f); // [-1,1]
            float t = 0.5f * (n + 1.0f);                      // [0,1]
            float h = (float)cfg::MIN_ELEV + t * (float)(cfg::MAX_ELEV - cfg::MIN_ELEV) - seaOffset;
            int hi = (int)std::round(h);
            out.heights[Chunk::idx(i, j)] = clampi(hi, cfg::MIN_ELEV, cfg::MAX_ELEV);
        }
    }
}
