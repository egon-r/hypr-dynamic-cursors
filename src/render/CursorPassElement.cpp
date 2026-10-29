#include "CursorPassElement.hpp"
#include "renderer.hpp"
#include "BlurShader.hpp"

#include <hyprland/src/render/Renderer.hpp>

using namespace Hyprutils::Utils;

CCursorPassElement::CCursorPassElement(const CCursorPassElement::SRenderData& data) : m_data(data) {
    ;
}

std::vector<UP<IPassElement>> CCursorPassElement::draw() {
    if (m_data.blurSamples > 1 && m_data.blurDisplacement != Vector2D{}) {
        const auto geometry = blurGeometry(m_data.box, m_data.box.rot, m_data.hotspot, m_data.stretchAngle, m_data.stretchMagnitude, m_data.blurDisplacement);
        if (!geometry.quad.empty() && drawCursorBlurred({m_data.tex, geometry.quad, geometry.uvs, geometry.deltaUV, m_data.blurSamples, m_data.nearest}))
            return {};
    }
    Mat3x3 transform = toTransform(m_data.box, m_data.box.rot, m_data.hotspot, m_data.stretchAngle, m_data.stretchMagnitude);
    auto   box       = m_data.box;
    box.rot          = 0;
    drawCursor(transform, m_data.tex, box, g_pHyprRenderer->m_renderData.damage, m_data.nearest);

    return {}; // no passes to be submitted later
}

bool CCursorPassElement::needsLiveBlur() {
    return false; // TODO?
}

bool CCursorPassElement::needsPrecomputeBlur() {
    return false; // TODO?
}

std::optional<CBox> CCursorPassElement::boundingBox() {
    const auto geometry =
        blurGeometry(m_data.box, m_data.box.rot, m_data.hotspot, m_data.stretchAngle, m_data.stretchMagnitude, m_data.blurSamples > 1 ? m_data.blurDisplacement : Vector2D{});
    auto         bounds = geometry.quad.copy().scale(1.F / g_pHyprRenderer->m_renderData.pMonitor->m_scale);
    const double right = std::ceil(bounds.x + bounds.w), bottom = std::ceil(bounds.y + bounds.h);
    bounds.x = std::floor(bounds.x);
    bounds.y = std::floor(bounds.y);
    bounds.w = right - bounds.x;
    bounds.h = bottom - bounds.y;
    return bounds;
}

CRegion CCursorPassElement::opaqueRegion() {
    return {}; // TODO:
}

void CCursorPassElement::discard() {
    ;
}
