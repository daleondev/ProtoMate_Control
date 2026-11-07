#pragma once
#include <cstdint>
namespace ui::gui::theme
{
    inline constexpr std::uint32_t BACKGROUND{ 0x101820 }, SURFACE{ 0x192a36 }, BORDER{ 0x354956 };
    inline constexpr std::uint32_t TEXT{ 0xedf3f8 }, MUTED{ 0x98adba }, ACCENT{ 0x2587f4 };
    inline constexpr std::uint32_t PRESSED{ 0x164b7f }, DISABLED{ 0x18232d };
    inline constexpr std::uint32_t CONTROL{ 0x223443 }, SELECTED{ 0x1269ce }, CONTROL_BORDER{ 0x3a5061 },
      SELECTED_BORDER{ 0x329fff };
    inline constexpr std::uint32_t DANGER{ 0x74333d }, DANGER_BORDER{ 0xb35762 }, DISABLED_TEXT{ 0x718493 };
    inline constexpr std::uint32_t GREEN{ 0x48ce86 }, AMBER{ 0xf2bd62 }, RED{ 0xe56873 };
    inline constexpr std::uint32_t X_AXIS{ 0xa63247 }, Y_AXIS{ 0x207a4b }, Z_AXIS{ 0x185fb2 };
    inline constexpr int SPACING{ 16 }, RADIUS{ 12 }, TOUCH_TARGET{ 56 }, ANIMATION_FRAMES{ 120 };
    inline constexpr int NAV_WIDTH{ 88 }, CONTENT_X{ 104 }, CONTENT_WIDTH{ 912 };
    inline constexpr int VIEW_X{ 104 }, VIEW_Y{ 100 }, VIEW_WIDTH{ 640 }, VIEW_HEIGHT{ 400 };
}
