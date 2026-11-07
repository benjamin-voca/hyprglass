#include "GlassChrome.hpp"
#include "GlassDecoration.hpp"
#include "GlassLight.hpp"
#include "Globals.hpp"
#include "RenderGuards.hpp"
#include "WindowGeometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <vector>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/window/WindowEffectsController.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>

// The window rule's to set: only the app knows where its toolbar ends, and a browser's is
// the same in every window. Tags add up, so a fixed band can go with a read strip.
static SChromeShape chromeShapeFor(const PHLWINDOW& window) {
    SChromeShape shape;
    for (const auto& tag : window->m_ruleApplicator->m_tagKeeper.getTags()) {
        if (tag.starts_with("hyprglass_chrome_auto")) {
            shape.height = shape.split = CHROME_AUTO;
            shape.side   = CHROME_AUTO;
        } else if (tag.starts_with("hyprglass_chrome_full")) {
            shape.full = true;
        } else if (int side = 0; std::sscanf(tag.c_str(), "hyprglass_chrome_side_%d", &side) == 1 && side > 0) {
            shape.side = side;
        } else if (int height = 0, split = 0; std::sscanf(tag.c_str(), "hyprglass_chrome_%d_%d", &height, &split) >= 1 && height > 0) {
            shape.height = height;
            shape.split  = split;
        }
    }
    return shape;
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
    if (const auto shape = chromeShapeFor(window); shape.any())
        g_pHyprRenderer->addPassElement(ctx, makeUnique<CGlassChromePassElement>(m_window, shape));
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

CGlassChromePassElement::CGlassChromePassElement(PHLWINDOWREF window, const SChromeShape& shape) : m_window(window), m_shape(shape) {}

std::vector<UP<IPassElement>> CGlassChromePassElement::draw(Render::CRenderContext& ctx) {
    const auto window  = m_window.lock();
    const auto monitor = ctx.m_data.pMonitor.lock();
    if (!g_pGlobalState || !window || !monitor)
        return {};
    if (auto* decoration = glassDecorationFor(window))
        decoration->renderChrome(ctx, monitor, m_shape);
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

namespace {

// Pixels read off the window, RGBA bytes.
struct SPixelLine {
    std::vector<uint8_t> data;
    int                  count = 0;

    [[nodiscard]] std::array<float, 3> at(int i) const {
        return {data[i * 4] / 255.0f, data[i * 4 + 1] / 255.0f, data[i * 4 + 2] / 255.0f};
    }
    [[nodiscard]] bool same(int a, int b) const {
        for (int channel = 0; channel < 3; ++channel) {
            if (std::abs(static_cast<int>(data[a * 4 + channel]) - static_cast<int>(data[b * 4 + channel])) > 3)
                return false;
        }
        return true;
    }
    // The run of one colour from `from`, going `step` pixels at a time: the index past it.
    [[nodiscard]] int runEnd(int from, int step = 1) const {
        int i = from;
        while (i >= 0 && i < count && same(i, from))
            i += step;
        return i;
    }
    // The colour most of the line is, in 4-bit steps: a page's background against its text.
    [[nodiscard]] std::array<float, 3> dominant() const {
        std::map<int, int> counts;
        for (int i = 0; i < count; ++i)
            ++counts[(data[i * 4] >> 4) << 8 | (data[i * 4 + 1] >> 4) << 4 | (data[i * 4 + 2] >> 4)];
        int best = 0, bestCount = -1;
        for (const auto& [key, n] : counts) {
            if (n > bestCount) {
                best      = key;
                bestCount = n;
            }
        }
        // an exact pixel of that bucket, not the bucket's rounded centre
        for (int i = 0; i < count; ++i) {
            if (((data[i * 4] >> 4) << 8 | (data[i * 4 + 1] >> 4) << 4 | (data[i * 4 + 2] >> 4)) == best)
                return at(i);
        }
        return {};
    }
};

bool readLine(const SP<Render::IFramebuffer>& source, int x, int y, int w, int h, SPixelLine& out) {
    if (w < 1 || h < 1 || x < 0 || y < 0 || x + w > source->m_size.x || y + h > source->m_size.y)
        return false;
    out.count = std::max(w, h);
    out.data.assign(static_cast<size_t>(w) * h * 4, 0);
    while (glGetError() != GL_NO_ERROR) {}
    g_pHyprOpenGL->bindFramebuffer(GL_READ_FRAMEBUFFER, GlassRenderer::framebufferId(source));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data.data());
    return glGetError() == GL_NO_ERROR; // else keep what we had: a framebuffer format this cannot read
}

} // namespace

// hyprglass_chrome_auto: read a column near the window's right end, past any tab or button, for
// where the flat bands end. A flat frame, then usually a flat toolbar (and bookmarks bar, in the
// same colour), then something else: the page. A window with no such bands gets no band, so a
// devtools window or a web app is never keyed by a browser's numbers. A row near the window's
// bottom gives the rest: a strip of one colour at the right end is a tab strip down the side,
// and the colour most of the row is is the page body's, for a window keyed whole.
void CGlassDecoration::detectChrome(const SP<Render::IFramebuffer>& source, const CBox& windowBox, float scale, const SChromeShape& shape) {
    const auto now = std::chrono::steady_clock::now();
    if (windowBox.w == m_chromeAutoWidth && windowBox.h == m_chromeAutoHeightSeen && now - m_chromeAutoAt < std::chrono::milliseconds(500))
        return;
    m_chromeAutoAt         = now;
    m_chromeAutoWidth      = windowBox.w;
    m_chromeAutoHeightSeen = windowBox.h;

    const int minimum = static_cast<int>(16.0f * scale);

    if (shape.height == CHROME_AUTO) {
        constexpr int MAX_ROWS = 512;
        const int     rows     = std::min({static_cast<int>(240.0f * scale), static_cast<int>(windowBox.h), MAX_ROWS});
        const int     x        = static_cast<int>(windowBox.x + windowBox.w - 6.0f * scale);
        SPixelLine    column;
        if (rows >= 24 && readLine(source, x, static_cast<int>(windowBox.y), 1, rows, column)) {
            const int start = static_cast<int>(4.0f * scale); // clear of the corner and the rim
            const int frame = column.runEnd(start);
            if (frame >= rows || frame < minimum) {
                m_chromeAutoHeight = m_chromeAutoSplit = 0;
            } else {
                const int toolbar = column.runEnd(frame + 1);
                if (toolbar - frame >= minimum && toolbar < rows) {
                    m_chromeAutoSplit  = frame;
                    m_chromeAutoHeight = toolbar;
                } else {
                    m_chromeAutoSplit  = 0;
                    m_chromeAutoHeight = frame;
                }
            }
        }
    }

    // The bottom row: a strip at the right end, and the page body's colour.
    const int  width = static_cast<int>(windowBox.w);
    const int  y     = static_cast<int>(windowBox.y + windowBox.h - 6.0f * scale);
    SPixelLine row;
    if (width < minimum * 2 || !readLine(source, static_cast<int>(windowBox.x), y, width, 1, row))
        return;
    m_chromeKeysValid = true;
    const int rightmost = width - 1 - static_cast<int>(4.0f * scale);
    const int stripEnd  = row.runEnd(rightmost, -1); // index left of the strip
    const int strip     = rightmost - stripEnd;
    if (shape.side == CHROME_AUTO)
        // a strip of a page's width is the page itself
        m_chromeAutoSide = (strip >= minimum && strip < width * 0.45) ? strip + static_cast<int>(4.0f * scale) : 0;
    m_chromeSideKey = row.at(rightmost);
    // the body colour comes from the row without the strip
    const int sideWidth = shape.side == CHROME_AUTO ? m_chromeAutoSide : std::max(0, static_cast<int>(shape.side * scale));
    SPixelLine body;
    body.count = std::max(1, width - sideWidth);
    body.data.assign(row.data.begin(), row.data.begin() + static_cast<size_t>(body.count) * 4);
    m_chromeBodyKey = body.dominant();
}

void CGlassDecoration::renderChrome(Render::CRenderContext& ctx, PHLMONITOR monitor, const SChromeShape& shape) {
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

    const CBox  windowBox = *optBox;
    const float scale     = monitor->m_scale;

    double heightPx = std::max(shape.height, 0) * scale;
    double splitPx  = std::max(shape.split, 0) * scale;
    double sidePx   = std::max(shape.side, 0) * scale;
    if (shape.height == CHROME_AUTO || shape.side == CHROME_AUTO || shape.side > 0 || shape.full)
        detectChrome(source, windowBox, scale, shape);
    if (shape.height == CHROME_AUTO) {
        heightPx = m_chromeAutoHeight;
        splitPx  = m_chromeAutoSplit;
    }
    if (shape.side == CHROME_AUTO)
        sidePx = m_chromeAutoSide;
    heightPx = std::min(heightPx, windowBox.h);
    sidePx   = std::min(sidePx, windowBox.w);

    auto& shaderManager = g_pGlobalState->shaderManager;
    const auto* program = shaderManager.chromeProgram();
    if (!program)
        return;

    const std::string     preset         = resolvePresetName();
    const SResolveContext resolveContext = {preset, resolveThemeIsDark(), g_pGlobalState->config, g_pGlobalState->customPresets};
    const auto&           config         = g_pGlobalState->config;
    const float           keep           = config.chromeOpacity ? static_cast<float>(**config.chromeOpacity) : 0.25f;

    // The glass again, clipped to the keyed box, then the app's pixels back over it with
    // their background lifted out.
    const auto keyed = [&](SP<Render::IFramebuffer>& copy, const CBox& box, int corners, const GlassRenderer::SChromeKey& key) {
        CRegion boxDamage = ctx.m_data.damage.copy().intersect(box);
        if (boxDamage.empty())
            return;
        Vector2D padding;
        GlassRenderer::sampleBackground(ctx, copy, source, box, padding, 1);
        CRegion fullDamage = ctx.m_data.damage;
        ctx.m_data.damage  = boxDamage;
        GlassRenderer::applyGlassEffect(ctx, m_sampleFramebuffer, source, windowBox, m_chromeAlpha,
                                        std::array<float, 4>{m_chromeRadius, m_chromeRadius, m_chromeRadius, m_chromeRadius}, m_chromePower,
                                        m_samplePaddingRatio, resolveContext, nullptr, m_chromeFocus, nullptr);
        GlassRenderer::drawChromeKey(ctx, *program, copy, source, box, padding, m_chromeRadius, m_chromePower, keep, m_chromeAlpha, corners, key);
        ctx.m_data.damage = fullDamage;
    };

    using GlassRenderer::CORNER_BL;
    using GlassRenderer::CORNER_BR;
    using GlassRenderer::CORNER_TL;
    using GlassRenderer::CORNER_TR;

    if (shape.full) {
        // The whole window: frame, toolbar and the page body are each a key.
        GlassRenderer::SChromeKey key;
        key.mode    = GlassRenderer::CHROME_FULL;
        key.splitPx = static_cast<float>(splitPx);
        key.bodyPx  = static_cast<float>(heightPx);
        key.body    = m_chromeKeysValid ? m_chromeBodyKey : std::array<float, 3>{};
        key.hasBody = m_chromeKeysValid;
        keyed(m_chromeCopy, windowBox, CORNER_TL | CORNER_TR | CORNER_BR | CORNER_BL, key);
        return;
    }

    if (heightPx > 0.0) {
        GlassRenderer::SChromeKey key;
        key.mode    = GlassRenderer::CHROME_TOP;
        key.splitPx = static_cast<float>(splitPx);
        keyed(m_chromeCopy, CBox{windowBox.x, windowBox.y, windowBox.w, heightPx}, CORNER_TL | CORNER_TR, key);
    }
    if (sidePx > 0.0 && m_chromeKeysValid) {
        GlassRenderer::SChromeKey key;
        key.mode = GlassRenderer::CHROME_SIDE;
        key.body = m_chromeSideKey;
        key.hasBody = true;
        const CBox strip = {windowBox.x + windowBox.w - sidePx, windowBox.y + heightPx, sidePx, windowBox.h - heightPx};
        keyed(m_chromeSideCopy, strip, CORNER_BR | (heightPx > 0.0 ? 0 : CORNER_TR), key);
    }
    if (heightPx > 0.0 || sidePx > 0.0)
        renderPageCard(ctx, monitor, windowBox, heightPx, sidePx, resolveContext);
}

// The page as a sheet lying in the glass: rounded corners where it meets the band and the
// strip, with glass showing in the cut, a rim along its edge, and a shadow it casts on the glass.
void CGlassDecoration::renderPageCard(Render::CRenderContext& ctx, PHLMONITOR monitor, const CBox& windowBox, double heightPx, double sidePx,
                                      const SResolveContext& resolveContext) {
    using GlassLight::f;
    const auto& config = g_pGlobalState->config;
    const float scale  = monitor->m_scale;
    const float radius = std::max(0.0f, f(config.chromePageRadius)) * scale;
    if (radius < 1.0f)
        return;
    const float rim       = std::clamp(f(config.chromePageRim), 0.0f, 1.0f);
    const float shadowPx  = std::max(0.0f, f(config.chromePageShadow)) * scale;
    const auto  source    = ctx.m_data.currentFB;
    const auto* program   = g_pGlobalState->shaderManager.pageProgram();
    if (!source || !program)
        return;

    const CBox page = {windowBox.x, windowBox.y + heightPx, windowBox.w - sidePx, windowBox.h - heightPx};
    if (page.w < radius * 4.0 || page.h < radius * 4.0)
        return;

    // The shadow falls on the glass: the card box reaches into the band and the strip by that much.
    const double top   = heightPx > 0.0 ? std::min(static_cast<double>(shadowPx), heightPx) : 0.0;
    const double right = sidePx > 0.0 ? std::min(static_cast<double>(shadowPx), sidePx) : 0.0;
    const CBox   card  = {page.x, page.y - top, page.w + right, page.h + top};

    // Corners that meet glass get the radius; the window's own corners stay as Hyprland rounds them.
    const float  rTL = heightPx > 0.0 ? radius : 0.0f;
    const float  rTR = (heightPx > 0.0 || sidePx > 0.0) ? radius : 0.0f;
    const float  rBR = sidePx > 0.0 ? radius : 0.0f;
    const float  rBL = 0.0f;

    // Only the ring can change how the page looks: the rim, the corners and the shadow.
    const int ring = static_cast<int>(std::ceil(std::max(radius, shadowPx) + 4.0f * scale));
    CRegion   ringRegion;
    ringRegion.add(CBox{card.x, card.y, card.w, static_cast<double>(ring)});
    ringRegion.add(CBox{card.x, card.y + card.h - ring, card.w, static_cast<double>(ring)});
    ringRegion.add(CBox{card.x, card.y, static_cast<double>(ring), card.h});
    ringRegion.add(CBox{card.x + card.w - ring, card.y, static_cast<double>(ring), card.h});
    CRegion ringDamage = ctx.m_data.damage.copy().intersect(ringRegion);
    if (ringDamage.empty())
        return;

    Vector2D padding;
    GlassRenderer::sampleBackground(ctx, m_pageCopy, source, card, padding, 1);

    // Glass in the cut of each rounded corner, and under the shadow margin.
    CRegion glassRegion;
    if (rTL > 0.0f)
        glassRegion.add(CBox{page.x, page.y, static_cast<double>(radius), static_cast<double>(radius)});
    if (rTR > 0.0f)
        glassRegion.add(CBox{page.x + page.w - radius, page.y, static_cast<double>(radius), static_cast<double>(radius)});
    if (rBR > 0.0f)
        glassRegion.add(CBox{page.x + page.w - radius, page.y + page.h - radius, static_cast<double>(radius), static_cast<double>(radius)});
    CRegion fullDamage = ctx.m_data.damage;
    ctx.m_data.damage  = ringDamage.copy().intersect(glassRegion);
    if (!ctx.m_data.damage.empty())
        GlassRenderer::applyGlassEffect(ctx, m_sampleFramebuffer, source, windowBox, m_chromeAlpha,
                                        std::array<float, 4>{m_chromeRadius, m_chromeRadius, m_chromeRadius, m_chromeRadius}, m_chromePower,
                                        m_samplePaddingRatio, resolveContext, nullptr, m_chromeFocus, nullptr);
    ctx.m_data.damage = ringDamage;

    GlassRenderer::SPageCard spec;
    spec.pageInCard = {static_cast<float>(page.x - card.x), static_cast<float>(page.y - card.y), static_cast<float>(page.x + page.w - card.x),
                       static_cast<float>(page.y + page.h - card.y)};
    spec.radii      = {rTL, rTR, rBR, rBL};
    spec.power      = m_chromePower;
    spec.rimWidth   = 1.6f * scale;
    spec.rim        = rim;
    spec.shadowPx   = shadowPx;
    spec.shadow     = 0.28f;
    spec.alpha      = m_chromeAlpha;
    const Vector2D light = g_pGlobalState->lightFrame.lightPos - Vector2D{card.x + card.w * 0.5, card.y + card.h * 0.5};
    const double   len   = std::hypot(light.x, light.y);
    spec.lightDir        = len > 1.0 ? Vector2D{light.x / len, light.y / len} : Vector2D{0.0, -1.0};
    GlassRenderer::drawPageCard(ctx, *program, m_pageCopy, source, card, padding, spec);
    ctx.m_data.damage = fullDamage;
}
