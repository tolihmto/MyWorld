#pragma once
#include <SFML/Graphics.hpp>
#include "config.hpp"

struct IsoParams {
    float rotDeg = 0.f;   // rotation around screen (0 = classic isometric diamond)
    float pitch  = 1.f;   // vertical scale to simulate camera tilt
};

sf::Vector2f isoProjectDyn(float i, float j, float elev, const IsoParams& P);
sf::Vector2f isoUnprojectDyn(const sf::Vector2f& p, const IsoParams& P);
