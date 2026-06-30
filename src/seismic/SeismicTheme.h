#pragma once

#include <cstdint>

#include "LGFX.h" // lgfx::color888

// Visual theme for the Seismic edition screens: a warm amber "instrument" palette on true black
// (free power on the AMOLED tier), with a magnitude colour ramp (the key visual on the quake radar)
// and white accents. Mirrors SpaceTheme / EamTheme so the screen code shares the same Palette +
// ScaleColor shape.
namespace seismic {

struct Palette {
    uint32_t fg;     // primary text / lit elements
    uint32_t dim;    // secondary text
    uint32_t faint;  // labels / range rings / inactive
    uint32_t accent; // highlights (white)
    uint32_t warn;   // elevated state (amber)
    uint32_t alert;  // high / emergency (red)
    uint32_t bg;     // background (true black)
};

inline Palette PaletteDefault()
{
    return {
        lgfx::color888(255, 196, 120), lgfx::color888(180, 120, 60), lgfx::color888(70, 48, 26),
        lgfx::color888(255, 255, 255), lgfx::color888(255, 170, 0), lgfx::color888(255, 60, 40),
        lgfx::color888(0, 0, 0)
    };
}

// Magnitude -> colour, USGS-style spectrum: micro (blue) -> minor (green) -> light (yellow) ->
// moderate (orange) -> strong+ (red). Drives the radar blips, the list rows, and the gauges.
inline uint32_t MagColor(float m)
{
    if (m >= 6.0f) return lgfx::color888(255, 50, 40);    // strong / major / great
    if (m >= 5.0f) return lgfx::color888(255, 120, 30);   // moderate
    if (m >= 4.0f) return lgfx::color888(255, 190, 0);    // light
    if (m >= 2.5f) return lgfx::color888(120, 210, 90);   // minor
    return lgfx::color888(70, 170, 255);                  // micro
}

// A short USGS-ish word for a magnitude band, for the detail card / stats.
inline const char* MagWord(float m)
{
    if (m >= 8.0f) return "GREAT";
    if (m >= 7.0f) return "MAJOR";
    if (m >= 6.0f) return "STRONG";
    if (m >= 5.0f) return "MODERATE";
    if (m >= 4.0f) return "LIGHT";
    if (m >= 2.5f) return "MINOR";
    return "MICRO";
}

// Scale a packed 0xRRGGBB colour by factor f (0..1). Used for night-dim and age-fade effects.
inline uint32_t ScaleColor(uint32_t rgb, float f)
{
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    const uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return lgfx::color888((uint8_t)(r * f), (uint8_t)(g * f), (uint8_t)(b * f));
}

} // namespace seismic
