// The public header compiles as C++ and links against the C library; its
// helper macros are constant expressions there too.
#include <jitterbug.h>

static const uint32_t words[] = {
    JB_GP1_DISPLAY_ENABLE(1),
    JB_GP1_DISPLAY_AREA(0, 240),
    JB_GP1_DISPLAY_MODE(320, 240),
    JB_GP0_DRAW_MODE(640, 0, JB_BLEND_ADD, JB_TEX_8BIT, 1),
    JB_GP0_TEXTURE_WINDOW(1, 2, 3, 4),
    JB_GP0_DRAW_AREA_TOP_LEFT(0, 0),
    JB_GP0_DRAW_AREA_BOTTOM_RIGHT(319, 239),
    JB_GP0_DRAW_OFFSET(-1, 2),
    JB_GP0_MASK(1, 0),
    JB_GP0_XY(3, 4),
    JB_GP0_CLEAR_CACHE,
    JB_RGB_TO_15(JB_NEUTRAL),
    JB_RGB15(1, 2, 3) | JB_TEXEL_SEMI,
};

int main()
{
    if (jb_version() != static_cast<unsigned>(JB_VERSION))
        return 1;
    return words[0] == 0x03000000u && words[12] == 0x8c41u ? 0 : 1;
}
