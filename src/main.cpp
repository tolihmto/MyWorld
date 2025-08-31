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
#include <random>
#include <tuple>
#include <filesystem>
#include <cstdio>
#include <iostream>
#include <chrono>
#include <ctime>
#include <cstring>
#include <functional>
#include <cctype>
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
#include "zip.hpp"

// Small string trim helper for Unix shell outputs
static inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

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

    // ZIP open dialog (.zip)
    auto openFileDialogZIP = [&]()->std::string{
#ifdef _WIN32
        wchar_t fileName[MAX_PATH] = L"";
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = L"ZIP Archives\0*.zip\0All Files\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        ofn.lpstrDefExt = L"zip";
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
            std::string r = runAndRead("kdialog --getopenfilename . '*.zip' 2>/dev/null");
            if (!r.empty()) return r;
        }
        // 2) zenity (GNOME/others)
        {
            std::string r = runAndRead("zenity --file-selection --file-filter='*.zip' 2>/dev/null");
            if (!r.empty()) return r;
        }
        // 3) yad
        {
            std::string r = runAndRead("yad --file-selection --file-filter='*.zip' 2>/dev/null");
            if (!r.empty()) return r;
        }
        // 4) qarma
        {
            std::string r = runAndRead("qarma --file-selection --file-filter='*.zip' 2>/dev/null");
            if (!r.empty()) return r;
        }
        std::cerr << "[Import ZIP] Aucun explorateur de fichiers détecté (kdialog/zenity/yad/qarma).\n"
                     "Installez 'zenity' pour une compatibilité à 100% (ex: sudo apt install -y zenity)." << std::endl;
        return {};
#endif
    };

    // Forwardable UI status function (assigned later once UI is initialized)
    std::function<void(const std::string&, float)> showStatusFn;
    // Forward declare importer; full definition is placed after UI/status/data are initialized
    std::function<void(const std::string&)> importWorldZip;
    // Terrain base color mapping (heights in unscaled units: same thresholds used in renderers before scaling)
    auto colorForHeightPicker = [&](float h){
        const sf::Color normalBlue(30, 144, 255);
        const sf::Color veryDarkBlue(0, 0, 80);
        const sf::Color sandYellow(255, 236, 170);
        const sf::Color grass(34, 139, 34);
        const sf::Color gray(128, 128, 128);
        const sf::Color rock(110, 110, 110);
        const sf::Color snow(245, 245, 245);
        auto lerp = [&](sf::Color a, sf::Color b, float t){
            t = std::clamp(t, 0.f, 1.f);
            auto L = [](uint8_t c){ return (int)c; };
            return sf::Color(
                (uint8_t)std::round(L(a.r) + (L(b.r) - L(a.r)) * t),
                (uint8_t)std::round(L(a.g) + (L(b.g) - L(a.g)) * t),
                (uint8_t)std::round(L(a.b) + (L(b.b) - L(a.b)) * t),
                255);
        };
        float sea0=0.f, deepMin=-100.f, coast2=2.f, beach4=4.f, grass6=6.f, gray10=10.f, rock14=12.f, snow16=14.f;
        if (h < sea0 && h >= deepMin) { float t=(sea0-h)/std::max(0.001f,(sea0-deepMin)); return lerp(normalBlue, veryDarkBlue, t); }
        if (h >= sea0 && h < coast2) { float t=(h-sea0)/std::max(0.001f,(coast2-sea0)); return lerp(normalBlue, sandYellow, t); }
        if (h >= coast2 && h < beach4) { float t=(h-coast2)/std::max(0.001f,(beach4-coast2)); return lerp(sandYellow, grass, t); }
        if (h >= beach4 && h < grass6) return grass;
        if (h >= grass6 && h <= gray10) { float t=(h-grass6)/std::max(0.001f,(gray10-grass6)); return lerp(grass, gray, t); }
        if (h > gray10 && h <= rock14) return rock;
        if (h > rock14 && h < snow16) { float t=(h-rock14)/std::max(0.001f,(snow16-rock14)); return lerp(gray, snow, t); }
        if (h >= snow16) return snow;
        return grass;
    };
    auto alphaOverPicker = [&](sf::Color base, sf::Color paint){
        float a = paint.a / 255.f;
        auto mix = [&](uint8_t cb, uint8_t cp){ return (uint8_t)std::round(cb * (1.f - a) + cp * a); };
        return sf::Color(mix(base.r, paint.r), mix(base.g, paint.g), mix(base.b, paint.b), 255);
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

    // Center the camera on world origin (0,0) to avoid tying to a fixed grid size
    sf::Vector2f worldCenter0 = isoProjectDyn(0.f, 0.f, 0.f, iso);
    view.setCenter(worldCenter0 + origin);
    window.setView(view);

    // Elevation grid (non-proc mode): dynamic size (gridSide+1)^2; default to cfg::GRID
    int gridSide = cfg::GRID;
    std::vector<int> heights((gridSide + 1) * (gridSide + 1), 0);
    auto idx = [&](int i, int j) { return i * (gridSide + 1) + j; };

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
    sf::Text helpWASD;
    sf::Text helpCtrl;
    sf::Text helpMarkerEdit;
    sf::Text fpsText;
    sf::Text zoomText; // debug zoom overlay
    // Status/progress overlay state
    sf::Text statusText;
    sf::Clock statusClock;
    float statusDuration = 3.0f; // seconds to fully hide
    float statusFadeTail = 0.6f; // last seconds used for fade-out
    std::string statusMsg;
    // Per-frame zoom debug variables (persist across frames; updated every frame)
    float dbg_edgePxAtCenter = 0.f;
    int   dbg_lodStride = 1;
    int   dbg_worldStride = 1;
    int   dbg_coarseStride = 0;
    int   dbg_coarseQuads = 0;
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
        helpCtrl.setString(U8("Ctrl + clic: aplatir (picker hauteur)"));

        helpMarkerEdit.setFont(uiFont);
        helpMarkerEdit.setCharacterSize(14);
        helpMarkerEdit.setFillColor(sf::Color(220, 220, 220));
        helpMarkerEdit.setString(U8("Entrée : valider le libellé\nSuppr : supprime le marqueur\nEsc : annule l'édition"));

        // Init FPS text
        fpsText.setFont(uiFont);
        fpsText.setCharacterSize(14);
        fpsText.setFillColor(sf::Color(200, 255, 200));
        fpsText.setString("FPS: --");
        // Init Zoom debug text
        zoomText.setFont(uiFont);
        zoomText.setCharacterSize(13);
        zoomText.setFillColor(sf::Color(200, 220, 255));
        zoomText.setString("zoom: --");
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

        // Status overlay text
        statusText.setFont(uiFont);
        statusText.setCharacterSize(16);
        statusText.setFillColor(sf::Color::White);
        statusText.setOutlineColor(sf::Color::Black);
        statusText.setOutlineThickness(1.f);
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
    enum class Tool { Bulldozer, Brush, Eraser, Locator, Pipette };
    Tool currentTool = Tool::Bulldozer;
    // Brush shapes (for painting tool)
    enum class BrushShape { Square, Circle, Manhattan, Gaussian };
    BrushShape currentBrushShape = BrushShape::Square;
    // Brush hardness (0..1). 1 => solid fill; 0 => point; mid => stochastic falloff
    float brushHardness = 1.0f;
    bool  hardnessDragging = false;
    // Inventory UI assets
    sf::Texture texBrush, texBulldozer, texEraser, texLocator, texPipette;
    sf::Sprite  sprBrush, sprBulldozer, sprEraser, sprLocator, sprPipette;
    bool brushIconLoaded = texBrush.loadFromFile("assets/images/pinceau.png");
    bool bulldozerIconLoaded = texBulldozer.loadFromFile("assets/images/pelle-excavatrice.png");
    bool eraserIconLoaded = texEraser.loadFromFile("assets/images/gomme.png");
    bool locatorIconLoaded = texLocator.loadFromFile("assets/images/localisateur.png");
    bool pipetteIconLoaded = texPipette.loadFromFile("assets/images/pipette.png");
    if (brushIconLoaded)  { sprBrush.setTexture(texBrush);      auto ts=texBrush.getSize();      if (ts.x>0&&ts.y>0) sprBrush.setScale(32.f/(float)ts.x, 32.f/(float)ts.y); }
    if (bulldozerIconLoaded){ sprBulldozer.setTexture(texBulldozer);auto ts=texBulldozer.getSize();if (ts.x>0&&ts.y>0) sprBulldozer.setScale(32.f/(float)ts.x, 32.f/(float)ts.y); }
    if (eraserIconLoaded){ sprEraser.setTexture(texEraser);    auto ts=texEraser.getSize();    if (ts.x>0&&ts.y>0) sprEraser.setScale(32.f/(float)ts.x, 32.f/(float)ts.y); }
    if (locatorIconLoaded){ sprLocator.setTexture(texLocator);  auto ts=texLocator.getSize();    if (ts.x>0&&ts.y>0) sprLocator.setScale(32.f/(float)ts.x, 32.f/(float)ts.y); }
    if (pipetteIconLoaded){ sprPipette.setTexture(texPipette);  auto ts=texPipette.getSize();    if (ts.x>0&&ts.y>0) sprPipette.setScale(32.f/(float)ts.x, 32.f/(float)ts.y); }
    auto inventoryRects = [&](){
        // Return five 32x32 rects centered at bottom
        auto sz = window.getSize(); float w=(float)sz.x, h=(float)sz.y;
        float box = 32.f; float gap = 16.f; float total = box*5.f + gap*4.f;
        float x0 = w*0.5f - total*0.5f; float y = h - 20.f - box; // 20px margin from bottom
        sf::FloatRect rBulldozer(x0, y, box, box);
        sf::FloatRect rBrush(x0 + (box + gap)*1.f, y, box, box);
        sf::FloatRect rEraser(x0 + (box + gap)*2.f, y, box, box);
        sf::FloatRect rLocator(x0 + (box + gap)*3.f, y, box, box);
        sf::FloatRect rPipette(x0 + (box + gap)*4.f, y, box, box);
        return std::tuple<sf::FloatRect,sf::FloatRect,sf::FloatRect,sf::FloatRect,sf::FloatRect>(rBulldozer, rBrush, rEraser, rLocator, rPipette);
    };

    // Locator markers and label editing state
    struct Marker { int I; int J; std::string label; sf::Color color; std::string icon; };
    std::vector<Marker> markers;
    bool labelEditing = false;
    int  labelEditIndex = -1;
    std::string labelBuffer;
    // Current selected marker color (used as default for new markers; used to set color when editing if wheel clicked)
    sf::Color currentMarkerColor = sf::Color::White;
    // Current selected default icon for new markers (filename only)
    std::string currentMarkerIcon;
    // Marker icon catalog (loaded from assets/images/marqueurs)
    struct IconItem { std::string name; sf::Texture tex; };
    std::vector<IconItem> markerIcons;
    float markerIconsScroll = 0.f; // vertical scroll in pixels within viewport
    // Warn once per missing icon filename when rendering markers
    std::unordered_set<std::string> missingMarkerIconWarned;

    auto hsvToRgb = [](float h)->sf::Color{
        // h in [0,360), s=1, v=1
        float s = 1.f, v = 1.f;
        float c = v * s;
        float hh = fmodf(h, 360.f) / 60.f;
        float x = c * (1.f - fabsf(fmodf(hh, 2.f) - 1.f));
        float r=0,g=0,b=0;
        int i = (int)hh;
        switch(i){
            case 0: r=c; g=x; b=0; break;
            case 1: r=x; g=c; b=0; break;
            case 2: r=0; g=c; b=x; break;
            case 3: r=0; g=x; b=c; break;
            case 4: r=x; g=0; b=c; break;
            default: r=c; g=0; b=x; break;
        }
        float m = v - c;
        r += m; g += m; b += m;
        auto to8 = [](float v){ return (sf::Uint8)std::clamp(int(v*255.f + 0.5f), 0, 255); };
        return sf::Color(to8(r), to8(g), to8(b), 230);
    };

    // Painting overlay: per world cell (I,J) -> color
    struct IJKey { int I; int J; };
    auto key64 = [](int I, int J)->long long { return ( (long long)I << 32) ^ (unsigned long long)(uint32_t)J; };
    std::unordered_map<long long, sf::Color> paintedCells;
    bool showColorHover = false;

    // Color picker (left, below buttons)
    sf::Color selectedColor = sf::Color::White;
    std::vector<sf::Color> colorHistory; // newest first, max 5
    const int colorWheelRadius = 52; // slightly smaller to reduce Brush/Pipette picker size
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

    // Brush shape test helpers
    auto inSquareChebyshev = [&](int di, int dj, int half)->bool {
        return std::max(std::abs(di), std::abs(dj)) <= half;
    };
    auto inCircleEuclid = [&](int di, int dj, int half)->bool {
        return (di*di + dj*dj) <= (half*half);
    };
    auto inManhattan = [&](int di, int dj, int half)->bool {
        return (std::abs(di) + std::abs(dj)) <= half;
    };
    // Deterministic hardness weight (0..1) instead of stochastic accept
    auto smoothstep = [](float a, float b, float x){
        float t = std::clamp((x - a) / std::max(1e-6f, (b - a)), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    };
    auto brushContains = [&](BrushShape shape, int di, int dj, int half, int ci, int cj)->bool {
        switch (shape) {
            case BrushShape::Square:    return inSquareChebyshev(di,dj,half);
            case BrushShape::Circle:    return inCircleEuclid(di,dj,half);
            case BrushShape::Manhattan: return inManhattan(di,dj,half);
            case BrushShape::Gaussian:  return inCircleEuclid(di,dj,half); // containment only; weight handled below
        }
        return inSquareChebyshev(di,dj,half);
    };
    auto metricDist = [&](BrushShape shape, int di, int dj)->float {
        switch (shape) {
            case BrushShape::Square:    return (float)std::max(std::abs(di), std::abs(dj));
            case BrushShape::Circle:    return std::sqrt((float)(di*di + dj*dj));
            case BrushShape::Manhattan: return (float)(std::abs(di) + std::abs(dj));
            case BrushShape::Gaussian:  return std::sqrt((float)(di*di + dj*dj));
        }
        return (float)std::max(std::abs(di), std::abs(dj));
    };
    auto weightForHardness = [&](BrushShape shape, int di, int dj, int half)->float {
        if (half <= 0) return (di == 0 && dj == 0) ? 1.f : 0.f;
        if (!brushContains(shape, di, dj, half, 0, 0)) return 0.f;
        float H = std::clamp(brushHardness, 0.f, 1.f);
        if (H >= 0.999f) return 1.f;
        if (H <= 0.001f) return (di == 0 && dj == 0) ? 1.f : 0.f;
        float d = metricDist(shape, di, dj);
        float halfF = (float)half;
        if (shape == BrushShape::Gaussian) {
            // Gaussian radial profile; map hardness to sigma (smaller sigma when harder)
            float sigmaMin = std::max(0.5f, halfF * 0.2f);
            float sigmaMax = std::max(1.0f, halfF * 0.8f);
            float sigma = sigmaMax - (sigmaMax - sigmaMin) * H;
            float r2 = d * d;
            return std::exp(-r2 / std::max(1e-4f, 2.f * sigma * sigma));
        }
        float rHard = halfF * H; // solid core
        if (d <= rHard) return 1.f;
        if (d >= halfF) return 0.f;
        // Smooth falloff from rHard..halfF
        float t = (halfF - d) / std::max(1e-4f, (halfF - rHard)); // 1 at rHard -> 0 at edge
        return smoothstep(0.f, 1.f, t);
    };
    auto makePaintColor = [&](float w)->sf::Color {
        w = std::clamp(w, 0.f, 1.f);
        sf::Uint8 a = (sf::Uint8)std::round(w * 255.f);
        return sf::Color(activeColor.r, activeColor.g, activeColor.b, a);
    };
    // 2D hash -> float in [0,1): mixes (i,j,stamp,seed) to avoid grid-aligned artifacts
    auto hash32 = [](uint32_t x){
        x += 0x6D2B79F5u;
        x = (x ^ (x >> 15)) * (x | 1u);
        x ^= x + (x ^ (x >> 7)) * (x | 61u);
        return (x ^ (x >> 14)) * 0x27d4eb2du;
    };
    auto hash2D01 = [&](int i, int j, uint32_t stamp, uint32_t seed){
        uint32_t a = (uint32_t)i * 0x9E3779B1u;
        uint32_t b = (uint32_t)j * 0x85EBCA6Bu;
        uint32_t c = stamp * 0xC2B2AE35u;
        uint32_t d = seed  * 0x27D4EB2Du;
        uint32_t h = a ^ (b + 0x9E3779B9u + (a<<6) + (a>>2));
        h ^= c + 0x85EBCA77u + (h<<6) + (h>>2);
        h ^= d + 0xC2B2AE3Du + (h<<6) + (h>>2);
        h = hash32(h);
        return (h & 0x00FFFFFFu) / 16777216.f;
    };

    uint32_t strokeSeed = 123456789u;
    uint32_t stampIndex = 0u; // increments every stamp to vary point pattern within a stroke

    // Stamp brush at grid cell (i0,j0), compositing over existing paint
    auto stampBrushAt = [&](int i0, int j0){
        int brush = std::clamp(brushSize, brushMin, brushMax);
        int half = brush - 1;
        // Small-brush alpha sensitivity: reduce per-stamp contribution when brush is tiny
        float r = std::max(1, half);
        float areaApprox = 3.14159f * r * r; // ~pi r^2
        float refArea = 200.f; // reference area where scale ~1
        float covScale = std::clamp(areaApprox / refArea, 0.05f, 1.0f);
        float H = std::clamp(brushHardness, 0.f, 1.f);
        if (H >= 0.999f) covScale = 1.0f; // full intensity: no alpha reduction even for small brush
        for (int di = -brush; di <= brush; ++di) {
            for (int dj = -brush; dj <= brush; ++dj) {
                int ci = i0 + di; int cj = j0 + dj;
                float w = weightForHardness(currentBrushShape, di, dj, half);
                if (w <= 0.f && currentBrushShape != BrushShape::Gaussian) continue;
                long long k = key64(ci, cj);
                // Point-cloud behavior for Gaussian: stochastic acceptance by weight
                bool accept = true;
                if (currentBrushShape == BrushShape::Gaussian) {
                    // Density = radial falloff * hardness
                    float d = std::sqrt((float)(di*di + dj*dj));
                    float halfF = std::max(1.f, (float)half);
                    float t = std::clamp(1.f - d / halfF, 0.f, 1.f);
                    float wRad = t * t * (3.f - 2.f * t); // smoothstep
                    float density = std::clamp(wRad * H, 0.f, 1.f);
                    // Base random for this cell
                    float rndSelf = hash2D01(ci, cj, stampIndex, strokeSeed);
                    // Blue-noise thinning: only keep if better than neighbors in a small window
                    int R = (H < 0.5f ? 1 : 2); // more spacing at higher hardness
                    bool isWinner = true;
                    if (density > 0.f) {
                        for (int oy = -R; oy <= R && isWinner; ++oy) {
                            for (int ox = -R; ox <= R; ++ox) {
                                if (ox == 0 && oy == 0) continue;
                                // Only compare a subset to limit cost (chessboard pattern)
                                if (((ox + oy) & 1) != 0) continue;
                                float rn = hash2D01(ci + ox, cj + oy, stampIndex, strokeSeed);
                                if (rn < rndSelf) { isWinner = false; break; }
                            }
                        }
                    }
                    accept = (rndSelf < density) && isWinner;
                }
                if (!accept) continue;
                sf::Color newC;
                if (currentBrushShape == BrushShape::Gaussian) {
                    // Constant per-dot opacity, independent of brush size; intensity affects density only
                    newC = sf::Color(activeColor.r, activeColor.g, activeColor.b, 255);
                } else {
                    newC = makePaintColor(w * covScale); // rgb = brush color, a = local coverage contribution
                }
                auto it = paintedCells.find(k);
                if (it == paintedCells.end()) {
                    // First time: set target color and initial coverage
                    paintedCells[k] = newC;
                } else {
                    // Accumulate coverage using union: cov' = 1 - (1-cov)*(1-w)
                    float covOld = it->second.a / 255.f;
                    float wEff = (newC.a / 255.f);
                    float covNew = 1.f - (1.f - covOld) * (1.f - wEff);
                    covNew = std::clamp(covNew, 0.f, 1.f);
                    // Incremental share that this stamp adds relative to the new total coverage
                    float add = covNew - covOld;
                    float t = (covNew > 1e-6f) ? (add / covNew) : 0.f;
                    // Allow recoloring even when alpha is saturated (no coverage increase)
                    float tMin = 0.30f * wEff; // stronger color blend to repaint over old strokes
                    t = std::max(t, tMin);
                    auto lerp8 = [&](sf::Uint8 a, sf::Uint8 b, float t){ return (sf::Uint8)std::round(a * (1.f - t) + b * t); };
                    sf::Color cur = it->second;
                    cur.r = lerp8(cur.r, newC.r, t);
                    cur.g = lerp8(cur.g, newC.g, t);
                    cur.b = lerp8(cur.b, newC.b, t);
                    cur.a = (sf::Uint8)std::round(covNew * 255.f);
                    it->second = cur;
                }
            }
        }
        ++stampIndex; // advance pattern for next stamp
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
    auto loadMarkerIcons = [&](){
        if (!markerIcons.empty()) return;
        std::filesystem::path dir("assets/images/marqueurs");
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) return;
        for (auto& p : std::filesystem::directory_iterator(dir, ec)) {
            if (!p.is_regular_file()) continue;
            auto ext = p.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
                IconItem it; it.name = p.path().filename().string();
                it.tex.loadFromFile(p.path().string());
                markerIcons.push_back(std::move(it));
            }
        }
        std::sort(markerIcons.begin(), markerIcons.end(), [](const IconItem& a, const IconItem& b){ return a.name < b.name; });
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
    bool paintingActive = false;
    sf::Vector2i lastPaintIJ(0,0);
    // Bulldozer throttle: keep continuous edit on hold but slower, for precise steps
    sf::Clock bulldozeClock;
    sf::Time  bulldozeTick = sf::milliseconds(40); // ~25 Hz for elevation steps while dragging

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
    int importTotalRows = (gridSide + 1);
    float importProgress = 0.f;
    auto beginImport = [&](const std::string& path){
        if (importing) { if (importFile.is_open()) importFile.close(); importing=false; }
        importFile.open(path);
        if (importFile.is_open()) { importing = true; importRow = 0; importProgress = 0.f; }
    };

    auto exportCSV = [&](const std::string& path){
        std::ofstream out(path);
        if (!out) return false;
        for (int i = 0; i <= gridSide; ++i) {
            for (int j = 0; j <= gridSide; ++j) {
                out << heights[idx(i,j)];
                if (j < gridSide) out << ",";
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
    // ZIP save dialog (.zip)
    auto saveFileDialogZIP = [&]()->std::string{
#ifdef _WIN32
        wchar_t fileName[MAX_PATH] = L"world.zip";
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = L"ZIP Archives\0*.zip\0All Files\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = L"zip";
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
            std::string r = runAndRead("kdialog --getsavefilename ./ world.zip '*.zip' 2>/dev/null");
            if (!r.empty()) return r;
        }
        // 2) zenity
        {
            std::string r = runAndRead("zenity --file-selection --save --confirm-overwrite --file-filter='*.zip' 2>/dev/null");
            if (!r.empty()) { if (r.find('.') == std::string::npos) r += ".zip"; return r; }
        }
        // 3) yad
        {
            std::string r = runAndRead("yad --file-selection --save --confirm-overwrite --file-filter='*.zip' 2>/dev/null");
            if (!r.empty()) { if (r.find('.') == std::string::npos) r += ".zip"; return r; }
        }
        // 4) qarma
        {
            std::string r = runAndRead("qarma --file-selection --save --confirm-overwrite --file-filter='*.zip' 2>/dev/null");
            if (!r.empty()) { if (r.find('.') == std::string::npos) r += ".zip"; return r; }
        }
        return {};
#endif
    };

    // JSON helpers
    auto jsonEscape = [&](const std::string& s){
        std::string out; out.reserve(s.size()+8);
        for (char ch : s) {
            switch (ch) {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if ((unsigned char)ch < 0x20) { char buf[7]; std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)ch); out += buf; }
                    else out += ch;
            }
        }
        return out;
    };

    // Helper: show short status overlay message (accept UTF-8 and convert for SFML)
    auto showStatus = [&](const std::string& m, float dur = 3.0f){
        statusDuration = dur;
        statusMsg = m;
        statusText.setString(sf::String::fromUtf8(m.begin(), m.end()));
        statusClock.restart();
    };
    showStatusFn = showStatus;

    // Now that all state exists, define the ZIP importer
    importWorldZip = [&] (const std::string& zipPath){
        if (__log) __log << "[" << __now() << "] importWorldZip begin <- " << zipPath << std::endl;
        if (showStatusFn) showStatusFn("Import ZIP: préparation...", 6.0f);
        ZipReader zr(zipPath);
        if (!zr.ok()) {
            std::cerr << "[Import ZIP] Failed to open ZIP: " << zipPath << "\n";
            if (showStatusFn) showStatusFn("Import ZIP: échec d'ouverture", 3.0f);
            if (__log) __log << "[" << __now() << "] ZIP open failed" << std::endl; return; }
        // Start from a clean state to avoid leftovers from previous session
        paintedCells.clear();
        markers.clear();
        chunkMgr.resetOverrides();
        // Also reset any marker label editing session
        labelEditing = false;
        labelEditIndex = -1;
        labelBuffer.clear();
        // Read world.json (seed/continents)
        std::vector<uint8_t> buf;
        if (zr.readFile("world.json", buf)) {
            std::string s(buf.begin(), buf.end());
            auto findInt = [&](const char* key, int def)->int{
                size_t p = s.find(key); if (p==std::string::npos) return def;
                p = s.find_first_of("0123456789-", p); if (p==std::string::npos) return def;
                try { return std::stoi(s.substr(p)); } catch (...) { return def; }
            };
            auto findBool = [&](const char* key, bool def)->bool{
                size_t p = s.find(key); if (p==std::string::npos) return def;
                size_t t = s.find_first_not_of(" \t\r\n:", p + std::strlen(key));
                if (t==std::string::npos) return def;
                if (s.compare(t, 4, "true") == 0) return true;
                if (s.compare(t, 5, "false") == 0) return false;
                return def;
            };
            int newSeed = findInt("\"seed\"", proceduralSeed);
            bool newCont = findBool("\"continents\"", continentsOpt);
            bool newProcedural = findBool("\"procedural\"", true); // default legacy behavior
            bool newWaterOnly = findBool("\"water_only\"", false);
            proceduralSeed = newSeed;
            continentsOpt = newCont;
            proceduralMode = newProcedural;
            waterOnly = newWaterOnly;
            chunkMgr.clear();
            if (proceduralMode) {
                chunkMgr.setMode(ChunkManager::Mode::Procedural, proceduralSeed);
                chunkMgr.setContinents(continentsOpt);
            } else {
                // Static/baked import: overrides will be provided by CSVs; ensure overrides are reset
                chunkMgr.resetOverrides();
            }
            if (fontLoaded) seedText.setString("Seed: " + std::to_string(proceduralSeed));
            if (showStatusFn) {
                std::string modeLabel = proceduralMode ? (waterOnly?"proc-eau":"proc") : (waterOnly?"statique-eau":"statique");
                showStatusFn(std::string("Seed: ") + std::to_string(proceduralSeed) + (continentsOpt?" (continents)":"") + " [" + modeLabel + "]", 3.0f);
            }
        } else {
            std::cerr << "[Import ZIP] Warning: world.json not found, keeping current seed/continents.\n";
        }

        // Parse painted.json into paintedCells
        size_t paintCount = 0;
        // Always clear; if file is missing we keep it empty
        paintedCells.clear();
        if (zr.readFile("painted.json", buf)) {
            std::string s(buf.begin(), buf.end());
            // Restrict scan to cells array to avoid capturing outer object
            size_t arrStartKey = s.find("\"cells\"");
            size_t arrStart = (arrStartKey==std::string::npos) ? std::string::npos : s.find('[', arrStartKey);
            size_t arrEnd   = (arrStart==std::string::npos) ? std::string::npos : s.find(']', arrStart);
            auto getIntAfter = [&](size_t& pos, const char* key, int def)->int{
                size_t p = s.find(key, pos); if (p==std::string::npos) return def;
                p = s.find_first_of("0123456789-", p + std::strlen(key)); if (p==std::string::npos) return def;
                size_t q = p; while (q < s.size() && (s[q]=='-' || (s[q]>='0' && s[q]<='9'))) ++q;
                pos = q; try { return std::stoi(s.substr(p, q-p)); } catch (...) { return def; }
            };
            if (arrStart!=std::string::npos && arrEnd!=std::string::npos && arrEnd>arrStart) {
                size_t pos = arrStart;
                while (true) {
                    size_t brace = s.find('{', pos);
                    if (brace == std::string::npos || brace >= arrEnd) break;
                    pos = brace + 1;
                    int I = getIntAfter(pos, "\"I\"", 0);
                    int J = getIntAfter(pos, "\"J\"", 0);
                    int r = getIntAfter(pos, "\"r\"", 0);
                    int g = getIntAfter(pos, "\"g\"", 0);
                    int b = getIntAfter(pos, "\"b\"", 0);
                    int a = getIntAfter(pos, "\"a\"", 255);
                    size_t close = s.find('}', pos);
                    if (close == std::string::npos || close > arrEnd) break;
                    pos = close + 1;
                    sf::Color c((sf::Uint8)std::clamp(r,0,255), (sf::Uint8)std::clamp(g,0,255), (sf::Uint8)std::clamp(b,0,255), (sf::Uint8)std::clamp(a,0,255));
                    paintedCells[key64(I,J)] = c;
                    ++paintCount;
                }
            }
        } else {
            std::cerr << "[Import ZIP] Info: painted.json not found, paint overlay cleared.\n";
        }

        // Parse markers.json into markers
        size_t markerCount = 0;
        // Always clear; if file is missing we keep it empty
        markers.clear();
        if (zr.readFile("markers.json", buf)) {
            std::string s(buf.begin(), buf.end());
            auto getIntAfterS = [&](size_t& pos, const char* key, int def)->int{
                size_t p = s.find(key, pos); if (p==std::string::npos) return def;
                p = s.find_first_of("0123456789-", p + std::strlen(key)); if (p==std::string::npos) return def;
                size_t q = p; while (q < s.size() && (s[q]=='-' || (s[q]>='0' && s[q]<='9'))) ++q;
                pos = q; try { return std::stoi(s.substr(p, q-p)); } catch (...) { return def; }
            };
            auto getStringAfterS = [&](size_t& pos, const char* key)->std::string{
                size_t p = s.find(key, pos); if (p==std::string::npos) return std::string();
                p = s.find(':', p + std::strlen(key)); if (p==std::string::npos) return std::string();
                p = s.find('"', p); if (p==std::string::npos) return std::string();
                size_t q = p + 1; std::string out; out.reserve(16);
                while (q < s.size()) {
                    char ch = s[q++];
                    if (ch == '\\') { if (q < s.size()) { char e = s[q++]; if (e=='"') out.push_back('"'); else if (e=='n') out.push_back('\n'); else if (e=='r') out.push_back('\r'); else if (e=='t') out.push_back('\t'); else out.push_back(e); } }
                    else if (ch == '"') break; else out.push_back(ch);
                }
                pos = q; return out;
            };
            size_t arrStartKey = s.find("\"markers\"");
            size_t arrStart = (arrStartKey==std::string::npos) ? std::string::npos : s.find('[', arrStartKey);
            size_t arrEnd   = (arrStart==std::string::npos) ? std::string::npos : s.find(']', arrStart);
            if (arrStart!=std::string::npos && arrEnd!=std::string::npos && arrEnd>arrStart) {
                size_t pos = arrStart;
                while (true) {
                    size_t brace = s.find('{', pos); if (brace == std::string::npos || brace >= arrEnd) break; pos = brace + 1;
                    int I = getIntAfterS(pos, "\"I\"", 0);
                    int J = getIntAfterS(pos, "\"J\"", 0);
                    std::string label = getStringAfterS(pos, "\"label\"");
                    int r = getIntAfterS(pos, "\"r\"", 255);
                    int g = getIntAfterS(pos, "\"g\"", 255);
                    int b = getIntAfterS(pos, "\"b\"", 255);
                    int a = getIntAfterS(pos, "\"a\"", 255);
                    // optional icon string (backward compatible)
                    std::string icon = getStringAfterS(pos, "\"icon\"");
                    size_t close = s.find('}', pos); if (close == std::string::npos || close > arrEnd) break; pos = close + 1;
                    Marker m; m.I = I; m.J = J; m.label = std::move(label);
                    m.color = sf::Color((sf::Uint8)std::clamp(r,0,255), (sf::Uint8)std::clamp(g,0,255), (sf::Uint8)std::clamp(b,0,255), (sf::Uint8)std::clamp(a,0,255));
                    m.icon = std::move(icon);
                    markers.push_back(std::move(m));
                    ++markerCount;
                }
            }
        } else {
            std::cerr << "[Import ZIP] Info: markers.json not found, no markers restored.\n";
        }

        // Parse colors.json into colorHistory
        size_t colorCount = 0;
        if (zr.readFile("colors.json", buf)) {
            colorHistory.clear();
            std::string s(buf.begin(), buf.end());
            size_t pos = 0; int count = 0;
            while (true) {
                size_t brace = s.find('{', pos); if (brace == std::string::npos) break; pos = brace + 1;
                auto getIntA = [&](const char* key, int def){
                    size_t p = s.find(key, pos); if (p==std::string::npos) return def;
                    p = s.find_first_of("0123456789-", p + std::strlen(key)); if (p==std::string::npos) return def;
                    size_t q = p; while (q < s.size() && (s[q]=='-' || (s[q]>='0' && s[q]<='9'))) ++q;
                    try { int v = std::stoi(s.substr(p, q-p)); pos = q; return v; } catch (...) { return def; }
                };
                int r = getIntA("\"r\"", 255);
                int g = getIntA("\"g\"", 255);
                int b = getIntA("\"b\"", 255);
                int a = getIntA("\"a\"", 255);
                size_t close = s.find('}', pos); if (close == std::string::npos) break; pos = close + 1;
                colorHistory.push_back(sf::Color((sf::Uint8)std::clamp(r,0,255), (sf::Uint8)std::clamp(g,0,255), (sf::Uint8)std::clamp(b,0,255), (sf::Uint8)std::clamp(a,0,255)));
                ++count; ++colorCount; if (count >= 5) break; // cap to 5
            }
        } else {
            std::cerr << "[Import ZIP] Info: colors.json not found, color history cleared.\n";
            colorHistory.clear();
        }
        // Extract maps/*.csv to disk
        auto files = zr.listFiles();
        size_t csvCount = 0, csvOk = 0;
        size_t iter = 0;
        for (const auto& name : files) {
            if (name.size() >= 5 && name.rfind("maps/", 0) == 0) {
                // Create parent directories
                std::filesystem::path p(name);
                if (name.back() == '/') continue; // skip directories
                ++csvCount;
                std::error_code ec; std::filesystem::create_directories(p.parent_path(), ec);
                std::vector<uint8_t> data;
                if (zr.readFile(name, data)) {
                    std::ofstream out(p, std::ios::binary);
                    if (out) {
                        if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
                        if (!out.fail()) ++csvOk; else std::cerr << "[Import ZIP] Write failed: " << p.string() << "\n";
                    } else {
                        std::cerr << "[Import ZIP] Cannot open for write: " << p.string() << "\n";
                    }
                } else {
                    std::cerr << "[Import ZIP] Failed to read entry: " << name << "\n";
                }
                // Periodic progress msg (note: visible only when function returns to render loop)
                ++iter; if ((iter % 50) == 0 && csvCount > 0) {
                    int pct = (int)std::round((csvOk * 100.0) / std::max<size_t>(1, csvCount));
                    if (showStatusFn) showStatusFn(std::string("Import ZIP: ") + std::to_string(pct) + "%...", 2.0f);
                }
            }
        }
        std::cerr << "[Import ZIP] Extracted CSVs: " << csvOk << "/" << csvCount << "\n";
        if (showStatusFn) showStatusFn(std::string("Import ZIP: ") + std::to_string(csvOk) + "/" + std::to_string(csvCount) +
                   " CSV, peinture " + std::to_string(paintCount) +
                   ", marqueurs " + std::to_string(markerCount) +
                   ", couleurs " + std::to_string(colorCount), 4.0f);
        if (__log) __log << "[" << __now() << "] importWorldZip done" << std::endl;
    };

    auto exportWorldZip = [&] (const std::string& zipPath){
        if (__log) __log << "[" << __now() << "] exportWorldZip begin -> " << zipPath << std::endl;
        // Flush chunks
        chunkMgr.saveAllDirty();
        // Build metadata
        std::ostringstream meta;
        meta << "{\n"
             << "  \"app\": \"MyWorld\",\n"
             << "  \"format\": 2,\n"
             << "  \"seed\": " << proceduralSeed << ",\n"
             << "  \"continents\": " << (continentsOpt?"true":"false") << ",\n"
             << "  \"procedural\": " << (proceduralMode?"true":"false") << ",\n"
             << "  \"water_only\": " << (waterOnly?"true":"false") << ",\n"
             << "  \"saved_at\": \"" << __now() << "\"\n"
             << "}\n";

        // Painted cells
        std::ostringstream paint;
        paint << "{\n  \"cells\": [\n";
        bool first = true;
        for (const auto& kv : paintedCells) {
            long long k = kv.first; const sf::Color& c = kv.second;
            int I = (int)(k >> 32);
            int J = (int)(uint32_t)(k & 0xFFFFFFFFull);
            if (!first) paint << ",\n"; first = false;
            paint << "    {\"I\":" << I << ",\"J\":" << J
                  << ",\"r\":" << (int)c.r << ",\"g\":" << (int)c.g
                  << ",\"b\":" << (int)c.b << ",\"a\":" << (int)c.a << "}";
        }
        paint << "\n  ]\n}\n";

        // Markers
        std::ostringstream marks;
        marks << "{\n  \"markers\": [\n";
        first = true;
        for (const auto& m : markers) {
            if (!first) marks << ",\n"; first = false;
            marks << "    {\"I\":" << m.I << ",\"J\":" << m.J
                  << ",\"label\":\"" << jsonEscape(m.label) << "\""
                  << ",\"r\":" << (int)m.color.r << ",\"g\":" << (int)m.color.g
                  << ",\"b\":" << (int)m.color.b << ",\"a\":" << (int)m.color.a
                  << ",\"icon\":\"" << jsonEscape(m.icon) << "\"}";
        }
        marks << "\n  ]\n}\n";

        // Color history
        std::ostringstream cols;
        cols << "{\n  \"colors\": [\n";
        for (size_t i=0;i<colorHistory.size();++i){ const auto& c = colorHistory[i];
            cols << "    {\"r\":" << (int)c.r << ",\"g\":" << (int)c.g
                 << ",\"b\":" << (int)c.b << ",\"a\":" << (int)c.a << "}";
            if (i + 1 < colorHistory.size()) cols << ",\n";
        }
        cols << "\n  ]\n}\n";

        // Prepare ZIP
        ZipWriter zw(zipPath);
        if (!zw.ok()) { if (__log) __log << "[" << __now() << "] ZIP open failed" << std::endl; return; }
        zw.addFile("world.json", meta.str());
        zw.addFile("painted.json", paint.str());
        zw.addFile("markers.json", marks.str());
        zw.addFile("colors.json", cols.str());

        // Add chunk CSVs under their existing maps path
        std::ostringstream dir;
        dir << "maps/seed_" << proceduralSeed; if (continentsOpt) dir << "_cont";
        std::error_code ec;
        if (std::filesystem::exists(dir.str(), ec) && std::filesystem::is_directory(dir.str(), ec)) {
            for (auto& p : std::filesystem::recursive_directory_iterator(dir.str(), ec)) {
                if (p.is_regular_file()) {
                    auto pathStr = p.path().generic_string();
                    // Read file
                    std::ifstream in(p.path(), std::ios::binary);
                    std::vector<uint8_t> buf;
                    if (in) {
                        in.seekg(0, std::ios::end); std::streamsize sz = in.tellg(); in.seekg(0, std::ios::beg);
                        if (sz > 0) { buf.resize((size_t)sz); in.read(reinterpret_cast<char*>(buf.data()), sz); }
                        zw.addFile(pathStr, buf);
                    }
                }
            }
        }
        zw.close();
        showStatus("Export ZIP terminé");
        if (__log) __log << "[" << __now() << "] exportWorldZip done" << std::endl;
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

    // Helper: detect if a screen-space point is over any UI element
    auto isOverUI = [&](sf::Vector2f screen)->bool {
        // Top-left buttons
        if (btnGenerate.getGlobalBounds().contains(screen)) return true;
        if (btnGrid.getGlobalBounds().contains(screen)) return true;
        if (btnContinents.getGlobalBounds().contains(screen)) return true;
        if (btnReset.getGlobalBounds().contains(screen)) return true;
        if (btnReseed.getGlobalBounds().contains(screen)) return true;
        if (seedBox.getGlobalBounds().contains(screen)) return true;
        if (btnBake.getGlobalBounds().contains(screen)) return true;
        // Inventory slots at bottom-center
        auto [rBulldozer, rBrush, rEraser, rLocator, rPipette] = inventoryRects();
        if (rBulldozer.contains(screen) || rBrush.contains(screen) || rEraser.contains(screen) || rLocator.contains(screen) || rPipette.contains(screen)) return true;
        // Size slider (right side)
        if (sliderTrackRect().contains(screen) || sliderThumbRect(brushSize).contains(screen)) return true;
        // Left color picker panel (visible in Brush, Locator and Pipette)
        bool wantPicker = (currentTool == Tool::Brush || currentTool == Tool::Locator || currentTool == Tool::Pipette);
        if (wantPicker) {
            float leftX = 16.f;
            float panelW = 140.f;
            float wheelTop = btnBake.getPosition().y + btnBake.getSize().y + 16.f;
            float panelH = ((currentTool == Tool::Locator) || (currentTool == Tool::Pipette))
                ? (colorWheelRadius*2.f + 56.f)
                : (colorWheelRadius*2.f + 56.f + 34.f + 28.f + 16.f + 24.f);
            sf::FloatRect panel(leftX, wheelTop, panelW, panelH);
            if (panel.contains(screen)) return true;
        }
        return false;
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
        // Basic unprojection (assumes elev=0). Kept for reference; replaced by pickIJAccurate below.
        sf::Vector2f local = world - origin;
        sf::Vector2f ij = isoUnprojectDyn(local, iso);
        int I = static_cast<int>(std::round(ij.x));
        int J = static_cast<int>(std::round(ij.y));
        if (!proceduralMode) { I = std::clamp(I, 0, gridSide); J = std::clamp(J, 0, gridSide); }
        return {I, J};
    };

    auto pointInsideGrid = [&](sf::Vector2f world)->bool {
        sf::Vector2f local = world - origin;
        sf::Vector2f ij = isoUnprojectDyn(local, iso);
        if (proceduralMode) return true; // infinite procedural world
        return (ij.x >= 0.f && ij.y >= 0.f && ij.x <= (float)gridSide && ij.y <= (float)gridSide);
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
            int iC = std::clamp(I, 0, gridSide);
            int jC = std::clamp(J, 0, gridSide);
            return heights[idx(iC, jC)];
        }
    };

    // Elevation-aware intersection picking: two-pass unproject compensating elevation
    auto pickIJAccurate = [&](sf::Vector2f world)->sf::Vector2i {
        sf::Vector2f local = world - origin;
        // First pass (elev=0)
        sf::Vector2f ij0 = isoUnprojectDyn(local, iso);
        int I0 = (int)std::round(ij0.x);
        int J0 = (int)std::round(ij0.y);
        // Sample elevation at first-pass pick
        int h = getIntersectionHeight(I0, J0);
        // Inverse rotate to iso space, then add elevation (it was subtracted before rotate in projection)
        float rad = -iso.rotDeg * 3.1415926535f / 180.f;
        float cs = std::cos(rad), sn = std::sin(rad);
        sf::Vector2f v{ local.x * cs - local.y * sn, local.x * sn + local.y * cs };
        v.y += (float)h;
        // Undo pitch and solve
        const float hx = cfg::TILE_W * 0.5f;
        const float hy = cfg::TILE_H * 0.5f;
        float vx = v.x;
        float vy = (iso.pitch != 0.f) ? v.y / iso.pitch : v.y;
        float ix = (hx != 0.f) ? vx / hx : 0.f;
        float iy = (hy != 0.f) ? vy / hy : 0.f;
        float i = (ix + iy) * 0.5f;
        float j = (iy - ix) * 0.5f;
        int I = (int)std::round(i);
        int J = (int)std::round(j);
        if (!proceduralMode) { I = std::clamp(I, 0, gridSide); J = std::clamp(J, 0, gridSide); }
        return {I, J};
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
                    if (ev.key.code == sf::Keyboard::Escape) {
                        // If editing text (marker label or seed), ESC cancels edit instead of closing
                        if (labelEditing) {
                            // If marker was newly created (original label empty), remove it on cancel
                            if (labelEditIndex >= 0 && labelEditIndex < (int)markers.size()) {
                                if (markers[labelEditIndex].label.empty()) {
                                    markers.erase(markers.begin() + labelEditIndex);
                                }
                            }
                            labelEditing = false; labelEditIndex = -1; labelBuffer.clear();
                        }
                    }
                    // Delete key: remove current edited marker, or hovered marker in Locator tool
                    if (ev.key.code == sf::Keyboard::Delete && currentTool == Tool::Locator) {
                        if (labelEditing && labelEditIndex >= 0 && labelEditIndex < (int)markers.size()) {
                            markers.erase(markers.begin() + labelEditIndex);
                            labelEditing = false; labelEditIndex = -1; labelBuffer.clear();
                        } else {
                            // Try delete hovered marker under mouse
                            sf::Vector2i mp = sf::Mouse::getPosition(window);
                            sf::Vector2f m = window.mapPixelToCoords(mp, view);
                            // Local helper to sample height at a world cell
                            auto sampleHeightAt = [&](int I, int J)->int{
                                if (proceduralMode) {
                                    auto floorDiv = [](int a, int b){ return (a >= 0) ? (a / b) : ((a - (b - 1)) / b); };
                                    int cx = floorDiv(I, cfg::CHUNK_SIZE);
                                    int cy = floorDiv(J, cfg::CHUNK_SIZE);
                                    int li = I - cx * cfg::CHUNK_SIZE;
                                    int lj = J - cy * cfg::CHUNK_SIZE;
                                    li = std::clamp(li, 0, cfg::CHUNK_SIZE);
                                    lj = std::clamp(lj, 0, cfg::CHUNK_SIZE);
                                    const Chunk& ch = chunkMgr.getChunk(cx, cy);
                                    int k = li * (cfg::CHUNK_SIZE + 1) + lj;
                                    return ch.heights[k];
                                } else {
                                    int Icl = std::clamp(I, 0, gridSide);
                                    int Jcl = std::clamp(J, 0, gridSide);
                                    return heights[idx(Icl, Jcl)];
                                }
                            };
                            int hitIndex = -1; float bestDist2 = 1e9f;
                            for (size_t i = 0; i < markers.size(); ++i) {
                                int h = sampleHeightAt(markers[i].I, markers[i].J);
                                sf::Vector2f p = isoProjectDyn((float)markers[i].I, (float)markers[i].J, (float)h, iso) + origin;
                                float dx = p.x - m.x, dy = p.y - m.y; float d2 = dx*dx + dy*dy;
                                if (d2 < bestDist2 && d2 < 20.f*20.f) { bestDist2 = d2; hitIndex = (int)i; }
                            }
                            if (hitIndex >= 0) {
                                markers.erase(markers.begin() + hitIndex);
                                if (labelEditing && labelEditIndex == hitIndex) { labelEditing = false; labelEditIndex = -1; labelBuffer.clear(); }
                            }
                        }
                    }
                    break;
                case sf::Event::MouseWheelScrolled:
                {
                    // Scroll icon grid (Locator) when hovering the grid in UI space
                    if (currentTool == Tool::Locator) {
                        float leftX = 16.f;
                        float panelW = 140.f;
                        float wheelTop = btnBake.getPosition().y + btnBake.getSize().y + 16.f;
                        const int N = 5; float sw = 22.f, sh = 22.f, gap = 6.f;
                        float hy = wheelTop + colorWheelRadius*2.f + 12.f;
                        const int cols = 3; const float igap = 6.f; const float cell = 32.f;
                        float gridW = cols*cell + (cols-1)*igap;
                        float gx = leftX + (panelW - gridW) * 0.5f;
                        float gy = hy + sh + 10.f;
                        const int rowsVisible = 5; float viewH = rowsVisible*cell + (rowsVisible-1)*igap;
                        sf::FloatRect iconView(gx, gy, gridW, viewH);
                        sf::Vector2f screen = window.mapPixelToCoords(sf::Vector2i((int)ev.mouseWheelScroll.x, (int)ev.mouseWheelScroll.y), window.getDefaultView());
                        if (iconView.contains(screen)) {
                            int count = (int)markerIcons.size() + 1; int rows = (count + cols - 1) / cols;
                            float contentH = rows*cell + std::max(0, rows-1)*igap;
                            float delta = -ev.mouseWheelScroll.delta * 24.f; // scroll step
                            markerIconsScroll = std::clamp(markerIconsScroll + delta, 0.f, std::max(0.f, contentH - viewH));
                            break; // do not treat as world zoom if we scrolled the grid
                        }
                    }
                    // Zoom with relaxed clamps: allow near-infinite zoom while keeping stability
                    // Perf gate: if FPS drops below 30, stop zooming OUT only; allow zoom-in to recover
                        // Use fpsValue computed by the periodic FPS updater; ignore if not yet initialized
                        if (fpsValue > 0.f && fpsValue < 30.f && ev.mouseWheelScroll.delta < 0) {
                            break;
                        }
                        sf::Vector2f viewSize = view.getSize();
                        sf::Vector2f defSize  = window.getDefaultView().getSize();
                        float curScale = std::max(viewSize.x / std::max(1.f, defSize.x), viewSize.y / std::max(1.f, defSize.y));
                        // Very wide safe range
                        const float minZoom = 1e-3f;
                        const float maxZoom = 1e+3f;
                        float desired = (ev.mouseWheelScroll.delta > 0) ? 0.9f : 1.1f;
                        float newScale = curScale * desired;
                        float apply = desired;
                        if (newScale < minZoom) apply = std::max(0.01f, minZoom / std::max(0.01f, curScale));
                        if (newScale > maxZoom) apply = std::max(0.01f, maxZoom / std::max(1.f, curScale));
                        if (std::isfinite(apply) && std::fabs(apply - 1.f) > 1e-4f) {
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
                            auto [rBull, rBrush, rEraser, rLocator, rPipette] = inventoryRects();
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
                            if (rLocator.contains(screen)) {
                                currentTool = Tool::Locator;
                                // Use the same color picker panel as Brush
                                showColorPicker = true;
                                showColorHover = true;
                                break;
                            }
                            if (rPipette.contains(screen)) {
                                currentTool = Tool::Pipette;
                                showColorPicker = true;
                                showColorHover = false;
                                break;
                            }
                        }
                        // Color picker interactions (left column below buttons)
                        if (showColorPicker) {
                            ensureColorWheel();
                            // Compute wheel center under bake button
                            float leftX = 16.f;
                            float panelW = (currentTool == Tool::Locator) ? std::max(140.f, 184.f) : 140.f;
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
                                    // If in Locator tool, mirror picker changes to marker color
                                    if (currentTool == Tool::Locator) {
                                        sf::Color c = activeColor;
                                        if (labelEditing && labelEditIndex >= 0 && labelEditIndex < (int)markers.size()) markers[labelEditIndex].color = c; else currentMarkerColor = c;
                                    }
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
                                        if (currentTool == Tool::Locator) {
                                            sf::Color c = activeColor;
                                            if (labelEditing && labelEditIndex >= 0 && labelEditIndex < (int)markers.size()) markers[labelEditIndex].color = c; else currentMarkerColor = c;
                                        }
                                        handledPick = true;
                                        break;
                                    }
                                }
                            }
                            // Tone slider interaction (disabled in Locator tool)
                            if (!handledPick && currentTool != Tool::Locator) {
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
                                    if (currentTool == Tool::Locator) {
                                        sf::Color c = activeColor;
                                        if (labelEditing && labelEditIndex >= 0 && labelEditIndex < (int)markers.size()) markers[labelEditIndex].color = c; else currentMarkerColor = c;
                                    }
                                    toneDragging = true;
                                    handledPick = true;
                                }
                            }
                            // Brush shape buttons interaction (below tone slider)
                            if (!handledPick) {
                                const float toneH = 18.f; const float tonePad = 12.f;
                                const int N = 5; float sw = 22.f, sh = 22.f, gap = 6.f;
                                float totalW = N*sw + (N-1)*gap;
                                float hx = leftX + (panelW - totalW) * 0.5f;
                                float hy = wheelRect.top + wheelRect.height + 12.f; // history y
                                float toneY = hy + sh + tonePad;
                                float shapesTop = toneY + toneH + 10.f;
                                float bw = 28.f, bh = 28.f, bgap = 6.f;
                                float totalBW = 4*bw + 3*bgap;
                                float bx = leftX + (panelW - totalBW) * 0.5f;
                                sf::FloatRect rSq(bx + 0*(bw+bgap), shapesTop, bw, bh);
                                sf::FloatRect rCi(bx + 1*(bw+bgap), shapesTop, bw, bh);
                                sf::FloatRect rMa(bx + 2*(bw+bgap), shapesTop, bw, bh);
                                sf::FloatRect rGa(bx + 3*(bw+bgap), shapesTop, bw, bh);
                                if (rSq.contains(screen)) { currentBrushShape = BrushShape::Square; handledPick = true; }
                                else if (rCi.contains(screen)) { currentBrushShape = BrushShape::Circle; handledPick = true; }
                                else if (rMa.contains(screen)) { currentBrushShape = BrushShape::Manhattan; handledPick = true; }
                                else if (rGa.contains(screen)) { currentBrushShape = BrushShape::Gaussian; handledPick = true; }
                            }
                            // Hardness slider interaction (below shape buttons) - only for Brush
                            if (!handledPick && currentTool == Tool::Brush) {
                                const float toneH = 18.f; const float tonePad = 12.f;
                                const int N = 5; float sw = 22.f, sh = 22.f, gap = 6.f;
                                float hy = wheelRect.top + wheelRect.height + 12.f; // history y
                                float toneY = hy + sh + tonePad;
                                float shapesTop = toneY + toneH + 10.f;
                                float bw = 28.f, bh = 28.f, bgap = 6.f;
                                float hardnessY = shapesTop + bh + 10.f;
                                float hardH = 14.f;
                                sf::FloatRect hardRect(leftX, hardnessY, panelW, hardH);
                                if (hardRect.contains(screen)) {
                                    float t = (screen.x - hardRect.left) / hardRect.width;
                                    brushHardness = std::clamp(t, 0.f, 1.f);
                                    hardnessDragging = true;
                                    handledPick = true;
                                }
                            }
                            if (handledPick) break; // consume click if we picked a color
                        }
                        // Icon grid click (Locator): handle in UI space when color picker visible
                        if (currentTool == Tool::Locator) {
                            loadMarkerIcons();
                            // Recompute geometry (match draw)
                            float leftX = 16.f;
                            float panelW = 140.f;
                            float wheelTop = btnBake.getPosition().y + btnBake.getSize().y + 16.f;
                            const int N = 5; float sw = 22.f, sh = 22.f, gap = 6.f;
                            float hy = wheelTop + colorWheelRadius*2.f + 12.f;
                            const int cols = 3; const float igap = 6.f; const float cell = 32.f;
                            float gridW = cols*cell + (cols-1)*igap;
                            float gx = leftX + (panelW - gridW) * 0.5f;
                            float gy = hy + sh + 10.f;
                            const int rowsVisible = 5; float viewH = rowsVisible*cell + (rowsVisible-1)*igap;
                            sf::FloatRect iconView(gx, gy, gridW, viewH);
                            if (iconView.contains(screen)) {
                                int count = (int)markerIcons.size() + 1; // include None cell
                                float localY = (screen.y - gy) + markerIconsScroll;
                                int c = (int)((screen.x - gx) / (cell + igap));
                                int r = (int)(localY / (cell + igap));
                                if (c >= 0 && c < cols && r >= 0) {
                                    int idx = r*cols + c;
                                    if (idx >= 0 && idx < count) {
                                        std::string chosen = (idx == 0) ? std::string() : markerIcons[idx-1].name;
                                        if (labelEditing && labelEditIndex >= 0 && labelEditIndex < (int)markers.size()) {
                                            // Apply icon to the marker but stay in edit mode (do not close)
                                            markers[labelEditIndex].icon = chosen;
                                        } else {
                                            currentMarkerIcon = chosen;
                                        }
                                        break;
                                    }
                                }
                            }
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
                            // Reset to water-only, clear paint & markers, keep procedural world active (same seed)
                            if (__log) __log << "[" << __now() << "] RESET clicked -> water-only + clear overlays" << std::endl;
                            proceduralMode = true;
                            waterOnly = true;
                            paintedCells.clear();
                            markers.clear();
                            chunkMgr.resetOverrides();
                            chunkMgr.setMode(ChunkManager::Mode::Procedural, proceduralSeed);
                            chunkMgr.setContinents(continentsOpt);
                            if (showStatusFn) showStatusFn("Monde réinitialisé: peinture et marqueurs effacés.", 3.0f);
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
                                int I0 = Icenter - gridSide / 2;
                                int J0 = Jcenter - gridSide / 2;
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
                                for (int i = 0; i <= gridSide; ++i) {
                                    for (int j = 0; j <= gridSide; ++j) {
                                        heights[i * (gridSide + 1) + j] = sampleWorld(I0 + i, J0 + j);
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
                            if (__log) __log << "[" << __now() << "] Export clicked" << std::endl; 
                            std::string path = saveFileDialogZIP();
                            if (!path.empty()) exportWorldZip(path);
                            break;
                        }
                        if (circleContains(importBtnPos, btnRadius, screen)) {
                            if (__log) __log << "[" << __now() << "] Import clicked" << std::endl; 
                            std::string path = openFileDialogZIP();
                            if (!path.empty()) importWorldZip(path);
                            break;
                        }
                        // Inventory toolbar (bottom-center) click detection
                        {
                            auto [rBull, rBrush, rEraser, rLocator, rPipette] = inventoryRects();
                            if (rBull.contains(screen)) {
                                currentTool = Tool::Bulldozer;
                                brushDragging = false; // cancel any slider drag on tool switch
                                showColorPicker = false;
                                showColorHover = false;
                                toneDragging = false;
                                hardnessDragging = false;
                                break;
                            }
                            if (rBrush.contains(screen)) {
                                currentTool = Tool::Brush;
                                brushDragging = false; // cancel any slider drag on tool switch
                                showColorPicker = true;
                                showColorHover = true;
                                break;
                            }
                            if (rEraser.contains(screen)) {
                                currentTool = Tool::Eraser;
                                brushDragging = false; // cancel any slider drag on tool switch
                                showColorPicker = false;
                                showColorHover = false;
                                toneDragging = false;
                                hardnessDragging = false;
                                break;
                            }
                            if (rLocator.contains(screen)) {
                                currentTool = Tool::Locator;
                                brushDragging = false; // cancel any slider drag on tool switch
                                showColorPicker = true;
                                showColorHover = true;
                                toneDragging = false;
                                hardnessDragging = false;
                                break;
                            }
                            if (rPipette.contains(screen)) {
                                currentTool = Tool::Pipette;
                                brushDragging = false;
                                showColorPicker = true;
                                showColorHover = false;
                                toneDragging = false;
                                hardnessDragging = false;
                                break;
                            }
                        }
                        // (height slider removed)
                        // Size slider interaction (right side) - enabled for Brush, Bulldozer, and Eraser
                        if (currentTool == Tool::Brush || currentTool == Tool::Bulldozer || currentTool == Tool::Eraser) {
                            if (sliderTrackRect().contains(screen) || sliderThumbRect(brushSize).contains(screen)) {
                                brushDragging = true;
                                brushSize = sliderPickValue(screen);
                                break;
                            }
                        }
                        // If Locator tool active: handle color wheel click in UI space
                        if (currentTool == Tool::Locator) {
                            // Color wheel parameters (UI default view coords)
                            sf::Vector2f wheelCenter(90.f, window.getDefaultView().getSize().y - 110.f);
                            float wheelR = 50.f; float wheelInner = 28.f; // ring
                            sf::Vector2f d = screen - wheelCenter;
                            float dist = std::sqrt(d.x*d.x + d.y*d.y);
                            if (dist >= wheelInner && dist <= wheelR) {
                                float ang = std::atan2(d.y, d.x); // [-pi,pi]
                                float deg = ang * 180.f / 3.14159265f; if (deg < 0) deg += 360.f;
                                sf::Color c = hsvToRgb(deg);
                                if (labelEditing && labelEditIndex >= 0 && labelEditIndex < (int)markers.size()) {
                                    markers[labelEditIndex].color = c;
                                } else {
                                    currentMarkerColor = c;
                                }
                                break;
                            }
                        }

                        // If mouse is over any UI, do NOT propagate to world interactions
                        if (isOverUI(screen)) {
                            break;
                        }

                        // Then, world interactions
                        sf::Vector2f world = window.mapPixelToCoords(mp, view);
                        // Marker dragging disabled: ignore left presses while editing
                        // (no-op)
                        // Right-click on an existing marker to edit its label (any tool)
                        if (ev.mouseButton.button == sf::Mouse::Right) {
                            // Helper to sample height for projection
                            auto sampleHeightAt = [&](int I, int J)->int{
                                if (proceduralMode) {
                                    auto floorDiv = [](int a, int b){ return (a >= 0) ? (a / b) : ((a - (b - 1)) / b); };
                                    int cx = floorDiv(I, cfg::CHUNK_SIZE);
                                    int cy = floorDiv(J, cfg::CHUNK_SIZE);
                                    int li = I - cx * cfg::CHUNK_SIZE;
                                    int lj = J - cy * cfg::CHUNK_SIZE;
                                    li = std::clamp(li, 0, cfg::CHUNK_SIZE);
                                    lj = std::clamp(lj, 0, cfg::CHUNK_SIZE);
                                    const Chunk& ch = chunkMgr.getChunk(cx, cy);
                                    int k = li * (cfg::CHUNK_SIZE + 1) + lj;
                                    return ch.heights[k];
                                } else {
                                    int Icl = std::clamp(I, 0, gridSide);
                                    int Jcl = std::clamp(J, 0, gridSide);
                                    return heights[idx(Icl, Jcl)];
                                }
                            };
                            // Compute zoom scale to derive hit radius (match rendering scale)
                            sf::Vector2f vsz = view.getSize();
                            sf::Vector2f dsz = window.getDefaultView().getSize();
                            float zoomScale = std::max(vsz.x / std::max(1.f, dsz.x), vsz.y / std::max(1.f, dsz.y));
                            float visScale = std::clamp(zoomScale, 1.f, 8.f);
                            float rBase = 7.f; float r = rBase * visScale;
                            int hitIndex = -1;
                            float bestDist2 = 1e30f;
                            for (int i=0;i<(int)markers.size();++i){
                                const auto& m = markers[i];
                                int h = sampleHeightAt(m.I, m.J);
                                sf::Vector2f p = isoProjectDyn((float)m.I, (float)m.J, (float)h, iso) + origin;
                                // Diamond proximity
                                sf::Vector2f d = p - world;
                                float dist2 = d.x*d.x + d.y*d.y;
                                bool hit = (dist2 <= r*r);
                                // Label bounds proximity (if any)
                                if (!hit && fontLoaded) {
                                    sf::Text t; t.setFont(uiFont);
                                    unsigned int cs = (unsigned int)std::clamp(16.f * visScale, 12.f, 64.f);
                                    t.setCharacterSize(cs);
                                    t.setString(m.label);
                                    auto b = t.getLocalBounds();
                                    t.setOrigin(b.left + b.width * 0.5f, b.top + b.height);
                                    t.setPosition(p.x, p.y - (5.f * visScale) - 4.f * visScale);
                                    sf::FloatRect gb = t.getGlobalBounds();
                                    if (gb.contains(world)) hit = true;
                                }
                                if (hit && dist2 < bestDist2) { bestDist2 = dist2; hitIndex = i; }
                            }
                            if (hitIndex >= 0) {
                                labelEditing = true;
                                labelEditIndex = hitIndex;
                                labelBuffer = markers[hitIndex].label;
                                break; // consume right-click
                            }
                        }
                        if (pointInsideGrid(world)) {
                            // Tool-dependent action on press (elevation-aware pick)
                            sf::Vector2i IJ = pickIJAccurate(world);
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
                                            if (!proceduralMode) { if (I < 0 || J < 0 || I > gridSide || J > gridSide) continue; }
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
                                            if (!proceduralMode) { if (I < 0 || J < 0 || I > gridSide || J > gridSide) continue; }
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
                                // Begin paint stroke and stamp at initial position
                                sf::Vector2f local = world - origin; sf::Vector2f ij = isoUnprojectDyn(local, iso);
                                int i0 = (int)std::floor(ij.x); int j0 = (int)std::floor(ij.y);
                                // new stroke seed for deterministic dithering along this stroke
                                static uint32_t seedCounter = 1u;
                                strokeSeed = (++seedCounter * 2654435761u) ^ (uint32_t)(i0 * 73856093) ^ (uint32_t)(j0 * 19349663);
                                stampIndex = 0u;
                                stampBrushAt(i0, j0);
                                paintingActive = true;
                                lastPaintIJ = sf::Vector2i(i0, j0);
                            }
                            else if (currentTool == Tool::Locator && ev.mouseButton.button == sf::Mouse::Left) {
                                // Do not place a new marker while editing an existing one
                                if (labelEditing) break;
                                // Place a new marker at nearest intersection and enter label edit
                                Marker m; m.I = IJ.x; m.J = IJ.y; m.label = ""; m.color = currentMarkerColor; m.icon = currentMarkerIcon;
                                markers.push_back(m);
                                labelEditing = true;
                                labelEditIndex = (int)markers.size() - 1;
                                labelBuffer.clear();
                            }
                            else if (currentTool == Tool::Pipette && ev.mouseButton.button == sf::Mouse::Left) {
                                // Sample visible color at clicked cell and set as selected color
                                sf::Vector2f local = world - origin; sf::Vector2f ij = isoUnprojectDyn(local, iso);
                                int ci = (int)std::floor(ij.x);
                                int cj = (int)std::floor(ij.y);
                                // Base color from terrain height at picked intersection IJ
                                int h = getIntersectionHeight(IJ.x, IJ.y);
                                sf::Color base = colorForHeightPicker((float)h);
                                // Overlay paint, if any
                                sf::Color finalC = base;
                                auto itp = paintedCells.find(key64(ci, cj));
                                if (itp != paintedCells.end()) finalC = alphaOverPicker(base, itp->second);
                                selectedColor = finalC;
                                // Reset tone (opacity/brightness) slider to center to match exact picked color
                                colorToneT = 0.5f;
                                toneDragging = false;
                                pushHistory(selectedColor);
                                rebuildToneTex();
                                activeColor = applyTone(selectedColor, colorToneT);
                                // No world mutation; just consume click
                                break;
                            }
                        } else {
                            // Start tilting
                            tilting = true;
                            tiltStartMouse = mp;
                            tiltStartRot = iso.rotDeg;
                            tiltStartPitch = iso.pitch;
                        }
                        // (removed unintended toneDragging handling here; handled in MouseMoved)
                        // Hardness slider drag update (screen space)
                        if (hardnessDragging) {
                            sf::Vector2i mp2 = sf::Mouse::getPosition(window);
                            sf::Vector2f screen2 = window.mapPixelToCoords(mp2, window.getDefaultView());
                            float leftX = 16.f;
                            float panelW = 140.f;
                            float wheelTop = btnBake.getPosition().y + btnBake.getSize().y + 16.f;
                            sf::Vector2f wheelCenter(leftX + panelW*0.5f, wheelTop + (float)colorWheelRadius);
                            const int N = 5; float sw = 22.f, sh = 22.f, gap = 6.f;
                            float hy = wheelCenter.y + colorWheelRadius + 12.f; // history y
                            const float toneH = 18.f; const float tonePad = 12.f;
                            float toneY = hy + sh + tonePad;
                            float shapesTop = toneY + toneH + 10.f;
                            float bw = 28.f, bh = 28.f;
                            float hardnessY = shapesTop + bh + 10.f;
                            float hardH = 14.f;
                            sf::FloatRect hardRect(leftX, hardnessY, panelW, hardH);
                            float t = (screen2.x - hardRect.left) / hardRect.width;
                            brushHardness = std::clamp(t, 0.f, 1.f);
                        }
                        // Update UI hover (screen space)
                        sf::Vector2i mpMove = sf::Mouse::getPosition(window);
                        sf::Vector2f screenMove = window.mapPixelToCoords(mpMove, window.getDefaultView());
                        genHover = btnGenerate.getGlobalBounds().contains(screenMove);
                        gridHover = btnGrid.getGlobalBounds().contains(screenMove);
                        continentsHover = btnContinents.getGlobalBounds().contains(screenMove);
                        bakeHover = btnBake.getGlobalBounds().contains(screenMove);
                        // (height slider removed)
                        if (brushDragging) {
                            // Update brush while dragging on slider
                            brushSize = sliderPickValue(screenMove);
                        }

                        // Marker dragging disabled: no position updates while moving

                        // Edit/Paint while dragging (if not panning/tilting), throttled by paintTick
                        if (!panning && !tilting && !brushDragging && !isOverUI(screenMove) && (sf::Mouse::isButtonPressed(sf::Mouse::Left) || sf::Mouse::isButtonPressed(sf::Mouse::Right))) {
                            if (paintClock.getElapsedTime() >= paintTick) {
                                paintClock.restart();
                                sf::Vector2f world = window.mapPixelToCoords(mpMove, view);
                                if (pointInsideGrid(world)) {
                                    sf::Vector2i IJ = pickIJAccurate(world);
                                    int brush = std::clamp(brushSize, brushMin, brushMax);
                                    bool ctrl = sf::Keyboard::isKeyPressed(sf::Keyboard::LControl) || sf::Keyboard::isKeyPressed(sf::Keyboard::RControl);
                                    if (currentTool == Tool::Bulldozer && ctrl && flattenPrimed && (sf::Mouse::isButtonPressed(sf::Mouse::Left) || sf::Mouse::isButtonPressed(sf::Mouse::Right))) {
                                        // Flatten to captured height while Ctrl is held
                                        int half = brush - 1;
                                        for (int di = -brush; di <= brush; ++di) {
                                            for (int dj = -brush; dj <= brush; ++dj) {
                                                int I = IJ.x + di;
                                                int J = IJ.y + dj;
                                                if (!proceduralMode) { if (I < 0 || J < 0 || I > gridSide || J > gridSide) continue; }
                                                if (std::max(std::abs(di), std::abs(dj)) > half) continue;
                                                if (proceduralMode) {
                                                    chunkMgr.applySetAt(I, J, flattenHeight);
                                                } else {
                                                    heights[idx(I, J)] = flattenHeight;
                                                }
                                            }
                                        }
                                    } else if (currentTool == Tool::Bulldozer) {
                                        // Continuous Bulldozer editing while holding, but throttled for precision
                                        if (bulldozeClock.getElapsedTime() >= bulldozeTick) {
                                            bulldozeClock.restart();
                                            int delta = sf::Mouse::isButtonPressed(sf::Mouse::Left) ? 1 : -1;
                                            int half = brush - 1;
                                            for (int di = -brush; di <= brush; ++di) {
                                                for (int dj = -brush; dj <= brush; ++dj) {
                                                    int I = IJ.x + di;
                                                    int J = IJ.y + dj;
                                                    if (!proceduralMode) { if (I < 0 || J < 0 || I > gridSide || J > gridSide) continue; }
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
                                        }
                                    } else if (currentTool == Tool::Brush && sf::Mouse::isButtonPressed(sf::Mouse::Left)) {
                                        // Continuous paint while dragging: stamp along the path
                                        sf::Vector2f local = world - origin; sf::Vector2f ij = isoUnprojectDyn(local, iso);
                                        int i1 = (int)std::floor(ij.x); int j1 = (int)std::floor(ij.y);
                                        if (!paintingActive) {
                                            static uint32_t seedCounter2 = 123u;
                                            strokeSeed = (++seedCounter2 * 2246822519u) ^ (uint32_t)(i1 * 83492791) ^ (uint32_t)(j1 * 2971215073u);
                                            stampIndex = 0u;
                                            stampBrushAt(i1, j1);
                                            paintingActive = true;
                                            lastPaintIJ = sf::Vector2i(i1, j1);
                                        } else {
                                            int x0 = lastPaintIJ.x, y0 = lastPaintIJ.y;
                                            int x1 = i1, y1 = j1;
                                            int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
                                            int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
                                            int err = dx + dy; // Bresenham
                                            int x = x0, y = y0;
                                            while (true) {
                                                stampBrushAt(x, y);
                                                if (x == x1 && y == y1) break;
                                                int e2 = 2 * err;
                                                if (e2 >= dy) { err += dy; x += sx; }
                                                if (e2 <= dx) { err += dx; y += sy; }
                                            }
                                            lastPaintIJ = sf::Vector2i(i1, j1);
                                        }
                                    } else if (currentTool == Tool::Eraser && sf::Mouse::isButtonPressed(sf::Mouse::Left)) {
                                        // Erase painted cells while dragging
                                        sf::Vector2f local = world - origin; sf::Vector2f ij = isoUnprojectDyn(local, iso);
                                        int i0 = (int)std::floor(ij.x); int j0 = (int)std::floor(ij.y);
                                        int half = brush - 1;
                                        for (int di = -brush; di <= brush; ++di) {
                                            for (int dj = -brush; dj <= brush; ++dj) {
                                                int ci = i0 + di; int cj = j0 + dj;
                                                float w = weightForHardness(currentBrushShape, di, dj, half);
                                                if (w <= 0.f) continue;
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
                            // Keep the view centered on world origin under new projection
                            sf::Vector2f newCenter = isoProjectDyn(0.f, 0.f, 0.f, iso) + origin;
                            view.setCenter(newCenter);
                            window.setView(view);
                            
                        }
                    }
                    break;
                case sf::Event::MouseButtonReleased:
                    // Stop panning/dragging on button release
                    if (ev.mouseButton.button == sf::Mouse::Middle) {
                        panning = false;
                    }
                    if (ev.mouseButton.button == sf::Mouse::Left) {
                        brushDragging = false;
                        toneDragging = false;
                        hardnessDragging = false;
                        paintingActive = false; // end brush stroke
                    }
                    if (ev.mouseButton.button == sf::Mouse::Right) {
                        tilting = false;
                    }
                    break;
                case sf::Event::TextEntered:
                    if (labelEditing) {
                        sf::Uint32 u = ev.text.unicode;
                        if (u == 13) { // Enter commit
                            if (labelEditIndex >= 0 && labelEditIndex < (int)markers.size()) {
                                markers[labelEditIndex].label = labelBuffer;
                            }
                            labelEditing = false; labelEditIndex = -1; labelBuffer.clear();
                        } else if (u == 27) { // Escape cancel
                            labelEditing = false; labelEditIndex = -1; labelBuffer.clear();
                        } else if (u == 8) { // Backspace
                            if (!labelBuffer.empty()) labelBuffer.pop_back();
                        } else if (u >= 32 && u < 127) { // Printable ASCII
                            if (labelBuffer.size() < 40) labelBuffer.push_back((char)u);
                        }
                    } else if (seedEditing) {
                        sf::Uint32 u = ev.text.unicode;
                        if (u == 13) { // Enter
                            if (!seedBuffer.empty()) {
                                uint32_t newSeed = (uint32_t)std::stoul(seedBuffer);
                                proceduralSeed = newSeed;
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
                if (importRow <= gridSide) {
                    std::stringstream ss(line);
                    std::string item; int col = 0;
                    while (std::getline(ss, item, ',') && col <= gridSide) {
                        int val = 0; try { val = std::stoi(trim(item)); } catch (...) { val = 0; }
                        heights[idx(importRow, col)] = val;
                        ++col;
                    }
                }
                ++importRow;
                importProgress = std::clamp(importRow / (float)importTotalRows, 0.f, 1.f);
                if (importRow > gridSide) {
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
        // Block camera movement while editing a marker label
        if (!labelEditing) {
            if (key(sf::Keyboard::W) || key(sf::Keyboard::Up)    || key(sf::Keyboard::Z)) move.y -= panSpeed * dt; // ZQSD alias
            if (key(sf::Keyboard::S) || key(sf::Keyboard::Down))                        move.y += panSpeed * dt;
            if (key(sf::Keyboard::A) || key(sf::Keyboard::Left)  || key(sf::Keyboard::Q)) move.x -= panSpeed * dt; // ZQSD alias
            if (key(sf::Keyboard::D) || key(sf::Keyboard::Right))                       move.x += panSpeed * dt;
        }
        if (move.x != 0.f || move.y != 0.f) {
            view.move(move);
            window.setView(view);
        }

        // Continuous mouse-drag updates outside event polling (smooth panning/tilting)
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
            // Keep the view centered on world origin under new projection
            sf::Vector2f newCenter = isoProjectDyn(0.f, 0.f, 0.f, iso) + origin;
            view.setCenter(newCenter);
            window.setView(view);
        }

        // Per-frame slider drag update (screen space) and compute screenNow for UI hover gating
        sf::Vector2i mpNow = sf::Mouse::getPosition(window);
        sf::Vector2f screenNow = window.mapPixelToCoords(mpNow, window.getDefaultView());
        if (brushDragging) {
            brushSize = sliderPickValue(screenNow);
        }

        // Per-frame edit/paint/erase while dragging (restores click-drag terrain editing)
        if (!panning && !tilting && !brushDragging && !isOverUI(screenNow) && (sf::Mouse::isButtonPressed(sf::Mouse::Left) || sf::Mouse::isButtonPressed(sf::Mouse::Right))) {
            if (paintClock.getElapsedTime() >= paintTick) {
                paintClock.restart();
                sf::Vector2i mpMove = sf::Mouse::getPosition(window);
                sf::Vector2f world = window.mapPixelToCoords(mpMove, view);
                if (pointInsideGrid(world)) {
                    sf::Vector2i IJ = pickIJAccurate(world);
                    int brush = std::clamp(brushSize, brushMin, brushMax);
                    bool ctrl = sf::Keyboard::isKeyPressed(sf::Keyboard::LControl) || sf::Keyboard::isKeyPressed(sf::Keyboard::RControl);
                    if (currentTool == Tool::Bulldozer && ctrl && flattenPrimed && (sf::Mouse::isButtonPressed(sf::Mouse::Left) || sf::Mouse::isButtonPressed(sf::Mouse::Right))) {
                        int half = brush - 1;
                        for (int di = -brush; di <= brush; ++di) {
                            for (int dj = -brush; dj <= brush; ++dj) {
                                int I = IJ.x + di;
                                int J = IJ.y + dj;
                                if (!proceduralMode) { if (I < 0 || J < 0 || I > gridSide || J > gridSide) continue; }
                                if (std::max(std::abs(di), std::abs(dj)) > half) continue;
                                if (proceduralMode) {
                                    chunkMgr.applySetAt(I, J, flattenHeight);
                                } else {
                                    heights[idx(I, J)] = flattenHeight;
                                }
                            }
                        }
                    } else if (currentTool == Tool::Bulldozer) {
                        // Continuous Bulldozer editing while holding, but throttled for precision
                        if (bulldozeClock.getElapsedTime() >= bulldozeTick) {
                            bulldozeClock.restart();
                            int delta = sf::Mouse::isButtonPressed(sf::Mouse::Left) ? 1 : -1;
                            int half = brush - 1;
                            for (int di = -brush; di <= brush; ++di) {
                                for (int dj = -brush; dj <= brush; ++dj) {
                                    int I = IJ.x + di;
                                    int J = IJ.y + dj;
                                    if (!proceduralMode) { if (I < 0 || J < 0 || I > gridSide || J > gridSide) continue; }
                                    if (std::max(std::abs(di), std::abs(dj)) > half) continue;
                                    if (proceduralMode) {
                                        if (waterOnly) {
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
                    } else if (currentTool == Tool::Brush && sf::Mouse::isButtonPressed(sf::Mouse::Left)) {
                        sf::Vector2f local = world - origin; sf::Vector2f ij = isoUnprojectDyn(local, iso);
                        int i1 = (int)std::floor(ij.x); int j1 = (int)std::floor(ij.y);
                        if (!paintingActive) {
                            static uint32_t seedCounter2 = 123u;
                            strokeSeed = (++seedCounter2 * 2246822519u) ^ (uint32_t)(i1 * 83492791) ^ (uint32_t)(j1 * 2971215073u);
                            stampIndex = 0u;
                            stampBrushAt(i1, j1);
                            paintingActive = true;
                            lastPaintIJ = sf::Vector2i(i1, j1);
                        } else {
                            int x0 = lastPaintIJ.x, y0 = lastPaintIJ.y;
                            int x1 = i1, y1 = j1;
                            int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
                            int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
                            int err = dx + dy; // Bresenham
                            int x = x0, y = y0;
                            while (true) {
                                stampBrushAt(x, y);
                                if (x == x1 && y == y1) break;
                                int e2 = 2 * err;
                                if (e2 >= dy) { err += dy; x += sx; }
                                if (e2 <= dx) { err += dx; y += sy; }
                            }
                            lastPaintIJ = sf::Vector2i(i1, j1);
                        }
                    } else if (currentTool == Tool::Eraser && sf::Mouse::isButtonPressed(sf::Mouse::Left)) {
                        sf::Vector2f local = world - origin; sf::Vector2f ij = isoUnprojectDyn(local, iso);
                        int i0 = (int)std::floor(ij.x); int j0 = (int)std::floor(ij.y);
                        int half = brush - 1;
                        for (int di = -brush; di <= brush; ++di) {
                            for (int dj = -brush; dj <= brush; ++dj) {
                                int ci = i0 + di; int cj = j0 + dj;
                                float w = weightForHardness(currentBrushShape, di, dj, half);
                                if (w <= 0.f) continue;
                                long long k = key64(ci, cj);
                                paintedCells.erase(k);
                            }
                        }
                    }
                }
            }
        }

        window.clear(sf::Color::Black);
        // Ensure world view is active before world rendering (avoid leaving default view from UI phase)
        window.setView(view);
        // Wireframe-only view: do not draw filled tiles or interstices

        // Build hover mask for current brush (selected shape + hardness), available to all render calls below
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
                        int ci = i0 + di;
                        int cj = j0 + dj;
                        float w = weightForHardness(currentBrushShape, di, dj, half);
                        if (w <= 0.f) continue;
                        if (!proceduralMode) {
                            if (ci < 0 || cj < 0 || ci >= gridSide || cj >= gridSide) continue;
                        }
                        hoverMask.insert(key64(ci, cj));
                    }
                }
            }
        }

        // View info used across render paths this frame
        const auto& v = window.getView();
        sf::Vector2f vc = v.getCenter();
        sf::Vector2f vs = v.getSize();

        // Compute screen-space driven LOD stride (powers of two for progressive merge)
        int lodStride = 1;
        int worldStride = 1; // will be set as power-of-two; may exceed CHUNK_SIZE
        float edgePxAtCenter = 0.f;
        {
            // Approximate on-screen pixel size of a unit tile edge around view center at zero elevation
            sf::Vector2f ijC = isoUnprojectDyn(vc - origin, iso);
            int Icenter = (int)std::floor(ijC.x + 0.5f);
            int Jcenter = (int)std::floor(ijC.y + 0.5f);
            sf::Vector2f P00w = isoProjectDyn((float)Icenter, (float)Jcenter, 0.f, iso) + origin;
            sf::Vector2f P10w = isoProjectDyn((float)Icenter + 1.f, (float)Jcenter, 0.f, iso) + origin;
            sf::Vector2f P01w = isoProjectDyn((float)Icenter, (float)Jcenter + 1.f, 0.f, iso) + origin;
            // Map to screen pixels using current view
            auto toPx = [&](const sf::Vector2f& w){
                sf::Vector2i p = window.mapCoordsToPixel(w, window.getView());
                return sf::Vector2f((float)p.x, (float)p.y);
            };
            sf::Vector2f P00 = toPx(P00w);
            sf::Vector2f P10 = toPx(P10w);
            sf::Vector2f P01 = toPx(P01w);
            auto len = [](sf::Vector2f a){ return std::sqrt(a.x*a.x + a.y*a.y); };
            float edgePx = std::max(len(P10 - P00), len(P01 - P00));
            edgePxAtCenter = edgePx;
            // Clamp minimum zoom-out: do not allow edgePx to go below 3.6
            if (edgePxAtCenter < 3.6f) edgePxAtCenter = 3.6f;
            const float pxThresh = 10.0f; // target on-screen edge length in pixels (earlier merge)
            // Choose stride from powers-of-two: 1,2,4,8,... for smooth, progressive merging
            int s = 1;
            while (edgePx * s < pxThresh && s < (1<<20)) s <<= 1;
            // Cap max stride to avoid far-zoom artifacts and heavy per-chunk loops
            s = std::min(s, 4);
            worldStride = s;
            lodStride = std::min(worldStride, cfg::CHUNK_SIZE);
            // publish to debug
            dbg_edgePxAtCenter = edgePxAtCenter;
            dbg_worldStride = worldStride;
            dbg_lodStride = lodStride;
        }

        // Far-zoom and snapshot system removed/disabled
        bool useFarSnapshot = false;
        bool buildingFarSnap = false;
        bool deferFarDraw = false;

        if (proceduralMode) {
            // Per-chunk rendering with culling based on view rectangle unprojected to grid space
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

            // LOD: compute stride based on on-screen pixel size of a unit tile edge
            // Radius culling is handled implicitly by view-rect -> chunk window; no arbitrary ring cap.

            // When zoomed out (even moderately), use a coarse world-space renderer.
            // This avoids per-chunk/per-cell overhead and scales with screen pixels.
            // Trigger it earlier than chunk-size, e.g., from stride >= 4.
            // Reset per-frame coarse debug stats
            dbg_coarseStride = 0;
            dbg_coarseQuads = 0;

            if (false && worldStride >= 4) { // coarse renderer disabled for now (visuals + perf)
                // Snap bounds to stride grid
                auto snapDown = [&](int v,int s){ return (int)std::floor((float)v / (float)s) * s; };
                auto snapUp   = [&](int v,int s){ return (int)std::ceil ((float)v / (float)s) * s; };
                int sCoarse = worldStride;
                // Cap quad count to avoid touching too many chunks when far out
                auto computeBounds = [&](int s, int& iStart, int& iEnd, int& jStart, int& jEnd){
                    iStart = snapDown(Imin, s);
                    iEnd   = snapUp  (Imax, s);
                    jStart = snapDown(Jmin, s);
                    jEnd   = snapUp  (Jmax, s);
                };
                int iStart, iEnd, jStart, jEnd; computeBounds(sCoarse, iStart, iEnd, jStart, jEnd);
                auto quadCount = [&](int s){ return std::max(0, (iEnd - iStart) / s) * std::max(0, (jEnd - jStart) / s); };
                const int MAX_COARSE_QUADS = 3000; // ~6k triangles, safer on low zoom
                while (quadCount(sCoarse) > MAX_COARSE_QUADS && sCoarse < (1<<22)) {
                    sCoarse <<= 1; computeBounds(sCoarse, iStart, iEnd, jStart, jEnd);
                }
                dbg_coarseStride = sCoarse;
                dbg_coarseQuads = std::max(0, (iEnd - iStart) / sCoarse) * std::max(0, (jEnd - jStart) / sCoarse);

                sf::VertexArray tris(sf::Triangles);
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
                    float sea0=0.f,deepMin=-100.f,coast2=2.f,beach4=4.f,grass6=6.f,gray10=10.f,rock14=12.f,snow16=14.f;
                    auto lerp=[&](sf::Color a,sf::Color b,float t){t=std::clamp(t,0.f,1.f);auto L=[](uint8_t c){return (int)c;};return sf::Color((uint8_t)std::round(L(a.r)+(L(b.r)-L(a.r))*t),(uint8_t)std::round(L(a.g)+(L(b.g)-L(a.g))*t),(uint8_t)std::round(L(a.b)+(L(b.b)-L(a.b))*t),(uint8_t)std::round(L(a.a)+(L(b.a)-L(a.a))*t));};
                    if (h < sea0 && h >= deepMin) { float t = (sea0 - h) / std::max(0.001f, (sea0 - deepMin)); return lerp(normalBlue, veryDarkBlue, t); }
                    if (h >= sea0 && h < coast2) { float t = (h - sea0) / std::max(0.001f, (coast2 - sea0)); return lerp(normalBlue, sandYellow, t); }
                    if (h >= coast2 && h < beach4) { float t = (h - coast2) / std::max(0.001f, (beach4 - coast2)); return lerp(sandYellow, grass, t); }
                    if (h >= beach4 && h < grass6) return grass;
                    if (h >= grass6 && h <= gray10) { float t = (h - grass6) / std::max(0.001f, (gray10 - grass6)); return lerp(grass, gray, t); }
                    if (h > gray10 && h <= rock14) return rock;
                    if (h > rock14 && h < snow16) { float t = (h - rock14) / std::max(0.001f, (snow16 - rock14)); return lerp(gray, snow, t); }
                    if (h >= snow16) return snow;
                    return grass;
                };

                // Per-frame height cache to avoid generating/loading the same chunks repeatedly
                std::unordered_map<long long, int> hcache;
                auto key64 = [](int I, int J)->long long { return (((long long)I) << 32) ^ (unsigned long long)(uint32_t)J; };
                auto sampleH = [&](int I, int J)->int{
                    long long k = key64(I, J);
                    auto it = hcache.find(k);
                    if (it != hcache.end()) return it->second;
                    int h = getIntersectionHeight(I, J);
                    hcache.emplace(k, h);
                    return h;
                };

                for (int i = iStart; i < iEnd; i += sCoarse) {
                    for (int j = jStart; j < jEnd; j += sCoarse) {
                        // Corners in world grid space
                        int i1 = i + sCoarse;
                        int j1 = j + sCoarse;
                        // Project at zero elevation; shading uses average height
                        sf::Vector2f A = isoProjectDyn((float)i,  (float)j,  0.f, iso) + origin;
                        sf::Vector2f B = isoProjectDyn((float)i1, (float)j,  0.f, iso) + origin;
                        sf::Vector2f C = isoProjectDyn((float)i1, (float)j1, 0.f, iso) + origin;
                        sf::Vector2f D = isoProjectDyn((float)i,  (float)j1, 0.f, iso) + origin;
                        // Single height sample per quad for performance
                        float hCenter = (float)sampleH(i + sCoarse / 2, j + sCoarse / 2);
                        float shade = 0.85f; // constant shade in coarse mode
                        // Base color (skip paint/hover overlays in coarse mode for performance)
                        sf::Color base = colorForHeight(hCenter);
                        sf::Color c = multColor(base, shade);
                        // Two triangles A-B-C and A-C-D
                        tris.append(sf::Vertex(A, c));
                        tris.append(sf::Vertex(B, c));
                        tris.append(sf::Vertex(C, c));
                        tris.append(sf::Vertex(A, c));
                        tris.append(sf::Vertex(C, c));
                        tris.append(sf::Vertex(D, c));
                    }
                }
                if (tris.getVertexCount() > 0) window.draw(tris);
            } else {
                /* snapshot path removed */
                // Global batched impostor: if tiles are very small, draw one quad per visible chunk
                // in a single vertex array to minimize draw calls. Avoids loading chunk data.
                // Coarse impostor disabled
                if (false) {
                    // Build one big triangle list for all visible chunks
                    sf::VertexArray trisAll(sf::Triangles);
                    // Simple color mapping (same thresholds as normal path)
                    auto lerpColor = [](sf::Color a, sf::Color b, float t){
                        t = std::clamp(t, 0.f, 1.f);
                        auto L = [](uint8_t c){ return (int)c; };
                        return sf::Color(
                            (uint8_t)std::round(L(a.r) + (L(b.r) - L(a.r)) * t),
                            (uint8_t)std::round(L(a.g) + (L(b.g) - L(a.g)) * t),
                            (uint8_t)std::round(L(a.b) + (L(b.b) - L(a.b)) * t),
                            (uint8_t)std::round(L(a.a) + (L(b.a) - L(a.a)) * t)
                        );
                    };
                    auto colorForHeightQuick = [&](float h){
                        const sf::Color normalBlue(30, 144, 255);
                        const sf::Color veryDarkBlue(0, 0, 80);
                        const sf::Color sandYellow(255, 236, 170);
                        const sf::Color grass(34, 139, 34);
                        const sf::Color gray(128, 128, 128);
                        const sf::Color rock(110, 110, 110);
                        const sf::Color snow(245, 245, 245);
                        float sea0=0.f,deepMin=-100.f,coast2=2.f,beach4=4.f,grass6=6.f,gray10=10.f,rock14=12.f,snow16=14.f;
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
                    // Per-frame height cache to avoid repeated generation
                    std::unordered_map<long long, int> hcache;
                    auto key64 = [](int I, int J)->long long { return (((long long)I) << 32) ^ (unsigned long long)(uint32_t)J; };
                    auto sampleH = [&](int I, int J)->int{
                        long long k = key64(I, J);
                        auto it = hcache.find(k);
                        if (it != hcache.end()) return it->second;
                        int h = getIntersectionHeight(I, J);
                        hcache.emplace(k, h);
                        return h;
                    };

                    // Decide how many chunks to merge per impostor quad depending on zoom
                    int chunkStride = 2; // merge 2x2 chunks by default at far zoom
                    // Coarse grouping disabled when global impostor path is disabled
                    // (no-op since impostor branch is disabled)

                    // Iterate visible chunks by groups
                    for (int cx = cx0; cx <= cx1; cx += chunkStride) {
                        for (int cy = cy0; cy <= cy1; cy += chunkStride) {
                            int I0 = cx * cfg::CHUNK_SIZE;
                            int J0 = cy * cfg::CHUNK_SIZE;
                            int groupSize = cfg::CHUNK_SIZE * chunkStride;
                            // Project 0-elev group corners
                            sf::Vector2f A0 = isoProjectDyn((float)I0,               (float)J0,               0.f, iso) + origin;
                            sf::Vector2f B0 = isoProjectDyn((float)(I0 + groupSize), (float)J0,               0.f, iso) + origin;
                            sf::Vector2f C0 = isoProjectDyn((float)(I0 + groupSize), (float)(J0 + groupSize), 0.f, iso) + origin;
                            sf::Vector2f D0 = isoProjectDyn((float)I0,               (float)(J0 + groupSize), 0.f, iso) + origin;
                            float minx = std::min(std::min(A0.x, B0.x), std::min(C0.x, D0.x));
                            float maxx = std::max(std::max(A0.x, B0.x), std::max(C0.x, D0.x));
                            float miny = std::min(std::min(A0.y, B0.y), std::min(C0.y, D0.y));
                            float maxy = std::max(std::max(A0.y, B0.y), std::max(C0.y, D0.y));
                            sf::FloatRect chRect(minx, miny, maxx - minx, maxy - miny);
                            sf::FloatRect viewRect(vc.x - vs.x * 0.5f - 64.f,
                                                   vc.y - vs.y * 0.5f - 64.f,
                                                   vs.x + 128.f, vs.y + 128.f);
                            if (!chRect.intersects(viewRect)) continue;

                            // Sample at group center
                            int Ic = I0 + groupSize / 2;
                            int Jc = J0 + groupSize / 2;
                            float h = (float)sampleH(Ic, Jc);
                            sf::Color c = colorForHeightQuick(h);

                            trisAll.append(sf::Vertex(A0, c));
                            trisAll.append(sf::Vertex(B0, c));
                            trisAll.append(sf::Vertex(C0, c));
                            trisAll.append(sf::Vertex(A0, c));
                            trisAll.append(sf::Vertex(C0, c));
                            trisAll.append(sf::Vertex(D0, c));
                        }
                    }
                    if (trisAll.getVertexCount() > 0) window.draw(trisAll);
                    // Skip normal path entirely in this mode
                } else {
                for (int cx = cx0; cx <= cx1; ++cx) {
                for (int cy = cy0; cy <= cy1; ++cy) {
                    const Chunk& ch = chunkMgr.getChunk(cx, cy);
                    int I0 = cx * cfg::CHUNK_SIZE;
                    int J0 = cy * cfg::CHUNK_SIZE;
                    // Fast screen-space culling of the whole chunk using 0-elev corners
                    sf::Vector2f A0 = isoProjectDyn((float)I0,                     (float)J0,                      0.f, iso) + origin;
                    sf::Vector2f B0 = isoProjectDyn((float)(I0 + cfg::CHUNK_SIZE), (float)J0,                      0.f, iso) + origin;
                    sf::Vector2f C0 = isoProjectDyn((float)(I0 + cfg::CHUNK_SIZE), (float)(J0 + cfg::CHUNK_SIZE),  0.f, iso) + origin;
                    sf::Vector2f D0 = isoProjectDyn((float)I0,                     (float)(J0 + cfg::CHUNK_SIZE),  0.f, iso) + origin;
                    float minx = std::min(std::min(A0.x, B0.x), std::min(C0.x, D0.x));
                    float maxx = std::max(std::max(A0.x, B0.x), std::max(C0.x, D0.x));
                    float miny = std::min(std::min(A0.y, B0.y), std::min(C0.y, D0.y));
                    float maxy = std::max(std::max(A0.y, B0.y), std::max(C0.y, D0.y));
                    sf::FloatRect chRect(minx, miny, maxx - minx, maxy - miny);
                    // Reuse expanded view rect from earlier
                    sf::FloatRect viewRect(vc.x - vs.x * 0.5f - 64.f,
                                           vc.y - vs.y * 0.5f - 64.f,
                                           vs.x + 128.f, vs.y + 128.f);
                    if (!chRect.intersects(viewRect)) continue;
                    // If water-only: base is flat sea (0). Show only user edits (overrides) above sea.
                    if (waterOnly) {
                        // Water impostor disabled
                        if (false) {
                            // Use 0-elev projected corners computed above (A0..D0)
                            auto lerpColor = [](sf::Color a, sf::Color b, float t){
                                t = std::clamp(t, 0.f, 1.f);
                                auto L = [](uint8_t c){ return (int)c; };
                                return sf::Color(
                                    (uint8_t)std::round(L(a.r) + (L(b.r) - L(a.r)) * t),
                                    (uint8_t)std::round(L(a.g) + (L(b.g) - L(a.g)) * t),
                                    (uint8_t)std::round(L(a.b) + (L(b.b) - L(a.b)) * t),
                                    (uint8_t)std::round(L(a.a) + (L(b.a) - L(a.a)) * t)
                                );
                            };
                            auto colorForHeightQuick = [&](float h){
                                const sf::Color normalBlue(30, 144, 255);
                                const sf::Color veryDarkBlue(0, 0, 80);
                                const sf::Color sandYellow(255, 236, 170);
                                float sea0 = 0.f, deepMin = -100.f, coast2 = 2.f;
                                if (h < sea0 && h >= deepMin) { float t = (sea0 - h) / std::max(0.001f, (sea0 - deepMin)); return lerpColor(normalBlue, veryDarkBlue, t); }
                                if (h >= sea0 && h < coast2) { float t = (h - sea0) / std::max(0.001f, (coast2 - sea0)); return lerpColor(normalBlue, sandYellow, t); }
                                return sandYellow;
                            };
                            float hAvg = 0.f; // water surface baseline
                            sf::Color c = colorForHeightQuick(hAvg);
                            sf::VertexArray tris(sf::Triangles);
                            tris.append(sf::Vertex(A0, c));
                            tris.append(sf::Vertex(B0, c));
                            tris.append(sf::Vertex(C0, c));
                            tris.append(sf::Vertex(A0, c));
                            tris.append(sf::Vertex(C0, c));
                            tris.append(sf::Vertex(D0, c));
                            window.draw(tris);
                            continue; // next chunk
                        }
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
                        bool useStrided = (lodStride > 1);
                        bool shadowsThisPass = shadowsEnabled && !useStrided; // disable shadows when merged
                        if (useStrided) {
                            auto cMap2dS = render::buildProjectedMapChunkStrided(waterBuf, cfg::CHUNK_SIZE, I0, J0, iso, origin, 1.0f, lodStride);
                            render::draw2DFilledCellsChunkStrided(
                                window, cMap2dS, waterBuf, cfg::CHUNK_SIZE,
                                shadowsThisPass, 1.0f, lodStride, I0, J0,
                                &paintedCells,
                                (showColorHover && currentTool == Tool::Brush) ? &hoverMask : nullptr,
                                (showColorHover && currentTool == Tool::Brush) ? &activeColor : nullptr);
                        } else {
                            auto cMap2d = render::buildProjectedMapChunk(waterBuf, cfg::CHUNK_SIZE, I0, J0, iso, origin, 1.0f);
                            render::draw2DFilledCellsChunk(window, cMap2d, waterBuf, cfg::CHUNK_SIZE, shadowsThisPass, 1.0f, lodStride, I0, J0, &paintedCells,
                                                        (showColorHover && currentTool == Tool::Brush) ? &hoverMask : nullptr,
                                                        (showColorHover && currentTool == Tool::Brush) ? &activeColor : nullptr);
                        }
                        bool drawGrid = showGrid; // show grid at all LODs
                        if (drawGrid) {
                            auto cMap2d = render::buildProjectedMapChunk(waterBuf, cfg::CHUNK_SIZE, I0, J0, iso, origin, 1.0f);
                            render::draw2DMapChunk(window, cMap2d, lodStride);
                        }
                    } else {
                        // General impostor disabled
                        if (false) {
                            auto lerpColor = [](sf::Color a, sf::Color b, float t){
                                t = std::clamp(t, 0.f, 1.f);
                                auto L = [](uint8_t c){ return (int)c; };
                                return sf::Color(
                                    (uint8_t)std::round(L(a.r) + (L(b.r) - L(a.r)) * t),
                                    (uint8_t)std::round(L(a.g) + (L(b.g) - L(a.g)) * t),
                                    (uint8_t)std::round(L(a.b) + (L(b.b) - L(a.b)) * t),
                                    (uint8_t)std::round(L(a.a) + (L(b.a) - L(a.a)) * t)
                                );
                            };
                            auto colorForHeightQuick = [&](float h){
                                const sf::Color normalBlue(30, 144, 255);
                                const sf::Color veryDarkBlue(0, 0, 80);
                                const sf::Color sandYellow(255, 236, 170);
                                const sf::Color grass(34, 139, 34);
                                const sf::Color gray(128, 128, 128);
                                const sf::Color rock(110, 110, 110);
                                const sf::Color snow(245, 245, 245);
                                float sea0=0.f,deepMin=-100.f,coast2=2.f,beach4=4.f,grass6=6.f,gray10=10.f,rock14=12.f,snow16=14.f;
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
                            // Sample a few points to approximate average height
                            int S = cfg::CHUNK_SIZE;
                            int W = S + 1;
                            auto idc = [&](int i, int j){ return i * W + j; };
                            auto clampIJ = [&](int v){ return std::clamp(v, 0, S); };
                            int i0 = 0, j0 = 0;
                            int i1 = S, j1 = 0;
                            int i2 = S, j2 = S;
                            int i3 = 0, j3 = S;
                            int ic = S/2, jc = S/2;
                            float hA = (float)ch.heights[idc(clampIJ(i0), clampIJ(j0))];
                            float hB = (float)ch.heights[idc(clampIJ(i1), clampIJ(j1))];
                            float hC = (float)ch.heights[idc(clampIJ(i2), clampIJ(j2))];
                            float hD = (float)ch.heights[idc(clampIJ(i3), clampIJ(j3))];
                            float hE = (float)ch.heights[idc(clampIJ(ic), clampIJ(jc))];
                            float hAvg = (hA + hB + hC + hD + 2.f*hE) / 6.f; // center weighted
                            sf::Color c = colorForHeightQuick(hAvg);
                            sf::VertexArray tris(sf::Triangles);
                            tris.append(sf::Vertex(A0, c));
                            tris.append(sf::Vertex(B0, c));
                            tris.append(sf::Vertex(C0, c));
                            tris.append(sf::Vertex(A0, c));
                            tris.append(sf::Vertex(C0, c));
                            tris.append(sf::Vertex(D0, c));
                            window.draw(tris);
                            continue; // next chunk
                        }
                        bool useStrided = (lodStride > 1);
                        bool shadowsThisPass = shadowsEnabled && !useStrided; // disable shadows when merged
                        if (useStrided) {
                            auto cMap2dS = render::buildProjectedMapChunkStrided(ch.heights, cfg::CHUNK_SIZE, I0, J0, iso, origin, 1.0f, lodStride);
                            render::draw2DFilledCellsChunkStrided(
                                window, cMap2dS, ch.heights, cfg::CHUNK_SIZE,
                                shadowsThisPass, 1.0f, lodStride, I0, J0,
                                &paintedCells,
                                (showColorHover && currentTool == Tool::Brush) ? &hoverMask : nullptr,
                                (showColorHover && currentTool == Tool::Brush) ? &activeColor : nullptr);
                        } else {
                            auto cMap2d = render::buildProjectedMapChunk(ch.heights, cfg::CHUNK_SIZE, I0, J0, iso, origin, 1.0f);
                            render::draw2DFilledCellsChunk(window, cMap2d, ch.heights, cfg::CHUNK_SIZE, shadowsThisPass, 1.0f, lodStride, I0, J0, &paintedCells,
                                                       (showColorHover && currentTool == Tool::Brush) ? &hoverMask : nullptr,
                                                       (showColorHover && currentTool == Tool::Brush) ? &activeColor : nullptr);
                        }
                        bool drawGrid = showGrid; // show grid at all LODs
                        if (drawGrid) {
                            auto cMap2d = render::buildProjectedMapChunk(ch.heights, cfg::CHUNK_SIZE, I0, J0, iso, origin, 1.0f);
                            render::draw2DMapChunk(window, cMap2d, lodStride);
                        }
                    }
                }
            }
        }
        }
        }
        else {
            // Non-procedural path: use global heights buffer
            auto map2d = render::buildProjectedMap(heights, iso, origin, 1.0f);
            render::draw2DFilledCells(window, map2d, heights, shadowsEnabled, 1.0f, &paintedCells, 
                                      (showColorHover && currentTool == Tool::Brush) ? &hoverMask : nullptr,
                                      (showColorHover && currentTool == Tool::Brush) ? &activeColor : nullptr);
            if (showGrid) render::draw2DMap(window, map2d);
        }


        // Snapshot capture removed

        // Draw Locator markers and cursor (world view)
        {
            auto sampleHeightAt = [&](int I, int J)->int{
                if (proceduralMode) {
                    auto floorDiv = [](int a, int b){ return (a >= 0) ? (a / b) : ((a - (b - 1)) / b); };
                    int cx = floorDiv(I, cfg::CHUNK_SIZE);
                    int cy = floorDiv(J, cfg::CHUNK_SIZE);
                    int li = I - cx * cfg::CHUNK_SIZE;
                    int lj = J - cy * cfg::CHUNK_SIZE;
                    li = std::clamp(li, 0, cfg::CHUNK_SIZE);
                    lj = std::clamp(lj, 0, cfg::CHUNK_SIZE);
                    const Chunk& ch = chunkMgr.getChunk(cx, cy);
                    int k = li * (cfg::CHUNK_SIZE + 1) + lj;
                    return ch.heights[k];
                } else {
                    int Icl = std::clamp(I, 0, gridSide);
                    int Jcl = std::clamp(J, 0, gridSide);
                    return heights[idx(Icl, Jcl)];
                }
            };
            // Compute zoom scale to size markers/text so they remain visible when zooming out
            sf::Vector2f vsz = view.getSize();
            sf::Vector2f dsz = window.getDefaultView().getSize();
            float zoomScale = std::max(vsz.x / std::max(1.f, dsz.x), vsz.y / std::max(1.f, dsz.y));
            float visScale = std::clamp(zoomScale, 1.f, 8.f);

            // Draw placed markers
            for (size_t mi = 0; mi < markers.size(); ++mi) {
                const auto& m = markers[mi];
                int h = sampleHeightAt(m.I, m.J);
                sf::Vector2f p = isoProjectDyn((float)m.I, (float)m.J, (float)h, iso) + origin;
                // diamond (bicone-like) marker
                float r = 5.f * visScale;
                sf::ConvexShape d; d.setPointCount(4);
                d.setPoint(0, {p.x, p.y - r});
                d.setPoint(1, {p.x + r, p.y});
                d.setPoint(2, {p.x, p.y + r});
                d.setPoint(3, {p.x - r, p.y});
                d.setFillColor(m.color);
                // Thicker outlines; especially when marker is in edit mode
                float baseOutline = std::max(1.f, 0.6f * visScale);
                if (labelEditing && (int)mi == labelEditIndex) baseOutline = std::max(baseOutline, 3.0f * visScale);
                d.setOutlineThickness(baseOutline);
                d.setOutlineColor(sf::Color::Black);
                window.draw(d);
                // label above
                if (fontLoaded) {
                    sf::Text t;
                    t.setFont(uiFont);
                    unsigned int cs = (unsigned int)std::clamp(16.f * visScale, 12.f, 64.f);
                    t.setCharacterSize(cs);
                    t.setFillColor(sf::Color::White);
                    std::string text = (labelEditing && (int)mi == labelEditIndex) ? labelBuffer : m.label;
                    t.setString(text);
                    auto b = t.getLocalBounds();
                    t.setOrigin(b.left + b.width * 0.5f, b.top + b.height);
                    t.setPosition(p.x, p.y - r - 4.f * visScale);
                    // shadow
                    sf::Text ts = t; ts.setFillColor(sf::Color(0,0,0,120)); ts.move(1.f * visScale,1.f * visScale); window.draw(ts);
                    window.draw(t);
                    // icon above label if any
                    if (!m.icon.empty()) {
                        // find icon
                        const sf::Texture* texPtr = nullptr;
                        for (const auto& it : markerIcons) { if (it.name == m.icon) { texPtr = &it.tex; break; } }
                        if (texPtr && texPtr->getSize().x > 0 && texPtr->getSize().y > 0) {
                            sf::Sprite spr(*texPtr);
                            auto tsz = texPtr->getSize();
                            float sc = (32.f * visScale) / std::max(1u, std::max(tsz.x, tsz.y));
                            spr.setScale(sc, sc);
                            auto labelPos = t.getPosition();
                            spr.setPosition(labelPos.x - (tsz.x*sc)*0.5f, labelPos.y - 18.f*visScale - (tsz.y*sc) - 4.f*visScale);
                            window.draw(spr);
                        } else {
                            if (missingMarkerIconWarned.insert(m.icon).second) {
                                std::cerr << "[Marker Icon] Missing or unloaded icon texture: '" << m.icon << "'\n";
                            }
                        }
                    }
                }
            }
            // Draw locator cursor under mouse
            if (currentTool == Tool::Locator) {
                sf::Vector2i mp = sf::Mouse::getPosition(window);
                sf::Vector2f world = window.mapPixelToCoords(mp, view);
                if (pointInsideGrid(world)) {
                    sf::Vector2i IJ = pickIJAccurate(world);
                    int h = sampleHeightAt(IJ.x, IJ.y);
                    sf::Vector2f p = isoProjectDyn((float)IJ.x, (float)IJ.y, (float)h, iso) + origin;
                    float rr = 7.f * visScale;
                    sf::ConvexShape d; d.setPointCount(4);
                    d.setPoint(0, {p.x, p.y - rr});
                    d.setPoint(1, {p.x + rr, p.y});
                    d.setPoint(2, {p.x, p.y + rr});
                    d.setPoint(3, {p.x - rr, p.y});
                    d.setFillColor(sf::Color(255, 255, 255, 30));
                    d.setOutlineThickness(std::max(1.5f, 2.f * visScale * 0.5f));
                    d.setOutlineColor(sf::Color(100,180,255,220));
                    window.draw(d);
                }
            }

            // Draw eraser footprint under mouse matching brush size/shape
            if (currentTool == Tool::Eraser) {
                sf::Vector2i mp = sf::Mouse::getPosition(window);
                sf::Vector2f world = window.mapPixelToCoords(mp, view);
                if (pointInsideGrid(world)) {
                    sf::Vector2i IJ = pickIJAccurate(world);
                    int brush = std::clamp(brushSize, brushMin, brushMax);
                    int half = brush - 1;
                    for (int di = -brush; di <= brush; ++di) {
                        for (int dj = -brush; dj <= brush; ++dj) {
                            float w = weightForHardness(currentBrushShape, di, dj, half);
                            if (w <= 0.f) continue;
                            int I = IJ.x + di; int J = IJ.y + dj;
                            int h = sampleHeightAt(I, J);
                            sf::Vector2f p = isoProjectDyn((float)I, (float)J, (float)h, iso) + origin;
                            // small dot per cell in footprint
                            float rpx = std::max(1.0f, 1.2f * visScale);
                            sf::CircleShape dot(rpx);
                            dot.setOrigin(rpx, rpx);
                            dot.setPosition(p);
                            sf::Uint8 a = (sf::Uint8)std::clamp((int)std::round(w * 200.f), 30, 220);
                            dot.setFillColor(sf::Color(255, 255, 255, a));
                            dot.setOutlineThickness(0.f);
                            window.draw(dot);
                        }
                    }
                }
            }
        }

        // Draw UI in screen space (default view)
        sf::View oldView = window.getView();
        window.setView(window.getDefaultView());
        // Snapshot draw removed
        // Update visual style based on hover
        if (genHover) {
            btnGenerate.setFillColor(sf::Color(50, 50, 50, 230));
        } else {
            btnGenerate.setFillColor(sf::Color(30, 30, 30, 200));
        }
        // Ensure color picker is visible for Brush and Locator tools
        bool wantPicker = (currentTool == Tool::Brush || currentTool == Tool::Locator);
        showColorPicker = wantPicker;
        showColorHover = wantPicker;

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
        // Status/progress overlay (top-left)
        if (fontLoaded) {
            float t = statusClock.getElapsedTime().asSeconds();
            if (!statusMsg.empty() && t < statusDuration) {
                statusText.setPosition(10.f, 8.f);
                auto b = statusText.getLocalBounds();
                sf::RectangleShape bg(sf::Vector2f(b.width + 16.f, b.height + 12.f));
                bg.setPosition(6.f, 4.f);
                // Fade tail alpha
                float solidTime = std::max(0.f, statusDuration - statusFadeTail);
                float alphaF = (t <= solidTime) ? 1.f : std::max(0.f, (statusDuration - t) / std::max(0.001f, statusFadeTail));
                auto mulA = [&](sf::Color c, float f){ c.a = (sf::Uint8)std::clamp(int(c.a * f), 0, 255); return c; };
                bg.setFillColor(mulA(sf::Color(0,0,0,160), alphaF));
                window.draw(bg);
                sf::Color txt = mulA(sf::Color::White, alphaF);
                sf::Color outl = mulA(sf::Color::Black, alphaF);
                statusText.setFillColor(txt);
                statusText.setOutlineColor(outl);
                window.draw(statusText);
                // Reset colors to default opaque for next use
                statusText.setFillColor(sf::Color::White);
                statusText.setOutlineColor(sf::Color::Black);
            }
        }
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
            // Always draw F11 line just above bottom margin
            helpF11.setPosition(16.f, baseY - 20.f);
            window.draw(helpF11);
            // Contextual second line
            if (currentTool == Tool::Bulldozer) {
                helpCtrl.setPosition(16.f, baseY - 40.f);
                window.draw(helpCtrl);
            } else if (currentTool == Tool::Locator && labelEditing) {
                // Place marker edit help above F11, accounting for two-line height
                auto hb = helpMarkerEdit.getLocalBounds();
                float offset = hb.height + 8.f; // 4px margin above F11 per line
                helpMarkerEdit.setPosition(16.f, (baseY - 20.f) - offset);
                window.draw(helpMarkerEdit);
            }
        }

        // Draw FPS (bottom-right). Keep other debug overlays disabled.
        if (fontLoaded) {
            auto tb = fpsText.getLocalBounds();
            float fx = (float)wsz.x - 16.f - tb.width;
            float fy = (float)wsz.y - 16.f - tb.height;
            fpsText.setPosition(fx, fy);
            window.draw(fpsText);
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
            // Remove on-screen debug: do not draw FPS/zoom overlays
            // Keep FPS computation for performance gating, but skip rendering any debug text
        }

        // (height slider removed)

        // Inventory toolbar (bottom-center) rendering
        {
            auto [rBulldozer, rBrush, rEraser, rLocator, rPipette] = inventoryRects();
            // Background bar spans all five
            float pad = 8.f;
            float left = std::min(std::min(std::min(rBulldozer.left, rBrush.left), std::min(rEraser.left, rLocator.left)), rPipette.left);
            float right = std::max(std::max(std::max(rBulldozer.left + rBulldozer.width, rBrush.left + rBrush.width), std::max(rEraser.left + rEraser.width, rLocator.left + rLocator.width)), rPipette.left + rPipette.width);
            float top = std::min(std::min(std::min(rBulldozer.top, rBrush.top), std::min(rEraser.top, rLocator.top)), rPipette.top);
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
            drawSlot(rLocator,    sprLocator,    currentTool == Tool::Locator);
            drawSlot(rPipette,    sprPipette,    currentTool == Tool::Pipette);

            // Update hover flag for brush color hover each frame
            showColorHover = (currentTool == Tool::Brush);
        }

        // Size slider (right side) - visible for Brush, Bulldozer and Eraser tools
        if (currentTool == Tool::Brush || currentTool == Tool::Bulldozer || currentTool == Tool::Eraser) {
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
                label.setString(U8("Size"));
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
        }
        // Draw color picker and history (screen space) for Brush, Locator and Pipette tools
        if (currentTool == Tool::Brush || currentTool == Tool::Locator || currentTool == Tool::Pipette) {
            ensureColorWheel();
            loadMarkerIcons();
            float leftX = 16.f;
            float basePanelW = 140.f;
            float panelW = basePanelW;
            float wheelTop = btnBake.getPosition().y + btnBake.getSize().y + 16.f;
            sf::Vector2f wheelCenter(leftX + panelW*0.5f, wheelTop + (float)colorWheelRadius);
            // Background panel
            float panelH;
            if (currentTool == Tool::Brush) {
                panelH = (colorWheelRadius*2.f + 56.f + 34.f + 28.f + 16.f + 24.f);
            } else if (currentTool == Tool::Locator) {
                // wheel + history + grid viewport (5 rows) + margins; fixed 32px icons
                const int cols = 3; const float gapG = 6.f; const float sh = 22.f;
                const float cell = 32.f;
                const int rowsVisible = 5;
                float viewH = rowsVisible*cell + (rowsVisible-1)*gapG;
                panelH = (colorWheelRadius*2.f + 12.f + sh + 10.f + viewH + 12.f);
            } else {
                panelH = (colorWheelRadius*2.f + 56.f);
            }
            sf::RectangleShape cpBG(sf::Vector2f(panelW, panelH));
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
            // Icons grid for Locator
            if (currentTool == Tool::Locator) {
                // Layout
                const int cols = 3;
                const float gap = 6.f;
                const float cell = 32.f;
                float gridW = cols*cell + (cols-1)*gap;
                float gx = leftX + (panelW - gridW) * 0.5f;
                float gy = hy + sh + 10.f;
                // Viewport height: show 5 rows
                const int rowsVisible = 5;
                float viewH = rowsVisible*cell + (rowsVisible-1)*gap;
                // Border
                sf::RectangleShape vp(sf::Vector2f(gridW, viewH));
                vp.setPosition(gx, gy);
                vp.setFillColor(sf::Color(40,40,40,220));
                vp.setOutlineThickness(1.f);
                vp.setOutlineColor(sf::Color(160,160,160));
                window.draw(vp);
                // Compute content (include a synthetic first cell = None)
                int count = (int)markerIcons.size() + 1;
                int rows = (count + cols - 1) / cols;
                float contentH = rows*cell + std::max(0, rows-1)*gap;
                // Clamp scroll
                markerIconsScroll = std::clamp(markerIconsScroll, 0.f, std::max(0.f, contentH - viewH));
                // Determine hovered cell
                sf::Vector2i mp = sf::Mouse::getPosition(window);
                sf::Vector2f mpos = window.mapPixelToCoords(mp, window.getDefaultView());
                int hoveredIndex = -1;
                if (sf::FloatRect(gx, gy, gridW, viewH).contains(mpos)) {
                    float localY = (mpos.y - gy) + markerIconsScroll;
                    int hc = (int)((mpos.x - gx) / (cell + gap));
                    int hr = (int)(localY / (cell + gap));
                    if (hc >= 0 && hc < cols && hr >= 0) {
                        int hidx = hr*cols + hc;
                        if (hidx >= 0 && hidx < count) hoveredIndex = hidx;
                    }
                }
                // Hover no longer changes icon; selection is applied on click and remains locked
                // Selected icon name (for highlight)
                std::string selectedName;
                if (labelEditing && labelEditIndex >= 0 && labelEditIndex < (int)markers.size()) {
                    selectedName = markers[labelEditIndex].icon;
                } else {
                    selectedName = currentMarkerIcon;
                }
                // Draw visible icons (no stencil; manual cull)
                for (int idx = 0; idx < count; ++idx) {
                    int r = idx / cols, c = idx % cols;
                    float x = gx + c*(cell+gap);
                    float y = gy + r*(cell+gap) - markerIconsScroll;
                    // cull
                    if (y + cell < gy || y > gy + viewH) continue;
                    sf::RectangleShape slot(sf::Vector2f(cell, cell));
                    slot.setPosition(x, y);
                    slot.setFillColor(sf::Color(50,50,50,180));
                    slot.setOutlineThickness(1.f);
                    slot.setOutlineColor(sf::Color(20,20,20,200));
                    window.draw(slot);
                    if (idx == 0) {
                        // None cell: draw a crossed box
                        sf::RectangleShape inner(sf::Vector2f(cell-8.f, cell-8.f));
                        inner.setPosition(x+4.f, y+4.f);
                        inner.setFillColor(sf::Color(70,70,70,180));
                        inner.setOutlineThickness(1.f);
                        inner.setOutlineColor(sf::Color(120,120,120,200));
                        window.draw(inner);
                        sf::Vertex lines[4] = {
                            sf::Vertex(sf::Vector2f(x+6.f, y+6.f), sf::Color(150,150,150,220)),
                            sf::Vertex(sf::Vector2f(x+cell-6.f, y+cell-6.f), sf::Color(150,150,150,220)),
                            sf::Vertex(sf::Vector2f(x+cell-6.f, y+6.f), sf::Color(150,150,150,220)),
                            sf::Vertex(sf::Vector2f(x+6.f, y+cell-6.f), sf::Color(150,150,150,220))
                        };
                        window.draw(lines, 2, sf::Lines);
                        window.draw(lines+2, 2, sf::Lines);
                    } else {
                        const auto& it = markerIcons[idx-1];
                        if (it.tex.getSize().x > 0) {
                            sf::Sprite s(it.tex);
                            auto tsz = it.tex.getSize();
                            float sx = cell / (float)tsz.x;
                            float sy = cell / (float)tsz.y;
                            float sc = std::min(sx, sy);
                            s.setScale(sc, sc);
                            s.setPosition(x + (cell - tsz.x*sc)*0.5f, y + (cell - tsz.y*sc)*0.5f);
                            window.draw(s);
                        }
                    }
                    // Hover/selected outlines
                    bool isSelected = (idx == 0 ? selectedName.empty() : (!selectedName.empty() && idx > 0 && markerIcons[idx-1].name == selectedName));
                    if (idx == hoveredIndex) {
                        sf::RectangleShape hov(sf::Vector2f(cell, cell));
                        hov.setPosition(x, y);
                        hov.setFillColor(sf::Color::Transparent);
                        hov.setOutlineThickness(2.f);
                        hov.setOutlineColor(sf::Color(180,180,180,200));
                        window.draw(hov);
                    }
                    if (isSelected) {
                        sf::RectangleShape sel(sf::Vector2f(cell, cell));
                        sel.setPosition(x, y);
                        sel.setFillColor(sf::Color::Transparent);
                        sel.setOutlineThickness(3.f);
                        sel.setOutlineColor(sf::Color(80,200,255,230));
                        window.draw(sel);
                    }
                }
                // Hover tooltip: show filename (or None) near cursor
                if (hoveredIndex >= 0) {
                    std::string tip;
                    if (hoveredIndex == 0) {
                        tip = "None";
                    } else {
                        if (hoveredIndex - 1 < (int)markerIcons.size()) {
                            tip = markerIcons[hoveredIndex - 1].name;
                            // strip extension for brevity
                            size_t dot = tip.find_last_of('.'); if (dot != std::string::npos) tip.erase(dot);
                        }
                    }
                    if (!tip.empty() && fontLoaded) {
                        sf::Text tt; tt.setFont(uiFont); tt.setString(tip); tt.setCharacterSize(12);
                        tt.setFillColor(sf::Color::White);
                        sf::FloatRect tb = tt.getLocalBounds();
                        sf::Vector2f pos = mpos + sf::Vector2f(12.f, 12.f);
                        float pad = 6.f; sf::RectangleShape bg(sf::Vector2f(tb.width + 2*pad, tb.height + 2*pad));
                        bg.setPosition(pos.x - pad + tb.left, pos.y - pad + tb.top);
                        bg.setFillColor(sf::Color(20,20,20,220));
                        bg.setOutlineThickness(1.f); bg.setOutlineColor(sf::Color(80,80,80));
                        window.draw(bg);
                        tt.setPosition(pos);
                        window.draw(tt);
                    }
                }
                // Adjust panel height to include grid
                // Redraw panel border extension if necessary (optional visual)
            }
            if (currentTool == Tool::Brush) {
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

                // Brush shape selector (4 buttons below tone slider)
                float shapesTop = toneY + toneH + 10.f;
                float bw = 28.f, bh = 28.f, bgap = 6.f;
                float totalBW = 4*bw + 3*bgap;
                float bx = leftX + (panelW - totalBW) * 0.5f;
                auto drawShapeBtn = [&](const sf::FloatRect& r, BrushShape bs, bool selected){
                    sf::RectangleShape box(sf::Vector2f(r.width, r.height));
                    box.setPosition(r.left, r.top);
                    box.setFillColor(sf::Color(40,40,40,220));
                    box.setOutlineThickness(selected ? 2.f : 1.f);
                    box.setOutlineColor(selected ? sf::Color(100,180,255) : sf::Color(150,150,150));
                    window.draw(box);
                    // draw symbol
                    float cx = r.left + r.width * 0.5f;
                    float cy = r.top  + r.height* 0.5f;
                    if (bs == BrushShape::Square) {
                        sf::RectangleShape s(sf::Vector2f(14.f, 14.f));
                        s.setOrigin(7.f,7.f); s.setPosition(cx, cy);
                        s.setFillColor(sf::Color(200,200,200));
                        window.draw(s);
                    } else if (bs == BrushShape::Circle) {
                        sf::CircleShape c(8.f); c.setOrigin(8.f,8.f); c.setPosition(cx, cy);
                        c.setFillColor(sf::Color(200,200,200));
                        window.draw(c);
                    } else if (bs == BrushShape::Manhattan) {
                        sf::ConvexShape d; d.setPointCount(4);
                        d.setPoint(0, {cx, cy-9.f}); d.setPoint(1, {cx+9.f, cy}); d.setPoint(2, {cx, cy+9.f}); d.setPoint(3, {cx-9.f, cy});
                        d.setFillColor(sf::Color(200,200,200));
                        window.draw(d);
                    } else if (bs == BrushShape::Gaussian) {
                        // draw a small point cloud deterministically
                        auto h01 = [](int x, int y, int salt)->float{
                            uint32_t h = (uint32_t)(x * 374761393u ^ y * 668265263u ^ 0x9E3779B9u ^ (uint32_t)salt);
                            h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
                            return (h & 0xFFFFFFu) / 16777215.f;
                        };
                        for (int n=0; n<12; ++n){
                            float u = h01((int)r.left + n*17, (int)r.top + n*31, 2025);
                            float v = h01((int)r.left + n*41, (int)r.top + n*29, 6061);
                            float px = r.left + 4.f + u * (r.width - 8.f);
                            float py = r.top  + 4.f + v * (r.height- 8.f);
                            sf::CircleShape dot(2.f); dot.setOrigin(2.f,2.f); dot.setPosition(px, py);
                            dot.setFillColor(sf::Color(200,200,200));
                            window.draw(dot);
                        }
                    }
                };
                sf::FloatRect rSq(bx + 0*(bw+bgap), shapesTop, bw, bh);
                sf::FloatRect rCi(bx + 1*(bw+bgap), shapesTop, bw, bh);
                sf::FloatRect rMa(bx + 2*(bw+bgap), shapesTop, bw, bh);
                sf::FloatRect rGa(bx + 3*(bw+bgap), shapesTop, bw, bh);
                drawShapeBtn(rSq, BrushShape::Square,    currentBrushShape == BrushShape::Square);
                drawShapeBtn(rCi, BrushShape::Circle,    currentBrushShape == BrushShape::Circle);
                drawShapeBtn(rMa, BrushShape::Manhattan, currentBrushShape == BrushShape::Manhattan);
                drawShapeBtn(rGa, BrushShape::Gaussian,  currentBrushShape == BrushShape::Gaussian);

                // Hardness slider (below shapes)
                float hardnessY = shapesTop + bh + 10.f;
                float hardH = 14.f;
                sf::FloatRect hardRect(leftX, hardnessY, panelW, hardH);
                // Bar background
                sf::RectangleShape hardBG(sf::Vector2f(hardRect.width, hardRect.height));
                hardBG.setPosition(hardRect.left, hardRect.top);
                hardBG.setFillColor(sf::Color(40,40,40,220));
                hardBG.setOutlineThickness(1.f);
                hardBG.setOutlineColor(sf::Color(160,160,160));
                window.draw(hardBG);
                // Fill according to hardness
                float fillW = std::clamp(brushHardness, 0.f, 1.f) * hardRect.width;
                sf::RectangleShape hardFill(sf::Vector2f(std::max(0.f, fillW), hardRect.height-2.f));
                hardFill.setPosition(hardRect.left+1.f, hardRect.top+1.f);
                hardFill.setFillColor(sf::Color(100,180,255,200));
                window.draw(hardFill);
                // Handle
                float hxpos = hardRect.left + fillW;
                sf::RectangleShape hardHandle(sf::Vector2f(2.f, hardRect.height));
                hardHandle.setPosition(hxpos - 1.f, hardRect.top);
                hardHandle.setFillColor(sf::Color::White);
                window.draw(hardHandle);
            }
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
