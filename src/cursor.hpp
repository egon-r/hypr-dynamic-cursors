#include <any>    // IWYU pragma: keep
#include <chrono> // IWYU pragma: keep
#define private public
#include <hyprland/src/pointer/PointerManager.hpp>
#undef private

#include <hyprcursor/hyprcursor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprutils/math/Vector2D.hpp>

#include "mode/ModeRotate.hpp"
#include "mode/ModeTilt.hpp"
#include "mode/ModeStretch.hpp"
#include "other/Shake.hpp"
#include "highres.hpp"
#include <vector>
#include "other/Trail.hpp"

/*
The last blur exposure a single monitor still shows, so damage coverage stays
bounded per monitor instead of growing with the movement history.
*/
struct SLastTrailExposure {
    PHLMONITORREF monitor; // weak; entries of removed monitors are skipped
    CBox          box;     // global logical extents of the last drawn exposure
};
class CDynamicCursors {
  public:
    CDynamicCursors();
    ~CDynamicCursors();

    /* hook on onCursorMoved */
    void onCursorMoved(Pointer::CPointerManager* pointers);
    /* called on tick */
    void onTick(Pointer::CPointerManager* pointers);

    /* hook on renderSoftwareCursorsFor */
    void renderSoftware(Pointer::CPointerManager* pointers, PHLMONITOR pMonitor, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> overridePos, bool screencopy,
                        bool forceRender);
    /* hook on damageIfSoftware*/
    void damageSoftware(Pointer::CPointerManager* pointers);
    /* hook on renderHWCursorBuffer */
    SP<Aquamarine::IBuffer> renderHardware(Pointer::CPointerManager* pointers, SP<Pointer::CPointerManager::SMonitorPointerState> state, SP<Render::ITexture> texture);
    /* hook on setHWCursorBuffer */
    bool                    setHardware(Pointer::CPointerManager* pointers, SP<Pointer::CPointerManager::SMonitorPointerState> state, SP<Aquamarine::IBuffer> buf);

    /* hook on setCursorFromName */
    void setShape(const std::string& name);
    /* hook on setCursorSoftware */
    void unsetShape();
    /* hook on updateTheme */
    void updateTheme();

    /* hook on move, indicate that next onCursorMoved is actual move */
    void setMove();

    void dispatchMagnify(std::optional<int> duration, std::optional<float> size);
    /* called from tickRaw even while the plugin is disabled, to release state the enabled-only paths would leak */
    void onDisabledTick(Pointer::CPointerManager* pointers);

  private:
    SP<CEventLoopTimer> tick;

    /* hyprcursor handler for highres images */
    CHighresHandler highres;

    // current state of the cursor
    SModeResult resultMode;
    double      resultShake;
    Vector2D    lastPos; // used for warp compensation

    SModeResult resultShown;

    // whether we have already locked software for cursor zoom
    bool zoomSoftware = false;
    // whether motion blur wants the software lock held
    bool blurSoftware = false;
    // whether the combined zoom/blur software lock is currently held
    bool softwareLockHeld = false;

    // modes
    CModeRotate  rotate;
    CModeTilt    tilt;
    CModeStretch stretch;

    /* returns the current mode, nullptr if none is selected */
    IMode* currentMode();
    IMode* lastMode; // used to reset the mode if it was switched (to prune stale data)

    // shake
    CShake shake;

    /* is set true if a genuine move is being performed, and will be reset to false after onCursorMoved */
    bool isMove = false;

    // cursor movement history for motion blur
    CTrailTracker                   trail;
    // per-monitor last drawn blur exposure, appended once per monitor
    std::vector<SLastTrailExposure> lastTrailExposures;
    // monitors this plugin holds a software lock on (reconciled on hotplug)
    std::vector<PHLMONITORREF>      lockedMonitors;
    bool                            trailRendered = false; // whether a blur trail may currently be visible anywhere
    std::string                     lastShape;             // shape name dedup for blur resets

    /* (de)engage the combined software cursor lock for zoom and blur */
    void updateSoftwareLock(Pointer::CPointerManager* pointers);
    /* blur housekeeping: config changes, trail expiry, cursor disappearance */
    void updateBlur(Pointer::CPointerManager* pointers);
    /* union of all live per-monitor exposures, invalid box when lastTrailAny() is false */
    CBox lastTrailUnion() const;
    /* whether any monitor still has a recorded exposure */
    bool lastTrailAny() const {
        return !lastTrailExposures.empty();
    }
    /* clear exposure bookkeeping without damaging */
    void                             resetTrailState();
    /* reset the blur history, damaging the rendered trail first so no ghost remains */
    void                             resetTrail(Pointer::CPointerManager* pointers);
    /* time window of movement the blur exposes, for a monitor running at `refreshHz` */
    static std::chrono::milliseconds blurWindow(float refreshHz);

    // calculates the current angle of the cursor, and changes the cursor shape
    void calculate(EModeUpdate type);
};

inline UP<CDynamicCursors> g_pDynamicCursors;
