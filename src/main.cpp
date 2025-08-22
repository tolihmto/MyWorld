#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif
#include "config.hpp"
#include "iso.hpp"
#include "terrain.hpp"
#include "render.hpp"

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
    sf::RenderWindow window(sf::VideoMode(cfg::WINDOW_W, cfg::WINDOW_H), "MyWorld - SFML Isometric Grid");
    window.setFramerateLimit(120);

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

    

    // UTF-8 helper
    auto U8 = [&](const std::string& s){ return sf::String::fromUtf8(s.begin(), s.end()); };

    // --- UI: "Générer" button ---
    sf::Font uiFont;
    bool fontLoaded = uiFont.loadFromFile("assets/fonts/arial.ttf");
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
    sf::Text btnText;
    sf::Text btnGridText;
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
    }

    bool genHover = false;  // hover state for the Generate button
    bool gridHover = false; // hover state for the Grid button
    bool showGrid = false;   // toggle wireframe visibility (default OFF)
    bool shadowsEnabled = false; // F2 toggles shadows (default OFF)

    // Brush size & slider UI (right side)
    int brushSize = 2;                 // radius in intersections
    const int brushMin = 1;
    const int brushMax = 8;
    bool brushDragging = false;
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

    // FPS counter state
    sf::Clock fpsClock;
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
        // Map value [brushMin..brushMax] -> y position along track (top=small, bottom=large)
        float t = (float)(value - brushMin) / (float)(brushMax - brushMin);
        float thumbH = 18.f, thumbW = 26.f;
        float cx = tr.left + tr.width * 0.5f;
        float y = tr.top + t * tr.height;
        return sf::FloatRect(cx - thumbW * 0.5f, y - thumbH * 0.5f, thumbW, thumbH);
    };
    auto sliderPickValue = [&](sf::Vector2f p)->int{
        sf::FloatRect tr = sliderTrackRect();
        float clampedY = std::clamp(p.y, tr.top, tr.top + tr.height);
        float t = (tr.height <= 0.f) ? 0.f : (clampedY - tr.top) / tr.height;
        int v = brushMin + (int)std::round(t * (brushMax - brushMin));
        return std::clamp(v, brushMin, brushMax);
    };

    // Panning vars
    bool panning = false;
    bool tilting = false;
    sf::Vector2i panStartMouse;
    sf::Vector2f panStartCenter;
    sf::Vector2i tiltStartMouse;
    float tiltStartRot = 0.f;
    float tiltStartPitch = 1.f;

    

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
        I = std::clamp(I, 0, cfg::GRID);
        J = std::clamp(J, 0, cfg::GRID);
        return {I, J};
    };

    auto pointInsideGrid = [&](sf::Vector2f world)->bool {
        sf::Vector2f local = world - origin;
        sf::Vector2f ij = isoUnprojectDyn(local, iso);
        return (ij.x >= 0.f && ij.y >= 0.f && ij.x <= (float)cfg::GRID && ij.y <= (float)cfg::GRID);
    };

    // Rendering moved to render::*

    // Main loop
    while (window.isOpen()) {
        sf::Event ev;
        while (window.pollEvent(ev)) {
            switch (ev.type) {
                case sf::Event::Closed:
                    window.close();
                    break;
                case sf::Event::KeyPressed:
                    if (ev.key.code == sf::Keyboard::Escape) window.close();
                    if (ev.key.code == sf::Keyboard::F11) {
                        recreateWindow(!isFullscreen);
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
                        
                    }
                    if (ev.key.code == sf::Keyboard::G) {
                        // keyboard shortcut to generate
                        uint32_t seed = (uint32_t)std::rand();
                        generateMap(seed);
                    }
                    if (ev.key.code == sf::Keyboard::F3) {
                        showGrid = !showGrid;
                    }
                    if (ev.key.code == sf::Keyboard::F2) {
                        shadowsEnabled = !shadowsEnabled;
                    }
                    break;
                case sf::Event::MouseWheelScrolled:
                    if (ev.mouseWheelScroll.delta > 0) view.zoom(0.9f);
                    else view.zoom(1.1f);
                    window.setView(view);
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
                        if (btnGenerate.getGlobalBounds().contains(screen)) {
                            uint32_t seed = (uint32_t)std::rand();
                            generateMap(seed);
                            break;
                        }
                        if (btnGrid.getGlobalBounds().contains(screen)) {
                            showGrid = !showGrid;
                            break;
                        }
                        // Import/Export buttons (top-right, round white)
                        if (circleContains(exportBtnPos, btnRadius, screen)) {
                            std::string path = saveFileDialogCSV();
                            if (!path.empty()) exportCSV(path);
                            break;
                        }
                        if (circleContains(importBtnPos, btnRadius, screen)) {
                            std::string path = openFileDialogCSV();
                            if (!path.empty()) beginImport(path);
                            break;
                        }
                        // Brush slider interaction (right side)
                        if (sliderTrackRect().contains(screen) || sliderThumbRect(brushSize).contains(screen)) {
                            brushDragging = true;
                            brushSize = sliderPickValue(screen);
                            break;
                        }
                        // Then, world interactions
                        sf::Vector2f world = window.mapPixelToCoords(mp, view);
                        if (pointInsideGrid(world)) {
                            // Elevation edit around nearest intersection (brush)
                            sf::Vector2i IJ = worldToGridIntersection(world);
                            int brush = std::clamp(brushSize, brushMin, brushMax);
                            bool ctrl = sf::Keyboard::isKeyPressed(sf::Keyboard::LControl) || sf::Keyboard::isKeyPressed(sf::Keyboard::RControl);
                            if (ctrl && ev.mouseButton.button == sf::Mouse::Left) {
                                // Capture flatten reference height on first Ctrl+click
                                flattenHeight = heights[idx(IJ.x, IJ.y)];
                                flattenPrimed = true;
                                // Immediately flatten current brush area
                                for (int di = -brush; di <= brush; ++di) {
                                    for (int dj = -brush; dj <= brush; ++dj) {
                                        int I = IJ.x + di;
                                        int J = IJ.y + dj;
                                        if (I < 0 || J < 0 || I > cfg::GRID || J > cfg::GRID) continue;
                                        if (di*di + dj*dj > brush*brush) continue;
                                        heights[idx(I, J)] = flattenHeight;
                                    }
                                }
                                
                            } else {
                                int delta = (ev.mouseButton.button == sf::Mouse::Left) ? 1 : -1;
                                for (int di = -brush; di <= brush; ++di) {
                                    for (int dj = -brush; dj <= brush; ++dj) {
                                        int I = IJ.x + di;
                                        int J = IJ.y + dj;
                                        if (I < 0 || J < 0 || I > cfg::GRID || J > cfg::GRID) continue;
                                        // circular brush shape
                                        if (di*di + dj*dj > brush*brush) continue;
                                        heights[idx(I, J)] += delta;
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
                    brushDragging = false;
                    break;
                case sf::Event::MouseMoved:
                    {
                        // Update UI hover (screen space)
                        sf::Vector2i mp = sf::Mouse::getPosition(window);
                        sf::Vector2f screen = window.mapPixelToCoords(mp, window.getDefaultView());
                        genHover = btnGenerate.getGlobalBounds().contains(screen);
                        gridHover = btnGrid.getGlobalBounds().contains(screen);
                        if (brushDragging) {
                            // Update brush while dragging on slider
                            brushSize = sliderPickValue(screen);
                        }

                        // Paint while dragging (if not panning/tilting), throttled by paintTick
                        if (!panning && !tilting && !brushDragging && (sf::Mouse::isButtonPressed(sf::Mouse::Left) || sf::Mouse::isButtonPressed(sf::Mouse::Right))) {
                            if (paintClock.getElapsedTime() >= paintTick) {
                                paintClock.restart();
                                sf::Vector2f world = window.mapPixelToCoords(mp, view);
                                if (pointInsideGrid(world)) {
                                    sf::Vector2i IJ = worldToGridIntersection(world);
                                    int brush = std::clamp(brushSize, brushMin, brushMax);
                                    bool ctrl = sf::Keyboard::isKeyPressed(sf::Keyboard::LControl) || sf::Keyboard::isKeyPressed(sf::Keyboard::RControl);
                                    if (ctrl && flattenPrimed && sf::Mouse::isButtonPressed(sf::Mouse::Left)) {
                                        // Flatten to captured height while Ctrl is held
                                        for (int di = -brush; di <= brush; ++di) {
                                            for (int dj = -brush; dj <= brush; ++dj) {
                                                int I = IJ.x + di;
                                                int J = IJ.y + dj;
                                                if (I < 0 || J < 0 || I > cfg::GRID || J > cfg::GRID) continue;
                                                if (di*di + dj*dj > brush*brush) continue;
                                                heights[idx(I, J)] = flattenHeight;
                                            }
                                        }
                                    } else {
                                        int delta = sf::Mouse::isButtonPressed(sf::Mouse::Left) ? 1 : -1;
                                        for (int di = -brush; di <= brush; ++di) {
                                            for (int dj = -brush; dj <= brush; ++dj) {
                                                int I = IJ.x + di;
                                                int J = IJ.y + dj;
                                                if (I < 0 || J < 0 || I > cfg::GRID || J > cfg::GRID) continue;
                                                if (di*di + dj*dj > brush*brush) continue;
                                                heights[idx(I, J)] += delta;
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
                case sf::Event::Resized:
                    // Adjust view to new window size and update UI positions
                    view.setSize((float)ev.size.width, (float)ev.size.height);
                    window.setView(view);
                    updateTopRightButtons();
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
        const float panSpeed = 300.f; // world units per second
        float dt = 1.f / 120.f; // approximate since we set framerate limit; could use clock for precision
        sf::Vector2f move(0.f, 0.f);
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::W) || sf::Keyboard::isKeyPressed(sf::Keyboard::Up))    move.y -= panSpeed * dt;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::S) || sf::Keyboard::isKeyPressed(sf::Keyboard::Down))  move.y += panSpeed * dt;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::A) || sf::Keyboard::isKeyPressed(sf::Keyboard::Left))  move.x -= panSpeed * dt;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::D) || sf::Keyboard::isKeyPressed(sf::Keyboard::Right)) move.x += panSpeed * dt;
        if (move.x != 0.f || move.y != 0.f) {
            view.move(move);
            window.setView(view);
        }

        window.clear(sf::Color::Black);
        // Wireframe-only view: do not draw filled tiles or interstices

        // Build 2D map of projected intersection points (with elevation) and draw filled cells + wireframe
        auto map2d = render::buildProjectedMap(heights, iso, origin);
        // Draw fills first
        render::draw2DFilledCells(window, map2d, heights, shadowsEnabled);
        // Overlay wireframe lines (toggle)
        if (showGrid) {
            render::draw2DMap(window, map2d);
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
            hoverOverlay.setFillColor(sf::Color(255, 255, 255, 28));
            window.draw(hoverOverlay);
        }
        if (fontLoaded) window.draw(btnText);

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

        // One window size fetch per frame for UI positions
        auto wsz = window.getSize();

        // Help text bottom-left
        if (fontLoaded) {
            float baseY = (float)wsz.y - 24.f;
            helpF11.setPosition(16.f, baseY - 20.f);
            helpCtrl.setPosition(16.f, baseY);
            window.draw(helpF11);
            window.draw(helpCtrl);
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
        window.setView(oldView);

        window.display();
    }

    return 0;
}
