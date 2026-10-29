#pragma once

#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Mat3x3.hpp>

#include <array>

/*
 * everything needed to draw one motion blurred cursor with the dedicated blur shader
 */
struct SCursorBlurDraw {
    SP<Render::ITexture>    tex;     // RGBA/RGBX 2D texture or EXTERNAL (EGLImage) texture
    CBox                    quad;    // coverage quad, monitor-local framebuffer pixels
    std::array<Vector2D, 4> uvs;     // current sprite uv per quad corner (TL, BL, TR, BR)
    Vector2D                deltaUV; // uv shift applied per blur step
    int                     samples = 1;
    bool                    nearest = false;
};

/*
 * draws the motion blurred cursor: for every fragment, samples the sprite along the
 * displacement and outputs the average, mirroring the native motionBlurSample maths.
 * caches the transformed average in a local texture, restores GL state, and
 * composites once through the native damage-scissored/color-managed texture path.
 */
bool drawCursorBlurred(const SCursorBlurDraw& draw);
void destroyCursorBlurRenderer();
