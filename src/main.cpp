#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <cstdio>
#include <iostream>
#include <chrono>
#include <ctime>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <commdlg.h>
#endif
#include "config.hpp"
#include "iso.hpp"
#include "terrain.hpp"
#include "render.hpp"
#include "chunks.hpp"

// MyWorld - Isometric diamond tiles with elevation editing, camera pan+zoom
// Grid: 20x20 tiles, each isometric tile nominal size 32x32 (diamond)
// Controls:
//  - Left click: raise nearest intersection
//  - Right click: lower nearest intersection
//  - Mouse wheel: zoom in/out
//  - WASD or Arrow keys: pan camera
//  - Middle mouse drag: pan camera
//  - R: reset view
//  - Esc: quit

// Config, iso, terrain and rendering are provided by headers above

int main() {
    // Minimal file logger to diagnose silent exit when running as GUI app
    std::ofstream __log("log.txt", std::ios::app);
    auto __now = [](){
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return std::string(buf);
    };
    if (__log) {
        __log << "[" << __now() << "] main() start" << std::endl;
    }
    try {
        // Redirect SFML error stream to our log as well
        if (__log) {
            sf::err().rdbuf(__log.rdbuf());
        }
    if (__log) __log << "[" << __now() << "] creating window" << std::endl;
    sf::RenderWindow window(sf::VideoMode(cfg::WINDOW_W, cfg::WINDOW_H), "MyWorld - SFML Isometric Grid");
    window.setFramerateLimit(120);
    if (__log) __log << "[" << __now() << "] window created: " << window.isOpen() << std::endl;

    // World view (camera)
    sf::View view(sf::FloatRect(0.f, 0.f, static_cast<float>(cfg::WINDOW_W), static_cast<float>(cfg::WINDOW_H)));
    window.setView(view);

    // Precompute a world origin so the grid is roughly centered near (0,0) world coords
    // We'll center via the view later; origin is just a local shift for drawing
    sf::Vector2f origin(0.f, 0.f);
    IsoParams iso; // start with default; user can tilt/rotate via dragging outside grid

    // Center the camera on the grid
    // Compute center of grid in world coords (tile centers)
    sf::Vector2f gridCenter = isoProjectDyn(cfg::GRID * 0.5f, cfg::GRID * 0.5f, 0.f, iso);
    view.setCenter(gridCenter + origin);
    window.setView(view);

    // Elevation grid on intersections: (GRID+1) x (GRID+1)
    std::vector<int> heights((cfg::GRID + 1) * (cfg::GRID + 1), 0);
    auto idx = [](int i, int j) { return i * (cfg::GRID + 1) + j; };

    // Terrain generation provided by terrain::generateMap
    auto generateMap = [&](uint32_t seed){ terrain::generateMap(heights, seed); };

    // Chunked world manager (procedural mode)
    ChunkManager chunkMgr;
    bool proceduralMode = true;   // start with procedural active
    bool waterOnly = true;        // show only water until user generates
    uint32_t proceduralSeed = (uint32_t)std::rand();

    

    // UTF-8 helper
    auto U8 = [&](const std::string& s){ return sf::String::fromUtf8(s.begin(), s.end()); };

    // --- UI: "Générer" button ---
    sf::Font uiFont;
    bool fontLoaded = uiFont.loadFromFile("assets/fonts/arial.ttf");
    if (__log) __log << "[" << __now() << "] fontLoaded=" << (fontLoaded?"true":"false") << std::endl;
    sf::RectangleShape btnGenerate(sf::Vector2f(140.f, 36.f));
    btnGenerate.setFillColor(sf::Color(30, 30, 30, 200));
    btnGenerate.setOutlineThickness(2.f);
    btnGenerate.setOutlineColor(sf::Color(200, 200, 200));
    btnGenerate.setPosition(16.f, 16.f);
    // Grid toggle button just below
    sf::RectangleShape btnGrid(sf::Vector2f(140.f, 36.f));
    btnGrid.setFillColor(sf::Color(30, 30, 30, 200));
    btnGrid.setOutlineThickness(2.f);
    btnGrid.setOutlineColor(sf::Color(200, 200, 200));
    btnGrid.setPosition(16.f, 16.f + 36.f + 8.f);
    // Continents toggle, RESET, Re-seed button and Seed input box, Bake button
    sf::RectangleShape btnContinents(sf::Vector2f(140.f, 36.f));
    btnContinents.setFillColor(sf::Color(30, 30, 30, 200));
    btnContinents.setOutlineThickness(2.f);
    btnContinents.setOutlineColor(sf::Color(200, 200, 200));
    btnContinents.setPosition(16.f, 16.f + (36.f + 8.f) * 2.f);
    sf::RectangleShape btnReset(sf::Vector2f(140.f, 36.f));
    btnReset.setFillColor(sf::Color(30, 30, 30, 200));
    btnReset.setOutlineThickness(2.f);
    btnReset.setOutlineColor(sf::Color(200, 200, 200));
    btnReset.setPosition(16.f, 16.f + (36.f + 8.f) * 3.f);
    sf::RectangleShape btnReseed(sf::Vector2f(140.f, 36.f));
    btnReseed.setFillColor(sf::Color(30, 30, 30, 200));
    btnReseed.setOutlineThickness(2.f);
    btnReseed.setOutlineColor(sf::Color(200, 200, 200));
    btnReseed.setPosition(16.f, 16.f + (36.f + 8.f) * 4.f);

    sf::RectangleShape seedBox(sf::Vector2f(140.f, 28.f));
    seedBox.setFillColor(sf::Color(20, 20, 20, 200));
    seedBox.setOutlineThickness(2.f);
    seedBox.setOutlineColor(sf::Color(180, 180, 180));
    seedBox.setPosition(16.f, 16.f + (36.f + 8.f) * 5.f);
    sf::RectangleShape btnBake(sf::Vector2f(140.f, 36.f));
    btnBake.setFillColor(sf::Color(30, 30, 30, 200));
    btnBake.setOutlineThickness(2.f);
    btnBake.setOutlineColor(sf::Color(200, 200, 200));
    btnBake.setPosition(16.f, 16.f + (36.f + 8.f) * 6.f);

    sf::Text btnText;
    sf::Text btnGridText;
    sf::Text btnContinentsText;
    sf::Text btnReseedText;
    sf::Text btnResetText;
    sf::Text seedText;
    sf::Text btnBakeText;
    // Persistent help and FPS texts (avoid realloc each frame)
    sf::Text helpF11;
    sf::Text helpCtrl;
    sf::Text fpsText;
    if (fontLoaded) {
        btnText.setFont(uiFont);
        btnText.setString(U8(u8"Générer"));
        btnText.setCharacterSize(18);
        btnText.setFillColor(sf::Color::White);
        // center text within button
        sf::FloatRect tb = btnText.getLocalBounds();
        btnText.setOrigin(tb.left + tb.width * 0.5f, tb.top + tb.height * 0.5f);
        btnText.setPosition(btnGenerate.getPosition() + sf::Vector2f(btnGenerate.getSize().x * 0.5f, btnGenerate.getSize().y * 0.5f));

        btnGridText.setFont(uiFont);
        btnGridText.setString(U8(u8"Grille"));
        btnGridText.setCharacterSize(18);
        btnGridText.setFillColor(sf::Color::White);
        auto tg = btnGridText.getLocalBounds();
        btnGridText.setOrigin(tg.left + tg.width * 0.5f, tg.top + tg.height * 0.5f);
        btnGridText.setPosition(btnGrid.getPosition() + sf::Vector2f(btnGrid.getSize().x * 0.5f, btnGrid.getSize().y * 0.5f));

        // Init bottom-left help texts
        helpF11.setFont(uiFont);
        helpF11.setCharacterSize(14);
        helpF11.setFillColor(sf::Color(220, 220, 220));
        helpF11.setString(U8("F11: Plein écran"));

        helpCtrl.setFont(uiFont);
        helpCtrl.setCharacterSize(14);
        helpCtrl.setFillColor(sf::Color(220, 220, 220));
        helpCtrl.setString(U8("Ctrl+clic: Aplanir le terrain (le 1er clic prend la hauteur)"));

        // Init FPS text
        fpsText.setFont(uiFont);
        fpsText.setCharacterSize(14);
        fpsText.setFillColor(sf::Color(200, 255, 200));
        fpsText.setString("FPS: --");
        btnContinentsText.setFont(uiFont);
        btnContinentsText.setString(U8(u8"Continents: OFF"));
        btnContinentsText.setCharacterSize(18);
        btnContinentsText.setFillColor(sf::Color::White);
        auto tc = btnContinentsText.getLocalBounds();
        btnContinentsText.setOrigin(tc.left + tc.width * 0.5f, tc.top + tc.height * 0.5f);
        btnContinentsText.setPosition(btnContinents.getPosition() + sf::Vector2f(btnContinents.getSize().x * 0.5f, btnContinents.getSize().y * 0.5f));

        btnReseedText.setFont(uiFont);
        btnReseedText.setString(U8(u8"Re-seed"));
        btnReseedText.setCharacterSize(18);
        btnReseedText.setFillColor(sf::Color::White);
        auto tr = btnReseedText.getLocalBounds();
        btnReseedText.setOrigin(tr.left + tr.width * 0.5f, tr.top + tr.height * 0.5f);
        btnReseedText.setPosition(btnReseed.getPosition() + sf::Vector2f(btnReseed.getSize().x * 0.5f, btnReseed.getSize().y * 0.5f));

        btnResetText.setFont(uiFont);
        btnResetText.setString(U8(u8"RESET"));
        btnResetText.setCharacterSize(18);
        btnResetText.setFillColor(sf::Color::White);
        auto trr = btnResetText.getLocalBounds();
        btnResetText.setOrigin(trr.left + trr.width * 0.5f, trr.top + trr.height * 0.5f);
        btnResetText.setPosition(btnReset.getPosition() + sf::Vector2f(btnReset.getSize().x * 0.5f, btnReset.getSize().y * 0.5f));

        seedText.setFont(uiFont);
        seedText.setCharacterSize(16);
        seedText.setFillColor(sf::Color(230, 230, 230));
        seedText.setString("Seed: 0");
        auto sb = seedBox.getGlobalBounds();
        seedText.setPosition(seedBox.getPosition().x + 8.f, seedBox.getPosition().y + 4.f);

        btnBakeText.setFont(uiFont);
        btnBakeText.setString(U8(u8"Figer"));
        btnBakeText.setCharacterSize(18);
        btnBakeText.setFillColor(sf::Color::White);
        auto tbk = btnBakeText.getLocalBounds();
        btnBakeText.setOrigin(tbk.left + tbk.width * 0.5f, tbk.top + tbk.height * 0.5f);
        btnBakeText.setPosition(btnBake.getPosition() + sf::Vector2f(btnBake.getSize().x * 0.5f, btnBake.getSize().y * 0.5f));
    }

    bool genHover = false;  // hover state for the Generate button
    bool gridHover = false; // hover state for the Grid button
    bool continentsHover = false;
    bool bakeHover = false;
    bool resetHover = false;
    bool showGrid = false;   // toggle wireframe visibility (default OFF)
    bool shadowsEnabled = false; // F2 toggles shadows (default OFF)
    bool continentsOpt = false; // current continents toggle
    // Seed input state
    bool seedEditing = false;
    std::string seedBuffer;

    // Brush size & slider UI (right side)
    int brushSize = 2;                 // radius in intersections
    const int brushMin = 1;
    const int brushMax = 8;
    bool brushDragging = false;
    
    // Tools / Inventory bar (bottom-center)
    enum class Tool { Bulldozer, Brush, Eraser };
    Tool currentTool = Tool::Bulldozer;
    // Inventory UI assets
    sf::Texture texBrush, texBulldozer, texEraser;
    sf::Sprite  sprBrush, sprBulldozer, sprEraser;
    bool brushIconLoaded = texBrush.loadFromFile("assets/images/pinceau.png");
    bool bulldozerIconLoaded = texBulldozer.loadFromFile("assets/images/pelle-excavatrice.png");
    bool eraserIconLoaded = texEraser.loadFromFile("assets/images/gomme.png");
    if (brushIconLoaded)  { sprBrush.setTexture(texBrush);      auto ts=texBrush.getSize();      if (ts.x>0&&ts.y>0) sprBrush.setScale(32.f/(float)ts.x, 32.f/(float)ts.y); }
    if (bulldozerIconLoaded){ sprBulldozer.setTexture(texBulldozer);auto ts=texBulldozer.getSize();if (ts.x>0&&ts.y>0) sprBulldozer.setScale(32.f/(float)ts.x, 32.f/(float)ts.y); }
    if (eraserIconLoaded){ sprEraser.setTexture(texEraser);    auto ts=texEraser.getSize();    if (ts.x>0&&ts.y>0) sprEraser.setScale(32.f/(float)ts.x, 32.f/(float)ts.y); }
    auto inventoryRects = [&](){
        // Return three 32x32 rects centered at bottom
        auto sz = window.getSize(); float w=(float)sz.x, h=(float)sz.y;
        float box = 32.f; float gap = 16.f; float total = box*3.f + gap*2.f;
        float x0 = w*0.5f - total*0.5f; float y = h - 20.f - box; // 20px margin from bottom
        sf::FloatRect rBulldozer(x0, y, box, box);
        sf::FloatRect rBrush(x0 + (box + gap)*1.f, y, box, box);
        sf::FloatRect rEraser(x0 + (box + gap)*2.f, y, box, box);
        return std::tuple<sf::FloatRect,sf::FloatRect,sf::FloatRect>(rBulldozer, rBrush, rEraser);
    };

    // Painting overlay: per world cell (I,J) -> color
    struct IJKey { int I; int J; };
    auto key64 = [](int I, int J)->long long { return ( (long long)I << 32) ^ (unsigned long long)(uint32_t)J; };
    std::unordered_map<long long, sf::Color> paintedCells;
    bool showColorHover = false;

    // Color picker (left, below buttons)
    sf::Color selectedColor = sf::Color::White;
    std::vector<sf::Color> colorHistory; // newest first, max 5
    const int colorWheelRadius = 60; // fits within 140px panel width
    bool colorWheelReady = false;
    sf::Texture colorWheelTex;
    sf::Sprite  colorWheelSpr;
    bool showColorPicker = false; // hidden by default, shown in Brush tool
    // Tone slider (white <-> color <-> black)
    float colorToneT = 0.5f; // 0=white, 0.5=selectedColor, 1=black
    bool  toneDragging = false;
    bool  toneTexReady = false;
    sf::Texture toneTex; // 1xW gradient, scaled in draw
    sf::Sprite  toneSpr;
    sf::Color   activeColor = selectedColor; // final color after tone applied
    auto lerpColor = [](sf::Color a, sf::Color b, float t){
        auto L = [&](sf::Uint8 u, sf::Uint8 v){ return (sf::Uint8)std::clamp((int)std::round(u + (v - u) * t), 0, 255); };
        return sf::Color(L(a.r,b.r), L(a.g,b.g), L(a.b,b.b), 255);
    };
    auto applyTone = [&](sf::Color base, float t)->sf::Color{
        t = std::clamp(t, 0.f, 1.f);
        if (t < 0.5f) {
            float k = t * 2.f; // 0..1
            return lerpColor(sf::Color::White, base, k);
        } else {
            float k = (t - 0.5f) * 2.f; // 0..1
            return lerpColor(base, sf::Color::Black, k);
        }
    };
    auto rebuildToneTex = [&](){
        const int W = 140; // panel width
        sf::Image img; img.create(W, 1, sf::Color::Transparent);
        for (int x=0; x<W; ++x) {
            float t = (W<=1) ? 0.f : (float)x / (float)(W-1);
            img.setPixel(x, 0, applyTone(selectedColor, t));
        }
        toneTex.loadFromImage(img);
        toneSpr.setTexture(toneTex);
        toneTexReady = true;
    };
    auto hsv2rgb = [](float h, float s, float v)->sf::Color{
        h = std::fmod(std::fabs(h), 360.f);
        float c = v * s;
        float x = c * (1 - std::fabs(std::fmod(h/60.f, 2.f) - 1));
        float m = v - c;
        float r=0,g=0,b=0;
        if (h < 60)      { r=c; g=x; b=0; }
        else if (h < 120){ r=x; g=c; b=0; }
        else if (h < 180){ r=0; g=c; b=x; }
        else if (h < 240){ r=0; g=x; b=c; }
        else if (h < 300){ r=x; g=0; b=c; }
        else             { r=c; g=0; b=x; }
        auto to8 = [&](float f){ int u = (int)std::round((f + m) * 255.f); return (sf::Uint8)std::clamp(u, 0, 255); };
        return sf::Color(to8(r), to8(g), to8(b));
    };
    auto ensureColorWheel = [&](){
        if (colorWheelReady) return;
        const int D = colorWheelRadius * 2;
        sf::Image img; img.create(D, D, sf::Color(0,0,0,0));
        sf::Vector2f c((float)colorWheelRadius, (float)colorWheelRadius);
        for (int y=0; y<D; ++y){
            for (int x=0; x<D; ++x){
                float dx = x - c.x;
                float dy = y - c.y;
                float r = std::sqrt(dx*dx + dy*dy);
                if (r <= colorWheelRadius) {
                    float angle = std::atan2(dy, dx) * 180.f / 3.14159265f; // [-180,180]
                    if (angle < 0) angle += 360.f;
                    float s = std::clamp(r / (float)colorWheelRadius, 0.f, 1.f);
                    sf::Color col = hsv2rgb(angle, s, 1.f);
                    img.setPixel(x, y, col);
                }
            }
        }
        colorWheelTex.loadFromImage(img);
        colorWheelSpr.setTexture(colorWheelTex);
        colorWheelReady = true;
    };
    auto pushHistory = [&](sf::Color c){
        // Remove duplicates of c, then push front, cap to 5
        auto it = std::remove_if(colorHistory.begin(), colorHistory.end(), [&](const sf::Color& k){return k==c;});
        colorHistory.erase(it, colorHistory.end());
        colorHistory.insert(colorHistory.begin(), c);
        if (colorHistory.size() > 5) colorHistory.resize(5);
    };
    
    // Terrain height slider removed; fixed scale used in rendering
    // Painting throttle (Option A): apply brush at fixed cadence independent of event rate
    sf::Clock paintClock;
    sf::Time  paintTick = sf::milliseconds(8); // ~125 Hz

    // Flatten tool (Ctrl+click): capture reference height on first Ctrl+click
    bool flattenPrimed = false;
    int  flattenHeight = 0;

    // Import/Export UI (top-right)
    sf::Texture texExport, texImport;
    sf::Sprite  sprExport, sprImport;
    bool exportLoaded = texExport.loadFromFile("assets/images/exporter.png");
    bool importLoaded = texImport.loadFromFile("assets/images/importer.png");
    if (__log) __log << "[" << __now() << "] exportLoaded=" << (exportLoaded?"true":"false")
                     << ", importLoaded=" << (importLoaded?"true":"false") << std::endl;
    if (exportLoaded) {
        sprExport.setTexture(texExport);
        auto ts = texExport.getSize();
        if (ts.x > 0 && ts.y > 0) sprExport.setScale(32.f / (float)ts.x, 32.f / (float)ts.y);
    }
    if (importLoaded) {
        sprImport.setTexture(texImport);
        auto ts = texImport.getSize();
        if (ts.x > 0 && ts.y > 0) sprImport.setScale(32.f / (float)ts.x, 32.f / (float)ts.y);
    }
    float btnRadius = 20.f;
    sf::Vector2f importBtnPos, exportBtnPos;
    auto updateTopRightButtons = [&](){
        auto sz = window.getSize();
        float w = (float)sz.x;
        importBtnPos = sf::Vector2f(w - 16.f - btnRadius*2.f - 8.f - btnRadius*2.f, 16.f + btnRadius);
        exportBtnPos = sf::Vector2f(w - 16.f - btnRadius, 16.f + btnRadius);
    };
    auto updateLeftButtons = [&](){
        // Keep left panel stacked with fixed spacing
        btnGenerate.setPosition(16.f, 16.f);
        btnGrid.setPosition(16.f, 16.f + 36.f + 8.f);
        btnContinents.setPosition(16.f, 16.f + (36.f + 8.f) * 2.f);
        btnReset.setPosition(16.f, 16.f + (36.f + 8.f) * 3.f);
        btnReseed.setPosition(16.f, 16.f + (36.f + 8.f) * 4.f);
        seedBox.setPosition(16.f, 16.f + (36.f + 8.f) * 5.f);
        btnBake.setPosition(16.f, 16.f + (36.f + 8.f) * 6.f);
        // Recenter texts
        auto tb = btnText.getLocalBounds();
        btnText.setOrigin(tb.left + tb.width * 0.5f, tb.top + tb.height * 0.5f);
        btnText.setPosition(btnGenerate.getPosition() + sf::Vector2f(btnGenerate.getSize().x * 0.5f, btnGenerate.getSize().y * 0.5f));
        auto tg = btnGridText.getLocalBounds();
        btnGridText.setOrigin(tg.left + tg.width * 0.5f, tg.top + tg.height * 0.5f);
        btnGridText.setPosition(btnGrid.getPosition() + sf::Vector2f(btnGrid.getSize().x * 0.5f, btnGrid.getSize().y * 0.5f));
        auto tc2 = btnContinentsText.getLocalBounds();
        btnContinentsText.setOrigin(tc2.left + tc2.width * 0.5f, tc2.top + tc2.height * 0.5f);
        btnContinentsText.setPosition(btnContinents.getPosition() + sf::Vector2f(btnContinents.getSize().x * 0.5f, btnContinents.getSize().y * 0.5f));
        auto trr2 = btnResetText.getLocalBounds();
        btnResetText.setOrigin(trr2.left + trr2.width * 0.5f, trr2.top + trr2.height * 0.5f);
        btnResetText.setPosition(btnReset.getPosition() + sf::Vector2f(btnReset.getSize().x * 0.5f, btnReset.getSize().y * 0.5f));
        auto tr2 = btnReseedText.getLocalBounds();
        btnReseedText.setOrigin(tr2.left + tr2.width * 0.5f, tr2.top + tr2.height * 0.5f);
        btnReseedText.setPosition(btnReseed.getPosition() + sf::Vector2f(btnReseed.getSize().x * 0.5f, btnReseed.getSize().y * 0.5f));
        seedText.setPosition(seedBox.getPosition().x + 8.f, seedBox.getPosition().y + 4.f);
        auto tbk2 = btnBakeText.getLocalBounds();
        btnBakeText.setOrigin(tbk2.left + tbk2.width * 0.5f, tbk2.top + tbk2.height * 0.5f);
        btnBakeText.setPosition(btnBake.getPosition() + sf::Vector2f(btnBake.getSize().x * 0.5f, btnBake.getSize().y * 0.5f));
    };

    // FPS counter state
    sf::Clock fpsClock;
    // Frame clock for consistent per-frame delta time
    sf::Clock frameClock;
    int fpsFrames = 0;
    float fpsValue = 0.f;
    updateTopRightButtons();
    auto circleContains = [&](sf::Vector2f center, float r, sf::Vector2f p){ sf::Vector2f d=p-center; return (d.x*d.x + d.y*d.y) <= r*r; };

    // Import progress state
    bool importing = false;
    std::ifstream importFile;
    int importRow = 0;
    int importTotalRows = (cfg::GRID + 1);
    float importProgress = 0.f;
    auto beginImport = [&](const std::string& path){
        if (importing) { if (importFile.is_open()) importFile.close(); importing=false; }
        importFile.open(path);
        if (importFile.is_open()) { importing = true; importRow = 0; importProgress = 0.f; }
    };

    auto exportCSV = [&](const std::string& path){
        std::ofstream out(path);
        if (!out) return false;
        for (int i = 0; i <= cfg::GRID; ++i) {
            for (int j = 0; j <= cfg::GRID; ++j) {
                out << heights[idx(i,j)];
                if (j < cfg::GRID) out << ",";
            }
            out << "\n";
        }
        return true;
    };

    auto trim = [](std::string s){
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        if (a==std::string::npos) return std::string();
        return s.substr(a, b-a+1);
    };

    // Cross-platform file dialogs (adaptive on Linux)
    auto openFileDialogCSV = [&]()->std::string{
#ifdef _WIN32
        wchar_t fileName[MAX_PATH] = L"";
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = L"CSV Files\0*.csv\0All Files\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        ofn.lpstrDefExt = L"csv";
        if (GetOpenFileNameW(&ofn)) {
            int len = WideCharToMultiByte(CP_UTF8, 0, fileName, -1, nullptr, 0, nullptr, nullptr);
            std::string s(len-1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, fileName, -1, s.data(), len, nullptr, nullptr);
            return s;
        }
        return {};
#else
        auto runAndRead = [&](const char* cmd)->std::string{
            FILE* pipe = popen(cmd, "r");
            if (!pipe) return std::string();
            char buffer[4096]; std::string result;
            while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
            int code = pclose(pipe);
            if (code == -1) return std::string();
            result = trim(result);
            return result;
        };
        // 1) kdialog (KDE)
        {
            std::string r = runAndRead("kdialog --getopenfilename . '*.csv' 2>/dev/null");
            if (!r.empty()) return r;
        }
        // 2) zenity (GNOME/others)
        {
            std::string r = runAndRead("zenity --file-selection --file-filter='*.csv' 2>/dev/null");
            if (!r.empty()) return r;
        }
        // 3) yad
        {
            std::string r = runAndRead("yad --file-selection --file-filter='*.csv' 2>/dev/null");
            if (!r.empty()) return r;
        }
        // 4) qarma (qt zenity clone)
        {
            std::string r = runAndRead("qarma --file-selection --file-filter='*.csv' 2>/dev/null");
            if (!r.empty()) return r;
        }
        std::cerr << "[Import/Export] Aucun explorateur de fichiers détecté (kdialog/zenity/yad/qarma).\n"
                     "Installez 'zenity' pour une compatibilité à 100% (ex: sudo apt install -y zenity)." << std::endl;
        return {};
#endif
    };
    auto saveFileDialogCSV = [&]()->std::string{
#ifdef _WIN32
        wchar_t fileName[MAX_PATH] = L"map.csv";
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = L"CSV Files\0*.csv\0All Files\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = L"csv";
        if (GetSaveFileNameW(&ofn)) {
            int len = WideCharToMultiByte(CP_UTF8, 0, fileName, -1, nullptr, 0, nullptr, nullptr);
            std::string s(len-1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, fileName, -1, s.data(), len, nullptr, nullptr);
            return s;
        }
        return {};
#else
        auto runAndRead = [&](const char* cmd)->std::string{
            FILE* pipe = popen(cmd, "r");
            if (!pipe) return std::string();
            char buffer[4096]; std::string result;
            while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
            int code = pclose(pipe);
            if (code == -1) return std::string();
            result = trim(result);
            return result;
        };
        // 1) kdialog
        {
            std::string r = runAndRead("kdialog --getsavefilename ./ map.csv '*.csv' 2>/dev/null");
            if (!r.empty()) return r;
        }
        // 2) zenity
        {
            std::string r = runAndRead("zenity --file-selection --save --confirm-overwrite --file-filter='*.csv' 2>/dev/null");
            if (!r.empty()) {
                if (r.find('.') == std::string::npos) r += ".csv"; return r;
            }
        }
        // 3) yad
        {
            std::string r = runAndRead("yad --file-selection --save --confirm-overwrite --file-filter='*.csv' 2>/dev/null");
            if (!r.empty()) { if (r.find('.') == std::string::npos) r += ".csv"; return r; }
        }
        // 4) qarma
        {
            std::string r = runAndRead("qarma --file-selection --save --confirm-overwrite --file-filter='*.csv' 2>/dev/null");
            if (!r.empty()) { if (r.find('.') == std::string::npos) r += ".csv"; return r; }
        }
        return {};
#endif
    };
    // Slider geometry (in screen space, default view)
    auto sliderTrackRect = [&](){
        float trackH = 220.f;
        float trackW = 10.f;
        auto sz = window.getSize();
        float x = (float)sz.x - 40.f; // anchor 40px from right edge of current window
        float y = 100.f;              // fixed top offset
        return sf::FloatRect(x, y, trackW, trackH);
    };
    auto sliderThumbRect = [&](int value){
        sf::FloatRect tr = sliderTrackRect();
        // Inverted: top = larger brush, bottom = smaller
        float t = (float)(brushMax - value) / (float)(brushMax - brushMin);
        float thumbH = 18.f, thumbW = 26.f;
        float cx = tr.left + tr.width * 0.5f;
        float y = tr.top + t * tr.height;
        return sf::FloatRect(cx - thumbW * 0.5f, y - thumbH * 0.5f, thumbW, thumbH);
    };
    auto sliderPickValue = [&](sf::Vector2f p)->int{
        sf::FloatRect tr = sliderTrackRect();
        float clampedY = std::clamp(p.y, tr.top, tr.top + tr.height);
        float t = (tr.height <= 0.f) ? 0.f : (clampedY - tr.top) / tr.height;
        // Inverted mapping: top -> brushMax, bottom -> brushMin
        int v = brushMax - (int)std::round(t * (brushMax - brushMin));
        return std::clamp(v, brushMin, brushMax);
    };

    // (height slider UI removed)

    // Panning vars
    bool panning = false;
    bool tilting = false;
    sf::Vector2i panStartMouse;
    sf::Vector2f panStartCenter;
    sf::Vector2i tiltStartMouse;
    float tiltStartRot = 0.f;
    float tiltStartPitch = 1.f;

    

    // Initialize procedural mode and seed text
    if (proceduralMode) {
        chunkMgr.setMode(ChunkManager::Mode::Procedural, proceduralSeed);
        chunkMgr.setContinents(continentsOpt);
        if (fontLoaded) seedText.setString("Seed: " + std::to_string(proceduralSeed));
    }

    // Fullscreen toggle (F11)
    bool isFullscreen = false;
    auto recreateWindow = [&](bool fullscreen){
        isFullscreen = fullscreen;
        sf::VideoMode mode;
        sf::Uint32 style;
        if (fullscreen) {
            mode = sf::VideoMode::getDesktopMode();
            style = sf::Style::Fullscreen;
        } else {
            mode = sf::VideoMode(cfg::WINDOW_W, cfg::WINDOW_H);
            style = sf::Style::Default;
        }
        // Preserve current center of view in world coords
        sf::Vector2f center = view.getCenter();
        window.create(mode, "MyWorld - SFML Isometric Grid", style);
        window.setFramerateLimit(120);
        view.setSize((float)mode.width, (float)mode.height);
        view.setCenter(center);
        window.setView(view);
        updateTopRightButtons();
    };

    // Utility lambdas
    auto clamp = [](int v, int lo, int hi){ return std::max(lo, std::min(hi, v)); };

    auto worldToGridIntersection = [&](sf::Vector2f world)->sf::Vector2i {
        sf::Vector2f local = world - origin;
        sf::Vector2f ij = isoUnprojectDyn(local, iso);
        int I = static_cast<int>(std::round(ij.x));
        int J = static_cast<int>(std::round(ij.y));
        if (!proceduralMode) {
            I = std::clamp(I, 0, cfg::GRID);
            J = std::clamp(J, 0, cfg::GRID);
        }
        return {I, J};
    };

    auto pointInsideGrid = [&](sf::Vector2f world)->bool {
        sf::Vector2f local = world - origin;
        sf::Vector2f ij = isoUnprojectDyn(local, iso);
        if (proceduralMode) return true; // infinite procedural world
        return (ij.x >= 0.f && ij.y >= 0.f && ij.x <= (float)cfg::GRID && ij.y <= (float)cfg::GRID);
    };

    // Query elevation at an intersection (I,J), accounting for mode and visibility settings
    auto getIntersectionHeight = [&](int I, int J)->int {
        if (proceduralMode) {
            auto floorDiv = [](int a, int b){ return (a >= 0) ? (a / b) : ((a - (b - 1)) / b); };
            int cx = floorDiv(I, cfg::CHUNK_SIZE);
            int cy = floorDiv(J, cfg::CHUNK_SIZE);
            const Chunk& ch = chunkMgr.getChunk(cx, cy);
            int li = I - cx * cfg::CHUNK_SIZE;
            int lj = J - cy * cfg::CHUNK_SIZE;
            li = std::clamp(li, 0, cfg::CHUNK_SIZE);
            lj = std::clamp(lj, 0, cfg::CHUNK_SIZE);
            int k = li * (cfg::CHUNK_SIZE + 1) + lj;
            if (waterOnly) {
                // Visible surface in water-only: override if present, else sea level (0)
                return (k < (int)ch.overrideMask.size() && ch.overrideMask[k]) ? ch.overrides[k] : 0;
            } else {
                return ch.heights[k];
            }
        } else {
            int iC = std::clamp(I, 0, cfg::GRID);
            int jC = std::clamp(J, 0, cfg::GRID);
            return heights[idx(iC, jC)];
        }
    };

    // Rendering moved to render::*

    // Main loop
    if (__log) __log << "[" << __now() << "] entering main loop" << std::endl;
    while (window.isOpen()) {
        sf::Event ev;
        while (window.pollEvent(ev)) {
            switch (ev.type) {
                case sf::Event::Closed:
                    window.close();
                    break;
                case sf::Event::KeyPressed:
                    if (ev.key.code == sf::Keyboard::Escape) { if (__log) __log << "[" << __now() << "] Escape pressed -> close" << std::endl; window.close(); }
                    if (ev.key.code == sf::Keyboard::F11) {
                        if (__log) __log << "[" << __now() << "] toggle fullscreen" << std::endl; recreateWindow(!isFullscreen);
                    }
                    if (ev.key.code == sf::Keyboard::R) {
                        // Reset camera and projection to defaults (45° rotation, pitch 1)
                        iso.rotDeg = 45.f;
                        iso.pitch  = 1.f;
                        view = sf::View(sf::FloatRect(0.f, 0.f, (float)cfg::WINDOW_W, (float)cfg::WINDOW_H));
                        // Recenter on grid center under current projection
                        sf::Vector2f newCenter = isoProjectDyn(cfg::GRID * 0.5f, cfg::GRID * 0.5f, 0.f, iso) + origin;
                        view.setCenter(newCenter);
                        window.setView(view);
                        if (__log) __log << "[" << __now() << "] Reset view (R)" << std::endl;
                    }
                    if (ev.key.code == sf::Keyboard::G) {
                        // Same as clicking "Générer": reveal terrain and reset user modifications
                        if (!proceduralMode) {
                            proceduralMode = true;
                            proceduralSeed = (uint32_t)std::rand();
                            chunkMgr.setMode(ChunkManager::Mode::Procedural, proceduralSeed);
                            chunkMgr.setContinents(continentsOpt);
                        }
                        chunkMgr.resetOverrides();
                        waterOnly = false;
                        if (fontLoaded) seedText.setString("Seed: " + std::to_string(proceduralSeed));
                    }
                    if (ev.key.code == sf::Keyboard::F3) {
                        showGrid = !showGrid; if (__log) __log << "[" << __now() << "] Grid toggle -> " << (showGrid?"ON":"OFF") << std::endl;
                    }
                    if (ev.key.code == sf::Keyboard::F2) {
                        shadowsEnabled = !shadowsEnabled; if (__log) __log << "[" << __now() << "] Shadows toggle -> " << (shadowsEnabled?"ON":"OFF") << std::endl;
                    }
                    break;
                case sf::Event::MouseWheelScrolled:
                    {
                        // Zoom with limits: prevent infinite zoom out/in
                        sf::Vector2f viewSize = view.getSize();
                        sf::Vector2f defSize  = window.getDefaultView().getSize();
                        float curScale = std::max(viewSize.x / std::max(1.f, defSize.x), viewSize.y / std::max(1.f, defSize.y));
                        const float minZoom = 0.35f;  // smallest scale (most zoomed in)
                        const float maxZoom = 6.0f;   // largest scale (most zoomed out)
                        float desired = (ev.mouseWheelScroll.delta > 0) ? 0.9f : 1.1f;
                        float newScale = curScale * desired;
                        float apply = desired;
                        if (newScale < minZoom) apply = std::max(0.01f, minZoom / std::max(0.01f, curScale));
                        if (newScale > maxZoom) apply = std::max(0.01f, maxZoom / std::max(1.f, curScale));
                        if (std::fabs(apply - 1.f) > 1e-4f) {
                            view.zoom(apply);
                            window.setView(view);
                        }
                    }
                    break;
                case sf::Event::MouseButtonPressed:
                    if (ev.mouseButton.button == sf::Mouse::Middle) {
                        panning = true;
                        panStartMouse = sf::Mouse::getPosition(window);
                        panStartCenter = view.getCenter();
                    } else if (ev.mouseButton.button == sf::Mouse::Left || ev.mouseButton.button == sf::Mouse::Right) {
                        sf::Vector2i mp = sf::Mouse::getPosition(window);
                        // First, check UI button in screen space (use default view)
                        sf::Vector2f screen = window.mapPixelToCoords(mp, window.getDefaultView());
                        // Inventory clicks (bottom center)
                        {
                            auto [rBull, rBrush, rEraser] = inventoryRects();
                            if (rBull.contains(screen)) {
                                currentTool = Tool::Bulldozer;
                                showColorPicker = false;
                                showColorHover = false;
                                break;
                            }
                            if (rBrush.contains(screen)) {
                                currentTool = Tool::Brush;
                                showColorPicker = true;
                                showColorHover = true;
                                break;
                            }
                            if (rEraser.contains(screen)) {
                                currentTool = Tool::Eraser;
                                showColorPicker = false;
                                showColorHover = false;
                                break;
                            }
                        }
                        // Color picker interactions (left column below buttons)
                        if (showColorPicker) {
                            ensureColorWheel();
                            // Compute wheel center under bake button
                            float leftX = 16.f;
                            float panelW = 140.f;
                            float wheelTop = btnBake.getPosition().y + btnBake.getSize().y + 16.f;
                            sf::Vector2f wheelCenter(leftX + panelW*0.5f, wheelTop + (float)colorWheelRadius);
                            sf::FloatRect wheelRect(wheelCenter.x - colorWheelRadius, wheelCenter.y - colorWheelRadius, (float)colorWheelRadius*2, (float)colorWheelRadius*2);
                            bool handledPick = false;
                            if (wheelRect.contains(screen)) {
                                sf::Vector2f p = screen - wheelCenter;
                                float r = std::sqrt(p.x*p.x + p.y*p.y);
                                if (r <= (float)colorWheelRadius) {
                                    float angle = std::atan2(p.y, p.x) * 180.f / 3.14159265f; if (angle < 0) angle += 360.f;
                                    float s = std::clamp(r / (float)colorWheelRadius, 0.f, 1.f);
                                    selectedColor = hsv2rgb(angle, s, 1.f);
                                    pushHistory(selectedColor);
                                    rebuildToneTex();
                                    activeColor = applyTone(selectedColor, colorToneT);
                                    handledPick = true;
                                }
                            }
                            // History swatches (5)
                            if (!handledPick) {
                                const int N = 5; float sw = 22.f, sh = 22.f, gap = 6.f;
                                float totalW = N*sw + (N-1)*gap;
                                float hx = leftX + (panelW - totalW) * 0.5f;
                                float hy = wheelRect.top + wheelRect.height + 12.f;
                                for (int i=0; i<(int)colorHistory.size() && i<N; ++i){
                                    sf::FloatRect rct(hx + i*(sw+gap), hy, sw, sh);
                                    if (rct.contains(screen)) {
                                        selectedColor = colorHistory[i];
                                        // Move selected to front
                                        pushHistory(selectedColor);
                                        rebuildToneTex();
                                        activeColor = applyTone(selectedColor, colorToneT);
                                        handledPick = true;
                                        break;
                                    }
                                }
                            }
                            // Tone slider interaction
                            if (!handledPick) {
                                const float toneH = 18.f; const float tonePad = 12.f;
                                const int N = 5; float sw = 22.f, sh = 22.f, gap = 6.f;
                                float totalW = N*sw + (N-1)*gap;
                                float hx = leftX + (panelW - totalW) * 0.5f;
                                float hy = wheelRect.top + wheelRect.height + 12.f; // history y
                                float toneY = hy + sh + tonePad;
                                sf::FloatRect toneRect(leftX, toneY, panelW, toneH);
                                if (toneRect.contains(screen)) {
                                    float t = (screen.x - toneRect.left) / toneRect.width;
                                    colorToneT = std::clamp(t, 0.f, 1.f);
                                    activeColor = applyTone(selectedColor, colorToneT);
                                    toneDragging = true;
                                    handledPick = true;
                                }
                            }
                            if (handledPick) break; // consume click if we picked a color
                        }
                        if (btnGenerate.getGlobalBounds().contains(screen)) {
                            // Reveal terrain from water-only
                            if (!proceduralMode) {
                                proceduralMode = true;
                                proceduralSeed = (uint32_t)std::rand();
                                chunkMgr.setMode(ChunkManager::Mode::Procedural, proceduralSeed);
                                chunkMgr.setContinents(continentsOpt);
                            }
                            // Reset all user overrides before revealing
                            chunkMgr.resetOverrides();
                            waterOnly = false;
                            if (fontLoaded) seedText.setString("Seed: " + std::to_string(proceduralSeed));
                            break;
                        }
                        if (btnGrid.getGlobalBounds().contains(screen)) {
                            showGrid = !showGrid; if (__log) __log << "[" << __now() << "] Grid toggle (button) -> " << (showGrid?"ON":"OFF") << std::endl;
                            break;
                        }
                        if (btnContinents.getGlobalBounds().contains(screen)) {
                            continentsOpt = !continentsOpt; if (__log) __log << "[" << __now() << "] Continents toggle -> " << (continentsOpt?"ON":"OFF") << std::endl;
                            if (fontLoaded) btnContinentsText.setString(U8(std::string("Continents: ") + (continentsOpt?"ON":"OFF")));
                            if (proceduralMode) { chunkMgr.setContinents(continentsOpt); }
                            // On turning continents ON, reinitialize user modifications
                            if (continentsOpt) { chunkMgr.resetOverrides(); }
                            break;
                        }
                        if (btnReset.getGlobalBounds().contains(screen)) {
                            // Reset to water-only but keep procedural world active (same seed)
                            if (__log) __log << "[" << __now() << "] RESET clicked -> water-only" << std::endl;
                            proceduralMode = true;
                            waterOnly = true;
                            chunkMgr.setMode(ChunkManager::Mode::Procedural, proceduralSeed);
                            chunkMgr.setContinents(continentsOpt);
                            break;
                        }
                        if (btnReseed.getGlobalBounds().contains(screen)) {
                            // Re-seed current procedural generator (if ON)
                            if (proceduralMode) {
                                proceduralSeed = (uint32_t)std::rand();
                                chunkMgr.setMode(ChunkManager::Mode::Procedural, proceduralSeed);
                                chunkMgr.setContinents(continentsOpt);
                                seedBuffer.clear();
                                seedText.setString("Seed: " + std::to_string(proceduralSeed));
                            }
                            break;
                        }
                        if (btnBake.getGlobalBounds().contains(screen)) {
                            // Bake: sample current procedural terrain into static heights and disable procedural mode
                            if (proceduralMode) {
                                // Determine world tile coordinate window centered at current view center
                                sf::Vector2f centerWorld = view.getCenter();
                                sf::Vector2f centerLocal = centerWorld - origin;
                                sf::Vector2f centerIJ = isoUnprojectDyn(centerLocal, iso);
                                int Icenter = (int)std::floor(centerIJ.x + 0.5f);
                                int Jcenter = (int)std::floor(centerIJ.y + 0.5f);
                                int I0 = Icenter - cfg::GRID / 2;
                                int J0 = Jcenter - cfg::GRID / 2;
                                auto sampleWorld = [&](int I, int J)->int{
                                    int cx = (I >= 0) ? (I / cfg::CHUNK_SIZE) : ((I - (cfg::CHUNK_SIZE - 1)) / cfg::CHUNK_SIZE);
                                    int cy = (J >= 0) ? (J / cfg::CHUNK_SIZE) : ((J - (cfg::CHUNK_SIZE - 1)) / cfg::CHUNK_SIZE);
                                    int li = I - cx * cfg::CHUNK_SIZE;
                                    int lj = J - cy * cfg::CHUNK_SIZE;
                                    if (li < 0) { cx -= 1; li += cfg::CHUNK_SIZE; }
                                    if (lj < 0) { cy -= 1; lj += cfg::CHUNK_SIZE; }
                                    const Chunk& ch = chunkMgr.getChunk(cx, cy);
                                    int S1 = cfg::CHUNK_SIZE + 1;
                                    li = std::clamp(li, 0, cfg::CHUNK_SIZE);
                                    lj = std::clamp(lj, 0, cfg::CHUNK_SIZE);
                                    return ch.heights[li * S1 + lj];
                                };
                                for (int i = 0; i <= cfg::GRID; ++i) {
                                    for (int j = 0; j <= cfg::GRID; ++j) {
                                        heights[i * (cfg::GRID + 1) + j] = sampleWorld(I0 + i, J0 + j);
                                    }
                                }
                                // Disable procedural mode so edits affect this baked map
                                proceduralMode = false;
                                chunkMgr.setMode(ChunkManager::Mode::Empty, 0);
                            }
                            break;
                        }
                        if (seedBox.getGlobalBounds().contains(screen)) {
                            seedEditing = true;
                            seedBuffer.clear();
                            break;
                        } else {
                            seedEditing = false;
                        }
                        // Import/Export buttons (top-right, round white)
                        if (circleContains(exportBtnPos, btnRadius, screen)) {
                            if (__log) __log << "[" << __now() << "] Export clicked" << std::endl; std::string path = saveFileDialogCSV();
                            if (!path.empty()) exportCSV(path);
                            break;
                        }
                        if (circleContains(importBtnPos, btnRadius, screen)) {
                            if (__log) __log << "[" << __now() << "] Import clicked" << std::endl; std::string path = openFileDialogCSV();
                            if (!path.empty()) beginImport(path);
                            break;
                        }
                        // Inventory toolbar (bottom-center) click detection
                        {
                            auto [rBulldozer, rBrush, rEraser] = inventoryRects();
                            if (rBulldozer.contains(screen)) {
                                currentTool = Tool::Bulldozer;
                                showColorPicker = false;
                                showColorHover = false;
                                toneDragging = false;
                                break;
                            }
                            if (rBrush.contains(screen)) {
                                currentTool = Tool::Brush;
                                showColorPicker = true;
                                showColorHover = true;
                                break;
                            }
                            if (rEraser.contains(screen)) {
                                currentTool = Tool::Eraser;
                                showColorPicker = false;
                                showColorHover = false;
                                toneDragging = false;
                                break;
                            }
                        }
                        // (height slider removed)
                        // Brush slider interaction (right side)
                        if (sliderTrackRect().contains(screen) || sliderThumbRect(brushSize).contains(screen)) {
                            brushDragging = true;
                            brushSize = sliderPickValue(screen);
                            break;
                        }
                        // Then, world interactions
                        sf::Vector2f world = window.mapPixelToCoords(mp, view);
                        if (pointInsideGrid(world)) {
                            // Tool-dependent action on press
                            sf::Vector2i IJ = worldToGridIntersection(world);
                            int brush = std::clamp(brushSize, brushMin, brushMax);
                            bool ctrl = sf::Keyboard::isKeyPressed(sf::Keyboard::LControl) || sf::Keyboard::isKeyPressed(sf::Keyboard::RControl);
                            if (currentTool == Tool::Bulldozer) {
                                // Elevation edit around nearest intersection (brush)
                                if (ctrl && (ev.mouseButton.button == sf::Mouse::Left || ev.mouseButton.button == sf::Mouse::Right)) {
                                    // Capture flatten reference height on first Ctrl+click
                                    if (proceduralMode) {
                                        auto floorDiv = [](int a, int b){ return (a >= 0) ? (a / b) : ((a - (b - 1)) / b); };
                                        int cx = floorDiv(IJ.x, cfg::CHUNK_SIZE);
                                        int cy = floorDiv(IJ.y, cfg::CHUNK_SIZE);
                                        const Chunk& ch = chunkMgr.getChunk(cx, cy);
                                        int li = IJ.x - cx * cfg::CHUNK_SIZE;
                                        int lj = IJ.y - cy * cfg::CHUNK_SIZE;
                                        li = std::clamp(li, 0, cfg::CHUNK_SIZE);
                                        lj = std::clamp(lj, 0, cfg::CHUNK_SIZE);
                                        int k = li * (cfg::CHUNK_SIZE + 1) + lj;
                                        if (waterOnly) {
                                            // Visible surface in water-only: override if present, else sea level (0)
                                            flattenHeight = (k < (int)ch.overrideMask.size() && ch.overrideMask[k]) ? ch.overrides[k] : 0;
                                        } else {
                                            // Terrain visible: use actual procedural height (with overrides already applied in ch.heights)
                                            flattenHeight = ch.heights[k];
                                        }
                                    } else {
                                        flattenHeight = heights[idx(IJ.x, IJ.y)];
                                    }
                                    flattenPrimed = true;
                                    // Immediately flatten current brush area (square brush)
                                    int half = brush - 1;
                                    for (int di = -brush; di <= brush; ++di) {
                                        for (int dj = -brush; dj <= brush; ++dj) {
                                            int I = IJ.x + di;
                                            int J = IJ.y + dj;
                                            if (!proceduralMode) { if (I < 0 || J < 0 || I > cfg::GRID || J > cfg::GRID) continue; }
                                            if (std::max(std::abs(di), std::abs(dj)) > half) continue;
                                            if (proceduralMode) {
                                                chunkMgr.applySetAt(I, J, flattenHeight);
                                            } else {
                                                heights[idx(I, J)] = flattenHeight;
                                            }
                                        }
                                    }
                                } else {
                                    int delta = (ev.mouseButton.button == sf::Mouse::Left) ? 1 : -1;
                                    int half = brush - 1;
                                    for (int di = -brush; di <= brush; ++di) {
                                        for (int dj = -brush; dj <= brush; ++dj) {
                                            int I = IJ.x + di;
                                            int J = IJ.y + dj;
                                            if (!proceduralMode) { if (I < 0 || J < 0 || I > cfg::GRID || J > cfg::GRID) continue; }
                                            // square brush shape (Chebyshev radius)
                                            if (std::max(std::abs(di), std::abs(dj)) > (brush - 1)) continue;
                                            if (proceduralMode) {
                                                if (waterOnly) {
                                                    // Start from sea (0) unless an override exists
                                                    auto floorDiv = [](int a, int b){ return (a >= 0) ? (a / b) : ((a - (b - 1)) / b); };
                                                    int cx = floorDiv(I, cfg::CHUNK_SIZE);
                                                    int cy = floorDiv(J, cfg::CHUNK_SIZE);
                                                    const Chunk& ch = chunkMgr.getChunk(cx, cy);
                                                    int li = I - cx * cfg::CHUNK_SIZE;
                                                    int lj = J - cy * cfg::CHUNK_SIZE;
                                                    li = std::clamp(li, 0, cfg::CHUNK_SIZE);
                                                    lj = std::clamp(lj, 0, cfg::CHUNK_SIZE);
                                                    int k = li * (cfg::CHUNK_SIZE + 1) + lj;
                                                    int base = (k < (int)ch.overrideMask.size() && ch.overrideMask[k]) ? ch.overrides[k] : 0;
                                                    int v = std::clamp(base + delta, cfg::MIN_ELEV, cfg::MAX_ELEV);
                                                    chunkMgr.applySetAt(I, J, v);
                                                } else {
                                                    chunkMgr.applyDeltaAt(I, J, delta);
                                                }
                                            } else {
                                                heights[idx(I, J)] += delta;
                                            }
                                        }
                                    }
                                }
                            } else if (currentTool == Tool::Brush && ev.mouseButton.button == sf::Mouse::Left) {
                                // Paint on click (same as drag logic)
                                sf::Vector2f local = world - origin; sf::Vector2f ij = isoUnprojectDyn(local, iso);
                                int i0 = (int)std::floor(ij.x); int j0 = (int)std::floor(ij.y);
                                int half = brush - 1;
                                for (int di = -brush; di <= brush; ++di) {
                                    for (int dj = -brush; dj <= brush; ++dj) {
                                        int ci = i0 + di; int cj = j0 + dj;
                                        if (std::max(std::abs(di), std::abs(dj)) > half) continue;
                                        long long k = key64(ci, cj);
                                        paintedCells[k] = activeColor;
                                    }
                                }
                            }
                        } else {
                            // Start tilting
                            tilting = true;
                            tiltStartMouse = mp;
                            tiltStartRot = iso.rotDeg;
                            tiltStartPitch = iso.pitch;
                        }
                    }
                    break;
                case sf::Event::MouseButtonReleased:
                    if (ev.mouseButton.button == sf::Mouse::Middle) panning = false;
                    if (ev.mouseButton.button == sf::Mouse::Left || ev.mouseButton.button == sf::Mouse::Right) tilting = false;
                    if (ev.mouseButton.button == sf::Mouse::Left) toneDragging = false;
                    brushDragging = false;
                    break;
                case sf::Event::MouseMoved:
                    {
                        // Tone slider drag update (screen space)
                        if (toneDragging) {
                            sf::Vector2i mp = sf::Mouse::getPosition(window);
                            sf::Vector2f screen = window.mapPixelToCoords(mp, window.getDefaultView());
                            float leftX = 16.f;
                            float panelW = 140.f;
                            float wheelTop = btnBake.getPosition().y + btnBake.getSize().y + 16.f;
                            sf::Vector2f wheelCenter(leftX + panelW*0.5f, wheelTop + (float)colorWheelRadius);
                            const int N = 5; float sw = 22.f, sh = 22.f, gap = 6.f;
                            float totalW = N*sw + (N-1)*gap;
                            float hx = leftX + (panelW - totalW) * 0.5f;
                            float hy = wheelCenter.y + colorWheelRadius + 12.f; // history y
                            const float toneH = 18.f; const float tonePad = 12.f;
                            float toneY = hy + sh + tonePad;
                            sf::FloatRect toneRect(leftX, toneY, panelW, toneH);
                            float t = (screen.x - toneRect.left) / toneRect.width;
                            colorToneT = std::clamp(t, 0.f, 1.f);
                            activeColor = applyTone(selectedColor, colorToneT);
                        }
                        // Update UI hover (screen space)
                        sf::Vector2i mp = sf::Mouse::getPosition(window);
                        sf::Vector2f screen = window.mapPixelToCoords(mp, window.getDefaultView());
                        genHover = btnGenerate.getGlobalBounds().contains(screen);
                        gridHover = btnGrid.getGlobalBounds().contains(screen);
                        continentsHover = btnContinents.getGlobalBounds().contains(screen);
                        bakeHover = btnBake.getGlobalBounds().contains(screen);
                        // (height slider removed)
                        if (brushDragging) {
                            // Update brush while dragging on slider
                            brushSize = sliderPickValue(screen);
                        }

                        // Edit/Paint while dragging (if not panning/tilting), throttled by paintTick
                        if (!panning && !tilting && !brushDragging && (sf::Mouse::isButtonPressed(sf::Mouse::Left) || sf::Mouse::isButtonPressed(sf::Mouse::Right))) {
                            if (paintClock.getElapsedTime() >= paintTick) {
                                paintClock.restart();
                                sf::Vector2f world = window.mapPixelToCoords(mp, view);
                                if (pointInsideGrid(world)) {
                                    sf::Vector2i IJ = worldToGridIntersection(world);
                                    int brush = std::clamp(brushSize, brushMin, brushMax);
                                    bool ctrl = sf::Keyboard::isKeyPressed(sf::Keyboard::LControl) || sf::Keyboard::isKeyPressed(sf::Keyboard::RControl);
                                    if (currentTool == Tool::Bulldozer && ctrl && flattenPrimed && (sf::Mouse::isButtonPressed(sf::Mouse::Left) || sf::Mouse::isButtonPressed(sf::Mouse::Right))) {
                                        // Flatten to captured height while Ctrl is held
                                        int half = brush - 1;
                                        for (int di = -brush; di <= brush; ++di) {
                                            for (int dj = -brush; dj <= brush; ++dj) {
                                                int I = IJ.x + di;
                                                int J = IJ.y + dj;
                                                if (!proceduralMode) { if (I < 0 || J < 0 || I > cfg::GRID || J > cfg::GRID) continue; }
                                                if (std::max(std::abs(di), std::abs(dj)) > half) continue;
                                                if (proceduralMode) {
                                                    chunkMgr.applySetAt(I, J, flattenHeight);
                                                } else {
                                                    heights[idx(I, J)] = flattenHeight;
                                                }
                                            }
                                        }
                                    } else if (currentTool == Tool::Bulldozer) {
                                        int delta = sf::Mouse::isButtonPressed(sf::Mouse::Left) ? 1 : -1;
                                        int half = brush - 1;
                                        for (int di = -brush; di <= brush; ++di) {
                                            for (int dj = -brush; dj <= brush; ++dj) {
                                                int I = IJ.x + di;
                                                int J = IJ.y + dj;
                                                if (!proceduralMode) { if (I < 0 || J < 0 || I > cfg::GRID || J > cfg::GRID) continue; }
                                                if (std::max(std::abs(di), std::abs(dj)) > half) continue;
                                                if (proceduralMode) {
                                                    if (waterOnly) {
                                                        // Start from sea (0) unless an override exists
                                                        auto floorDiv = [](int a, int b){ return (a >= 0) ? (a / b) : ((a - (b - 1)) / b); };
                                                        int cx = floorDiv(I, cfg::CHUNK_SIZE);
                                                        int cy = floorDiv(J, cfg::CHUNK_SIZE);
                                                        const Chunk& ch = chunkMgr.getChunk(cx, cy);
                                                        int li = I - cx * cfg::CHUNK_SIZE;
                                                        int lj = J - cy * cfg::CHUNK_SIZE;
                                                        li = std::clamp(li, 0, cfg::CHUNK_SIZE);
                                                        lj = std::clamp(lj, 0, cfg::CHUNK_SIZE);
                                                        int k = li * (cfg::CHUNK_SIZE + 1) + lj;
                                                        int base = (k < (int)ch.overrideMask.size() && ch.overrideMask[k]) ? ch.overrides[k] : 0;
                                                        int v = std::clamp(base + delta, cfg::MIN_ELEV, cfg::MAX_ELEV);
                                                        chunkMgr.applySetAt(I, J, v);
                                                    } else {
                                                        chunkMgr.applyDeltaAt(I, J, delta);
                                                    }
                                                } else {
                                                    heights[idx(I, J)] += delta;
                                                }
                                            }
                                        }
                                    } else if (currentTool == Tool::Brush && sf::Mouse::isButtonPressed(sf::Mouse::Left)) {
                                        // Paint while dragging
                                        sf::Vector2f local = world - origin; sf::Vector2f ij = isoUnprojectDyn(local, iso);
                                        int i0 = (int)std::floor(ij.x); int j0 = (int)std::floor(ij.y);
                                        int half = brush - 1;
                                        for (int di = -brush; di <= brush; ++di) {
                                            for (int dj = -brush; dj <= brush; ++dj) {
                                                int ci = i0 + di; int cj = j0 + dj;
                                                if (std::max(std::abs(di), std::abs(dj)) > half) continue;
                                                long long k = key64(ci, cj);
                                                paintedCells[k] = activeColor;
                                            }
                                        }
                                    } else if (currentTool == Tool::Eraser && sf::Mouse::isButtonPressed(sf::Mouse::Left)) {
                                        // Erase painted cells while dragging
                                        sf::Vector2f local = world - origin; sf::Vector2f ij = isoUnprojectDyn(local, iso);
                                        int i0 = (int)std::floor(ij.x); int j0 = (int)std::floor(ij.y);
                                        int half = brush - 1;
                                        for (int di = -brush; di <= brush; ++di) {
                                            for (int dj = -brush; dj <= brush; ++dj) {
                                                int ci = i0 + di; int cj = j0 + dj;
                                                if (std::max(std::abs(di), std::abs(dj)) > half) continue;
                                                long long k = key64(ci, cj);
                                                paintedCells.erase(k);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (panning) {
                            sf::Vector2i now = sf::Mouse::getPosition(window);
                            sf::Vector2f delta = window.mapPixelToCoords(panStartMouse, view) - window.mapPixelToCoords(now, view);
                            view.setCenter(panStartCenter + delta);
                            window.setView(view);
                        } else if (tilting) {
                            sf::Vector2i now = sf::Mouse::getPosition(window);
                            sf::Vector2i d = now - tiltStartMouse;
                            iso.rotDeg = tiltStartRot + d.x * 0.2f; // horizontal drag rotates
                            iso.pitch  = std::clamp(tiltStartPitch * std::exp(-d.y * 0.003f), 0.3f, 2.0f); // vertical drag tilts
                            // Keep the view centered on grid center under new projection
                            sf::Vector2f newCenter = isoProjectDyn(cfg::GRID * 0.5f, cfg::GRID * 0.5f, 0.f, iso) + origin;
                            view.setCenter(newCenter);
                            window.setView(view);
                            
                        }
                    }
                    break;
                case sf::Event::TextEntered:
                    if (seedEditing) {
                        sf::Uint32 u = ev.text.unicode;
                        if (u == 13) { // Enter
                            seedEditing = false;
                            if (!seedBuffer.empty()) {
                                unsigned long long v = 0ULL;
                                try { v = std::stoull(seedBuffer); } catch (...) { v = 0ULL; }
                                proceduralSeed = static_cast<uint32_t>(v & 0xFFFFFFFFu);
                                if (proceduralMode) {
                                    chunkMgr.setMode(ChunkManager::Mode::Procedural, proceduralSeed);
                                }
                                seedText.setString("Seed: " + std::to_string(proceduralSeed));
                                seedBuffer.clear();
                            }
                        } else if (u == 27) { // Esc
                            seedEditing = false;
                            seedBuffer.clear();
                        } else if (u == 8) { // Backspace
                            if (!seedBuffer.empty()) seedBuffer.pop_back();
                            seedText.setString("Seed: " + seedBuffer);
                        } else if (u >= '0' && u <= '9') {
                            if (seedBuffer.size() < 10) {
                                seedBuffer.push_back(static_cast<char>(u));
                                seedText.setString("Seed: " + seedBuffer);
                            }
                        }
                    }
                    break;
                case sf::Event::Resized:
                    // Adjust view to new window size and update UI positions
                    view.setSize((float)ev.size.width, (float)ev.size.height);
                    window.setView(view);
                    updateTopRightButtons();
                    updateLeftButtons();
                    break;
                default: break;
            }
        }

        // Import processing (incremental per frame)
        if (importing && importFile.is_open()) {
            std::string line;
            int linesPerFrame = 8;
            for (int k = 0; k < linesPerFrame && importing; ++k) {
                if (!std::getline(importFile, line)) { importing=false; importFile.close(); break; }
                if (importRow <= cfg::GRID) {
                    std::stringstream ss(line);
                    std::string item; int col = 0;
                    while (std::getline(ss, item, ',') && col <= cfg::GRID) {
                        int val = 0; try { val = std::stoi(trim(item)); } catch (...) { val = 0; }
                        heights[idx(importRow, col)] = val;
                        ++col;
                    }
                }
                ++importRow;
                importProgress = std::clamp(importRow / (float)importTotalRows, 0.f, 1.f);
                if (importRow > cfg::GRID) {
                    // Still read remaining lines to reach EOF quickly
                    if (importRow >= importTotalRows) { /* done */ }
                }
            }
            
        }

        // Keyboard panning
        const float panSpeedBase = 600.f; // doubled speed: base world units per second at default zoom
        // Scale speed with zoom level: larger view size => faster pan
        sf::Vector2f viewSize = view.getSize();
        sf::Vector2f defSize  = window.getDefaultView().getSize();
        float zoomScale = std::max(viewSize.x / std::max(1.f, defSize.x), viewSize.y / std::max(1.f, defSize.y));
        float panSpeed = panSpeedBase * std::max(0.1f, zoomScale);
        // Real delta time to keep speed consistent across refresh rates/fullscreen
        float dt = frameClock.restart().asSeconds();
        dt = std::clamp(dt, 0.0005f, 0.05f); // clamp to avoid spikes
        sf::Vector2f move(0.f, 0.f);
        auto key = [&](sf::Keyboard::Key k){ return sf::Keyboard::isKeyPressed(k); };
        if (key(sf::Keyboard::W) || key(sf::Keyboard::Up)    || key(sf::Keyboard::Z)) move.y -= panSpeed * dt; // ZQSD alias
        if (key(sf::Keyboard::S) || key(sf::Keyboard::Down))                        move.y += panSpeed * dt;
        if (key(sf::Keyboard::A) || key(sf::Keyboard::Left)  || key(sf::Keyboard::Q)) move.x -= panSpeed * dt; // ZQSD alias
        if (key(sf::Keyboard::D) || key(sf::Keyboard::Right))                       move.x += panSpeed * dt;
        if (move.x != 0.f || move.y != 0.f) {
            view.move(move);
            window.setView(view);
        }

        window.clear(sf::Color::Black);
        // Ensure world view is active before world rendering (avoid leaving default view from UI phase)
        window.setView(view);
        // Wireframe-only view: do not draw filled tiles or interstices

        // Build hover mask for current brush (Chebyshev square), available to all render calls below
        std::unordered_set<long long> hoverMask;
        if (showColorHover && currentTool == Tool::Brush) {
            sf::Vector2i mp = sf::Mouse::getPosition(window);
            sf::Vector2f world = window.mapPixelToCoords(mp, view);
            if (pointInsideGrid(world)) {
                sf::Vector2f ij = isoUnprojectDyn(world - origin, iso);
                int i0 = (int)std::floor(ij.x);
                int j0 = (int)std::floor(ij.y);
                int brush = std::clamp(brushSize, brushMin, brushMax);
                int half = brush - 1;
                for (int di = -brush; di <= brush; ++di) {
                    for (int dj = -brush; dj <= brush; ++dj) {
                        if (std::max(std::abs(di), std::abs(dj)) > half) continue;
                        int ci = i0 + di;
                        int cj = j0 + dj;
                        if (!proceduralMode) {
                            if (ci < 0 || cj < 0 || ci >= cfg::GRID || cj >= cfg::GRID) continue;
                        }
                        hoverMask.insert(key64(ci, cj));
                    }
                }
            }
        }

        if (proceduralMode) {
            // Per-chunk rendering with culling based on view rectangle unprojected to grid space
            const auto& v = window.getView();
            sf::Vector2f vc = v.getCenter();
            sf::Vector2f vs = v.getSize();
            const float margin = 64.f;
            sf::FloatRect viewRect(vc.x - vs.x * 0.5f - margin,
                                   vc.y - vs.y * 0.5f - margin,
                                   vs.x + 2 * margin, vs.y + 2 * margin);

            // Unproject view rect corners to approximate visible I,J bounds
            auto unproj = [&](sf::Vector2f w){ return isoUnprojectDyn(w - origin, iso); };
            sf::Vector2f p0(viewRect.left, viewRect.top);
            sf::Vector2f p1(viewRect.left + viewRect.width, viewRect.top);
            sf::Vector2f p2(viewRect.left + viewRect.width, viewRect.top + viewRect.height);
            sf::Vector2f p3(viewRect.left, viewRect.top + viewRect.height);
            sf::Vector2f ij0 = unproj(p0);
            sf::Vector2f ij1 = unproj(p1);
            sf::Vector2f ij2 = unproj(p2);
            sf::Vector2f ij3 = unproj(p3);
            float minI = std::min(std::min(ij0.x, ij1.x), std::min(ij2.x, ij3.x));
            float maxI = std::max(std::max(ij0.x, ij1.x), std::max(ij2.x, ij3.x));
            float minJ = std::min(std::min(ij0.y, ij1.y), std::min(ij2.y, ij3.y));
            float maxJ = std::max(std::max(ij0.y, ij1.y), std::max(ij2.y, ij3.y));
            // Expand a bit to avoid missing borders
            minI -= cfg::CHUNK_SIZE * 0.5f; maxI += cfg::CHUNK_SIZE * 0.5f;
            minJ -= cfg::CHUNK_SIZE * 0.5f; maxJ += cfg::CHUNK_SIZE * 0.5f;

            auto floorDiv = [](int a, int b){ return (a >= 0) ? (a / b) : ((a - (b - 1)) / b); };
            int Imin = (int)std::floor(minI);
            int Imax = (int)std::ceil (maxI);
            int Jmin = (int)std::floor(minJ);
            int Jmax = (int)std::ceil (maxJ);
            int cx0 = floorDiv(Imin, cfg::CHUNK_SIZE);
            int cx1 = floorDiv(Imax, cfg::CHUNK_SIZE);
            int cy0 = floorDiv(Jmin, cfg::CHUNK_SIZE);
            int cy1 = floorDiv(Jmax, cfg::CHUNK_SIZE);

            // LOD: limit chunk generation/draw radius according to zoom (and a hard cap)
            sf::Vector2f defSize = window.getDefaultView().getSize();
            float zoomScale = std::max(vs.x / std::max(1.f, defSize.x), vs.y / std::max(1.f, defSize.y));
            // Smaller radius when zoomed out (large zoomScale), bigger when zoomed in
            const int hardMaxRadius = 10;   // never generate beyond this many chunks from center
            const float lodBase = 7.5f;     // tune base radius
            int allowedRadius = (int)std::clamp(std::round(lodBase / std::max(0.5f, zoomScale)), 2.f, (float)hardMaxRadius);

            // Determine center chunk from view center in grid coords
            sf::Vector2f ijC = unproj(vc);
            int Icenter = (int)std::floor(ijC.x + 0.5f);
            int Jcenter = (int)std::floor(ijC.y + 0.5f);
            int ccx = floorDiv(Icenter, cfg::CHUNK_SIZE);
            int ccy = floorDiv(Jcenter, cfg::CHUNK_SIZE);

            for (int cx = cx0; cx <= cx1; ++cx) {
                for (int cy = cy0; cy <= cy1; ++cy) {
                    // Skip chunks outside LOD radius (Chebyshev distance for square ring)
                    int dx = std::abs(cx - ccx);
                    int dy = std::abs(cy - ccy);
                    if (std::max(dx, dy) > allowedRadius) continue;
                    const Chunk& ch = chunkMgr.getChunk(cx, cy);
                    int I0 = cx * cfg::CHUNK_SIZE;
                    int J0 = cy * cfg::CHUNK_SIZE;
                    // If water-only: base is flat sea (0). Show only user edits (overrides) above sea.
                    if (waterOnly) {
                        static std::vector<int> waterBuf;
                        const int side1 = (cfg::CHUNK_SIZE + 1);
                        waterBuf.resize(side1 * side1);
                        for (size_t k = 0; k < waterBuf.size(); ++k) {
                            int v = 0;
                            // Only show explicit edits; ignore generated terrain
                            if (k < ch.overrideMask.size() && ch.overrideMask[k]) v = ch.overrides[k];
                            // Keep full range so digging (<0) is visible as deeper water
                            waterBuf[k] = std::clamp(v, cfg::MIN_ELEV, cfg::MAX_ELEV);
                        }
                        auto cMap2d = render::buildProjectedMapChunk(waterBuf, cfg::CHUNK_SIZE, I0, J0, iso, origin, 1.0f);
                        render::draw2DFilledCellsChunk(window, cMap2d, waterBuf, cfg::CHUNK_SIZE, shadowsEnabled, 1.0f, I0, J0, &paintedCells,
                                                       (showColorHover && currentTool == Tool::Brush) ? &hoverMask : nullptr,
                                                       (showColorHover && currentTool == Tool::Brush) ? &activeColor : nullptr);
                        if (showGrid) render::draw2DMapChunk(window, cMap2d);
                    } else {
                        auto cMap2d = render::buildProjectedMapChunk(ch.heights, cfg::CHUNK_SIZE, I0, J0, iso, origin, 1.0f);
                        render::draw2DFilledCellsChunk(window, cMap2d, ch.heights, cfg::CHUNK_SIZE, shadowsEnabled, 1.0f, I0, J0, &paintedCells,
                                                       (showColorHover && currentTool == Tool::Brush) ? &hoverMask : nullptr,
                                                       (showColorHover && currentTool == Tool::Brush) ? &activeColor : nullptr);
                        if (showGrid) render::draw2DMapChunk(window, cMap2d);
                    }
                }
            }
        } else {
            // Non-procedural path: use global heights buffer
            auto map2d = render::buildProjectedMap(heights, iso, origin, 1.0f);
            render::draw2DFilledCells(window, map2d, heights, shadowsEnabled, 1.0f, &paintedCells, 
                                      (showColorHover && currentTool == Tool::Brush) ? &hoverMask : nullptr,
                                      (showColorHover && currentTool == Tool::Brush) ? &activeColor : nullptr);
            if (showGrid) render::draw2DMap(window, map2d);
        }


        // Draw UI in screen space (default view)
        sf::View oldView = window.getView();
        window.setView(window.getDefaultView());
        // Update visual style based on hover
        if (genHover) {
            btnGenerate.setFillColor(sf::Color(50, 50, 50, 230));
        } else {
            btnGenerate.setFillColor(sf::Color(30, 30, 30, 200));
        }
        window.draw(btnGenerate);
        // Subtle overlay to emphasize hover
        if (genHover) {
            sf::RectangleShape hoverOverlay(btnGenerate.getSize());
            hoverOverlay.setPosition(btnGenerate.getPosition());
            hoverOverlay.setFillColor(sf::Color(255, 255, 255, 20));
            window.draw(hoverOverlay);
        }
        window.draw(btnText);
        // Draw Grid toggle
        // Grid button
        if (gridHover) {
            btnGrid.setFillColor(sf::Color(50, 50, 50, 230));
        } else {
            btnGrid.setFillColor(sf::Color(30, 30, 30, 200));
        }
        window.draw(btnGrid);
        if (gridHover) {
            sf::RectangleShape hoverOverlay2(btnGrid.getSize());
            hoverOverlay2.setPosition(btnGrid.getPosition());
            hoverOverlay2.setFillColor(sf::Color(255, 255, 255, 28));
            window.draw(hoverOverlay2);
        }
        if (fontLoaded) window.draw(btnGridText);

        // Continents toggle
        if (continentsHover) {
            btnContinents.setFillColor(sf::Color(50, 50, 50, 230));
        } else {
            btnContinents.setFillColor(sf::Color(30, 30, 30, 200));
        }
        window.draw(btnContinents);
        if (continentsHover) {
            sf::RectangleShape hov(btnContinents.getSize());
            hov.setPosition(btnContinents.getPosition());
            hov.setFillColor(sf::Color(255,255,255,20));
            window.draw(hov);
        }
        if (fontLoaded) window.draw(btnContinentsText);

        // RESET button
        if (resetHover) {
            btnReset.setFillColor(sf::Color(50, 50, 50, 230));
        } else {
            btnReset.setFillColor(sf::Color(30, 30, 30, 200));
        }
        window.draw(btnReset);
        if (resetHover) {
            sf::RectangleShape hov(btnReset.getSize());
            hov.setPosition(btnReset.getPosition());
            hov.setFillColor(sf::Color(255,255,255,20));
            window.draw(hov);
        }
        if (fontLoaded) window.draw(btnResetText);

        // Reseed
        window.draw(btnReseed);
        if (fontLoaded) window.draw(btnReseedText);
        // Seed box
        window.draw(seedBox);
        if (fontLoaded) window.draw(seedText);
        // Bake
        if (bakeHover) {
            btnBake.setFillColor(sf::Color(50, 50, 50, 230));
        } else {
            btnBake.setFillColor(sf::Color(30, 30, 30, 200));
        }
        window.draw(btnBake);
        if (bakeHover) {
            sf::RectangleShape hov(btnBake.getSize());
            hov.setPosition(btnBake.getPosition());
            hov.setFillColor(sf::Color(255,255,255,20));
            window.draw(hov);
        }
        if (fontLoaded) window.draw(btnBakeText);

        // One window size fetch per frame for UI positions
        auto wsz = window.getSize();

        // Help text bottom-left
        if (fontLoaded) {
            float baseY = (float)wsz.y - 24.f;
            helpF11.setPosition(16.f, baseY - 20.f);
            helpCtrl.setPosition(16.f, baseY);
            window.draw(helpF11);
            if (currentTool == Tool::Bulldozer) {
                window.draw(helpCtrl);
            }
        }

        // Import/Export buttons rendering (top-right)
        auto drawRoundButton = [&](sf::Vector2f center, const sf::Sprite& icon, bool hover){
            sf::CircleShape c(btnRadius);
            c.setOrigin(btnRadius, btnRadius);
            c.setPosition(center);
            c.setFillColor(sf::Color::White);
            c.setOutlineThickness(1.f);
            c.setOutlineColor(sf::Color(200,200,200));
            window.draw(c);
            if (icon.getTexture()) {
                sf::Sprite s(icon);
                sf::FloatRect b = s.getLocalBounds();
                s.setOrigin(b.width*0.5f, b.height*0.5f);
                s.setPosition(center);
                window.draw(s);
            }
            if (hover) {
                sf::CircleShape h(btnRadius);
                h.setOrigin(btnRadius, btnRadius);
                h.setPosition(center);
                h.setFillColor(sf::Color(0,0,0,25));
                window.draw(h);
            }
        };
        sf::Vector2i mp = sf::Mouse::getPosition(window);
        sf::Vector2f screen = window.mapPixelToCoords(mp, window.getDefaultView());
        bool hoverExport = circleContains(exportBtnPos, btnRadius, screen);
        bool hoverImport = circleContains(importBtnPos, btnRadius, screen);
        // Update hovers for left buttons
        genHover = btnGenerate.getGlobalBounds().contains(screen);
        gridHover = btnGrid.getGlobalBounds().contains(screen);
        continentsHover = btnContinents.getGlobalBounds().contains(screen);
        resetHover = btnReset.getGlobalBounds().contains(screen);
        bakeHover = btnBake.getGlobalBounds().contains(screen);
        drawRoundButton(importBtnPos, sprImport, hoverImport);
        drawRoundButton(exportBtnPos, sprExport, hoverExport);

        // Import progress bar (bottom-right)
        if (importing) {
            float w = 220.f, h = 12.f;
            float x = (float)wsz.x - 16.f - w;
            float y = (float)wsz.y - 16.f - h;
            sf::RectangleShape bg(sf::Vector2f(w, h));
            bg.setPosition(x, y);
            bg.setFillColor(sf::Color(255,255,255,40));
            bg.setOutlineThickness(1.f);
            bg.setOutlineColor(sf::Color(200,200,200));
            window.draw(bg);
            sf::RectangleShape fg(sf::Vector2f(w * std::clamp(importProgress, 0.f, 1.f), h));
            fg.setPosition(x, y);
            fg.setFillColor(sf::Color(100, 180, 255, 200));
            window.draw(fg);
        }

        // FPS counter (bottom-right)
        if (fontLoaded) {
            fpsFrames++;
            float elapsed = fpsClock.getElapsedTime().asSeconds();
            if (elapsed >= 0.25f) {
                fpsValue = fpsFrames / elapsed;
                fpsFrames = 0;
                fpsClock.restart();
                fpsText.setString("FPS: " + std::to_string((int)std::round(fpsValue)));
            }
            auto tb = fpsText.getLocalBounds();
            float fx = (float)wsz.x - 16.f - tb.width;
            float fy = (float)wsz.y - 16.f - tb.height;
            fpsText.setPosition(fx, fy);
            window.draw(fpsText);
        }

        // (height slider removed)

        // Inventory toolbar (bottom-center) rendering
        {
            auto [rBulldozer, rBrush, rEraser] = inventoryRects();
            // Background bar spans all three
            float pad = 8.f;
            float left = std::min(rBulldozer.left, std::min(rBrush.left, rEraser.left));
            float right = std::max(rBulldozer.left + rBulldozer.width, std::max(rBrush.left + rBrush.width, rEraser.left + rEraser.width));
            float top = std::min(rBulldozer.top, std::min(rBrush.top, rEraser.top));
            float height = rBulldozer.height;
            sf::FloatRect barRect(left - pad, top - pad, (right - left) + pad*2.f, height + pad*2.f);
            sf::RectangleShape bar(sf::Vector2f(barRect.width, barRect.height));
            bar.setPosition(barRect.left, barRect.top);
            bar.setFillColor(sf::Color(20,20,20,200));
            bar.setOutlineThickness(1.f);
            bar.setOutlineColor(sf::Color(200,200,200));
            window.draw(bar);

            // Draw slots
            auto drawSlot = [&](const sf::FloatRect& r, const sf::Sprite& icon, bool selected){
                sf::RectangleShape box(sf::Vector2f(r.width, r.height));
                box.setPosition(r.left, r.top);
                box.setFillColor(sf::Color(40,40,40,220));
                box.setOutlineThickness(selected ? 2.f : 1.f);
                box.setOutlineColor(selected ? sf::Color(100,180,255) : sf::Color(150,150,150));
                window.draw(box);
                if (icon.getTexture()) {
                    sf::Sprite s(icon);
                    sf::FloatRect lb = s.getLocalBounds();
                    s.setOrigin(lb.left + lb.width*0.5f, lb.top + lb.height*0.5f);
                    s.setPosition(r.left + r.width*0.5f, r.top + r.height*0.5f);
                    window.draw(s);
                }
            };
            drawSlot(rBulldozer, sprBulldozer, currentTool == Tool::Bulldozer);
            drawSlot(rBrush,      sprBrush,      currentTool == Tool::Brush);
            drawSlot(rEraser,     sprEraser,     currentTool == Tool::Eraser);

            // Update hover flag for brush color hover each frame
            showColorHover = (currentTool == Tool::Brush);
        }

        // Brush slider (right side)
        sf::FloatRect tr = sliderTrackRect();
        sf::RectangleShape track(sf::Vector2f(tr.width, tr.height));
        track.setPosition({tr.left, tr.top});
        track.setFillColor(sf::Color(80, 80, 80, 200));
        track.setOutlineThickness(1.f);
        track.setOutlineColor(sf::Color(200, 200, 200));
        window.draw(track);

        sf::FloatRect th = sliderThumbRect(brushSize);
        sf::RectangleShape thumb(sf::Vector2f(th.width, th.height));
        thumb.setPosition({th.left, th.top});
        thumb.setFillColor(sf::Color(200, 200, 200, brushDragging ? 255 : 230));
        thumb.setOutlineThickness(1.f);
        thumb.setOutlineColor(sf::Color::Black);
        window.draw(thumb);

        if (fontLoaded) {
            sf::Text label;
            label.setFont(uiFont);
            label.setCharacterSize(16);
            label.setFillColor(sf::Color::White);
            label.setString(U8("Brush"));
            label.setPosition(tr.left - 6.f, tr.top - 24.f);
            window.draw(label);

            sf::Text val;
            val.setFont(uiFont);
            val.setCharacterSize(16);
            val.setFillColor(sf::Color::White);
            val.setString(std::to_string(brushSize));
            val.setPosition(tr.left - 6.f, tr.top + tr.height + 6.f);
            window.draw(val);
        }
        // Draw color picker and history (screen space) only for Brush tool
        if (currentTool == Tool::Brush) {
            ensureColorWheel();
            float leftX = 16.f;
            float panelW = 140.f;
            float wheelTop = btnBake.getPosition().y + btnBake.getSize().y + 16.f;
            sf::Vector2f wheelCenter(leftX + panelW*0.5f, wheelTop + (float)colorWheelRadius);
            // Background panel
            sf::RectangleShape cpBG(sf::Vector2f(panelW, colorWheelRadius*2.f + 56.f + 34.f));
            cpBG.setPosition(leftX, wheelTop);
            cpBG.setFillColor(sf::Color(30,30,30,200));
            cpBG.setOutlineThickness(1.f);
            cpBG.setOutlineColor(sf::Color(200,200,200));
            window.draw(cpBG);
            // Wheel sprite
            colorWheelSpr.setPosition(wheelCenter.x - colorWheelRadius, wheelCenter.y - colorWheelRadius);
            window.draw(colorWheelSpr);
            // Selected color indicator at center
            sf::CircleShape dot(6.f);
            dot.setOrigin(6.f,6.f);
            dot.setPosition(wheelCenter);
            // Display activeColor (after tone) as the selected indicator
            dot.setFillColor(activeColor);
            dot.setOutlineThickness(2.f);
            dot.setOutlineColor(sf::Color::Black);
            window.draw(dot);
            // History swatches
            const int N = 5; float sw = 22.f, sh = 22.f, gap = 6.f;
            float totalW = N*sw + (N-1)*gap;
            float hx = leftX + (panelW - totalW) * 0.5f;
            float hy = wheelTop + colorWheelRadius*2.f + 12.f;
            for (int i=0; i<N; ++i) {
                sf::RectangleShape sq(sf::Vector2f(sw, sh));
                sq.setPosition(hx + i*(sw+gap), hy);
                if (i < (int)colorHistory.size()) sq.setFillColor(colorHistory[i]);
                else sq.setFillColor(sf::Color(80,80,80));
                sq.setOutlineThickness(1.f);
                sq.setOutlineColor(sf::Color::Black);
                window.draw(sq);
            }
            // Tone slider
            if (!toneTexReady) rebuildToneTex();
            const float toneH = 18.f; const float tonePad = 12.f;
            float toneY = hy + sh + tonePad;
            // Border/background
            sf::RectangleShape toneBG(sf::Vector2f(panelW, toneH));
            toneBG.setPosition(leftX, toneY);
            toneBG.setFillColor(sf::Color(50,50,50,220));
            toneBG.setOutlineThickness(1.f);
            toneBG.setOutlineColor(sf::Color(200,200,200));
            window.draw(toneBG);
            // Gradient sprite scaled to fit
            toneSpr.setPosition(leftX, toneY + 0.5f);
            toneSpr.setScale(panelW / (float)toneTex.getSize().x, (toneH-1.f) / std::max(1.f, (float)toneTex.getSize().y));
            window.draw(toneSpr);
            // Handle marker
            float handleX = leftX + colorToneT * panelW;
            sf::RectangleShape handle(sf::Vector2f(2.f, toneH));
            handle.setPosition(handleX - 1.f, toneY);
            handle.setFillColor(sf::Color::White);
            window.draw(handle);
        }
        window.setView(oldView);

        // No separate overlay: hover highlighting is applied directly in renderers via hoverMask

        window.display();
    }

    if (__log) __log << "[" << __now() << "] main loop ended, exiting cleanly" << std::endl;
    } catch (const std::exception& ex) {
        if (__log) { __log << "[" << __now() << "] exception: " << ex.what() << std::endl; }
        else { std::ofstream f("log.txt", std::ios::app); if (f) f << "[exception] " << ex.what() << std::endl; }
    } catch (...) {
        if (__log) { __log << "[" << __now() << "] unknown exception" << std::endl; }
        else { std::ofstream f("log.txt", std::ios::app); if (f) f << "[unknown exception]" << std::endl; }
    }

    return 0;
}
