#pragma once
#include <SFML/Graphics.hpp>
#include <vector>
#include "iso.hpp"

namespace render {
    std::vector<std::vector<sf::Vector2f>> buildProjectedMap(
        const std::vector<int>& heights,
        const IsoParams& iso,
        const sf::Vector2f& origin);

    void draw2DMap(sf::RenderTarget& target,
                   const std::vector<std::vector<sf::Vector2f>>& map2d);

    void draw2DFilledCells(sf::RenderTarget& target,
                           const std::vector<std::vector<sf::Vector2f>>& map2d,
                           const std::vector<int>& heights,
                           bool enableShadows);

    // --- Per-chunk rendering (arbitrary size S=(side-1)) ---
    std::vector<std::vector<sf::Vector2f>> buildProjectedMapChunk(
        const std::vector<int>& heights, // size (S+1)*(S+1)
        int S,
        int I0, int J0,               // world origin (grid coords) of this chunk
        const IsoParams& iso,
        const sf::Vector2f& origin);

    void draw2DMapChunk(sf::RenderTarget& target,
                        const std::vector<std::vector<sf::Vector2f>>& map2d);

    void draw2DFilledCellsChunk(sf::RenderTarget& target,
                                const std::vector<std::vector<sf::Vector2f>>& map2d,
                                const std::vector<int>& heights,
                                int S,
                                bool enableShadows);
}
