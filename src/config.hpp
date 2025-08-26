#pragma once

namespace cfg {
    // Chunked world configuration
    constexpr int CHUNK_SIZE = 60;           // tiles per chunk side (chunk grid is (CHUNK_SIZE+1)^2 vertices)
    constexpr int MAX_CACHED_CHUNKS = 121;   // simple LRU budget (e.g., 11x11 visible)

    constexpr int GRID = 300;                // 300 tiles per side
    constexpr float TILE_W = 32.f;           // visual diamond width in pixels
    constexpr float TILE_H = 16.f;           // visual diamond height in pixels (smaller to yield diamond look)
    constexpr float ELEV_STEP = 6.f;         // pixel offset per height unit (slightly less dramatic elevation)
    // Expanded range for impressive peaks and depths
    constexpr int MAX_ELEV = 256;            // much higher cap to avoid plateau
    constexpr int MIN_ELEV = -64;            // deeper valleys allowed
    constexpr unsigned WINDOW_W = 1280;
    constexpr unsigned WINDOW_H = 800;

    // Noise parameters
    constexpr float NOISE_BASE_SCALE = 3.5f;     // how many large features across the grid
    constexpr float NOISE_EXP = 0.95f;           // gamma (<1 accentue les pics, >1 aplanit)
    constexpr float NOISE_RIDGED_WEIGHT = 0.35f; // 0..1: mélange FBM standard vs ridged (pics marqués)
    constexpr float NOISE_WARP_STRENGTH = 0.25f; // intensité du petit warp pour disperser les pics
    constexpr float NOISE_WARP_SCALE = 0.8f;     // échelle du bruit de warp (faible fréquence)
    // Global height compression (apply to computed height before sea offset). Smaller => much flatter terrain
    constexpr float HEIGHT_SCALE = 0.10f;        // ~10x moins haut

    // Sea level control (height units to subtract after mapping). Higher => plus d'eau
    constexpr float SEA_OFFSET = 6.5f;

    // Island mask: plus on s'éloigne du centre, plus le terrain baisse (eau aux bords)
    constexpr float ISLAND_RADIUS = 0.42f;   // rayon (0..~0.7). Plus petit => plus d'eau
    constexpr float ISLAND_POWER  = 2.0f;    // exponent pour la pente de la côte (2..4)

    // Rare high mountains (spikes): probability per vertex and height boost (height units)
    constexpr float RARE_PEAK_PROB  = 0.0f;     // disabled random spikes
    constexpr float RARE_PEAK_BOOST = 22.0f;    // irrelevant if prob is 0

    // Mountain chain mask: low-frequency ridged band to form continuous ranges
    constexpr float MNT_MASK_FREQ      = 0.6f;   // lower => larger chains; relative to base scale coords
    constexpr float MNT_MASK_THRESH    = 0.62f;  // threshold to activate chain boost
    constexpr float MNT_MASK_STRENGTH  = 14.0f;  // additional height units along chains (on top of base height)
    constexpr float MNT_MASK_WARP      = 0.5f;   // domain warp intensity for chains
}
