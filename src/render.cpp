#include "render.hpp"
#include "config.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
    inline int idx(int i, int j) { return i * (cfg::GRID + 1) + j; }

    inline void appendLine(sf::VertexArray& va, const sf::Vector2f& p1, const sf::Vector2f& p2, sf::Color col) {
        va.append(sf::Vertex(p1, col));
        va.append(sf::Vertex(p2, col));
    }

    inline bool rectsIntersect(const sf::FloatRect& a, const sf::FloatRect& b) {
        return a.intersects(b);
    }

    inline sf::FloatRect quadBounds(const sf::Vector2f& A, const sf::Vector2f& B,
                                    const sf::Vector2f& C, const sf::Vector2f& D) {
        float minx = std::min(std::min(A.x, B.x), std::min(C.x, D.x));
        float maxx = std::max(std::max(A.x, B.x), std::max(C.x, D.x));
        float miny = std::min(std::min(A.y, B.y), std::min(C.y, D.y));
        float maxy = std::max(std::max(A.y, B.y), std::max(C.y, D.y));
        return sf::FloatRect(minx, miny, maxx - minx, maxy - miny);
    }
}

namespace render {

std::vector<std::vector<sf::Vector2f>> buildProjectedMap(
    const std::vector<int>& heights,
    const IsoParams& iso,
    const sf::Vector2f& origin)
{
    std::vector<std::vector<sf::Vector2f>> map2d(cfg::GRID + 1, std::vector<sf::Vector2f>(cfg::GRID + 1));
    for (int i = 0; i <= cfg::GRID; ++i) {
        for (int j = 0; j <= cfg::GRID; ++j) {
            int h = heights[idx(i, j)];
            map2d[i][j] = isoProjectDyn((float)i, (float)j, h * cfg::ELEV_STEP, iso) + origin;
        }
    }
    return map2d;
}

void draw2DMap(sf::RenderTarget& target, const std::vector<std::vector<sf::Vector2f>>& map2d) {
    int H = (int)map2d.size();
    if (H == 0) return;
    int W = (int)map2d[0].size();

    const auto& view = target.getView();
    sf::Vector2f vc = view.getCenter();
    sf::Vector2f vs = view.getSize();
    const float margin = 64.f;
    sf::FloatRect viewRect(vc.x - vs.x * 0.5f - margin,
                           vc.y - vs.y * 0.5f - margin,
                           vs.x + 2 * margin, vs.y + 2 * margin);

    int stride = 1; // draw every segment
    sf::VertexArray lines(sf::Lines);
    const sf::Color col = sf::Color::White;

    for (int i = 0; i < H; i += stride) {
        for (int j = 0; j < W; j += stride) {
            if (j + stride < W) {
                const sf::Vector2f& p1 = map2d[i][j];
                const sf::Vector2f& p2 = map2d[i][j + stride];
                sf::FloatRect segRect(std::min(p1.x, p2.x), std::min(p1.y, p2.y),
                                      std::fabs(p1.x - p2.x), std::fabs(p1.y - p2.y));
                if (rectsIntersect(segRect, viewRect)) appendLine(lines, p1, p2, col);
            }
            if (i + stride < H) {
                const sf::Vector2f& p1 = map2d[i][j];
                const sf::Vector2f& p2 = map2d[i + stride][j];
                sf::FloatRect segRect(std::min(p1.x, p2.x), std::min(p1.y, p2.y),
                                      std::fabs(p1.x - p2.x), std::fabs(p1.y - p2.y));
                if (rectsIntersect(segRect, viewRect)) appendLine(lines, p1, p2, col);
            }
        }
    }
    if (lines.getVertexCount() > 0) target.draw(lines);
}

void draw2DFilledCells(sf::RenderTarget& target,
                       const std::vector<std::vector<sf::Vector2f>>& map2d,
                       const std::vector<int>& heights,
                       bool enableShadows)
{
    int H = (int)map2d.size();
    if (H == 0) return;
    int W = (int)map2d[0].size();

    const auto& view = target.getView();
    sf::Vector2f vc = view.getCenter();
    sf::Vector2f vs = view.getSize();
    const float margin = 64.f;
    sf::FloatRect viewRect(vc.x - vs.x * 0.5f - margin,
                           vc.y - vs.y * 0.5f - margin,
                           vs.x + 2 * margin, vs.y + 2 * margin);

    int stride = 1; // fill every cell (no LOD)

    // --- Fast slope-based lighting (approximate) ---
    // Light direction in grid space with vertical component
    // Goal: shadows cast toward the NORTH => light comes from the SOUTH (positive Y)
    // Lower Z makes longer, more pronounced peak shadows.
    const sf::Vector3f lightDir = sf::Vector3f(0.f, 1.f, 0.8f); // from south to north, lower elevation
    auto norm3 = [](sf::Vector3f v){
        float L = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        if (L <= 1e-6f) return sf::Vector3f(0,0,1);
        return sf::Vector3f(v.x/L, v.y/L, v.z/L);
    };
    const sf::Vector3f Ldir = norm3(lightDir);

    // --- Heightmap-based cast shadows (full grid) ---
    std::vector<uint8_t> shadowMask; shadowMask.resize((size_t)H * (size_t)W, 0);
    auto id = [&](int i, int j){ return i * W + j; };
    auto inBounds = [&](int i, int j){ return (i >= 0 && j >= 0 && i < H && j < W); };
    if (enableShadows) {
        // 2D light dir in grid space (projection of lightDir onto the grid plane)
        // March TOWARD the light source: step along -Ldir in grid indices via x -= lightDirGrid.x
        sf::Vector2f lightDirGrid(Ldir.x, Ldir.y);
        float len2 = std::sqrt(lightDirGrid.x * lightDirGrid.x + lightDirGrid.y * lightDirGrid.y);
        if (len2 > 0.f) {
            lightDirGrid.x /= len2;
            lightDirGrid.y /= len2;
        } else {
            lightDirGrid = sf::Vector2f(-1.f, -1.f);
        }
        // Rise per cell along the light direction, in height units (same units as 'heights')
        float horizLen = std::sqrt(Ldir.x * Ldir.x + Ldir.y * Ldir.y);
        float elev = std::atan2(std::max(1e-4f, Ldir.z), std::max(1e-4f, horizLen)); // elevation above horizon
        const float risePerStep = (float)std::max(0.02f, std::tan(elev)); // allow small values for longer shadows
        const int maxSteps = 96;

        // March from each cell towards the light source; if any encountered terrain
        // is above the rising reference line, the cell is shadowed.
        for (int i = 0; i < H; ++i) {
            for (int j = 0; j < W; ++j) {
                float baseH = (float)std::clamp(heights[idx(i, j)], cfg::MIN_ELEV, cfg::MAX_ELEV);
                float x = (float)i;
                float y = (float)j;
                float refH = baseH - 0.02f; // bias slightly below to keep tall peaks effective
                bool shadowed = false;
                for (int s = 0; s < maxSteps; ++s) {
                    x -= lightDirGrid.x;
                    y -= lightDirGrid.y;
                    refH += risePerStep;
                    int ii = (int)std::floor(x + 0.5f);
                    int jj = (int)std::floor(y + 0.5f);
                    if (!inBounds(ii, jj)) break;
                    float h = (float)std::clamp(heights[idx(ii, jj)], cfg::MIN_ELEV, cfg::MAX_ELEV);
                    if (h > refH) { shadowed = true; break; }
                }
                shadowMask[id(i, j)] = shadowed ? 1 : 0;
            }
        }
    }

    auto lerpColor = [](sf::Color a, sf::Color b, float t){
        t = std::clamp(t, 0.f, 1.f);
        auto L = [](uint8_t c){ return (int)c; };
        uint8_t r = (uint8_t)std::round(L(a.r) + (L(b.r) - L(a.r)) * t);
        uint8_t g = (uint8_t)std::round(L(a.g) + (L(b.g) - L(a.g)) * t);
        uint8_t bch = (uint8_t)std::round(L(a.b) + (L(b.b) - L(a.b)) * t);
        uint8_t aCh = (uint8_t)std::round(L(a.a) + (L(b.a) - L(a.a)) * t);
        return sf::Color(r, g, bch, aCh);
    };

    auto colorForHeight = [&](float h){
        const sf::Color normalBlue(30, 144, 255);
        const sf::Color veryDarkBlue(0, 0, 80);
        const sf::Color sandYellow(255, 236, 170); // light yellow
        const sf::Color grass(34, 139, 34);
        const sf::Color gray(128, 128, 128);
        const sf::Color rock(110, 110, 110);
        const sf::Color snow(245, 245, 245);

        if (h < 0.f && h >= -5.f) {
            float t = (-h) / 5.f; // 0 -> 0, -5 -> 1
            return lerpColor(normalBlue, veryDarkBlue, t);
        }
        if (h >= 0.f && h < 1.f) {
            float t = (h - 0.f) / 1.f; // 0->0, 1->1
            return lerpColor(normalBlue, sandYellow, t);
        }
        if (h >= 1.f && h < 2.f) {
            float t = (h - 1.f) / 1.f; // 1->0, 2->1
            return lerpColor(sandYellow, grass, t);
        }
        if (h >= 2.f && h < 3.f) return grass;
        if (h >= 3.f && h <= 5.f) {
            float t = (h - 3.f) / 2.f; // 3->0, 5->1
            return lerpColor(grass, gray, t);
        }
        if (h > 5.f && h <= 7.f) return rock;
        if (h > 7.f && h < 8.f) {
            float t = (h - 7.f) / 1.f; // 7->0, 8->1
            return lerpColor(gray, snow, t);
        }
        if (h >= 8.f) return snow;
        return grass;
    };

    // Pre-batch triangles
    sf::VertexArray tris(sf::Triangles);

    auto multColor = [](sf::Color c, float f){
        auto clampU8 = [](int v){ return (uint8_t)std::clamp(v, 0, 255); };
        return sf::Color(clampU8((int)std::round(c.r * f)),
                         clampU8((int)std::round(c.g * f)),
                         clampU8((int)std::round(c.b * f)),
                         c.a);
    };

    // Fast normal using central differences in grid space (z in world units)
    auto heightAt = [&](int ii, int jj){ return (float)std::clamp(heights[idx(ii, jj)], cfg::MIN_ELEV, cfg::MAX_ELEV) * cfg::ELEV_STEP; };
    auto normalAt = [&](int ii, int jj){
        int i0 = std::max(ii - 1, 0), i1 = std::min(ii + 1, H - 1);
        int j0 = std::max(jj - 1, 0), j1 = std::min(jj + 1, W - 1);
        float dzdi = (heightAt(i1, jj) - heightAt(i0, jj)) * 0.5f; // slope along i
        float dzdj = (heightAt(ii, j1) - heightAt(ii, j0)) * 0.5f; // slope along j
        // Tangents: ti along +i, tj along +j. Normal ~ cross(ti, tj).
        sf::Vector3f ti(1.f, 0.f, dzdi);
        sf::Vector3f tj(0.f, 1.f, dzdj);
        sf::Vector3f n(
            ti.y * tj.z - ti.z * tj.y,
            ti.z * tj.x - ti.x * tj.z,
            ti.x * tj.y - ti.y * tj.x
        );
        return norm3(n);
    };

    for (int i = 0; i < H - 1; i += stride) {
        for (int j = 0; j < W - 1; j += stride) {
            const sf::Vector2f& A = map2d[i][j];
            const sf::Vector2f& B = map2d[std::min(i + stride, H - 1)][j];
            const sf::Vector2f& C = map2d[std::min(i + stride, H - 1)][std::min(j + stride, W - 1)];
            const sf::Vector2f& D = map2d[i][std::min(j + stride, W - 1)];

            if (!rectsIntersect(quadBounds(A, B, C, D), viewRect)) continue;

            float hA = (float)std::clamp(heights[idx(i, j)], cfg::MIN_ELEV, cfg::MAX_ELEV);
            float hB = (float)std::clamp(heights[idx(std::min(i + stride, H - 1), j)], cfg::MIN_ELEV, cfg::MAX_ELEV);
            float hC = (float)std::clamp(heights[idx(std::min(i + stride, H - 1), std::min(j + stride, W - 1))], cfg::MIN_ELEV, cfg::MAX_ELEV);
            float hD = (float)std::clamp(heights[idx(i, std::min(j + stride, W - 1))], cfg::MIN_ELEV, cfg::MAX_ELEV);
            float hAvg = 0.25f * (hA + hB + hC + hD);

            sf::Vector3f n = normalAt(i, j);
            float ndotl = std::max(0.0f, n.x * Ldir.x + n.y * Ldir.y + n.z * Ldir.z);
            // Ensure some ambient term
            float shade = 0.5f + 0.5f * ndotl; // [0.5..1.0]

            float shadeFinal = shade;
            if (enableShadows) {
                // Shadow factor from mask (average of quad corners for smoother edges)
                float sh = 0.f;
                sh += shadowMask[id(i, j)];
                sh += shadowMask[id(std::min(i + stride, H - 1), j)];
                sh += shadowMask[id(std::min(i + stride, H - 1), std::min(j + stride, W - 1))];
                sh += shadowMask[id(i, std::min(j + stride, W - 1))];
                sh *= 0.25f; // [0..1]
                float shadowFactor = 1.0f - 0.35f * sh; // darken up to 35%
                shadeFinal = shade * shadowFactor;
            }

            sf::Color base = colorForHeight(hAvg);
            sf::Color c = multColor(base, shadeFinal);

            // Two triangles: A-B-C and A-C-D
            tris.append(sf::Vertex(A, c));
            tris.append(sf::Vertex(B, c));
            tris.append(sf::Vertex(C, c));

            tris.append(sf::Vertex(A, c));
            tris.append(sf::Vertex(C, c));
            tris.append(sf::Vertex(D, c));
        }
    }
    if (tris.getVertexCount() > 0) target.draw(tris);
}

// -------- Per-chunk variants --------

std::vector<std::vector<sf::Vector2f>> buildProjectedMapChunk(
    const std::vector<int>& heights,
    int S,
    int I0, int J0,
    const IsoParams& iso,
    const sf::Vector2f& origin)
{
    const int W = S + 1;
    auto idc = [&](int i, int j){ return i * W + j; };
    std::vector<std::vector<sf::Vector2f>> map2d(W, std::vector<sf::Vector2f>(W));
    for (int i = 0; i <= S; ++i) {
        for (int j = 0; j <= S; ++j) {
            int h = heights[idc(i, j)];
            map2d[i][j] = isoProjectDyn((float)(I0 + i), (float)(J0 + j), h * cfg::ELEV_STEP, iso) + origin;
        }
    }
    return map2d;
}

void draw2DMapChunk(sf::RenderTarget& target,
                    const std::vector<std::vector<sf::Vector2f>>& map2d)
{
    int H = (int)map2d.size();
    if (H == 0) return;
    int W = (int)map2d[0].size();

    const auto& view = target.getView();
    sf::Vector2f vc = view.getCenter();
    sf::Vector2f vs = view.getSize();
    const float margin = 64.f;
    sf::FloatRect viewRect(vc.x - vs.x * 0.5f - margin,
                           vc.y - vs.y * 0.5f - margin,
                           vs.x + 2 * margin, vs.y + 2 * margin);

    int stride = 1;
    sf::VertexArray lines(sf::Lines);
    const sf::Color col = sf::Color::White;
    for (int i = 0; i < H; i += stride) {
        for (int j = 0; j < W; j += stride) {
            if (j + stride < W) {
                const sf::Vector2f& p1 = map2d[i][j];
                const sf::Vector2f& p2 = map2d[i][j + stride];
                sf::FloatRect segRect(std::min(p1.x, p2.x), std::min(p1.y, p2.y),
                                      std::fabs(p1.x - p2.x), std::fabs(p1.y - p2.y));
                if (rectsIntersect(segRect, viewRect)) appendLine(lines, p1, p2, col);
            }
            if (i + stride < H) {
                const sf::Vector2f& p1 = map2d[i][j];
                const sf::Vector2f& p2 = map2d[i + stride][j];
                sf::FloatRect segRect(std::min(p1.x, p2.x), std::min(p1.y, p2.y),
                                      std::fabs(p1.x - p2.x), std::fabs(p1.y - p2.y));
                if (rectsIntersect(segRect, viewRect)) appendLine(lines, p1, p2, col);
            }
        }
    }
    if (lines.getVertexCount() > 0) target.draw(lines);
}

void draw2DFilledCellsChunk(sf::RenderTarget& target,
                            const std::vector<std::vector<sf::Vector2f>>& map2d,
                            const std::vector<int>& heights,
                            int S,
                            bool enableShadows)
{
    int H = (int)map2d.size();
    if (H == 0) return;
    int W = (int)map2d[0].size();
    auto idc = [&](int i, int j){ return i * W + j; };

    const auto& view = target.getView();
    sf::Vector2f vc = view.getCenter();
    sf::Vector2f vs = view.getSize();
    const float margin = 64.f;
    sf::FloatRect viewRect(vc.x - vs.x * 0.5f - margin,
                           vc.y - vs.y * 0.5f - margin,
                           vs.x + 2 * margin, vs.y + 2 * margin);

    int stride = 1;

    const sf::Vector3f lightDir = sf::Vector3f(0.f, 1.f, 0.8f);
    auto norm3 = [](sf::Vector3f v){
        float L = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        if (L <= 1e-6f) return sf::Vector3f(0,0,1);
        return sf::Vector3f(v.x/L, v.y/L, v.z/L);
    };
    const sf::Vector3f Ldir = norm3(lightDir);

    std::vector<uint8_t> shadowMask; shadowMask.resize((size_t)H * (size_t)W, 0);
    auto id = [&](int i, int j){ return i * W + j; };
    auto inBounds = [&](int i, int j){ return (i >= 0 && j >= 0 && i < H && j < W); };
    if (enableShadows) {
        sf::Vector2f lightDirGrid(Ldir.x, Ldir.y);
        float len2 = std::sqrt(lightDirGrid.x * lightDirGrid.x + lightDirGrid.y * lightDirGrid.y);
        if (len2 > 0.f) { lightDirGrid.x /= len2; lightDirGrid.y /= len2; } else { lightDirGrid = sf::Vector2f(-1.f, -1.f); }
        float horizLen = std::sqrt(Ldir.x * Ldir.x + Ldir.y * Ldir.y);
        float elev = std::atan2(std::max(1e-4f, Ldir.z), std::max(1e-4f, horizLen));
        const float risePerStep = (float)std::max(0.02f, std::tan(elev));
        const int maxSteps = 96;
        for (int i = 0; i < H; ++i) {
            for (int j = 0; j < W; ++j) {
                float baseH = (float)std::clamp(heights[idc(i, j)], cfg::MIN_ELEV, cfg::MAX_ELEV);
                float x = (float)i;
                float y = (float)j;
                float refH = baseH - 0.02f;
                bool shadowed = false;
                for (int s = 0; s < maxSteps; ++s) {
                    x -= lightDirGrid.x;
                    y -= lightDirGrid.y;
                    refH += risePerStep;
                    int ii = (int)std::floor(x + 0.5f);
                    int jj = (int)std::floor(y + 0.5f);
                    if (!inBounds(ii, jj)) break;
                    float h = (float)std::clamp(heights[idc(ii, jj)], cfg::MIN_ELEV, cfg::MAX_ELEV);
                    if (h > refH) { shadowed = true; break; }
                }
                shadowMask[id(i, j)] = shadowed ? 1 : 0;
            }
        }
    }

    auto lerpColor = [](sf::Color a, sf::Color b, float t){
        t = std::clamp(t, 0.f, 1.f);
        auto L = [](uint8_t c){ return (int)c; };
        uint8_t r = (uint8_t)std::round(L(a.r) + (L(b.r) - L(a.r)) * t);
        uint8_t g = (uint8_t)std::round(L(a.g) + (L(b.g) - L(a.g)) * t);
        uint8_t bch = (uint8_t)std::round(L(a.b) + (L(b.b) - L(a.b)) * t);
        uint8_t aCh = (uint8_t)std::round(L(a.a) + (L(b.a) - L(a.a)) * t);
        return sf::Color(r, g, bch, aCh);
    };
    auto multColor = [](sf::Color c, float f){
        auto clampU8 = [](int v){ return (uint8_t)std::clamp(v, 0, 255); };
        return sf::Color(clampU8((int)std::round(c.r * f)),
                         clampU8((int)std::round(c.g * f)),
                         clampU8((int)std::round(c.b * f)),
                         c.a);
    };
    auto colorForHeight = [&](float h){
        const sf::Color normalBlue(30, 144, 255);
        const sf::Color veryDarkBlue(0, 0, 80);
        const sf::Color sandYellow(255, 236, 170);
        const sf::Color grass(34, 139, 34);
        const sf::Color gray(128, 128, 128);
        const sf::Color rock(110, 110, 110);
        const sf::Color snow(245, 245, 245);
        if (h < 0.f && h >= -5.f) { float t = (-h) / 5.f; return lerpColor(normalBlue, veryDarkBlue, t); }
        if (h >= 0.f && h < 1.f) { float t = (h - 0.f) / 1.f; return lerpColor(normalBlue, sandYellow, t); }
        if (h >= 1.f && h < 2.f) { float t = (h - 1.f) / 1.f; return lerpColor(sandYellow, grass, t); }
        if (h >= 2.f && h < 3.f) return grass;
        if (h >= 3.f && h <= 5.f) { float t = (h - 3.f) / 2.f; return lerpColor(grass, gray, t); }
        if (h > 5.f && h <= 7.f) return rock;
        if (h > 7.f && h < 8.f) { float t = (h - 7.f) / 1.f; return lerpColor(gray, snow, t); }
        if (h >= 8.f) return snow;
        return grass;
    };

    auto heightAt = [&](int ii, int jj){ return (float)std::clamp(heights[idc(ii, jj)], cfg::MIN_ELEV, cfg::MAX_ELEV) * cfg::ELEV_STEP; };
    auto normalAt = [&](int ii, int jj){
        int i0 = std::max(ii - 1, 0), i1 = std::min(ii + 1, H - 1);
        int j0 = std::max(jj - 1, 0), j1 = std::min(jj + 1, W - 1);
        float dzdi = (heightAt(i1, jj) - heightAt(i0, jj)) * 0.5f;
        float dzdj = (heightAt(ii, j1) - heightAt(ii, j0)) * 0.5f;
        sf::Vector3f ti(1.f, 0.f, dzdi);
        sf::Vector3f tj(0.f, 1.f, dzdj);
        sf::Vector3f n(
            ti.y * tj.z - ti.z * tj.y,
            ti.z * tj.x - ti.x * tj.z,
            ti.x * tj.y - ti.y * tj.x
        );
        return norm3(n);
    };

    sf::VertexArray tris(sf::Triangles);
    for (int i = 0; i < H - 1; i += stride) {
        for (int j = 0; j < W - 1; j += stride) {
            const sf::Vector2f& A = map2d[i][j];
            const sf::Vector2f& B = map2d[std::min(i + stride, H - 1)][j];
            const sf::Vector2f& C = map2d[std::min(i + stride, H - 1)][std::min(j + stride, W - 1)];
            const sf::Vector2f& D = map2d[i][std::min(j + stride, W - 1)];
            if (!rectsIntersect(quadBounds(A, B, C, D), viewRect)) continue;
            float hA = (float)std::clamp(heights[idc(i, j)], cfg::MIN_ELEV, cfg::MAX_ELEV);
            float hB = (float)std::clamp(heights[idc(std::min(i + stride, H - 1), j)], cfg::MIN_ELEV, cfg::MAX_ELEV);
            float hC = (float)std::clamp(heights[idc(std::min(i + stride, H - 1), std::min(j + stride, W - 1))], cfg::MIN_ELEV, cfg::MAX_ELEV);
            float hD = (float)std::clamp(heights[idc(i, std::min(j + stride, W - 1))], cfg::MIN_ELEV, cfg::MAX_ELEV);
            float hAvg = 0.25f * (hA + hB + hC + hD);
            sf::Vector3f n = normalAt(i, j);
            float ndotl = std::max(0.0f, n.x * Ldir.x + n.y * Ldir.y + n.z * Ldir.z);
            float shade = 0.5f + 0.5f * ndotl;
            float shadeFinal = shade;
            if (enableShadows) {
                float sh = 0.f;
                sh += shadowMask[id(i, j)];
                sh += shadowMask[id(std::min(i + stride, H - 1), j)];
                sh += shadowMask[id(std::min(i + stride, H - 1), std::min(j + stride, W - 1))];
                sh += shadowMask[id(i, std::min(j + stride, W - 1))];
                sh *= 0.25f;
                float shadowFactor = 1.0f - 0.35f * sh;
                shadeFinal = shade * shadowFactor;
            }
            auto base = colorForHeight(hAvg);
            auto c = multColor(base, shadeFinal);
            tris.append(sf::Vertex(A, c));
            tris.append(sf::Vertex(B, c));
            tris.append(sf::Vertex(C, c));
            tris.append(sf::Vertex(A, c));
            tris.append(sf::Vertex(C, c));
            tris.append(sf::Vertex(D, c));
        }
    }
    if (tris.getVertexCount() > 0) target.draw(tris);
}

} // namespace render
