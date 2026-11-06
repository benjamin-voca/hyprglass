#include "Frost.hpp"
#include "WindowGeometry.hpp"

#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Region.hpp>

namespace Frost {

namespace {

// Hyprland draws tiled windows, then floating ones, then pinned ones, each group in
// list order (later is above), with a visible special workspace over the regular ones.
int drawTier(const PHLWINDOW& window) {
    const int layer = (window->m_state & Desktop::View::WINDOW_STATE_PINNED) ? 2 : (window->isFloating() ? 1 : 0);
    return window->onSpecialWorkspace() ? layer + 3 : layer;
}

} // namespace

float coveredFraction(const PHLWINDOW& window, const PHLMONITOR& monitor, const CBox& glassBox) {
    const double glassArea = glassBox.w * glassBox.h;
    if (!window || !monitor || !(glassArea > 0.0))
        return 0.0f;

    // Under a fullscreen window only it, and the floating windows allowed over it, are drawn.
    PHLWINDOW fullscreenWindow;
    if (window->m_workspace) {
        fullscreenWindow = Fullscreen::controller()->getFullscreenWindow(window->m_workspace);
        if (fullscreenWindow && fullscreenWindow->m_workspace != window->m_workspace)
            fullscreenWindow = nullptr;
    }

    const int ownTier = drawTier(window);
    bool      seenOwn = false;
    CRegion   covered;

    for (const auto& other : Desktop::viewState()->windows()) {
        if (!other)
            continue;
        if (other == window) {
            seenOwn = true;
            continue;
        }

        const int  tier  = drawTier(other);
        const bool below = tier < ownTier || (tier == ownTier && !seenOwn);
        if (!below)
            continue;

        if (!other->mapped() || other->isHidden() || other->presentation().alphaTotal() == 0.f)
            continue;
        if (fullscreenWindow && other != fullscreenWindow && !(other->isFloating() && other->shouldRenderOverFullscreen()))
            continue;
        if (!g_pHyprRenderer->shouldRenderWindow(other, monitor))
            continue;

        const auto box = WindowGeometry::computeWindowBox(other, monitor);
        if (!box)
            continue;

        const CBox overlap = box->intersection(glassBox);
        if (overlap.w > 0.0 && overlap.h > 0.0)
            covered.add(overlap);
    }

    double coveredArea = 0.0;
    for (const auto& rectangle : covered.getRects())
        coveredArea += static_cast<double>(rectangle.x2 - rectangle.x1) * static_cast<double>(rectangle.y2 - rectangle.y1);

    return static_cast<float>(std::clamp(coveredArea / glassArea, 0.0, 1.0));
}

int visibleWindowRects(const PHLMONITOR& monitor, const CBox& layerBox, float* out, int maxRects) {
    if (!monitor || !out)
        return 0;

    int count = 0;
    for (const auto& window : Desktop::viewState()->windows()) {
        if (count >= maxRects)
            break;
        if (!window || !window->mapped() || window->isHidden() || window->presentation().alphaTotal() == 0.f)
            continue;
        if (!g_pHyprRenderer->shouldRenderWindow(window, monitor))
            continue;

        const auto box = WindowGeometry::computeWindowBox(window, monitor);
        if (!box)
            continue;

        const CBox overlap = box->intersection(layerBox);
        if (overlap.w <= 0.0 || overlap.h <= 0.0)
            continue;

        out[count * 4 + 0] = static_cast<float>(box->x - layerBox.x);
        out[count * 4 + 1] = static_cast<float>(box->y - layerBox.y);
        out[count * 4 + 2] = static_cast<float>(box->x + box->w - layerBox.x);
        out[count * 4 + 3] = static_cast<float>(box->y + box->h - layerBox.y);
        ++count;
    }
    return count;
}

} // namespace Frost
