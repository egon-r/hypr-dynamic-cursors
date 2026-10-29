#pragma once

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/helpers/math/Math.hpp>

/*
 *  creates a transformation intended to be baked into the monitor transformation to transform the cursor with a normal render call
 *  (this is how we avoid copying the whole render function into the plugin)
 *
 *  maps render-space (framebuffer pixel) points to render-space points rotated around box.pos() + hotspot,
 *  with the plugin stretch applied. Input and output are both framebuffer-pixel coordinates.
 *
 *  TODO: refactor box out of here and do the compensation back in drawCursor()
 */
Mat3x3 toTransform(CBox& box, float rotation, Vector2D hotspot, float stretchAngle, Vector2D stretch);

struct SCursorBlurGeometry {
    CBox                    quad;
    std::array<Vector2D, 4> uvs;
    Vector2D                deltaUV;
};

SCursorBlurGeometry blurGeometry(const CBox& box, float rotation, const Vector2D& hotspot, float stretchAngle, const Vector2D& stretch, const Vector2D& displacementFb);

/*
 *  dispatches a draw call using the current hyprland renderer to draw the cursor with the provided transform
 *  will modify the monitor transform and do a tex pass immediately
 */
void drawCursor(const Mat3x3& transform, SP<Render::ITexture> tex, CBox box, CRegion damage, bool nearest);

/*
 *  releases all GPU resources cached by the cursor motion blur renderer (program, VAO/VBO).
 *  Must be called with a current GL context, e.g. on plugin unload after removing all pass elements.
 */
void destroyCursorBlurRenderer();
