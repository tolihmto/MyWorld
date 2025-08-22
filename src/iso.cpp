#include "iso.hpp"
#include <cmath>

sf::Vector2f isoProjectDyn(float i, float j, float elev, const IsoParams& P) {
    const float hx = cfg::TILE_W * 0.5f;
    const float hy = cfg::TILE_H * 0.5f;
    // Base iso (before rotation), apply pitch on y and elevation as upward screen offset
    sf::Vector2f v{ (i - j) * hx, (i + j) * hy * P.pitch - elev };
    // Rotate by rotDeg
    float rad = P.rotDeg * 3.1415926535f / 180.f;
    float cs = std::cos(rad), sn = std::sin(rad);
    return { v.x * cs - v.y * sn, v.x * sn + v.y * cs };
}

sf::Vector2f isoUnprojectDyn(const sf::Vector2f& p, const IsoParams& P) {
    const float hx = cfg::TILE_W * 0.5f;
    const float hy = cfg::TILE_H * 0.5f;
    // Inverse rotate
    float rad = -P.rotDeg * 3.1415926535f / 180.f;
    float cs = std::cos(rad), sn = std::sin(rad);
    sf::Vector2f v{ p.x * cs - p.y * sn, p.x * sn + p.y * cs };
    // Undo pitch (elevation unknown => assume elev=0 for hit test)
    float vx = v.x;
    float vy = (P.pitch != 0.f) ? v.y / P.pitch : v.y;
    // Solve iso equations
    float ix = (hx != 0.f) ? vx / hx : 0.f;
    float iy = (hy != 0.f) ? vy / hy : 0.f;
    float i = (ix + iy) * 0.5f;
    float j = (iy - ix) * 0.5f;
    return {i, j};
}
