#include "render.hpp"
#include "config.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace {
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
    
    // Single alpha compositing helper available to all functions in this file
    inline sf::Color alphaOver(sf::Color base, sf::Color paint){
        float a = paint.a / 255.f;
        auto mix = [&](uint8_t cb, uint8_t cp){ return (uint8_t)std::round(cb * (1.f - a) + cp * a); };
        return sf::Color(mix(base.r, paint.r), mix(base.g, paint.g), mix(base.b, paint.b), 255);
    }
}

namespace render {

std::vector<std::vector<sf::Vector2f>> buildProjectedMap(
    const std::vector<int>& heights,
    const IsoParams& iso,
    const sf::Vector2f& origin,
    float heightScale)
{
    // Infer grid dimensions from heights size: (W x W) where W^2 == heights.size()
    int W = (int)std::round(std::sqrt((double)heights.size()));
    if (W <= 0) return {};
    int H = W;
    std::vector<std::vector<sf::Vector2f>> map2d(H, std::vector<sf::Vector2f>(W));
    auto id = [&](int i, int j){ return i * W + j; };
    for (int i = 0; i < H; ++i) {
        for (int j = 0; j < W; ++j) {
            int h = heights[id(i, j)];
            map2d[i][j] = isoProjectDyn((float)i, (float)j, (h * heightScale) * cfg::ELEV_STEP, iso) + origin;
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
                       bool enableShadows,
                       float heightScale,
                       const std::unordered_map<long long, sf::Color>* paintedCells,
                       const std::unordered_set<long long>* hoverMask,
                       const sf::Color* hoverColor)
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
                float baseH = (float)std::clamp(heights[id(i, j)], cfg::MIN_ELEV, cfg::MAX_ELEV);
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
                    float h = (float)std::clamp(heights[id(ii, jj)], cfg::MIN_ELEV, cfg::MAX_ELEV);
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
        const sf::Color sandYellow(255, 236, 170);
        const sf::Color grass(34, 139, 34);
        const sf::Color gray(128, 128, 128);
        const sf::Color rock(110, 110, 110);
        const sf::Color snow(245, 245, 245);

        // Thresholds per spec (values multiplied by heightScale to remain invariant)
        float sea0      =  0.f  * heightScale;   // water surface
        float deepMin   = -100.f * heightScale;  // deep sea lower bound
        float coast2    =  2.f  * heightScale;   // coast end
        float beach4    =  4.f  * heightScale;   // beach->grass end
        float grass6    =  6.f  * heightScale;   // solid grass end
        float gray10    = 10.f  * heightScale;   // grass->gray end
        float rock14    = 12.f  * heightScale;   // rock end (lowered)
        float snow16    = 14.f  * heightScale;   // snow start (lowered)

        // Deep sea gradient: [deepMin .. sea0)
        if (h < sea0 && h >= deepMin) {
            float t = (sea0 - h) / std::max(0.001f, (sea0 - deepMin)); // 0 at surface -> 1 at deep
            return lerpColor(normalBlue, veryDarkBlue, t);
        }
        // 0..2: blue -> sand
        if (h >= sea0 && h < coast2) {
            float t = (h - sea0) / std::max(0.001f, (coast2 - sea0));
            return lerpColor(normalBlue, sandYellow, t);
        }
        // 2..4: sand -> grass
        if (h >= coast2 && h < beach4) {
            float t = (h - coast2) / std::max(0.001f, (beach4 - coast2));
            return lerpColor(sandYellow, grass, t);
        }
        // 4..6: grass
        if (h >= beach4 && h < grass6) return grass;
        // 6..10: grass -> gray
        if (h >= grass6 && h <= gray10) {
            float t = (h - grass6) / std::max(0.001f, (gray10 - grass6));
            return lerpColor(grass, gray, t);
        }
        // 10..14: rock
        if (h > gray10 && h <= rock14) return rock;
        // 14..16: gray -> snow
        if (h > rock14 && h < snow16) {
            float t = (h - rock14) / std::max(0.001f, (snow16 - rock14));
            return lerpColor(gray, snow, t);
        }
        // >= 16: snow
        if (h >= snow16) return snow;
        // Fallback
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

    // Per-quad normal from local 3D points (avoids cross-chunk dependency)
    auto quadShade = [&](int i, int j, int stride)->float{
        // Read raw heights (unclamped to allow > MAX_ELEV visual effect)
        float hA = (float)heights[id(i, j)] * heightScale;
        float hB = (float)heights[id(std::min(i + stride, H - 1), j)] * heightScale;
        float hC = (float)heights[id(std::min(i + stride, H - 1), std::min(j + stride, W - 1))] * heightScale;
        float hD = (float)heights[id(i, std::min(j + stride, W - 1))] * heightScale;
        // 3D points in grid space
        sf::Vector3f A3((float)i,               (float)j,               hA * cfg::ELEV_STEP);
        sf::Vector3f B3((float)std::min(i+stride, H-1), (float)j,               hB * cfg::ELEV_STEP);
        sf::Vector3f C3((float)std::min(i+stride, H-1), (float)std::min(j+stride, W-1), hC * cfg::ELEV_STEP);
        sf::Vector3f D3((float)i,               (float)std::min(j+stride, W-1), hD * cfg::ELEV_STEP);
        auto sub = [](sf::Vector3f a, sf::Vector3f b){ return sf::Vector3f(a.x-b.x, a.y-b.y, a.z-b.z); };
        auto cross = [](sf::Vector3f a, sf::Vector3f b){ return sf::Vector3f(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x); };
        sf::Vector3f n1 = norm3(cross(sub(B3, A3), sub(C3, A3)));
        sf::Vector3f n2 = norm3(cross(sub(C3, A3), sub(D3, A3)));
        float ndotl1 = std::max(0.0f, n1.x * Ldir.x + n1.y * Ldir.y + n1.z * Ldir.z);
        float ndotl2 = std::max(0.0f, n2.x * Ldir.x + n2.y * Ldir.y + n2.z * Ldir.z);
        float ndotl = 0.5f * (ndotl1 + ndotl2);
        // Ambient term
        return 0.5f + 0.5f * ndotl; // [0.5..1.0]
    };

    for (int i = 0; i < H - 1; i += stride) {
        for (int j = 0; j < W - 1; j += stride) {
            const sf::Vector2f& A = map2d[i][j];
            const sf::Vector2f& B = map2d[std::min(i + stride, H - 1)][j];
            const sf::Vector2f& C = map2d[std::min(i + stride, H - 1)][std::min(j + stride, W - 1)];
            const sf::Vector2f& D = map2d[i][std::min(j + stride, W - 1)];

            if (!rectsIntersect(quadBounds(A, B, C, D), viewRect)) continue;

            float hA = (float)heights[id(i, j)] * heightScale;
            float hB = (float)heights[id(std::min(i + stride, H - 1), j)] * heightScale;
            float hC = (float)heights[id(std::min(i + stride, H - 1), std::min(j + stride, W - 1))] * heightScale;
            float hD = (float)heights[id(i, std::min(j + stride, W - 1))] * heightScale;
            float hAvg = 0.25f * (hA + hB + hC + hD);

            float shade = quadShade(i, j, stride);

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

            // Base color: start from terrain; then alpha-blend painted color if present; then hover tint
            sf::Color base = colorForHeight(hAvg);
            long long key = (((long long)i) << 32) ^ (unsigned long long)(uint32_t)j;
            if (paintedCells) {
                auto it = paintedCells->find(key);
                if (it != paintedCells->end()) base = alphaOver(base, it->second);
            }
            if (hoverMask && hoverColor) {
                if (hoverMask->find(key) != hoverMask->end()) {
                    // Blend 70% base with 30% hover color to indicate hover, keeping base alpha
                    auto blend = [&](sf::Color a, sf::Color b){
                        float t = 0.3f;
                        return sf::Color(
                            (uint8_t)std::round(a.r*(1-t) + b.r*t),
                            (uint8_t)std::round(a.g*(1-t) + b.g*t),
                            (uint8_t)std::round(a.b*(1-t) + b.b*t),
                            a.a
                        );
                    };
                    base = blend(base, *hoverColor);
                }
            }
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
    const sf::Vector2f& origin,
    float heightScale)
{
    const int W = S + 1;
    auto idc = [&](int i, int j){ return i * W + j; };
    std::vector<std::vector<sf::Vector2f>> map2d(W, std::vector<sf::Vector2f>(W));
    for (int i = 0; i <= S; ++i) {
        for (int j = 0; j <= S; ++j) {
            int h = heights[idc(i, j)];
            map2d[i][j] = isoProjectDyn((float)(I0 + i), (float)(J0 + j), (h * heightScale) * cfg::ELEV_STEP, iso) + origin;
        }
    }
    return map2d;
}

void draw2DMapChunk(sf::RenderTarget& target,
                    const std::vector<std::vector<sf::Vector2f>>& map2d,
                    int stride)
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

    if (stride <= 0) stride = 1;
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
                            bool enableShadows,
                            float heightScale,
                            int stride,
                            int I0, int J0,
                            const std::unordered_map<long long, sf::Color>* paintedCells,
                            const std::unordered_set<long long>* hoverMask,
                            const sf::Color* hoverColor)
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

    // use provided stride parameter

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
    if (enableShadows && stride == 1) {
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

        float sea0      =  0.f  * heightScale;
        float deepMin   = -100.f * heightScale;
        float coast2    =  2.f  * heightScale;
        float beach4    =  4.f  * heightScale;
        float grass6    =  6.f  * heightScale;
        float gray10    = 10.f  * heightScale;
        float rock14    = 12.f  * heightScale;
        float snow16    = 14.f  * heightScale;

        if (h < sea0 && h >= deepMin) { float t = (sea0 - h) / std::max(0.001f, (sea0 - deepMin)); return lerpColor(normalBlue, veryDarkBlue, t); }
        if (h >= sea0 && h < coast2) { float t = (h - sea0) / std::max(0.001f, (coast2 - sea0)); return lerpColor(normalBlue, sandYellow, t); }
        if (h >= coast2 && h < beach4) { float t = (h - coast2) / std::max(0.001f, (beach4 - coast2)); return lerpColor(sandYellow, grass, t); }
        if (h >= beach4 && h < grass6) return grass;
        if (h >= grass6 && h <= gray10) { float t = (h - grass6) / std::max(0.001f, (gray10 - grass6)); return lerpColor(grass, gray, t); }
        if (h > gray10 && h <= rock14) return rock;
        if (h > rock14 && h < snow16) { float t = (h - rock14) / std::max(0.001f, (snow16 - rock14)); return lerpColor(gray, snow, t); }
        if (h >= snow16) return snow;
        return grass;
    };

    // Per-quad shading in chunk mode
    auto quadShadeC = [&](int i, int j, int stride)->float{
        float hA = (float)heights[idc(i, j)] * heightScale;
        float hB = (float)heights[idc(std::min(i + stride, H - 1), j)] * heightScale;
        float hC = (float)heights[idc(std::min(i + stride, H - 1), std::min(j + stride, W - 1))] * heightScale;
        float hD = (float)heights[idc(i, std::min(j + stride, W - 1))] * heightScale;
        sf::Vector3f A3((float)i,               (float)j,               hA * cfg::ELEV_STEP);
        sf::Vector3f B3((float)std::min(i+stride, H-1), (float)j,               hB * cfg::ELEV_STEP);
        sf::Vector3f C3((float)std::min(i+stride, H-1), (float)std::min(j+stride, W-1), hC * cfg::ELEV_STEP);
        sf::Vector3f D3((float)i,               (float)std::min(j+stride, W-1), hD * cfg::ELEV_STEP);
        auto sub = [](sf::Vector3f a, sf::Vector3f b){ return sf::Vector3f(a.x-b.x, a.y-b.y, a.z-b.z); };
        auto cross = [](sf::Vector3f a, sf::Vector3f b){ return sf::Vector3f(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x); };
        sf::Vector3f n1 = norm3(cross(sub(B3, A3), sub(C3, A3)));
        sf::Vector3f n2 = norm3(cross(sub(C3, A3), sub(D3, A3)));
        float ndotl1 = std::max(0.0f, n1.x * Ldir.x + n1.y * Ldir.y + n1.z * Ldir.z);
        float ndotl2 = std::max(0.0f, n2.x * Ldir.x + n2.y * Ldir.y + n2.z * Ldir.z);
        float ndotl = 0.5f * (ndotl1 + ndotl2);
        return 0.5f + 0.5f * ndotl;
    };

    sf::VertexArray tris(sf::Triangles);
    for (int i = 0; i < H - 1; i += stride) {
        for (int j = 0; j < W - 1; j += stride) {
            const sf::Vector2f& A = map2d[i][j];
            const sf::Vector2f& B = map2d[std::min(i + stride, H - 1)][j];
            const sf::Vector2f& C = map2d[std::min(i + stride, H - 1)][std::min(j + stride, W - 1)];
            const sf::Vector2f& D = map2d[i][std::min(j + stride, W - 1)];
            if (!rectsIntersect(quadBounds(A, B, C, D), viewRect)) continue;
            float hA = (float)heights[idc(i, j)] * heightScale;
            float hB = (float)heights[idc(std::min(i + stride, H - 1), j)] * heightScale;
            float hC = (float)heights[idc(std::min(i + stride, H - 1), std::min(j + stride, W - 1))] * heightScale;
            float hD = (float)heights[idc(i, std::min(j + stride, W - 1))] * heightScale;
            float hAvg = 0.25f * (hA + hB + hC + hD);
            float shade = quadShadeC(i, j, stride);
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
            // Base color: terrain, then alpha over paint, then hover
            auto base = colorForHeight(hAvg);
            long long key = (((long long)(I0 + i)) << 32) ^ (unsigned long long)(uint32_t)(J0 + j);
            if (paintedCells) {
                auto it = paintedCells->find(key);
                if (it != paintedCells->end()) base = alphaOver(base, it->second);
            }
            if (hoverMask && hoverColor) {
                if (hoverMask->find(key) != hoverMask->end()) {
                    float t = 0.3f;
                    base = sf::Color(
                        (uint8_t)std::round(base.r*(1-t) + hoverColor->r*t),
                        (uint8_t)std::round(base.g*(1-t) + hoverColor->g*t),
                        (uint8_t)std::round(base.b*(1-t) + hoverColor->b*t),
                        base.a
                    );
                }
            }
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
 
// --- Strided per-chunk projection: builds a reduced map sampling every 'stride' cell ---
std::vector<std::vector<sf::Vector2f>> render::buildProjectedMapChunkStrided(
    const std::vector<int>& heights,
    int S,
    int I0, int J0,
    const IsoParams& iso,
    const sf::Vector2f& origin,
    float heightScale,
    int stride)
{
    if (stride <= 1) return buildProjectedMapChunk(heights, S, I0, J0, iso, origin, heightScale);
    const int W = S + 1;
    const int Hs = (W + stride - 1) / stride; // ceil((S+1)/stride)
    auto idc = [&](int i, int j){ return i * W + j; };
    std::vector<std::vector<sf::Vector2f>> map2d(Hs, std::vector<sf::Vector2f>(Hs));
    for (int si = 0; si < Hs; ++si) {
        int i = std::min(si * stride, W - 1);
        for (int sj = 0; sj < Hs; ++sj) {
            int j = std::min(sj * stride, W - 1);
            int h = heights[idc(i, j)];
            map2d[si][sj] = isoProjectDyn((float)(I0 + i), (float)(J0 + j), (h * heightScale) * cfg::ELEV_STEP, iso) + origin;
        }
    }
    return map2d;
}

// --- Strided per-chunk filled rendering: uses reduced map and samples heights with 'stride' ---
void render::draw2DFilledCellsChunkStrided(
    sf::RenderTarget& target,
    const std::vector<std::vector<sf::Vector2f>>& map2dStrided,
    const std::vector<int>& heights,
    int S,
    bool enableShadows,
    float heightScale,
    int stride,
    int I0, int J0,
    const std::unordered_map<long long, sf::Color>* paintedCells,
    const std::unordered_set<long long>* hoverMask,
    const sf::Color* hoverColor)
{
    int Hs = (int)map2dStrided.size();
    if (Hs == 0) return;
    int Ws = (int)map2dStrided[0].size();
    const int W = S + 1;
    auto idc = [&](int i, int j){ return i * W + j; };

    const auto& view = target.getView();
    sf::Vector2f vc = view.getCenter();
    sf::Vector2f vs = view.getSize();
    const float margin = 64.f;
    sf::FloatRect viewRect(vc.x - vs.x * 0.5f - margin,
                           vc.y - vs.y * 0.5f - margin,
                           vs.x + 2 * margin, vs.y + 2 * margin);

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
        float sea0 = 0.f * heightScale;
        float deepMin = -100.f * heightScale;
        float coast2 = 2.f * heightScale;
        float beach4 = 4.f * heightScale;
        float grass6 = 6.f * heightScale;
        float gray10 = 10.f * heightScale;
        float rock14 = 12.f * heightScale;
        float snow16 = 14.f * heightScale;
        if (h < sea0 && h >= deepMin) { float t = (sea0 - h) / std::max(0.001f, (sea0 - deepMin)); return lerpColor(normalBlue, veryDarkBlue, t); }
        if (h >= sea0 && h < coast2) { float t = (h - sea0) / std::max(0.001f, (coast2 - sea0)); return lerpColor(normalBlue, sandYellow, t); }
        if (h >= coast2 && h < beach4) { float t = (h - coast2) / std::max(0.001f, (beach4 - coast2)); return lerpColor(sandYellow, grass, t); }
        if (h >= beach4 && h < grass6) return grass;
        if (h >= grass6 && h <= gray10) { float t = (h - grass6) / std::max(0.001f, (gray10 - grass6)); return lerpColor(grass, gray, t); }
        if (h > gray10 && h <= rock14) return rock;
        if (h > rock14 && h < snow16) { float t = (h - rock14) / std::max(0.001f, (snow16 - rock14)); return lerpColor(gray, snow, t); }
        if (h >= snow16) return snow;
        return grass;
    };

    // Simple per-quad shading like chunk variant using heights sampled with stride
    auto quadShadeS = [&](int si, int sj)->float{
        auto clampIdx = [&](int v){ return std::min(v, W - 1); };
        int iA = clampIdx(si * stride);
        int jA = clampIdx(sj * stride);
        int iB = clampIdx((si + 1) * stride);
        int jB = jA;
        int iC = iB;
        int jC = clampIdx((sj + 1) * stride);
        int iD = iA;
        int jD = jC;
        float hA = (float)heights[idc(iA, jA)] * heightScale;
        float hB = (float)heights[idc(iB, jB)] * heightScale;
        float hC = (float)heights[idc(iC, jC)] * heightScale;
        float hD = (float)heights[idc(iD, jD)] * heightScale;
        sf::Vector3f A3((float)iA, (float)jA, hA * cfg::ELEV_STEP);
        sf::Vector3f B3((float)iB, (float)jB, hB * cfg::ELEV_STEP);
        sf::Vector3f C3((float)iC, (float)jC, hC * cfg::ELEV_STEP);
        sf::Vector3f D3((float)iD, (float)jD, hD * cfg::ELEV_STEP);
        auto sub = [](sf::Vector3f a, sf::Vector3f b){ return sf::Vector3f(a.x-b.x, a.y-b.y, a.z-b.z); };
        auto cross = [](sf::Vector3f a, sf::Vector3f b){ return sf::Vector3f(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x); };
        auto norm3loc = [](sf::Vector3f v){ float L = std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); if (L<=1e-6f) return sf::Vector3f(0,0,1); return sf::Vector3f(v.x/L,v.y/L,v.z/L); };
        const sf::Vector3f LdirLoc = norm3loc(sf::Vector3f(0.f, 1.f, 0.8f));
        sf::Vector3f n1 = norm3loc(cross(sub(B3, A3), sub(C3, A3)));
        sf::Vector3f n2 = norm3loc(cross(sub(C3, A3), sub(D3, A3)));
        float ndotl1 = std::max(0.0f, n1.x*LdirLoc.x + n1.y*LdirLoc.y + n1.z*LdirLoc.z);
        float ndotl2 = std::max(0.0f, n2.x*LdirLoc.x + n2.y*LdirLoc.y + n2.z*LdirLoc.z);
        float ndotl = 0.5f*(ndotl1+ndotl2);
        return 0.5f + 0.5f*ndotl;
    };

    sf::VertexArray tris(sf::Triangles);
    for (int si = 0; si < Hs - 1; ++si) {
        for (int sj = 0; sj < Ws - 1; ++sj) {
            const sf::Vector2f& A = map2dStrided[si][sj];
            const sf::Vector2f& B = map2dStrided[si + 1][sj];
            const sf::Vector2f& C = map2dStrided[si + 1][sj + 1];
            const sf::Vector2f& D = map2dStrided[si][sj + 1];
            if (!rectsIntersect(quadBounds(A, B, C, D), viewRect)) continue;

            auto clampIdx = [&](int v){ return std::min(v, W - 1); };
            int iA = clampIdx(si * stride), jA = clampIdx(sj * stride);
            int iB = clampIdx((si + 1) * stride), jB = jA;
            int iC = iB, jC = clampIdx((sj + 1) * stride);
            int iD = iA, jD = jC;
            float hA = (float)heights[idc(iA, jA)] * heightScale;
            float hB = (float)heights[idc(iB, jB)] * heightScale;
            float hC = (float)heights[idc(iC, jC)] * heightScale;
            float hD = (float)heights[idc(iD, jD)] * heightScale;
            float hAvg = 0.25f * (hA + hB + hC + hD);

            float shade = quadShadeS(si, sj);
            float shadeFinal = shade; // shadows skipped for stride>1 to save cost

            sf::Color base = colorForHeight(hAvg);
            long long key = (((long long)(I0 + iA)) << 32) ^ (unsigned long long)(uint32_t)(J0 + jA);
            if (paintedCells) {
                auto it = paintedCells->find(key);
                if (it != paintedCells->end()) base = alphaOver(base, it->second);
            }
            if (hoverMask && hoverColor) {
                if (hoverMask->find(key) != hoverMask->end()) {
                    float t = 0.3f;
                    base = sf::Color(
                        (uint8_t)std::round(base.r*(1-t) + hoverColor->r*t),
                        (uint8_t)std::round(base.g*(1-t) + hoverColor->g*t),
                        (uint8_t)std::round(base.b*(1-t) + hoverColor->b*t),
                        base.a
                    );
                }
            }
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
