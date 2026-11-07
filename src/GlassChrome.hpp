#pragma once

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <string>

// Chrome glass: a browser cannot make its toolbar translucent, and only draws one buffer.
// This pass sits above the window's surface and below its popups. It redraws the window's
// glass over its toolbar band and, for a browser with its tabs down the side, the strip at
// its right end, then puts back what the app drew there with its flat background colour
// lifted out. Page content stays exactly as the app drew it, and with chrome_page_radius set
// it gets rounded corners and a rim where it meets the glass, like a sheet lying in a frame.
//
// hyprglass_chrome_auto reads the window for where its flat bands end. hyprglass_chrome_<height>
// [_<tab strip height>] fixes the band in logical px, hyprglass_chrome_side_<width> the strip,
// and hyprglass_chrome_full keys the whole window: a new tab page over the wallpaper.
inline constexpr int CHROME_AUTO = -1;

struct SChromeShape {
    int  height = 0; // top band, logical px, CHROME_AUTO to read it
    int  split  = 0; // where the tab strip ends and the toolbar starts
    int  side   = 0; // right strip, logical px, CHROME_AUTO to read it
    bool full   = false;

    [[nodiscard]] bool any() const { return height != 0 || side != 0 || full; }
};

class CGlassChromeDecoration : public IHyprWindowDecoration {
  public:
    explicit CGlassChromeDecoration(PHLWINDOW window);

    [[nodiscard]] SDecorationPositioningInfo getPositioningInfo() override;
    void                                     onPositioningReply(const SDecorationPositioningReply& reply) override;
    void                                     draw(Render::CRenderContext& ctx, PHLMONITOR monitor, float const& alpha, const Render::SWindowRenderPresentation&) override;
    [[nodiscard]] eDecorationType            getDecorationType() override;
    void                                     updateWindow(PHLWINDOW window) override;
    void                                     damageEntire() override;
    [[nodiscard]] eDecorationLayer           getDecorationLayer() override;
    [[nodiscard]] uint64_t                   getDecorationFlags() override;
    [[nodiscard]] std::string                getDisplayName() override;

    [[nodiscard]] PHLWINDOW owner() const { return m_window.lock(); }

  private:
    PHLWINDOWREF m_window;
};

class CGlassChromePassElement : public IPassElement {
  public:
    CGlassChromePassElement(PHLWINDOWREF window, const SChromeShape& shape);

    std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override;
    [[nodiscard]] bool                needsLiveBlur(Render::CRenderContext&) override { return false; }
    [[nodiscard]] bool                needsPrecomputeBlur(Render::CRenderContext&) override { return false; }
    [[nodiscard]] std::optional<CBox> boundingBox(Render::CRenderContext& ctx) override;

    [[nodiscard]] const char*      passName() override { return "CGlassChromePassElement"; }
    [[nodiscard]] ePassElementType type() override { return EK_CUSTOM; }

  private:
    PHLWINDOWREF m_window;
    SChromeShape m_shape;
};
