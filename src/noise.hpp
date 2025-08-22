#pragma once
#include <cstdint>

namespace noise {

// Simple deterministic 2D value-noise + FBM helper
float fbm(float x, float y, uint32_t seed,
          int octaves = 5,
          float lacunarity = 2.0f,
          float gain = 0.5f);

}
