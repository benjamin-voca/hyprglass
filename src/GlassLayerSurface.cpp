#include "GlassLayerSurface.hpp"
#include "BackgroundDamageObserver.hpp"
#include "BuiltInPresets.hpp"
#include "Diagnostics.hpp"
#include "GlassRenderer.hpp"
#include "GlassSnapshotElement.hpp"
#include "Frost.hpp"
#include "Globals.hpp"
#include "LayerGeometry.hpp"
#include "WorkspaceAnimation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <GLES3/gl32.h>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Misc.hpp>

CGlassLayerSurface::CGlassLayerSurface(PHLLS layerSurface)
    : m_layerSurface(layerSurface) {
}

CGlassLayerSurface::~CGlassLayerSurface() {
    // Damage the area where glass was last drawn so the compositor
    // re-renders it without the glass effect (prevents ghost artifacts).
    if (g_pHyprRenderer && m_lastSize.x > 0 && m_lastSize.y > 0 &&
        std::isfinite(m_lastPosition.x) && std::isfinite(m_lastPosition.y) &&
        std::isfinite(m_lastSize.x) && std::isfinite(m_lastSize.y)) {
        auto box = CBox{m_lastPosition, m_lastSize};
        box.expand(GlassRenderer::SAMPLE_PADDING_PX).noNegativeSize();
        if (box.w > 0.0 && box.h > 0.0)
            g_pHyprRenderer->damageBox(box);
    }
}

bool CGlassLayerSurface::resolveThemeIsDark() const {
    try {
        const auto& config = g_pGlobalState->config;
        const auto theme = readStringConfig(config.defaultTheme);
        if (!theme.empty())
            return theme != "light";
    } catch (...) {}

    return true;
}

bool CGlassLayerSurface::resolveXray() const {
    // A background or bottom layer would sample its own previous output from
    // the snapshot, and live sampling already shows nothing but what is under it.
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface || layerSurface->m_layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM || !xrayHasBackground())
        return false;

    // Without a workspace no window is drawn and no snapshot is ever taken.
    const auto monitor = layerSurface->m_monitor.lock();
    if (!monitor || !monitor->m_activeWorkspace)
        return false;

    // The lockscreen pass renders layers with no snapshot copy before them, so a
    // resample there would defer every frame until unlock.
    if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
        return false;

    const bool            isDark = resolveThemeIsDark();
    const std::string     preset = resolvePresetName();
    const SResolveContext ctx    = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};
    return resolvePresetInt(ctx, &SPresetValues::xray, &SOverridableConfig::xray, GlobalDefaults::XRAY) > 0;
}

bool CGlassLayerSurface::xraySnapshotCovers(Render::CRenderContext& ctx, PHLMONITOR monitor) const {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface || !monitor)
        return false;

    const auto layerBox = LayerGeometry::computeLayerBox(layerSurface, monitor);
    if (!layerBox)
        return false;

    // The full layer box: sampleAndRedirect() samples it or the blur region
    // inside it, so covering it covers both.
    return static_cast<bool>(xraySnapshotCovering(monitor, ctx.m_data.currentFB, *layerBox));
}

std::string CGlassLayerSurface::resolvePresetName() const {
    try {
        // Per-namespace preset override (highest priority)
        const auto layerSurface = m_layerSurface.lock();
        if (layerSurface) {
            const auto& nsPresets = g_pGlobalState->layerNamespacePresets;
            auto it = nsPresets.find(layerSurface->m_namespace);
            if (it != nsPresets.end())
                return it->second;
        }

        const auto& config = g_pGlobalState->config;

        // Layer-wide preset override
        const auto layerPreset = readStringConfig(config.layersPreset);
        if (!layerPreset.empty())
            return std::string(layerPreset);

        // Fall back to global default preset
        const auto defaultPreset = readStringConfig(config.defaultPreset);
        if (!defaultPreset.empty())
            return std::string(defaultPreset);
    } catch (...) {}

    return "default";
}

ELayerMaskMode CGlassLayerSurface::resolveMaskMode() const {
    if (const auto layerSurface = m_layerSurface.lock()) {
        const auto& overrides = g_pGlobalState->layerNamespaceMaskModes;
        if (auto it = overrides.find(layerSurface->m_namespace); it != overrides.end())
            return it->second;
    }

    if (auto mode = parseLayerMaskMode(readStringConfig(g_pGlobalState->config.layersMaskMode)))
        return *mode;
    return ELayerMaskMode::AUTO;
}

std::optional<SLayerShape> CGlassLayerSurface::resolveShape() const {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return std::nullopt;

    const auto& shapes = g_pGlobalState->layerNamespaceShapes;
    if (auto it = shapes.find(layerSurface->m_namespace); it != shapes.end())
        return it->second;
    return std::nullopt;
}

CGlassLayerSurface::EMaskSource CGlassLayerSurface::resolveMaskSource() const {
    const auto layerSurface = m_layerSurface.lock();
    const auto wlSurface    = layerSurface ? layerSurface->wlSurface() : nullptr; // root surface only
    const bool hasEffect    = wlSurface && wlSurface->m_hasBackgroundEffect;
    const bool regionEmpty  = !wlSurface || wlSurface->m_blurRegion.empty();

    switch (resolveMaskMode()) {
        case ELayerMaskMode::ALPHA:
            return EMaskSource::ALPHA_THRESHOLD;
        case ELayerMaskMode::REGION:
            return (hasEffect && !regionEmpty) ? EMaskSource::PROTOCOL_REGION : EMaskSource::NONE;
        case ELayerMaskMode::CONTOUR:
            return g_pGlobalState->contourUnavailable ? EMaskSource::ALPHA_THRESHOLD : EMaskSource::CONTOUR;
        case ELayerMaskMode::AUTO:
        default:
            if (hasEffect)
                return regionEmpty ? EMaskSource::NONE : EMaskSource::PROTOCOL_REGION;
            return EMaskSource::ALPHA_THRESHOLD;
    }
}

bool CGlassLayerSurface::liveResampleEnabled() const {
    if (const auto layerSurface = m_layerSurface.lock()) {
        const auto& overrides = g_pGlobalState->layerNamespaceLiveResample;
        if (auto it = overrides.find(layerSurface->m_namespace); it != overrides.end())
            return it->second;
    }

    const auto& config = g_pGlobalState->config;
    return config.layersLiveResample && **config.layersLiveResample;
}

PHLLS CGlassLayerSurface::getLayerSurface() const {
    return m_layerSurface.lock();
}

CRegion CGlassLayerSurface::transformedBlurRegion(PHLMONITOR monitor, const CBox& layerBox) const {
    const auto layerSurface = m_layerSurface.lock();
    const auto wlSurface    = layerSurface ? layerSurface->wlSurface() : nullptr;
    if (!layerSurface || !wlSurface || !monitor)
        return {};

    const auto logicalSize = layerSurface->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    // surface-local logical region -> monitor-local pixels, the space layerBox is in
    CRegion region = wlSurface->m_blurRegion.copy();
    region.intersect(0, 0, logicalSize.x, logicalSize.y); // spec: clipped to surface size
    region.scale(static_cast<float>(monitor->m_scale));
    region.translate(layerBox.pos());
    region.intersect(layerBox.x, layerBox.y, layerBox.w, layerBox.h); // defensive
    return region;
}

std::optional<CBox> CGlassLayerSurface::regionBoundingBoxAbsolute(PHLMONITOR monitor, const CBox& layerBox) const {
    if (!monitor)
        return std::nullopt;

    auto region = transformedBlurRegion(monitor, layerBox);
    if (region.empty())
        return std::nullopt;

    return region.getExtents();
}

std::optional<CBox> CGlassLayerSurface::regionBoundingBoxGlobal() const {
    const auto layerSurface = m_layerSurface.lock();
    const auto wlSurface    = layerSurface ? layerSurface->wlSurface() : nullptr;
    if (!layerSurface || !wlSurface)
        return std::nullopt;

    const auto position    = layerSurface->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto logicalSize = layerSurface->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    // Stays in the layer's own global-logical family (no monitor scale/transform
    // applied): BackgroundDamageObserver's damagedBox is global-logical too.
    CRegion region = wlSurface->m_blurRegion.copy();
    region.intersect(0, 0, logicalSize.x, logicalSize.y);
    region.translate(position);

    if (region.empty())
        return std::nullopt;

    return region.getExtents();
}

void CGlassLayerSurface::damageIfMoved() {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    const auto currentPosition = layerSurface->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto currentSize     = layerSurface->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    if (currentSize.x <= 0.0 || currentSize.y <= 0.0 ||
        !std::isfinite(currentPosition.x) || !std::isfinite(currentPosition.y) ||
        !std::isfinite(currentSize.x) || !std::isfinite(currentSize.y))
        return;

    const bool isAnimating = layerSurface->positionAnimation()->isBeingAnimated() ||
                             layerSurface->sizeAnimation()->isBeingAnimated() ||
                             layerSurface->alpha()[Desktop::View::LS_ALPHA_FADE]->isBeingAnimated() ||
                             !layerSurface->mapped();

    const bool moved = currentPosition != m_lastPosition || currentSize != m_lastSize;

    if (moved || isAnimating) {
        m_lastPosition  = currentPosition;
        m_lastSize      = currentSize;

        damageSampleRegion();

        if (const auto monitor = layerSurface->m_monitor.lock())
            g_pGlobalState->bumpSceneGeneration(monitor);
    } else if (const auto& config = g_pGlobalState->config;
               config.layersForceLiveResample && **config.layersForceLiveResample) {
        // keep frames flowing so the forced per-frame resample actually runs
        damageSampleRegion();
    }
}

void CGlassLayerSurface::damageSampleRegion() {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    const auto monitor = layerSurface->m_monitor.lock();
    const float scale = monitor ? monitor->m_scale : 1.0f;
    auto box = CBox{layerSurface->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT),
                    layerSurface->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
    box.expand(GlassRenderer::SAMPLE_PADDING_PX / scale).noNegativeSize();
    if (box.w > 0.0 && box.h > 0.0 &&
        std::isfinite(box.x) && std::isfinite(box.y) && std::isfinite(box.w) && std::isfinite(box.h))
        g_pHyprRenderer->damageBox(box);
}

// Motion only shows on panels: strips stay still by design. A shell's input region is its panels,
// its bar and its frame, so redraw the wide rectangles and leave the screen's edges out of it.
void CGlassLayerSurface::damagePanels() {
    const auto layerSurface = m_layerSurface.lock();
    const auto wlSurface    = layerSurface ? layerSurface->wlSurface() : nullptr;
    const auto resource     = wlSurface ? wlSurface->resource() : nullptr;
    if (!resource)
        return;

    const auto position = layerSurface->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    for (const auto& rect : resource->m_current.effectiveInputRegion().getRects()) {
        const double w = rect.x2 - rect.x1, h = rect.y2 - rect.y1;
        if (std::min(w, h) < 120.0)
            continue;
        CBox box{position.x + rect.x1, position.y + rect.y1, w, h};
        box.expand(8.0);
        g_pHyprRenderer->damageBox(box);
    }
}

void CGlassLayerSurface::markBackgroundDirty() {
    if (m_backgroundDirty)
        return;

    const auto& config = g_pGlobalState->config;
    const int64_t fps = config.layersLiveResampleFps ? **config.layersLiveResampleFps : 0;
    const auto now = std::chrono::steady_clock::now();
    if (fps > 0) {
        const auto interval = std::chrono::nanoseconds(1'000'000'000 / fps);
        if (now - m_lastDirtyMark < interval) {
            m_deferredResampleAt = m_lastDirtyMark + interval;
            BackgroundDamageObserver::deferResample(*m_deferredResampleAt);
            return;
        }
    }
    m_deferredResampleAt.reset();
    m_lastDirtyMark = now;

    m_backgroundDirty = true;
    // damage the full sample region: outside the committed area the framebuffer
    // still holds our previous glass output, which must not be re-sampled
    damageSampleRegion();
}

void CGlassLayerSurface::markContourContentChanged() {
    switch (m_contour.invalidate(std::chrono::steady_clock::now())) {
        case Contour::CField::EInvalidation::Now:
            // the field is rebuilt from a frame that redraws the whole layer
            damageSampleRegion();
            break;
        case Contour::CField::EInvalidation::Deferred:
            BackgroundDamageObserver::deferResample(*m_contour.deferredAt());
            break;
        case Contour::CField::EInvalidation::None:
            break;
    }
}

void CGlassLayerSurface::flushDeferredResample(std::chrono::steady_clock::time_point now) {
    switch (m_contour.flushDeferred(now)) {
        case Contour::CField::EInvalidation::Now:
            damageSampleRegion();
            break;
        case Contour::CField::EInvalidation::Deferred:
            BackgroundDamageObserver::deferResample(*m_contour.deferredAt());
            break;
        case Contour::CField::EInvalidation::None:
            break;
    }

    if (!m_deferredResampleAt)
        return;

    if (now < *m_deferredResampleAt) {
        BackgroundDamageObserver::deferResample(*m_deferredResampleAt);
        return;
    }

    m_deferredResampleAt.reset();
    markBackgroundDirty();
}

std::optional<CBox> CGlassLayerSurface::sampleRegionBox(PHLMONITOR monitor, const CBox& layerBox) const {
    if (resolveMaskSource() != EMaskSource::PROTOCOL_REGION)
        return std::nullopt;

    return regionBoundingBoxAbsolute(monitor, layerBox);
}

bool CGlassLayerSurface::wantsBackgroundResample(PHLMONITOR monitor, const CBox& layerBox, bool xray) const {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return true;

    // A workspace slide or fade moves the whole scene behind us without changing
    // any window's own geometry, so no window decoration update reports it.
    const bool isAnimating = layerSurface->positionAnimation()->isBeingAnimated() ||
                             layerSurface->sizeAnimation()->isBeingAnimated() ||
                             layerSurface->alpha()[Desktop::View::LS_ALPHA_FADE]->isBeingAnimated() ||
                             WorkspaceAnimation::anyWorkspaceAnimating(monitor);
    const auto& config    = g_pGlobalState->config;
    const bool  forceLive = config.layersForceLiveResample && **config.layersForceLiveResample;

    // A region resize/move is otherwise invisible here (a layer's own commit
    // doesn't mark itself dirty) — without this the mask shape would update
    // immediately while the sample stays from the old box. Also true leaving
    // region mode: the cached texture is still region-shrunk.
    const bool regionBoxChanged = sampleRegionBox(monitor, layerBox) != m_cachedRegionSampleBox;

    return !m_hasCachedSample ||
           g_pGlobalState->getSceneGeneration(monitor) != m_lastSceneGeneration ||
           isAnimating || m_backgroundDirty || forceLive || regionBoxChanged ||
           xray != m_cachedFromSnapshot;
}

void CGlassLayerSurface::sampleAndRedirect(Render::CRenderContext& ctx, PHLMONITOR monitor, float alpha, bool xray) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    shaderManager.initializeIfNeeded();

    if (!shaderManager.isInitialized())
        return;

    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    auto source = ctx.m_data.currentFB;
    if (!source)
        return;

    auto layerBox = LayerGeometry::computeLayerBox(layerSurface, monitor);
    if (!layerBox)
        return;

    // PROTOCOL_REGION: glass only ever shows inside the blur region, so blurred
    // data outside it is provably never sampled. m_cachedTransformedRegion is
    // computed once here and reused by compositeAndRestore()'s regionRects
    // upload later this same frame — never widens boundingBox() or the
    // composite draw, which stay full-layer (see their own resolveMaskSource()
    // call sites elsewhere in this file). regionSampleBox becomes
    // m_cachedRegionSampleBox only when the sample is actually taken below.
    const std::optional<CBox> regionSampleBox = sampleRegionBox(monitor, *layerBox);
    m_cachedTransformedRegion = regionSampleBox ? transformedBlurRegion(monitor, *layerBox) : CRegion{};

    // Brackets everything below, including the conditional resample and the
    // temp-FBO redirect/clear that always runs: GL forbids a concurrent
    // GL_TIME_ELAPSED query, so on a cache miss the SampleBackground/
    // BlurBackground brackets those calls open underneath this one just no-op.
    Diagnostics::CScopedStageTimer stageTimer(ctx, Diagnostics::EStage::LayerSample);
    const MONITORID monitorId = monitor ? monitor->m_id : -1; // -1 mirrors Hyprland's own MONITOR_INVALID

    // Decide whether we need to re-sample and re-blur the background.
    // When only the layer surface content changed (e.g. waybar clock tick)
    // but no window moved behind us, we reuse the cached blurred background.
    // This skips the most expensive GPU work (blit + 6 blur passes).
    const uint64_t currentGeneration = g_pGlobalState->getSceneGeneration(monitor);
    const bool     backgroundChanged = wantsBackgroundResample(monitor, *layerBox, xray);

    // X-ray never samples the live frame: it holds the windows it hides.
    const CBox sampleBox     = regionSampleBox.value_or(*layerBox);
    const auto snapshot      = (xray && backgroundChanged) ? xraySnapshotCovering(monitor, source, sampleBox) : nullptr;
    const bool sampleCovered = !backgroundChanged ||
                               (xray ? static_cast<bool>(snapshot) :
                                       GlassRenderer::sampleRegionCovered(sampleBox, source, ctx.m_data.damage));

    if (!layerSurface->mapped()) {
        // During fade-out, re-sampling captures stale pixels. Reuse cached sample.
        if (!m_hasCachedSample)
            return;
        Diagnostics::recordLayerCacheHit(monitorId);
    } else if (!sampleCovered) {
        // the work buffer is cleared outside this frame's damage: sampling now would
        // cache black. Without a cache, no redirect: compositeAndRestore() bails.
        m_backgroundDirty = true;
        damageSampleRegion();
        Diagnostics::recordLayerDeferredResample(monitorId);
        if (xray)
            Diagnostics::recordXrayDeferredResample(monitorId);
        if (!m_hasCachedSample || (xray && !m_cachedFromSnapshot))
            return;
    } else if (backgroundChanged) {
        Diagnostics::recordLayerCacheMiss(monitorId);

        const bool isDark                     = resolveThemeIsDark();
        const std::string preset              = resolvePresetName();
        const SResolveContext resolveContext  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

        float blurStrength   = resolvePresetFloat(resolveContext, &SPresetValues::blurStrength, &SOverridableConfig::blurStrength);
        int downscale        = blurStrength >= GlassRenderer::BLUR_DOWNSCALE_THRESHOLD ? GlassRenderer::BLUR_DOWNSCALE_MAX : 1;

        GlassRenderer::sampleBackground(ctx, m_sampleFramebuffer, xray ? snapshot : source, sampleBox, m_samplePaddingRatio, downscale);
        // only here: a deferred or cached frame keeps compositing the old texture
        m_cachedRegionSampleBox = regionSampleBox;

        float blurRadius     = blurStrength * 12.0f / downscale;
        int blurIterations   = std::clamp(static_cast<int>(resolvePresetInt(resolveContext, &SPresetValues::blurIterations, &SOverridableConfig::blurIterations)), 1, 5);

        if (resolveContext.config.blurFold && **resolveContext.config.blurFold) {
            const GlassRenderer::SFoldedBlur folded = GlassRenderer::foldBlurPasses(blurRadius, blurIterations);
            blurRadius     = folded.radius;
            blurIterations = folded.iterations;
        }

        GlassRenderer::blurBackground(ctx, m_sampleFramebuffer, blurRadius, blurIterations, source);
        m_frost.sampleChanged();

        m_hasCachedSample      = true;
        m_cachedFromSnapshot   = xray;
        m_lastSceneGeneration  = currentGeneration;
        m_backgroundDirty      = false;
    } else {
        // background unchanged, reuse cached blur — skip 7 GPU operations
        Diagnostics::recordLayerCacheHit(monitorId);
    }

    // Redirect surface rendering to a temp FBO cleared to transparent.
    // The original renderLayer (called between pre/post elements) will render
    // the surface into this FBO. compositeAndRestore uses its alpha as a mask.
    // Size from the source FB, which is what the surface is drawn into.
    int monitorWidth  = static_cast<int>(source->m_size.x);
    int monitorHeight = static_cast<int>(source->m_size.y);

    // In FP16/HDR mode, the source FB uses RGBA16F which has full alpha precision.
    // Use the source format to avoid clipping HDR color values.
    // In SDR mode, force ARGB8888 because monitor FBOs (XRGB2101010 etc.) have
    // limited/no alpha, which would quantize mask values and break the discard.
    DRMFormat tempFormat = (monitor->useFP16()) ? source->m_drmFormat : DRM_FORMAT_ARGB8888;

    if (!m_surfaceTempFramebuffer)
        m_surfaceTempFramebuffer = g_pHyprRenderer->createFB("hyprglass-layer-temp");

    if (m_surfaceTempFramebuffer->m_size.x != monitorWidth || m_surfaceTempFramebuffer->m_size.y != monitorHeight ||
        m_surfaceTempFramebuffer->m_drmFormat != tempFormat)
        m_surfaceTempFramebuffer->alloc(monitorWidth, monitorHeight, tempFormat);

    m_savedCurrentFB = source;

    ctx.m_data.currentFB = m_surfaceTempFramebuffer;
    g_pHyprOpenGL->bindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(m_surfaceTempFramebuffer.get())->getFBID());

    // Unpadded: the composite quad only ever reads the layer box (never
    // padded), so the SAMPLE_PADDING_PX-expanded part of a padded clear is never
    // sampled — true regardless of mask mode, not the region-sized clear a
    // literal reading of "region-aware" would suggest (that would under-clear
    // real surface area outside the region that the composite quad does read).
    CBox clearBox = layerBox->intersection(CBox{0.0, 0.0, static_cast<double>(monitorWidth), static_cast<double>(monitorHeight)}).noNegativeSize().round();

    if (std::isfinite(clearBox.x) && std::isfinite(clearBox.y) && std::isfinite(clearBox.w) && std::isfinite(clearBox.h) &&
        clearBox.w > 0.0 && clearBox.h > 0.0) {
        g_pHyprOpenGL->scissor(ctx, clearBox, false);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        g_pHyprOpenGL->scissor(ctx, nullptr);
    }

    m_redirectedThisFrame = true;
}

float CGlassLayerSurface::resolveMaskThreshold() const {
    const auto& cfg = g_pGlobalState->config;
    float threshold = cfg.layersMaskThreshold ? static_cast<float>(**cfg.layersMaskThreshold) : 0.001f;
    if (const auto layerSurface = m_layerSurface.lock()) {
        const auto thresholdIt = g_pGlobalState->layerNamespaceMaskThresholds.find(layerSurface->m_namespace);
        if (thresholdIt != g_pGlobalState->layerNamespaceMaskThresholds.end())
            threshold = thresholdIt->second;
    }
    return threshold;
}

std::optional<Contour::SBuildKey> CGlassLayerSurface::contourBuildKey(PHLMONITOR monitor, const CBox& layerBox, const Vector2D& targetSize, float alpha,
                                                                      const SResolveContext& resolveContext) const {
    if (!monitor)
        return std::nullopt;

    const int   boxWidth  = static_cast<int>(layerBox.w);
    const int   boxHeight = static_cast<int>(layerBox.h);
    const float scale     = monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;

    // The shader's own bezel and bevel widths, in the same units: they set how far the field reaches.
    const float reference = Contour::referencePx(boxWidth, boxHeight, scale);
    const float bezelPx   = GlassRenderer::effectiveEdgeThickness(
                              resolvePresetFloat(resolveContext, &SPresetValues::edgeThickness, &SOverridableConfig::edgeThickness),
                              resolvePresetFloat(resolveContext, &SPresetValues::edgeThicknessPx, &SOverridableConfig::edgeThicknessPx), scale, reference) *
                          reference;
    const float rimPx = std::max(resolvePresetFloat(resolveContext, &SPresetValues::bevelSize, &SOverridableConfig::bevelSize) * scale, 1.0f);

    const auto plan = Contour::makePlan(boxWidth, boxHeight, bezelPx, rimPx, scale);
    if (!plan)
        return std::nullopt;

    // Part of the box off the screen was never drawn: what the field saw there depends on where the box sits.
    const bool onScreen = layerBox.x >= 0.0 && layerBox.y >= 0.0 && layerBox.x + layerBox.w <= targetSize.x &&
                          layerBox.y + layerBox.h <= targetSize.y;

    // a NaN would never equal itself in the field's build key and rebuild it every frame
    const float maskThreshold = resolveMaskThreshold();

    return Contour::SBuildKey{
        .plan        = *plan,
        .threshold   = std::isfinite(maskThreshold) ? maskThreshold : 0.001f,
        .provisional = alpha < Contour::COMPLETE_ALPHA,
        .boxX        = onScreen ? 0 : static_cast<int>(layerBox.x),
        .boxY        = onScreen ? 0 : static_cast<int>(layerBox.y),
    };
}

bool CGlassLayerSurface::needsContourBuild(Render::CRenderContext& ctx, PHLMONITOR monitor, const CBox& layerBox, float alpha) const {
    if (m_contour.pending())
        return true;

    // A held field is rebuilt when what it depends on changed too: the fade finished, the box
    // moved on or off the screen, the threshold or the geometry changed.
    const auto target = ctx.m_data.currentFB;
    if (!m_contour.held() || !target || g_pGlobalState->contourUnavailable || resolveMaskSource() != EMaskSource::CONTOUR)
        return false;

    const bool            isDark         = resolveThemeIsDark();
    const std::string     preset         = resolvePresetName();
    const SResolveContext resolveContext = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};
    const auto            key            = contourBuildKey(monitor, layerBox, target->m_size, alpha, resolveContext);
    return key && m_contour.wantsBuild(*key);
}

bool CGlassLayerSurface::attachContour(Render::CRenderContext& ctx, PHLMONITOR monitor, const CBox& layerBox, float alpha,
                                       const SResolveContext& resolveContext, GlassRenderer::SMaskInfo& maskInfo) {
    if (g_pGlobalState->contourUnavailable)
        return false;

    const auto target = ctx.m_data.currentFB;
    if (!target || !m_surfaceTempFramebuffer || !monitor)
        return false;

    const auto key = contourBuildKey(monitor, layerBox, target->m_size, alpha, resolveContext);
    if (!key)
        return false;

    const auto& plan = key->plan;
    m_contour.request();

    if (m_contour.wantsBuild(*key)) {
        // The temp FBO only holds what this frame redrew: a field needs all of the layer in it.
        if (GlassRenderer::damageCoversBox(layerBox, target, ctx.m_data.damage)) {
            if (!m_contour.build(ctx, *key, m_surfaceTempFramebuffer, layerBox, alpha, target) && g_pGlobalState->contourUnavailable)
                return false;
        } else if (m_contour.noteUncoveredFrame())
            damageSampleRegion();
    }

    // A stale field of the same geometry still beats the rectangle for the frame or two before the next build.
    if (!m_contour.usableFor(plan))
        return false;

    maskInfo.contourTextureId   = m_contour.framebuffer()->getTexture()->m_texID;
    maskInfo.contourUVScale     = {layerBox.w / (static_cast<double>(plan.fieldWidth) * plan.divisor),
                                   layerBox.h / (static_cast<double>(plan.fieldHeight) * plan.divisor)};
    maskInfo.contourReferencePx = plan.referencePx;
    maskInfo.contourReachPx     = plan.reachPx;
    return true;
}

int CGlassLayerSurface::frostBoxes(const PHLMONITOR& monitor, const float* windows, int windowCount, float* out, int maxRects) const {
    if (windowCount == 0)
        return 0;

    const auto layerSurface = m_layerSurface.lock();
    const auto wlSurface    = layerSurface ? layerSurface->wlSurface() : nullptr;
    const auto resource     = wlSurface ? wlSurface->resource() : nullptr;
    if (!resource)
        return 0;

    // A shell's input region is its panels, and only a panel over a window frosts at all. A surface
    // that never set a region takes input everywhere, which the state keeps apart from its rects. The
    // shader thins the frost out with distance from the window, slowly enough to leave no seam.
    // The margin keeps the mask's soft edge outside the panel.
    const float scale  = monitor->m_scale;
    const float margin = 60.0f * scale;
    int         count  = 0;
    for (const auto& rect : resource->m_current.effectiveInputRegion().getRects()) {
        if (count >= maxRects)
            break;
        const float x1 = rect.x1 * scale, y1 = rect.y1 * scale, x2 = rect.x2 * scale, y2 = rect.y2 * scale;
        bool        overWindow = false;
        for (int i = 0; i < windowCount && !overWindow; ++i) {
            const float w = std::min(x2, windows[i * 4 + 2]) - std::max(x1, windows[i * 4 + 0]);
            const float h = std::min(y2, windows[i * 4 + 3]) - std::max(y1, windows[i * 4 + 1]);
            overWindow    = w > 1.0f && h > 1.0f;
        }
        if (!overWindow)
            continue;
        out[count * 4 + 0] = x1 - margin;
        out[count * 4 + 1] = y1 - margin;
        out[count * 4 + 2] = x2 + margin;
        out[count * 4 + 3] = y2 + margin;
        ++count;
    }
    return count;
}

void CGlassLayerSurface::compositeAndRestore(Render::CRenderContext& ctx, PHLMONITOR monitor, float alpha, EMaskSource maskSource) {
    // Restore the original currentFB before compositing
    if (m_savedCurrentFB) {
        ctx.m_data.currentFB = m_savedCurrentFB;
        g_pHyprOpenGL->bindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(m_savedCurrentFB.get())->getFBID());
        m_savedCurrentFB.reset();
    }

    // The render pass can discard the pre-surface element without discarding
    // this one (its bounding box is evaluated first, against a superset of the
    // damage the pre-surface element sees — see disableSimplification() in
    // GlassLayerPassElement.cpp). Without this flag we'd mask/composite against
    // whatever m_surfaceTempFramebuffer held from an earlier frame.
    if (!m_redirectedThisFrame)
        return;
    m_redirectedThisFrame = false;

    auto& shaderManager = g_pGlobalState->shaderManager;
    if (!shaderManager.isInitialized() || !m_hasCachedSample)
        return;

    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    auto target = ctx.m_data.currentFB;
    if (!target)
        return;

    auto layerBox = LayerGeometry::computeLayerBox(layerSurface, monitor);
    if (!layerBox)
        return;

    // Brackets mask setup + the applyGlassEffect call below; that call's own
    // ApplyGlassEffect bracket sees this one already open and no-ops instead
    // of nesting (GL forbids concurrent GL_TIME_ELAPSED queries).
    Diagnostics::CScopedStageTimer stageTimer(ctx, Diagnostics::EStage::LayerComposite);
    if (monitor)
        Diagnostics::recordLayerGlassDraw(monitor->m_id);

    const CBox box = *layerBox;

    const bool isDark                     = resolveThemeIsDark();
    const std::string preset              = resolvePresetName();
    const SResolveContext resolveContext  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

    float cornerRadius  = 0.0f;
    float roundingPower = 2.0f;

    // Use the temp FBO's rendered alpha as a mask: glass only where the surface
    // has visible content (alpha > 0). The temp FBO is in monitor coordinates,
    // so we map from the glass quad UV to monitor UV.
    int monitorWidth  = static_cast<int>(m_surfaceTempFramebuffer->m_size.x);
    int monitorHeight = static_cast<int>(m_surfaceTempFramebuffer->m_size.y);

    GlassRenderer::SMaskInfo maskInfo{
        .textureId = m_surfaceTempFramebuffer->getTexture()->m_texID,
        .target    = GL_TEXTURE_2D,
        .uvOffset  = {box.x / monitorWidth, box.y / monitorHeight},
        .uvScale   = {box.w / monitorWidth, box.h / monitorHeight},
    };

    switch (maskSource) {
        case EMaskSource::CONTOUR: // cuts like the alpha mask: the field only shapes the edge
        case EMaskSource::ALPHA_THRESHOLD: {
            const auto& cfg           = g_pGlobalState->config;
            const float maskThreshold = resolveMaskThreshold();

            // The temp FBO stores the layer after Hyprland applies fade alpha. Keep
            // mask_threshold relative to the layer's content alpha, otherwise fade-out
            // makes the mask fall below threshold early and the glass blinks off.
            maskInfo.maskMode       = 0;
            maskInfo.alphaThreshold = maskThreshold * std::clamp(alpha, 0.0f, 1.0f);

            // Region mode leaves coverage at 0: the app's region alone decides where glass goes.
            float coverage  = cfg.layersMaskFeather ? static_cast<float>(**cfg.layersMaskFeather) : 0.0f;
            auto coverageIt = g_pGlobalState->layerNamespaceMaskFeathers.find(layerSurface->m_namespace);
            if (coverageIt != g_pGlobalState->layerNamespaceMaskFeathers.end())
                coverage = coverageIt->second;
            // Capped so full-alpha content still gets full-strength glass.
            coverage          = std::clamp(coverage, 0.0f, std::max(0.0f, 1.0f - maskThreshold));
            maskInfo.coverage = coverage * std::clamp(alpha, 0.0f, 1.0f);
            break;
        }
        case EMaskSource::PROTOCOL_REGION: {
            maskInfo.maskMode       = 1;
            maskInfo.alphaThreshold = 0.0f; // unused in region mode

            // Reuses sampleAndRedirect()'s computation from this same frame
            // (guarded by m_redirectedThisFrame above, so it did run); recomputes
            // as a defensive fallback only if that's somehow empty here.
            if (m_cachedTransformedRegion.empty())
                m_cachedTransformedRegion = transformedBlurRegion(monitor, box);
            CRegion& region = m_cachedTransformedRegion;

            const auto rects = region.getRects();
            if (rects.size() <= static_cast<size_t>(GlassRenderer::MAX_REGION_RECTS)) {
                maskInfo.regionRectCount = static_cast<int>(rects.size());
                for (size_t i = 0; i < rects.size(); i++) {
                    const auto& r = rects[i];
                    maskInfo.regionRects[i] = {static_cast<float>(r.x1 - box.x), static_cast<float>(r.y1 - box.y),
                                                static_cast<float>(r.x2 - r.x1), static_cast<float>(r.y2 - r.y1)};
                }
            } else {
                // Overflow: one bounding rect rather than dropping rects (a hole
                // reads as more broken than a few extra glassed pixels at concave corners).
                const auto extents = region.getExtents();
                maskInfo.regionRectCount = 1;
                maskInfo.regionRects[0]  = {static_cast<float>(extents.x - box.x), static_cast<float>(extents.y - box.y),
                                             static_cast<float>(extents.w), static_cast<float>(extents.h)};
            }
            break;
        }
        case EMaskSource::NONE:
            // hkRenderLayer takes the plain-renderLayer path for NONE; never reaches here.
            break;
    }

    // The traced outline is the glass shape: a fixed shape would only fight it, so it applies
    // when the layer has no field (yet).
    const bool contoured = maskSource == EMaskSource::CONTOUR && attachContour(ctx, monitor, box, alpha, resolveContext, maskInfo);

    // A fixed shape replaces the layer box as the glass outline. The quad stays
    // the layer box, so content outside the shape is still drawn, without glass.
    if (const auto shape = contoured ? std::nullopt : resolveShape()) {
        const double scale = monitor->m_scale;
        CBox shapeBox{box.x + shape->insetLeft * scale, box.y + shape->insetTop * scale,
                      box.w - (shape->insetLeft + shape->insetRight) * scale, box.h - (shape->insetTop + shape->insetBottom) * scale};
        shapeBox.round();

        // insets wider than the layer leave no shape: the whole box keeps its glass
        if (std::isfinite(shapeBox.x) && std::isfinite(shapeBox.y) && std::isfinite(shapeBox.w) && std::isfinite(shapeBox.h) &&
            shapeBox.w > 0.0 && shapeBox.h > 0.0) {
            const float maxRadius = static_cast<float>(std::min(shapeBox.w, shapeBox.h)) / 2.0f;
            cornerRadius          = shape->radius < 0.0f ? maxRadius : std::min(shape->radius * static_cast<float>(scale), maxRadius);

            maskInfo.glassBoxOffsetPx = Vector2D(shapeBox.x - box.x, shapeBox.y - box.y);
            maskInfo.glassBoxSizePx   = Vector2D(shapeBox.w, shapeBox.h);
        }
    }

    // A region-mode sample covers only the region's bounding box, smaller than
    // the full-layer quad: remap the quad UV into that texture (toSampleBoxUV()
    // in Shaders.hpp). Keyed on the box the cached texture was taken from, not
    // this frame's mask mode or region, which a deferred resample leaves behind.
    if (m_cachedRegionSampleBox && box.w > 0.0 && box.h > 0.0) {
        const CBox& sampleBox   = *m_cachedRegionSampleBox;
        maskInfo.sampleUVOffset = Vector2D((sampleBox.x - box.x) / box.w, (sampleBox.y - box.y) / box.h);
        maskInfo.sampleUVScale  = Vector2D(sampleBox.w / box.w, sampleBox.h / box.h);
    }

    // hg.cards(), for this namespace on this monitor.
    std::array<float, 24 * 4> cardRects{};
    std::array<float, 24>     cardRadii{};
    if (contoured && monitor) {
        const auto found = g_pGlobalState->layerCards.find(layerSurface->m_namespace + "|" + monitor->m_name);
        if (found != g_pGlobalState->layerCards.end()) {
            const float scale = monitor->m_scale;
            for (const auto& card : found->second) {
                if (maskInfo.cardCount >= 24)
                    break;
                const int i        = maskInfo.cardCount++;
                cardRects[i * 4 + 0] = card[0] * scale;
                cardRects[i * 4 + 1] = card[1] * scale;
                cardRects[i * 4 + 2] = (card[0] + card[2]) * scale;
                cardRects[i * 4 + 3] = (card[1] + card[3]) * scale;
                cardRadii[i]         = card[4] * scale;
            }
            maskInfo.cardRects = cardRects.data();
            maskInfo.cardRadii = cardRadii.data();
        }
    }

    // Frost: traced layers only, and only over the windows behind them.
    GlassRenderer::SFrostInput frostInput;
    std::array<float, 16 * 4>  frostRects{};
    std::array<float, 16 * 4>  frostWindows{};
    if (contoured) {
        const float frostSetting = resolvePresetFloat(resolveContext, &SPresetValues::frost, &SOverridableConfig::frost);
        if (frostSetting > 0.0f || m_frost.active()) {
            maskInfo.frostWindowCount = frostSetting > 0.0f ? Frost::visibleWindowRects(monitor, box, frostWindows.data(), 16) : 0;
            maskInfo.frostWindows     = frostWindows.data();
            maskInfo.frostRectCount   = frostBoxes(monitor, frostWindows.data(), maskInfo.frostWindowCount, frostRects.data(), 16);
            maskInfo.frostRects       = frostRects.data();

            if (m_frost.step(maskInfo.frostRectCount > 0 ? frostSetting : 0.0f, true, std::chrono::steady_clock::now()))
                damageSampleRegion();

            if (m_frost.amount() > Frost::MINIMUM_AMOUNT) {
                const float blurStrength = resolvePresetFloat(resolveContext, &SPresetValues::blurStrength, &SOverridableConfig::blurStrength);
                const int   downscale    = blurStrength >= GlassRenderer::BLUR_DOWNSCALE_THRESHOLD ? GlassRenderer::BLUR_DOWNSCALE_MAX : 1;
                // frost_blur means what it says here: the copy shrinks until the radius fits the kernel,
                // on top of the sample's own downscale
                const float wanted  = resolvePresetFloat(resolveContext, &SPresetValues::frostBlur, &SOverridableConfig::frostBlur) * 12.0f / downscale;
                const int   divisor = Frost::wideCopyDivisor(wanted);
                const float radius  = wanted / static_cast<float>(divisor);

                if (m_frost.needsBuild(m_sampleFramebuffer, radius, divisor)) {
                    float frostRadius     = radius;
                    int   frostIterations = Frost::BLUR_ITERATIONS;
                    if (const auto& cfg = g_pGlobalState->config; cfg.blurFold && **cfg.blurFold) {
                        const GlassRenderer::SFoldedBlur folded = GlassRenderer::foldBlurPasses(frostRadius, frostIterations);
                        frostRadius     = folded.radius;
                        frostIterations = folded.iterations;
                    }
                    if (GlassRenderer::buildFrostCopy(ctx, m_sampleFramebuffer, m_frost.copy(), m_frost.temporary(), frostRadius, frostIterations, target, divisor, &m_frost.stages()))
                        m_frost.built(radius);
                }

                if (!m_frost.needsBuild(m_sampleFramebuffer, radius, divisor))
                    frostInput = {m_frost.copy(), m_frost.amount()};
            } else {
                m_frost.releaseCopy();
            }
        }
    }

    // The glass shader composites both the glass effect and the surface content
    // in a single pass: glass behind, surface on top, using the temp FBO alpha.
    GlassRenderer::applyGlassEffect(ctx,
                                     m_sampleFramebuffer, target,
                                     box, alpha,
                                     std::array<float, 4>{cornerRadius, cornerRadius, cornerRadius, cornerRadius},
                                     roundingPower, m_samplePaddingRatio, resolveContext,
                                     &maskInfo, 0.0f, frostInput.copy ? &frostInput : nullptr);
}
