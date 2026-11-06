#pragma once

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <string>

// Chrome glass: a browser cannot make its toolbar translucent, and only draws one buffer.
// This pass sits above the window's surface and below its popups. It redraws the window's
// glass over the top <height> px, then puts back what the app drew there with its flat
// background colour lifted out. Page content below stays exactly as the app drew it.
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
    CGlassChromePassElement(PHLWINDOWREF window, int heightLogical, int splitLogical);

    std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override;
    [[nodiscard]] bool                needsLiveBlur(Render::CRenderContext&) override { return false; }
    [[nodiscard]] bool                needsPrecomputeBlur(Render::CRenderContext&) override { return false; }
    [[nodiscard]] std::optional<CBox> boundingBox(Render::CRenderContext& ctx) override;

    [[nodiscard]] const char*      passName() override { return "CGlassChromePassElement"; }
    [[nodiscard]] ePassElementType type() override { return EK_CUSTOM; }

  private:
    PHLWINDOWREF m_window;
    int          m_heightLogical = 0;
    int          m_splitLogical  = 0;
};
