#include "renderer.hpp"

#include <hyprland/src/render/Renderer.hpp>
#include <algorithm>
#include <cmath>

Mat3x3 toTransform(CBox& box, float rotation, Vector2D hotspot, float stretchAngle, Vector2D stretch) {
    auto mat = Mat3x3::identity();
    mat.translate(box.pos());

    if (stretch != Vector2D{1, 1}) {
        // center to origin, rotate, shift up, scale, undo
        // we do the shifting up, so the stretch is "only to one side"

        mat.translate({box.w / 2, box.h / 2});
        mat.rotate(stretchAngle);
        mat.translate({0.f, box.h / 2});
        mat.scale(stretch);
        mat.translate({0.f, box.h / -2});
        mat.rotate(-stretchAngle);
        mat.translate({box.w / -2, box.h / -2});
    }

    if (rotation != 0) {
        mat.translate(hotspot);
        mat.rotate(rotation);
        mat.translate(-hotspot);
    }

    mat.translate(-box.pos());

    return mat;
}

void drawCursor(const Mat3x3& transform, SP<Render::ITexture> tex, CBox box, CRegion damage, bool nearest) {
    Mat3x3 proj = g_pHyprRenderer->m_renderData.targetProjection.copy().multiply(transform);

    std::swap(g_pHyprRenderer->m_renderData.targetProjection, proj);
    g_pHyprRenderer->m_renderData.useNearestNeighbor = nearest;

    g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = tex, .box = box}, damage);

    std::swap(g_pHyprRenderer->m_renderData.targetProjection, proj);
    g_pHyprRenderer->m_renderData.useNearestNeighbor = false;
}

SCursorBlurGeometry blurGeometry(const CBox& box, float rotation, const Vector2D& hotspot, float stretchAngle, const Vector2D& stretch, const Vector2D& displacementFb) {
    auto                sprite = box;
    const auto          m      = toTransform(sprite, rotation, hotspot, stretchAngle, stretch).getMatrix();
    const auto          apply  = [&m](const Vector2D& p) { return Vector2D{m[0] * p.x + m[1] * p.y + m[2], m[3] * p.x + m[4] * p.y + m[5]}; };
    const Vector2D      origin = apply(box.pos());
    const Vector2D      axisX  = apply(box.pos() + Vector2D{box.w, 0.0}) - origin;
    const Vector2D      axisY  = apply(box.pos() + Vector2D{0.0, box.h}) - origin;
    const double        det    = axisX.x * axisY.y - axisX.y * axisY.x;
    SCursorBlurGeometry geometry;
    if (std::abs(det) < 1e-8)
        return geometry;
    const auto                    uv      = [&](const Vector2D& p) { return Vector2D{(p.x * axisY.y - p.y * axisY.x) / det, (axisX.x * p.y - axisX.y * p.x) / det}; };
    const std::array<Vector2D, 4> corners = {origin, origin + axisY, origin + axisX, origin + axisX + axisY};
    double                        left = origin.x, top = origin.y, right = origin.x, bottom = origin.y;
    for (const auto& p : corners) {
        left   = std::min({left, p.x, p.x - displacementFb.x});
        top    = std::min({top, p.y, p.y - displacementFb.y});
        right  = std::max({right, p.x, p.x - displacementFb.x});
        bottom = std::max({bottom, p.y, p.y - displacementFb.y});
    }
    geometry.quad                      = {std::floor(left), std::floor(top), std::ceil(right) - std::floor(left), std::ceil(bottom) - std::floor(top)};
    const std::array<Vector2D, 4> quad = {geometry.quad.pos(), geometry.quad.pos() + Vector2D{0.0, geometry.quad.h}, geometry.quad.pos() + Vector2D{geometry.quad.w, 0.0},
                                          geometry.quad.pos() + geometry.quad.size()};
    for (size_t i = 0; i < quad.size(); ++i)
        geometry.uvs[i] = uv(quad[i] - origin);
    geometry.deltaUV = uv(displacementFb);
    return geometry;
}
