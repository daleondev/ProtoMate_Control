// Instantiate the SDK's RGB565 software fallback exactly once.
// Framebuffers are non-cacheable on GIGA, so the default cache hooks apply.
#include <touchgfx/hal/PaintRGB565Impl.hpp>
extern "C" void touchgfx_paint_link() {}
