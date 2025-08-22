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
}
