#include <any>    // IWYU pragma: keep
#include <chrono> // IWYU pragma: keep
#include <ranges> // IWYU pragma: keep
#define private public
#include <hyprland/src/pointer/cursor/CursorManager.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#undef private

#include <hyprcursor/hyprcursor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprlang.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <climits>
#include <gbm.h>
#include <numbers>

#include "cursor.hpp"
#include "render/renderer.hpp"
#include "config/ConfigManager.hpp"
#include "mode/Mode.hpp"
#include "render/CursorPassElement.hpp"

static constexpr double MIN_TRAIL_DISPLACEMENT = 0.5; // logical px below which no blur is worth rendering

/*
The union of two boxes, mirroring the native MotionBlur::extents.
*/
static CBox boxUnion(const CBox& a, const CBox& b) {
    const double x1 = std::min(a.x, b.x);
    const double y1 = std::min(a.y, b.y);
    const double x2 = std::max(a.x + a.w, b.x + b.w);
    const double y2 = std::max(a.y + a.h, b.y + b.h);

    return {x1, y1, x2 - x1, y2 - y1};
}

/*
The padded box the cursor sprite can occupy when drawn at `pos`, mirroring the
damage padding of damageSoftware: generous enough for any hotspot, rotation and stretch.
*/
static CBox paddedCursorBox(Pointer::CPointerManager* pointers, double zoom, const Vector2D& pos) {
    Vector2D size     = pointers->m_currentCursorImage.size / pointers->m_currentCursorImage.scale * zoom;
    int      diagonal = size.size();
    Vector2D padding  = {diagonal, diagonal};

    return CBox{pos, size + (padding * 2)}.translate(-(pointers->m_currentCursorImage.hotspot * zoom + padding));
}

void tickRaw(SP<CEventLoopTimer> self, void* data) {
    if (g_pConfigHandler->isEnabled())
        g_pDynamicCursors->onTick(Pointer::mgr().get());
    else if (g_pDynamicCursors)
        // release locks and transient state which the enabled-only paths can no longer reach
        g_pDynamicCursors->onDisabledTick(Pointer::mgr().get());

    const int TIMEOUT = g_pHyprRenderer->m_mostHzMonitor && g_pHyprRenderer->m_mostHzMonitor->m_refreshRate > 0 ? 1000.0 / g_pHyprRenderer->m_mostHzMonitor->m_refreshRate : 16;
    self->updateTimeout(std::chrono::milliseconds(TIMEOUT));
}
CDynamicCursors::CDynamicCursors() {
    this->tick = SP<CEventLoopTimer>(new CEventLoopTimer(std::chrono::microseconds(500), tickRaw, nullptr));
    g_pEventLoopManager->addTimer(this->tick);

    // hide/show transitions can bypass the shape hooks (e.g. resetCursorImage);
    // drop the blur history synchronously when the cursor becomes invisible so it
    // can never smear across a visibility change
    static const auto LISTENER = Pointer::mgr()->m_events.cursorChanged.listen([this]() -> void {
        if (g_pDynamicCursors.get() == this && (!Pointer::mgr()->hasCursor() || !Pointer::mgr()->getCurrentCursorTexture()))
            this->resetTrail(Pointer::mgr().get());
    });
}

CDynamicCursors::~CDynamicCursors() {
    g_pEventLoopManager->removeTimer(this->tick);
    this->tick.reset();

    // release software lock, damaging the last frame so nothing stale persists;
    // release only the monitors we own, never counts other lockers hold
    if (softwareLockHeld) {
        this->damageSoftware(Pointer::mgr().get());

        for (auto& m : lockedMonitors) {
            if (!m.expired())
                Pointer::mgr()->unlockSoftwareForMonitor(m.lock());
        }
        lockedMonitors.clear();
        softwareLockHeld = false;
    }
}

/*
Reimplements rendering of the software cursor.
Is also largely identical to hyprlands impl, but uses our custom rendering to rotate the cursor.
*/
void CDynamicCursors::renderSoftware(Pointer::CPointerManager* pointers, PHLMONITOR pMonitor, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> overridePos,
                                     bool screencopy, bool forceRender) {
    if (!pointers->hasCursor())
        return;

    auto state = pointers->stateFor(pMonitor);
    auto zoom  = resultShown.scale;

    if (!state->hardwareFailed && state->softwareLocks == 0 && !screencopy) {
        if (pointers->m_currentCursorImage.surface)
            pointers->m_currentCursorImage.surface->resource()->frame(now);

        return;
    }

    // don't render cursor on screencopy if using sw cursors
    // otherwise we draw the cursor again for screencopy when using sw cursors
    // unless this is toplevel capture and we *actually* have to force render cursors
    if (screencopy && !forceRender && (state->hardwareFailed || state->softwareLocks != 0))
        return;

    auto box = state->box.copy();
    if (overridePos.has_value()) {
        box.x = overridePos->x;
        box.y = overridePos->y;

        box.translate(-pointers->m_currentCursorImage.hotspot);
    }

    auto texture = pointers->getCurrentCursorTexture();
    bool nearest = false;

    if (zoom > 1) {
        // this first has to undo the hotspot transform from getCursorBoxGlobal
        box.x += pointers->m_currentCursorImage.hotspot.x;
        box.y += pointers->m_currentCursorImage.hotspot.y;

        auto high = highres.getTexture();

        if (high) {
            texture  = high;
            auto buf = highres.getBuffer();

            // we calculate a more accurate hotspot location if we have bigger shapes
            box.x -= (buf->m_hotspot.x / buf->size.x) * pointers->m_currentCursorImage.size.x * zoom;
            box.y -= (buf->m_hotspot.y / buf->size.y) * pointers->m_currentCursorImage.size.y * zoom;

            // only use nearest-neighbour if magnifying over size
            nearest = CONFIG(highresNearest) == 2 && pointers->m_currentCursorImage.size.x * zoom > buf->size.x;

        } else {
            box.x -= pointers->m_currentCursorImage.hotspot.x * zoom;
            box.y -= pointers->m_currentCursorImage.hotspot.y * zoom;

            nearest = CONFIG(highresNearest);
        }
    }

    if (!texture)
        return;

    box.w *= zoom;
    box.h *= zoom;

    // genuine motion blur: hand the recent movement to the renderer, which averages
    // the sprite along it instead of drawing the sharp cursor
    std::optional<Vector2D> displacement;
    if (CONFIG(motionBlurEnabled) && !overridePos.has_value())
        displacement = trail.displacement(now, blurWindow(pMonitor->m_refreshRate));

    CCursorPassElement::SRenderData data;
    data.tex              = texture;
    data.hotspot          = pointers->m_currentCursorImage.hotspot * state->monitor->m_scale * zoom;
    data.nearest          = nearest;
    data.stretchAngle     = resultShown.stretch.angle;
    data.stretchMagnitude = resultShown.stretch.magnitude;

    // the sharp path culls pre-scale exactly like stock hyprland; the blur path scales
    // first to build the exact swept quad in framebuffer space
    const bool BLUR = displacement && displacement->size() > MIN_TRAIL_DISPLACEMENT;

    if (BLUR) {
        box.scale(pMonitor->m_scale);
        box.x   = std::round(box.x);
        box.y   = std::round(box.y);
        box.rot = resultShown.rotation;

        // exact swept extents of the transformed sprite (rotation, stretch, hotspot
        // and displacement included), in framebuffer pixels of this monitor
        const auto GEOMETRY = blurGeometry(box, resultShown.rotation, pointers->m_currentCursorImage.hotspot * pMonitor->m_scale * zoom, resultShown.stretch.angle,
                                           resultShown.stretch.magnitude, *displacement * pMonitor->m_scale);

        if (GEOMETRY.quad.intersection(CBox{{}, pMonitor->m_size * pMonitor->m_scale}).empty())
            return;

        // remember where this monitor's exposure is drawn (global logical extents) so
        // it can be damaged when movement stops; the hooked damageSoftware already
        // covers the previous exposure, so replacing the entry needs no extra damage.
        // framebuffer -> logical first, then translate by the logical monitor position,
        // rounded outwards so the damage can never clip the trail
        auto         TRAIL = GEOMETRY.quad.copy().scale(1.F / pMonitor->m_scale).translate(pMonitor->m_position);
        const double right = std::ceil(TRAIL.x + TRAIL.w), bottom = std::ceil(TRAIL.y + TRAIL.h);
        TRAIL.x = std::floor(TRAIL.x);
        TRAIL.y = std::floor(TRAIL.y);
        TRAIL.w = right - TRAIL.x;
        TRAIL.h = bottom - TRAIL.y;

        auto it = std::ranges::find_if(lastTrailExposures, [&pMonitor](const auto& e) { return e.monitor.lock() == pMonitor; });
        if (it != lastTrailExposures.end())
            it->box = TRAIL;
        else
            lastTrailExposures.push_back({pMonitor, TRAIL});

        trailRendered         = true;
        data.blurDisplacement = *displacement * pMonitor->m_scale;
        data.blurSamples      = static_cast<int>(std::clamp<long>(CONFIG(motionBlurSamples), 1, 64));
    } else {
        if (box.intersection(CBox{{}, {pMonitor->m_size}}).empty())
            return;

        box.scale(pMonitor->m_scale);
        box.x   = std::round(box.x);
        box.y   = std::round(box.y);
        box.rot = resultShown.rotation;
    }

    data.box = box;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CCursorPassElement>(data));

    if (pointers->m_currentCursorImage.surface)
        pointers->m_currentCursorImage.surface->resource()->frame(now);
}

/*
This function implements damaging the screen such that the software cursor is drawn.
It is largely identical to hyprlands implementation, but expands the damage region, to accommodate various rotations.
*/
void CDynamicCursors::damageSoftware(Pointer::CPointerManager* pointers) {

    // we damage a padding of the diagonal around the hotspot, to accommodate for all possible hotspots and rotations
    auto zoom = resultShown.scale;
    CBox b    = paddedCursorBox(pointers, zoom, pointers->m_pointerPos);

    // cover the blur trails of previous frames, so no ghost of them persists
    if (lastTrailAny())
        b = boxUnion(b, lastTrailUnion());

    static auto PNOHW = CConfigValue<Hyprlang::INT>("cursor:no_hardware_cursors");

    // damage every monitor the cursor or its trail overlaps individually: one
    // monitor's frame-skip policy must not suppress damage on the other outputs
    for (auto& mw : pointers->m_monitorStates) {
        if (mw->monitor.expired())
            continue;

        if ((mw->softwareLocks > 0 || mw->hardwareFailed || *PNOHW) && b.overlaps({mw->monitor->m_position, mw->monitor->m_size})) {
            mw->monitor->addDamage(b.copy().translate(-mw->monitor->m_position).scale(mw->monitor->m_scale).round());
        }
    }
}

/*
This function reimplements the hardware cursor buffer drawing.
It is largely copied from hyprland, but adjusted to allow the cursor to be rotated.
*/
SP<Aquamarine::IBuffer> CDynamicCursors::renderHardware(Pointer::CPointerManager* pointers, SP<Pointer::CPointerManager::SMonitorPointerState> state,
                                                        SP<Render::ITexture> texture) {
    auto output = state->monitor->m_output;

    auto maxSize = output->cursorPlaneSize();
    auto zoom    = resultShown.scale;

    auto cursorSize     = pointers->m_currentCursorImage.size * zoom;
    int  cursorDiagonal = cursorSize.size();
    auto cursorPadding  = Vector2D{cursorDiagonal, cursorDiagonal};
    auto targetSize     = cursorSize + cursorPadding * 2;

    if (maxSize == Vector2D{})
        return nullptr;

    if (maxSize != Vector2D{-1, -1}) {
        if (targetSize.x > maxSize.x || targetSize.y > maxSize.y) {
            Log::logger->log(Log::TRACE, "hardware cursor too big! {} > {}", pointers->m_currentCursorImage.size, maxSize);
            return nullptr;
        }
    } else {
        maxSize = targetSize;
        if (maxSize.x < 16 || maxSize.y < 16)
            maxSize = {16, 16}; // fix some annoying crashes in nest
    }

    if (!state->monitor->m_cursorSwapchain || maxSize != state->monitor->m_cursorSwapchain->currentOptions().size ||
        state->monitor->m_cursorSwapchain->currentOptions().length != 3) {

        if (!state->monitor->m_cursorSwapchain) {
            auto backend = state->monitor->m_output->getBackend();
            auto primary = backend->getPrimary();

            state->monitor->m_cursorSwapchain = Aquamarine::CSwapchain::create(state->monitor->m_output->getBackend()->preferredAllocator(), primary ? primary.lock() : backend);
        }

        auto options = state->monitor->m_cursorSwapchain->currentOptions();
        options.size = maxSize;
        // we still have to create a triple buffering swapchain, as we seem to be running into some sort of race condition
        // or something. I'll continue debugging this when I find some energy again, I've spent too much time here already.
        options.length   = 3;
        options.scanout  = true;
        options.cursor   = true;
        options.multigpu = state->monitor->m_output->getBackend()->preferredAllocator()->drmFD() != g_pCompositor->m_drm.fd;
        // We do not set the format. If it's unset (DRM_FORMAT_INVALID) then the swapchain will pick for us,
        // but if it's set, we don't wanna change it.

        if (!state->monitor->m_cursorSwapchain->reconfigure(options)) {
            Log::logger->log(Log::TRACE, "Failed to reconfigure cursor swapchain");
            return nullptr;
        }
    }

    // if we already rendered the cursor, revert the swapchain to avoid rendering the cursor over
    // the current front buffer
    // this flag will be reset in the preRender hook, so when we commit this buffer to KMS
    // see https://github.com/hyprwm/Hyprland/commit/4c3b03516209a49244a8f044143c1162752b8a7a
    // this is however still not enough, see above
    if (state->cursorRendered)
        state->monitor->m_cursorSwapchain->rollback();

    state->cursorRendered = true;

    auto buf = state->monitor->m_cursorSwapchain->next(nullptr);
    if (!buf) {
        Log::logger->log(Log::TRACE, "Failed to acquire a buffer from the cursor swapchain");
        return nullptr;
    }

    CRegion damage = {0, 0, INT16_MAX, INT16_MAX};

    g_pHyprRenderer->m_renderData.pMonitor = state->monitor;
    auto RBO                               = g_pHyprRenderer->getOrCreateRenderbuffer(buf, state->monitor->m_cursorSwapchain->currentOptions().format);

    // we just fail if we cannot create a render buffer, this will force hl to render software cursors, which we support
    if (!RBO)
        return nullptr;

    RBO->bind();

    CRegion damageRegion = {0, 0, INT_MAX, INT_MAX};
    g_pHyprRenderer->beginFullFakeRender(state->monitor.lock(), damageRegion, RBO->getFB());
    g_pHyprRenderer->startRenderPass();

    if (CONFIG(hwDebug))
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{rand() / float(RAND_MAX), rand() / float(RAND_MAX), rand() / float(RAND_MAX), 1.F}});
    else
        g_pHyprRenderer->draw(CClearPassElement::SClearData{{0.F, 0.F, 0.F, 0.F}});

    CBox   xbox      = {cursorPadding, Vector2D{Pointer::mgr()->m_currentCursorImage.size / Pointer::mgr()->m_currentCursorImage.scale * state->monitor->m_scale * zoom}.round()};
    Mat3x3 transform = toTransform(xbox, resultShown.rotation, Pointer::mgr()->m_currentCursorImage.hotspot * state->monitor->m_scale * zoom, resultShown.stretch.angle,
                                   resultShown.stretch.magnitude);

    drawCursor(transform, texture, xbox, damageRegion, zoom > 1 && CONFIG(highresNearest));

    g_pHyprRenderer->endRender();
    g_pHyprRenderer->m_renderData.pMonitor.reset();

    return buf;
}

/*
Implements the hardware cursor setting.
It is also mostly the same as stock hyprland, but with the hotspot translated more into the middle.
*/
bool CDynamicCursors::setHardware(Pointer::CPointerManager* pointers, SP<Pointer::CPointerManager::SMonitorPointerState> state, SP<Aquamarine::IBuffer> buf) {
    if (!(state->monitor->m_output->getBackend()->capabilities() & Aquamarine::IBackendImplementation::eBackendCapabilities::AQ_BACKEND_CAPABILITY_POINTER))
        return false;

    if (!state->monitor->m_cursorSwapchain)
        return false;

    // we need to transform the hotspot manually as we need to indent it by the padding
    int      diagonal = pointers->m_currentCursorImage.size.size();
    Vector2D padding  = {diagonal, diagonal};

    const auto HOTSPOT = CBox{((pointers->m_currentCursorImage.hotspot * state->monitor->m_scale) + padding) * resultShown.scale, {0, 0}}
                             .transform(Math::wlTransformToHyprutils(Math::invertTransform(state->monitor->m_transform)),
                                        state->monitor->m_cursorSwapchain->currentOptions().size.x, state->monitor->m_cursorSwapchain->currentOptions().size.y)
                             .pos();

    Log::logger->log(Log::TRACE, "[pointer] hw transformed hotspot for {}: {}", state->monitor->m_name, HOTSPOT);

    if (!state->monitor->m_output->setCursor(buf, HOTSPOT))
        return false;

    state->cursorFrontBuffer = buf;

    if (!state->monitor->shouldSkipScheduleFrameOnMouseEvent())
        state->monitor->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_CURSOR_SHAPE);

    state->monitor->m_scanoutNeedsCursorUpdate = true;

    return true;
}

/*
Handles cursor move events.
*/
void CDynamicCursors::onCursorMoved(Pointer::CPointerManager* pointers) {
    if (!pointers->hasCursor()) {
        // do not let a move event of a hidden cursor classify the next warp as genuine
        isMove = false;
        return;
    }

    // warps must not smear across the screen, genuine moves feed the trail
    if (isMove)
        trail.record(pointers->m_pointerPos, Time::steadyNow());
    else
        resetTrail(pointers);

    const auto CURSORBOX = pointers->getCursorBoxGlobal();
    bool       recalc    = false;

    for (auto& m : State::monitorState()->monitors()) {
        auto state = pointers->stateFor(m);

        state->box = pointers->getCursorBoxLogicalForMonitor(state->monitor.lock());

        auto CROSSES = !m->logicalBox().intersection(CURSORBOX).empty();

        if (!CROSSES && state->cursorFrontBuffer) {
            Log::logger->log(Log::TRACE, "onCursorMoved for output {}: cursor left the viewport, removing it from the backend", m->m_name);
            pointers->setHWCursorBuffer(state, nullptr);
            continue;
        } else if (CROSSES && !state->cursorFrontBuffer) {
            Log::logger->log(Log::TRACE, "onCursorMoved for output {}: cursor entered the output, but no front buffer, forcing recalc", m->m_name);
            recalc = true;
        }

        if (!state->entered)
            continue;

        Hyprutils::Utils::CScopeGuard x([m] { m->onCursorMovedOnMonitor(); });

        if (state->hardwareFailed)
            continue;

        const auto CURSORPOS = pointers->getCursorPosForMonitor(m);
        m->m_output->moveCursor(CURSORPOS);

        state->monitor->m_scanoutNeedsCursorUpdate = true;
    }

    if (recalc)
        pointers->updateCursorBackend();

    // ignore warp
    if (!isMove && CONFIG(ignoreWarps)) {
        auto mode = this->currentMode();
        if (mode)
            mode->warp(lastPos, pointers->m_pointerPos);

        if (CONFIG(shakeEnabled))
            shake.warp(lastPos, pointers->m_pointerPos);
    }

    calculate(MOVE);

    isMove  = false;
    lastPos = pointers->m_pointerPos;
}

void CDynamicCursors::setShape(const std::string& shape) {
    // a shape change changes the sprite; never blur between two shapes
    if (shape != lastShape) {
        resetTrail(Pointer::mgr().get());
        lastShape = shape;
    }

    g_pConfigHandler->m_shapeRules->activate(shape);
    highres.loadShape(shape);
}

void CDynamicCursors::unsetShape() {
    setShape("clientside");
}

void CDynamicCursors::updateTheme() {
    // the theme reload replaces all textures; drop any trail of the old ones
    resetTrail(Pointer::mgr().get());
    highres.update();
}

/*
Handle cursor tick events.
*/
void CDynamicCursors::onTick(Pointer::CPointerManager* pointers) {
    updateBlur(pointers);
    calculate(TICK);
}

IMode* CDynamicCursors::currentMode() {
    switch (CONFIG(mode)) {
        case MODE_ROTATE: return &rotate;
        case MODE_TILT: return &tilt;
        case MODE_STRETCH: return &stretch;
        default: return nullptr;
    }
}

void CDynamicCursors::calculate(EModeUpdate type) {

    IMode* mode = currentMode();

    // calculate angle and zoom
    if (mode) {
        // reset mode if it has changed
        if (mode != lastMode)
            mode->reset();

        if (mode->strategy() == type)
            resultMode = mode->update(Pointer::mgr()->m_pointerPos);
    } else
        resultMode = SModeResult();

    lastMode = mode;

    if (CONFIG(shakeEnabled)) {
        if (type == TICK)
            resultShake = shake.update(Pointer::mgr()->m_pointerPos);

        // reset mode results if shaking
        if (resultShake > 1 && !CONFIG(shakeEffects))
            resultMode = SModeResult();
    } else
        resultShake = 1;

    auto result = resultMode;
    result.scale *= resultShake;

    if (resultShown.hasDifference(&result, CONFIG(threshold) * (std::numbers::pi / 180.0), 0.01, 0.01)) {
        resultShown = result;
        resultShown.clamp(CONFIG(threshold) * (std::numbers::pi / 180.0), 0.01, 0.01); // clamp low values so it is rendered pixel-perfectly when no effect

        // damage software and change hardware cursor shape
        Pointer::mgr()->damageIfSoftware();

        bool entered = false;

        for (auto& m : State::monitorState()->monitors()) {
            auto state = Pointer::mgr()->stateFor(m);

            if (state->entered)
                entered = true;
            if (state->hardwareFailed || !state->entered)
                continue;

            Pointer::mgr()->attemptHardwareCursor(state);
        }

        // there should always be one monitor entered
        // this fixes an issue where the cursor shape would not properly update after change
        if (!entered) {
            Log::logger->log(Log::INFO, "[dynamic-cursors] updating because none entered");
            Pointer::mgr()->recheckEnteredOutputs();
            Pointer::mgr()->updateCursorBackend();
        }
    }

    // lock software cursors if zooming; re-evaluated on every calculate, so a zoom
    // desire surviving a plugin disable/enable cycle re-asserts its software lock
    const bool ZOOM = resultShown.scale > 1;
    if (ZOOM != zoomSoftware) {
        zoomSoftware = ZOOM;
        updateSoftwareLock(Pointer::mgr().get());
    }
}
/*
(De)engage the single software cursor lock shared by zoom and motion blur.
*/
void CDynamicCursors::updateSoftwareLock(Pointer::CPointerManager* pointers) {
    const bool WANT = zoomSoftware || blurSoftware;

    if (!WANT) {
        if (softwareLockHeld) {
            // damage so the last software frame is cleared properly
            this->damageSoftware(pointers);

            // release only the locks we own
            for (auto& m : lockedMonitors) {
                if (!m.expired())
                    pointers->unlockSoftwareForMonitor(m.lock());
            }
            lockedMonitors.clear();
            softwareLockHeld = false;
        }
        return;
    }

    if (!softwareLockHeld) {
        pointers->lockSoftwareAll();
        softwareLockHeld = true;

        lockedMonitors.clear();
        for (auto& mw : pointers->m_monitorStates) {
            if (!mw->monitor.expired())
                lockedMonitors.push_back(mw->monitor);
        }
        return;
    }

    // reconcile: lock monitors that appeared while the lock was held. ownership is
    // checked, not the aggregate count: a new monitor already locked by someone else
    // must still become ours, or blur loses it when that other locker releases
    for (auto& mw : pointers->m_monitorStates) {
        if (mw->monitor.expired())
            continue;

        const bool OWNED = std::ranges::any_of(lockedMonitors, [&mw](const auto& m) { return m.lock() == mw->monitor.lock(); });
        if (OWNED)
            continue;

        pointers->lockSoftwareForMonitor(mw->monitor.lock());
        lockedMonitors.push_back(mw->monitor);
    }
}

/*
Blur housekeeping on tick: follow config changes, reconcile the software lock,
expire the rendered trail once movement has stopped, and never carry history
across a visibility change.
*/
void CDynamicCursors::updateBlur(Pointer::CPointerManager* pointers) {
    const bool WANT = CONFIG(motionBlurEnabled);
    if (WANT != blurSoftware) {
        blurSoftware = WANT;
        if (!WANT)
            resetTrail(pointers);
    }

    // reconcile every tick: also covers monitors hotplugged while the lock is held
    // and runs even while hidden, so disabling never strands the lock
    updateSoftwareLock(pointers);

    // a hidden or texture-less cursor must not keep (or record) history, so it can
    // never smear across a hide/show transition
    if (!pointers->hasCursor() || !pointers->getCurrentCursorTexture()) {
        if (lastTrailAny() || trailRendered)
            resetTrail(pointers);
        else
            trail.reset();
        return;
    }

    // keep the history rolling so the displacement decays after movement stops
    trail.record(pointers->m_pointerPos, Time::steadyNow());

    // the cursor stopped moving: expire the rendered trail with one final damage.
    // conservative: only expire once even the slowest monitor still rendering the
    // cursor has decayed, so faster monitors never cut a slower one's exposure short
    if (trailRendered) {
        float slowestHz = 0;
        for (auto& mw : pointers->m_monitorStates) {
            if (mw->monitor.expired() || !(mw->softwareLocks > 0 || mw->hardwareFailed))
                continue;

            const auto HZ = mw->monitor.lock()->m_refreshRate;
            if (HZ > 0 && (slowestHz == 0.F || HZ < slowestHz))
                slowestHz = HZ;
        }

        const auto DISPLACEMENT = trail.displacement(Time::steadyNow(), blurWindow(slowestHz));
        if (!DISPLACEMENT || DISPLACEMENT->size() <= MIN_TRAIL_DISPLACEMENT) {
            // trail fully decayed: one last damage renders the sharp cursor
            pointers->damageIfSoftware(); // hooked, so the trail areas are covered
            resetTrailState();
        } else
            // still decaying: schedule redraws so the trail shrinks smoothly instead
            // of staying frozen until the final expiry. the per-monitor exposure
            // footprints keep the damaged area bounded
            pointers->damageIfSoftware();
    }
}

/*
Clear exposure bookkeeping without damaging.
*/
void CDynamicCursors::resetTrailState() {
    lastTrailExposures.clear();
    trailRendered = false;
    trail.reset();
}

/*
Reset the blur history, damaging the rendered trail first so no ghost remains.
Only usable while the plugin is enabled, as it relies on the damage hook.
*/
void CDynamicCursors::resetTrail(Pointer::CPointerManager* pointers) {
    if (lastTrailAny()) {
        pointers->damageIfSoftware();
        lastTrailExposures.clear();
    }
    trailRendered = false;
    trail.reset();
}

/*
The union of all live per-monitor exposures.
*/
CBox CDynamicCursors::lastTrailUnion() const {
    CBox out{};
    bool any = false;

    for (const auto& e : lastTrailExposures) {
        if (e.monitor.expired())
            continue;

        out = any ? boxUnion(out, e.box) : e.box;
        any = true;
    }

    return out;
}

/*
The time window of movement the blur exposes on a monitor running at `refreshHz`:
`strength` refresh intervals, clamped to what the bounded history retains.
*/
std::chrono::milliseconds CDynamicCursors::blurWindow(float refreshHz) {
    const float STRENGTH = std::clamp(CONFIG(motionBlurStrength), 1.0F, 16.0F);
    const float HZ       = refreshHz > 0 ? refreshHz : 60.F;
    return std::chrono::milliseconds{std::clamp(static_cast<long>(STRENGTH * (1000.F / HZ)), 8L, 250L)};
}

/*
Runs on every tick while the plugin is disabled: releases locks and transient
state which the enabled-only code paths can no longer reach.
*/
void CDynamicCursors::onDisabledTick(Pointer::CPointerManager* pointers) {
    zoomSoftware = false;
    blurSoftware = false;

    if (softwareLockHeld) {
        // our own damageSoftware covers the trail area, the stock damage hook would not
        this->damageSoftware(pointers);

        // release only the locks we own; monitors hotplugged while disabled were never locked
        for (auto& m : lockedMonitors) {
            if (!m.expired())
                pointers->unlockSoftwareForMonitor(m.lock());
        }
        lockedMonitors.clear();
        softwareLockHeld = false;
    }

    resetTrailState();
}

void CDynamicCursors::setMove() {
    isMove = true;
}

void CDynamicCursors::dispatchMagnify(std::optional<int> duration, std::optional<float> size) {
    if (!CONFIG(shakeEnabled))
        return;

    shake.force(duration, size);
}
