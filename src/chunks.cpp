#include "chunks.hpp"
#include "noise.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

static inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

void ChunkManager::saveAllDirty() {
    for (auto& kv : _cache) {
        Entry& e = kv.second;
        if (e.dirty) {
            saveOverrides(e.ch, kv.first.cx, kv.first.cy);
            e.dirty = false;
        }
    }
}
static inline int floorDiv(int a, int b) { return (a >= 0) ? (a / b) : ((a - (b - 1)) / b); }

using std::string;
namespace fs = std::filesystem;

void ChunkManager::clear() {
    // Save any dirty chunks to disk, then clear structures
    for (auto& kv : _cache) {
        Entry& e = kv.second;
        if (e.dirty) {
            saveOverrides(e.ch, kv.first.cx, kv.first.cy);
            e.dirty = false;
        }
    }
    _cache.clear();
    _lru.clear();
}

void ChunkManager::resetOverrides() {
    // Delete persisted overrides directory for current seed/continents
    std::ostringstream oss;
    oss << "maps/seed_" << _seed;
    if (_continents) oss << "_cont";
    fs::path dir(oss.str());
    std::error_code ec;
    fs::remove_all(dir, ec); // ignore errors

    // Clear in-memory cache WITHOUT saving dirty chunks
    _cache.clear();
    _lru.clear();
}

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
    // Load persisted overrides if any
    loadOverrides(e.ch, cx, cy);
    _lru.push_front(key);
    e.it = _lru.begin();
    _cache.emplace(key, std::move(e));
    // Evict if needed
    while (_cache.size() > static_cast<size_t>(cfg::MAX_CACHED_CHUNKS)) {
        const ChunkKey& oldKey = _lru.back();
        auto itold = _cache.find(oldKey);
        if (itold != _cache.end()) {
            if (itold->second.dirty) {
                saveOverrides(itold->second.ch, itold->first.cx, itold->first.cy);
                itold->second.dirty = false;
            }
            _cache.erase(itold);
        }
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

    // Effective mountain mask tuning depending on continents toggle
    const float mFreq     = cfg::MNT_MASK_FREQ * (_continents ? 0.5f : 1.f);   // larger chains on continents
    const float mWarp     = cfg::MNT_MASK_WARP * (_continents ? 0.5f : 1.f);   // less wiggly
    const float mThresh   = cfg::MNT_MASK_THRESH + (_continents ? 0.10f : 0.f); // activate less often
    const float mStrength = cfg::MNT_MASK_STRENGTH * (_continents ? 0.35f : 1.f); // softer relief

    for (int i = 0; i <= S; ++i) {
        for (int j = 0; j <= S; ++j) {
            int I = I0 + i; // world tile coords
            int J = J0 + j;
            float x = I * worldFreq;
            float y = J * worldFreq;
            float n = noise::fbm(x, y, _seed, 5, 2.0f, 0.5f); // [-1,1]
            float t = 0.5f * (n + 1.0f);                      // [0,1]
            float h0 = (float)cfg::MIN_ELEV + t * (float)(cfg::MAX_ELEV - cfg::MIN_ELEV);
            float h = h0 * cfg::HEIGHT_SCALE - seaOffset;
            int hi = (int)std::round(h);

            // Mountain chain mask: low-frequency ridged band with domain warp
            {
                float cx = x * mFreq;
                float cy = y * mFreq;
                // Domain warp via low-octave fbm noise
                float nwx = noise::fbm(cx * 0.5f, cy * 0.5f, _seed + 9001u, 3, 2.0f, 0.5f); // [-1,1]
                float nwy = noise::fbm((cx + 5.3f) * 0.5f, (cy - 2.7f) * 0.5f, _seed + 1723u, 3, 2.0f, 0.5f);
                float wx = nwx * mWarp;
                float wy = nwy * mWarp;
                float nm = noise::fbm(cx + wx, cy + wy, _seed + 1337u, 4, 2.0f, 0.5f); // [-1,1]
                float nm01 = 0.5f * (nm + 1.f); // [0,1]
                float mr = 1.f - std::fabs(2.f * nm01 - 1.f); // ridged band [0,1]
                mr = std::clamp(mr, 0.0f, 1.0f);
                if (mr > mThresh) {
                    float tmask = (mr - mThresh) / std::max(1e-4f, 1.f - mThresh);
                    hi += (int)std::round(tmask * mStrength);
                }
            }
            // Rare high mountain spikes (only on land). Deterministic per (I,J,_seed).
            if (hi > 0) {
                // 32-bit integer hash
                auto hash2 = [](int xk, int yk, uint32_t s)->uint32_t{
                    uint32_t h = s;
                    h ^= 0x9E3779B9u + (uint32_t)xk + (h<<6) + (h>>2);
                    h ^= 0x85EBCA6Bu + (uint32_t)yk + (h<<6) + (h>>2);
                    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
                    return h;
                };
                uint32_t hv = hash2(I, J, _seed ^ 0xBEEF1234u);
                float r = (hv & 0xFFFFFF) / 16777215.f; // [0,1]
                if (r < cfg::RARE_PEAK_PROB) {
                    hi += (int)std::round(cfg::RARE_PEAK_BOOST);
                }
            }
            out.heights[Chunk::idx(i, j)] = clampi(hi, cfg::MIN_ELEV, cfg::MAX_ELEV);
        }
    }
}

void ChunkManager::applySetAt(int I, int J, int value) {
    const int S = cfg::CHUNK_SIZE;
    int cx = floorDiv(I, S);
    int cy = floorDiv(J, S);
    int li = clampi(I - cx * S, 0, S);
    int lj = clampi(J - cy * S, 0, S);

    auto ensureChunk = [&](int ecx, int ecy) -> Entry& {
        ChunkKey k{ecx, ecy};
        auto it = _cache.find(k);
        if (it == _cache.end()) {
            Entry e{};
            generateChunk(e.ch, ecx, ecy);
            _lru.push_front(k);
            e.it = _lru.begin();
            it = _cache.emplace(k, std::move(e)).first;
            while (_cache.size() > static_cast<size_t>(cfg::MAX_CACHED_CHUNKS)) {
                const ChunkKey& oldKey = _lru.back();
                _cache.erase(oldKey);
                _lru.pop_back();
            }
        } else {
            _lru.erase(it->second.it);
            _lru.push_front(k);
            it->second.it = _lru.begin();
        }
        return it->second;
    };

    int v = clampi(value, cfg::MIN_ELEV, cfg::MAX_ELEV);

    auto writeTo = [&](int ecx, int ecy, int lli, int llj){
        Entry& e = ensureChunk(ecx, ecy);
        int kk = Chunk::idx(lli, llj);
        e.ch.overrides[kk] = v;
        e.ch.overrideMask[kk] = 1u;
        e.ch.heights[kk] = v;
        e.dirty = true;
    };

    // Primary chunk
    writeTo(cx, cy, li, lj);
    // Mirror across edges/corners
    if (li == 0)        writeTo(cx - 1, cy, S, lj);
    if (li == S)        writeTo(cx + 1, cy, 0, lj);
    if (lj == 0)        writeTo(cx, cy - 1, li, S);
    if (lj == S)        writeTo(cx, cy + 1, li, 0);
    if (li == 0 && lj == 0)     writeTo(cx - 1, cy - 1, S, S);
    if (li == 0 && lj == S)     writeTo(cx - 1, cy + 1, S, 0);
    if (li == S && lj == 0)     writeTo(cx + 1, cy - 1, 0, S);
    if (li == S && lj == S)     writeTo(cx + 1, cy + 1, 0, 0);
}

void ChunkManager::applyDeltaAt(int I, int J, int delta) {
    const int S = cfg::CHUNK_SIZE;
    int cx = floorDiv(I, S);
    int cy = floorDiv(J, S);
    int li = clampi(I - cx * S, 0, S);
    int lj = clampi(J - cy * S, 0, S);

    auto ensureChunk = [&](int ecx, int ecy) -> Entry& {
        ChunkKey k{ecx, ecy};
        auto it = _cache.find(k);
        if (it == _cache.end()) {
            Entry e{};
            generateChunk(e.ch, ecx, ecy);
            _lru.push_front(k);
            e.it = _lru.begin();
            it = _cache.emplace(k, std::move(e)).first;
            while (_cache.size() > static_cast<size_t>(cfg::MAX_CACHED_CHUNKS)) {
                const ChunkKey& oldKey = _lru.back();
                _cache.erase(oldKey);
                _lru.pop_back();
            }
        } else {
            _lru.erase(it->second.it);
            _lru.push_front(k);
            it->second.it = _lru.begin();
        }
        return it->second;
    };

    // Determine target value from primary chunk height + delta
    Entry& e0 = ensureChunk(cx, cy);
    int k0 = Chunk::idx(li, lj);
    int base = e0.ch.overrideMask[k0] ? e0.ch.overrides[k0] : e0.ch.heights[k0];
    int v = clampi(base + delta, cfg::MIN_ELEV, cfg::MAX_ELEV);

    auto writeTo = [&](int ecx, int ecy, int lli, int llj){
        Entry& e = ensureChunk(ecx, ecy);
        int kk = Chunk::idx(lli, llj);
        e.ch.overrides[kk] = v;
        e.ch.overrideMask[kk] = 1u;
        e.ch.heights[kk] = v;
        e.dirty = true;
    };

    // Write to primary and neighbors
    writeTo(cx, cy, li, lj);
    if (li == 0)        writeTo(cx - 1, cy, S, lj);
    if (li == S)        writeTo(cx + 1, cy, 0, lj);
    if (lj == 0)        writeTo(cx, cy - 1, li, S);
    if (lj == S)        writeTo(cx, cy + 1, li, 0);
    if (li == 0 && lj == 0)     writeTo(cx - 1, cy - 1, S, S);
    if (li == 0 && lj == S)     writeTo(cx - 1, cy + 1, S, 0);
    if (li == S && lj == 0)     writeTo(cx + 1, cy - 1, 0, S);
    if (li == S && lj == S)     writeTo(cx + 1, cy + 1, 0, 0);
}

// ===== Persistence helpers =====
string ChunkManager::chunkPath(int cx, int cy) const {
    // maps/seed_<seed>[_cont]/cX_Y.csv
    std::ostringstream oss;
    oss << "maps/seed_" << _seed;
    if (_continents) oss << "_cont";
    string dir = oss.str();
    std::ostringstream fn;
    fn << dir << "/c" << cx << "_" << cy << ".csv";
    return fn.str();
}

void ChunkManager::ensureDir() const {
    std::ostringstream oss;
    oss << "maps/seed_" << _seed;
    if (_continents) oss << "_cont";
    fs::path p(oss.str());
    std::error_code ec;
    fs::create_directories(p, ec);
}

void ChunkManager::loadOverrides(Chunk& ch, int cx, int cy) {
    // Reset masks (in case caller reuses chunk object)
    std::fill(ch.overrideMask.begin(), ch.overrideMask.end(), 0);
    std::fill(ch.overrides.begin(), ch.overrides.end(), 0);
    string path = chunkPath(cx, cy);
    std::ifstream in(path);
    if (!in) return;
    const int S1 = cfg::CHUNK_SIZE + 1;
    string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        string a,b,c;
        if (!std::getline(ss, a, ',')) continue;
        if (!std::getline(ss, b, ',')) continue;
        if (!std::getline(ss, c, ',')) continue;
        int i = 0, j = 0, v = 0;
        try { i = std::stoi(a); j = std::stoi(b); v = std::stoi(c); } catch (...) { continue; }
        if (i < 0 || j < 0 || i >= S1 || j >= S1) continue;
        int k = Chunk::idx(i, j);
        v = clampi(v, cfg::MIN_ELEV, cfg::MAX_ELEV);
        ch.overrides[k] = v;
        ch.overrideMask[k] = 1u;
        ch.heights[k] = v; // apply on top of generated
    }
}

void ChunkManager::saveOverrides(const Chunk& ch, int cx, int cy) {
    ensureDir();
    string path = chunkPath(cx, cy);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    const int S1 = cfg::CHUNK_SIZE + 1;
    for (int i = 0; i < S1; ++i) {
        for (int j = 0; j < S1; ++j) {
            int k = Chunk::idx(i, j);
            if (!ch.overrideMask[k]) continue;
            out << i << "," << j << "," << ch.overrides[k] << "\n";
        }
    }
}
