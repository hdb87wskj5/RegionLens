#pragma once
#include "PointerSpeed.h"
#include <functional>
#include <commctrl.h>

namespace RegionLens::native
{
    struct PointerSpeedTrack { LONG first{}, last{}; };
    PointerSpeedTrack ConfigurePointerSpeedSlider(HWND slider, PointerSpeed value);
    bool SeekPointerSpeedSlider(HWND slider, POINT& press, PointerSpeedTrack track);
    // Non-modal native trackbar. The owner must suspend mapping BEFORE Show,
    // and keep that suspension until Closed, including on creation failure.
    class PointerSpeedPopup
    {
    public:
        ~PointerSpeedPopup();
        bool Show(HWND owner, POINT anchor, PointerSpeed value,
            std::function<void(PointerSpeed)> changed, std::function<void()> closed);
        void Close();
        HWND Window() const noexcept { return m_window; }
    private:
        static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
        static LRESULT CALLBACK SliderProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
        void UpdateValue();
        HWND m_window{}, m_slider{};
        HFONT m_font{};
        int m_dpi{ 96 };
        int m_wheelRemainder{};
        bool m_shown{};
        PointerSpeed m_value;
        PointerSpeedTrack m_track;
        std::function<void(PointerSpeed)> m_changed;
        std::function<void()> m_closed;
    };
}
