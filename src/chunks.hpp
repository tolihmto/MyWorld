#pragma once
#include <vector>
#include <unordered_map>
#include <list>
#include <cstdint>
#include <utility>
#include <string>
#include "config.hpp"

// Chunked world primitives
struct ChunkKey {
    int cx;
    int cy;
    bool operator==(const ChunkKey& o) const noexcept { return cx == o.cx && cy == o.cy; }
};

struct ChunkKeyHash {
    size_t operator()(const ChunkKey& k) const noexcept {
        // 64-bit mix
        uint64_t x = static_cast<uint64_t>(static_cast<uint32_t>(k.cx));
        uint64_t y = static_cast<uint64_t>(static_cast<uint32_t>(k.cy));
        uint64_t h = x * 0x9E3779B185EBCA87ULL ^ (y + 0xC2B2AE3D27D4EB4FULL + (x<<6) + (x>>2));
        return static_cast<size_t>(h ^ (h >> 32));
    }
};

struct Chunk {
    // Heights at grid intersections: (CHUNK_SIZE+1) x (CHUNK_SIZE+1)
    std::vector<int> heights;
    // Overrides: if mask[k]!=0, heights[k] has been forced to overrides[k]
    std::vector<int> overrides;
    std::vector<uint8_t> overrideMask;
    Chunk()
        : heights((cfg::CHUNK_SIZE + 1) * (cfg::CHUNK_SIZE + 1), 0)
        , overrides((cfg::CHUNK_SIZE + 1) * (cfg::CHUNK_SIZE + 1), 0)
        , overrideMask((cfg::CHUNK_SIZE + 1) * (cfg::CHUNK_SIZE + 1), 0) {}
    static inline int idx(int i, int j) { return i * (cfg::CHUNK_SIZE + 1) + j; }
};

class ChunkManager {
public:
    enum class Mode { Empty, Procedural };

    explicit ChunkManager() : _mode(Mode::Empty), _seed(0) {}

    void setMode(Mode m, uint32_t seed) {
        _mode = m; _seed = seed; _cache.clear();
    }
    Mode mode() const { return _mode; }
    uint32_t seed() const { return _seed; }
    void setContinents(bool c) { _continents = c; clear(); }
    bool continents() const { return _continents; }

    // Get or build chunk at (cx, cy)
    const Chunk& getChunk(int cx, int cy);

    // Editing APIs (world coordinates in tile intersections)
    void applyDeltaAt(int I, int J, int delta);
    void applySetAt(int I, int J, int value);

    // Clears cache (persists dirty chunks first)
    void clear();

private:
    Mode _mode;
    uint32_t _seed;
    bool _continents = false;
    struct Entry { Chunk ch; bool dirty = false; std::list<ChunkKey>::iterator it; };
    std::unordered_map<ChunkKey, Entry, ChunkKeyHash> _cache;
    std::list<ChunkKey> _lru; // most-recent at front

    void generateChunk(Chunk& out, int cx, int cy);
    // Persistence helpers
    std::string chunkPath(int cx, int cy) const;
    void ensureDir() const;
    void loadOverrides(Chunk& ch, int cx, int cy);
    void saveOverrides(const Chunk& ch, int cx, int cy);
};
