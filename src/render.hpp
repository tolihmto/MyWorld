#pragma once
#include <SFML/Graphics.hpp>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "iso.hpp"

namespace render {
    std::vector<std::vector<sf::Vector2f>> buildProjectedMap(
        const std::vector<int>& heights,
        const IsoParams& iso,
        const sf::Vector2f& origin,
        float heightScale);

    void draw2DMap(sf::RenderTarget& target,
                   const std::vector<std::vector<sf::Vector2f>>& map2d);

    void draw2DFilledCells(sf::RenderTarget& target,
                           const std::vector<std::vector<sf::Vector2f>>& map2d,
                           const std::vector<int>& heights,
                           bool enableShadows,
                           float heightScale,
                           const std::unordered_map<long long, sf::Color>* paintedCells = nullptr,
                           const std::unordered_set<long long>* hoverMask = nullptr,
                           const sf::Color* hoverColor = nullptr);

    // --- Per-chunk rendering (arbitrary size S=(side-1)) ---
    std::vector<std::vector<sf::Vector2f>> buildProjectedMapChunk(
        const std::vector<int>& heights, // size (S+1)*(S+1)
        int S,
        int I0, int J0,               // world origin (grid coords) of this chunk
        const IsoParams& iso,
        const sf::Vector2f& origin,
        float heightScale);

    // Strided map build: projects only every 'stride' cell into a reduced map of size ceil((S+1)/stride)
    std::vector<std::vector<sf::Vector2f>> buildProjectedMapChunkStrided(
        const std::vector<int>& heights, // size (S+1)*(S+1)
        int S,
        int I0, int J0,
        const IsoParams& iso,
        const sf::Vector2f& origin,
        float heightScale,
        int stride);

    void draw2DMapChunk(sf::RenderTarget& target,
                        const std::vector<std::vector<sf::Vector2f>>& map2d,
                        int stride = 1);

    void draw2DFilledCellsChunk(sf::RenderTarget& target,
                                const std::vector<std::vector<sf::Vector2f>>& map2d,
                                const std::vector<int>& heights,
                                int S,
                                bool enableShadows,
                                float heightScale,
                                int stride = 1,
                                int I0 = 0, int J0 = 0,
                                const std::unordered_map<long long, sf::Color>* paintedCells = nullptr,
                                const std::unordered_set<long long>* hoverMask = nullptr,
                                const sf::Color* hoverColor = nullptr);

    // Strided draw variant: expects a pre-subsampled map where adjacent entries correspond to 'stride' in world grid
    void draw2DFilledCellsChunkStrided(sf::RenderTarget& target,
                                       const std::vector<std::vector<sf::Vector2f>>& map2dStrided,
                                       const std::vector<int>& heights,
                                       int S,
                                       bool enableShadows,
                                       float heightScale,
                                       int stride,
                                       int I0 = 0, int J0 = 0,
                                       const std::unordered_map<long long, sf::Color>* paintedCells = nullptr,
                                       const std::unordered_set<long long>* hoverMask = nullptr,
                                       const sf::Color* hoverColor = nullptr);
}

