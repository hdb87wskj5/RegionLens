#pragma once
#include "RegionTypes.h"
#include "LiveResizePolicy.h"
#include <array>

namespace RegionLens::native
{
    enum class LensQualityMode : uint32_t { Smooth, Clear };
    enum class LensSharpness : uint32_t { Off, Low, Medium, High };
    struct LensQualitySettings
    {
        LensQualityMode mode{ LensQualityMode::Smooth };
        LensSharpness sharpness{ LensSharpness::Off };
        bool operator==(LensQualitySettings const&) const = default;
    };
    // Creation preference only. Value-initialized settings still mean the
    // unsharpened path used by explicit Off, selection, and failure recovery.
    inline constexpr LensQualitySettings NewLensQuality{ LensQualityMode::Clear, LensSharpness::Medium };
    inline LensQualitySettings QualityForLevel(LensSharpness level) noexcept
    { return level == LensSharpness::Off ? LensQualitySettings{} : LensQualitySettings{ LensQualityMode::Clear, level }; }
    struct CaptureStamp
    {
        uint64_t instance{}, revision{};
        uint64_t capturedQpc{}, arrivedQpc{}; // Timing metadata is not a cache identity.
        bool operator==(CaptureStamp const& other) const noexcept
        { return instance==other.instance && revision==other.revision; }
    };
    inline float SharpnessAmount(LensSharpness value) noexcept
    {
        switch (value) {
        case LensSharpness::Low: return 0.2f;
        case LensSharpness::Medium: return 0.4f;
        case LensSharpness::High: return 0.6f;
        default: return 0.0f;
        }
    }
    inline bool UseClearQuality(LensQualitySettings settings, PixelRect source,
        RenderExtent destination, bool interactive, bool composition) noexcept
    {
        return composition && !interactive && settings.mode == LensQualityMode::Clear && !source.Empty() &&
            destination.width >= uint32_t(source.width) && destination.height >= uint32_t(source.height) &&
            (destination.width > uint32_t(source.width) || destination.height > uint32_t(source.height));
    }
    struct QualityCacheKey
    {
        CaptureStamp frame;
        PixelRect source;
        RenderExtent output;
        bool operator==(QualityCacheKey const&) const = default;
    };
    // Independent pause reasons: closing settings must not resume a selection
    // or re-arm lenses cleared by global failure shutdown. Quality is non-modal.
    class MappingUiPauseState
    {
    public:
        void Selection(bool value) noexcept { m_selection = value; }
        void Settings(bool value) noexcept { m_settings = value; }
        void SpeedPopup(bool value) noexcept { m_speedPopup = value; }
        void Hidden(bool value) noexcept { m_hidden = value; }
        bool Hidden() const noexcept { return m_hidden; }
        bool SelectionActive() const noexcept { return m_selection; }
        bool Paused() const noexcept
        {
            return m_selection || m_settings || m_speedPopup || m_hidden;
        }
    private:
        bool m_selection{};
        bool m_settings{};
        bool m_speedPopup{};
        bool m_hidden{};
    };
}
