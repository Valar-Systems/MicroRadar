#pragma once

#include <cstdint>

#include "LGFX.h" // lgfx::color888

// Visual theme for the Angler edition screens: a cool water-teal palette on true black (free power
// on the AMOLED tier), with a warm gold accent reserved for the sun / active bite window. Mirrors
// SpaceTheme / SeismicTheme / BirdingTheme so the screen code shares the same Palette + ScaleColor
// shape.
namespace angler {

struct Palette {
    uint32_t fg;     // primary text / lit elements (water teal)
    uint32_t dim;    // secondary text
    uint32_t faint;  // labels / range rings / inactive
    uint32_t accent; // sun / active bite window highlight (warm gold)
    uint32_t warn;   // elevated state (amber)
    uint32_t alert;  // emergency (red)
    uint32_t bg;     // background (true black)
};

inline Palette PaletteDefault()
{
    return {
        lgfx::color888(90, 220, 200), lgfx::color888(50, 140, 130), lgfx::color888(24, 70, 66),
        lgfx::color888(255, 205, 110), lgfx::color888(255, 180, 0), lgfx::color888(255, 70, 50),
        lgfx::color888(0, 0, 0)
    };
}

// Scale a packed 0xRRGGBB colour by factor f (0..1). Used for night-dim and glow/fade effects.
inline uint32_t ScaleColor(uint32_t rgb, float f)
{
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    const uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return lgfx::color888((uint8_t)(r * f), (uint8_t)(g * f), (uint8_t)(b * f));
}

} // namespace angler
