#include "GlassChrome.hpp"
#include "GlassDecoration.hpp"
#include "Globals.hpp"
#include "RenderGuards.hpp"
#include "WindowGeometry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/window/WindowEffectsController.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

// hyprglass_chrome_<height>[_<tab strip height>], {0, 0} without such a tag. These are the
// window rule's to set: only the app knows where its toolbar ends, and a browser's is the
// same in every window.
struct SChromeShape {
    int height = 0;
    int split  = 0;
};

inline constexpr int CHROME_AUTO = -1;

static SChromeShape chromeShapeFor(const PHLWINDOW& window) {
    for (const auto& tag : window->m_ruleApplicator->m_tagKeeper.getTags()) {
        if (tag.starts_with("hyprglass_chrome_auto"))
            return {CHROME_AUTO, CHROME_AUTO};
        SChromeShape shape;
        if (std::sscanf(tag.c_str(), "hyprglass_chrome_%d_%d", &shape.height, &shape.split) >= 1 && shape.height > 0)
            return shape;
    }
    return {};
}

CGlassChromeDecoration::CGlassChromeDecoration(PHLWINDOW window) : IHyprWindowDecoration(window), m_window(window) {}

SDecorationPositioningInfo CGlassChromeDecoration::getPositioningInfo() {
    SDecorationPositioningInfo info;
    info.priority       = 9999;
    info.policy         = DECORATION_POSITION_ABSOLUTE;
    info.desiredExtents = {{0, 0}, {0, 0}};
    return info;
}

void CGlassChromeDecoration::onPositioningReply(const SDecorationPositioningReply&) {}

void CGlassChromeDecoration::draw(Render::CRenderContext& ctx, PHLMONITOR, float const&, const Render::SWindowRenderPresentation&) {
    const auto window = m_window.lock();
    if (!g_pGlobalState || !window || RenderGuards::isForeignRender(ctx))
        return;
    if (const auto shape = chromeShapeFor(window); shape.height != 0)
        g_pHyprRenderer->addPassElement(ctx, makeUnique<CGlassChromePassElement>(m_window, shape.height, shape.split));
}

eDecorationType CGlassChromeDecoration::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CGlassChromeDecoration::updateWindow(PHLWINDOW) {}

void CGlassChromeDecoration::damageEntire() {}

eDecorationLayer CGlassChromeDecoration::getDecorationLayer() {
    return DECORATION_LAYER_OVER;
}

uint64_t CGlassChromeDecoration::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

std::string CGlassChromeDecoration::getDisplayName() {
    return "HyprGlassChrome";
}

CGlassChromePassElement::CGlassChromePassElement(PHLWINDOWREF window, int heightLogical, int splitLogical) :
    m_window(window), m_heightLogical(heightLogical), m_splitLogical(splitLogical) {}

std::vector<UP<IPassElement>> CGlassChromePassElement::draw(Render::CRenderContext& ctx) {
    const auto window  = m_window.lock();
    const auto monitor = ctx.m_data.pMonitor.lock();
    if (!g_pGlobalState || !window || !monitor)
        return {};
    if (auto* decoration = glassDecorationFor(window))
        decoration->renderChrome(ctx, monitor, m_heightLogical, m_splitLogical);
    return {};
}

std::optional<CBox> CGlassChromePassElement::boundingBox(Render::CRenderContext& ctx) {
    const auto window  = m_window.lock();
    const auto monitor = ctx.m_data.pMonitor.lock();
    if (!window || !monitor)
        return std::nullopt;
    auto box = WindowGeometry::computeWindowBox(window, monitor);
    if (!box)
        return std::nullopt;
    const float scale = monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
    box->scale(1.0 / scale).round();
    return box;
}

// hyprglass_chrome_auto: read a column near the window's right end, past any tab or button, for
// where the flat bands end. A flat frame, then usually a flat toolbar (and bookmarks bar, in the
// same colour), then something else: the page. A window with no such bands gets no chrome pass,
// so a devtools window or a web app is never keyed by a browser's numbers.
void CGlassDecoration::detectChrome(const SP<Render::IFramebuffer>& source, const CBox& windowBox, float scale) {
    const auto now = std::chrono::steady_clock::now();
    if (windowBox.w == m_chromeAutoWidth && now - m_chromeAutoAt < std::chrono::milliseconds(500))
        return;
    m_chromeAutoAt    = now;
    m_chromeAutoWidth = windowBox.w;

    constexpr int MAX_ROWS = 512;
    const int     rows     = std::min({static_cast<int>(240.0f * scale), static_cast<int>(windowBox.h), MAX_ROWS});
    const int     x        = static_cast<int>(windowBox.x + windowBox.w - 6.0f * scale);
    const int     y        = static_cast<int>(windowBox.y);
    if (rows < 24 || x < 0 || y < 0 || x >= source->m_size.x || y + rows > source->m_size.y)
        return;

    std::array<uint8_t, MAX_ROWS * 4> column{};
    while (glGetError() != GL_NO_ERROR) {}
    g_pHyprOpenGL->bindFramebuffer(GL_READ_FRAMEBUFFER, GlassRenderer::framebufferId(source));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, 1, rows, GL_RGBA, GL_UNSIGNED_BYTE, column.data());
    if (glGetError() != GL_NO_ERROR)
        return; // keep what we had: a framebuffer format this cannot read

    const auto same = [&](int a, int b) {
        for (int channel = 0; channel < 3; ++channel) {
            if (std::abs(static_cast<int>(column[a * 4 + channel]) - static_cast<int>(column[b * 4 + channel])) > 3)
                return false;
        }
        return true;
    };
    const auto runEnd = [&](int from) {
        int row = from;
        while (row < rows && same(row, from))
            ++row;
        return row;
    };

    const int minimum = static_cast<int>(16.0f * scale);
    const int start   = static_cast<int>(4.0f * scale); // clear of the corner and the rim
    const int frame   = runEnd(start);
    if (frame >= rows || frame < minimum) {
        m_chromeAutoHeight = m_chromeAutoSplit = 0;
        return;
    }

    const int toolbar = runEnd(frame + 1);
    if (toolbar - frame >= minimum && toolbar < rows) {
        m_chromeAutoSplit  = frame;
        m_chromeAutoHeight = toolbar;
    } else {
        m_chromeAutoSplit  = 0;
        m_chromeAutoHeight = frame;
    }
}

void CGlassDecoration::renderChrome(Render::CRenderContext& ctx, PHLMONITOR monitor, int heightLogical, int splitLogical) {
    const auto window = m_window.lock();
    if (!window || !monitor || !g_pGlobalState)
        return;
    // Only over a glass draw of this same frame; the sample could be stale or absent otherwise.
    if (m_chromeReadySerial != g_pGlobalState->frameSerial || !m_sampleFramebuffer || !m_hasCachedSample)
        return;
    // Fullscreen has no toolbar, and a transformed window is drawn through a redirected pass.
    if (Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN || window->effects().hasActiveTransformers() || RenderGuards::isForeignRender(ctx))
        return;

    const auto source = ctx.m_data.currentFB;
    if (!source)
        return;
    const auto optBox = WindowGeometry::computeWindowBox(window, monitor);
    if (!optBox)
        return;

    const CBox windowBox = *optBox;

    double heightPx = heightLogical * monitor->m_scale;
    double splitPx  = splitLogical * monitor->m_scale;
    if (heightLogical == CHROME_AUTO) {
        detectChrome(source, windowBox, monitor->m_scale);
        heightPx = m_chromeAutoHeight;
        splitPx  = m_chromeAutoSplit;
    }
    if (heightPx <= 0.0)
        return;

    const CBox chrome = {windowBox.x, windowBox.y, windowBox.w, std::min(heightPx, windowBox.h)};

    CRegion chromeDamage = ctx.m_data.damage.copy().intersect(chrome);
    if (chromeDamage.empty())
        return;

    auto& shaderManager = g_pGlobalState->shaderManager;
    const auto* program = shaderManager.chromeProgram();
    if (!program)
        return;

    Vector2D padding;
    GlassRenderer::sampleBackground(ctx, m_chromeCopy, source, chrome, padding, 1);

    const std::string     preset         = resolvePresetName();
    const SResolveContext resolveContext = {preset, resolveThemeIsDark(), g_pGlobalState->config, g_pGlobalState->customPresets};

    // The glass again, as the frame is clipped to the part of it that gets keyed.
    CRegion fullDamage = ctx.m_data.damage;
    ctx.m_data.damage  = chromeDamage;
    GlassRenderer::applyGlassEffect(ctx, m_sampleFramebuffer, source, windowBox, m_chromeAlpha,
                                    std::array<float, 4>{m_chromeRadius, m_chromeRadius, m_chromeRadius, m_chromeRadius}, m_chromePower,
                                    m_samplePaddingRatio, resolveContext, nullptr, m_chromeFocus, nullptr);

    const auto& config = g_pGlobalState->config;
    GlassRenderer::drawChromeKey(ctx, *program, m_chromeCopy, source, chrome, padding, m_chromeRadius, m_chromePower,
                                 config.chromeOpacity ? static_cast<float>(**config.chromeOpacity) : 0.25f, m_chromeAlpha,
                                 static_cast<float>(splitPx));
    ctx.m_data.damage = fullDamage;
}
