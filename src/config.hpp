#pragma once

namespace cfg {
    // Chunked world configuration
    constexpr int CHUNK_SIZE = 60;           // tiles per chunk side (chunk grid is (CHUNK_SIZE+1)^2 vertices)
    constexpr int MAX_CACHED_CHUNKS = 121;   // simple LRU budget (e.g., 11x11 visible)

    constexpr int GRID = 300;                // 300 tiles per side
    constexpr float TILE_W = 32.f;           // visual diamond width in pixels
    constexpr float TILE_H = 16.f;           // visual diamond height in pixels (smaller to yield diamond look)
    constexpr float ELEV_STEP = 8.f;         // pixel offset per height unit (more dramatic elevation)
    // Expanded range for impressive peaks and depths
    constexpr int MAX_ELEV = 18;             // clamp limits (allow higher peaks)
    constexpr int MIN_ELEV = -5;
    constexpr unsigned WINDOW_W = 1280;
    constexpr unsigned WINDOW_H = 800;

    // Noise parameters
    constexpr float NOISE_BASE_SCALE = 3.5f; // how many large features across the grid
    constexpr float NOISE_EXP = 0.85f;       // gamma (<1 accentue les pics, >1 aplanit)
    constexpr float NOISE_RIDGED_WEIGHT = 0.6f; // 0..1: mélange FBM standard vs ridged (pics marqués)
    constexpr float NOISE_WARP_STRENGTH = 0.25f; // intensité du petit warp pour disperser les pics
    constexpr float NOISE_WARP_SCALE = 0.8f;     // échelle du bruit de warp (faible fréquence)

    // Sea level control (height units to subtract after mapping). Higher => plus d'eau
    constexpr float SEA_OFFSET = 1.0f;

    // Island mask: plus on s'éloigne du centre, plus le terrain baisse (eau aux bords)
    constexpr float ISLAND_RADIUS = 0.45f;   // rayon (0..~0.7). Plus petit => plus d'eau
    constexpr float ISLAND_POWER  = 2.0f;    // exponent pour la pente de la côte (2..4)
}
