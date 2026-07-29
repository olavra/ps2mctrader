/*
 * PS2 Memory Card Trader - throwaway gsKit render test.
 * Not part of the real app; verifies the font atlas and sprite pipeline
 * render correctly before porting the whole UI over from debug.h.
 */

#include <tamtypes.h>
#include <kernel.h>
#include <stdio.h>

#include "gs_render.h"
#include "ui.h"

int main(int argc, char *argv[])
{
    printf("test_render: starting\n");

    gs_render_init();

    printf("test_render: gs_render_init done\n");

    int frame = 0;
    while (1) {
        frame++;
        if (frame == 1) {
            printf("test_render: drawing first frame\n");
        }
        gs_render_begin_frame();

        /* Opaque colored bands behind the sample text so alpha blending is
           actually visible - on a pure black background, alpha-blended and
           opaque-antialiased text are pixel-identical. */
        gs_fill_rect(24, 8 * GS_CHAR_H - 2, 400, GS_CHAR_H, UI_RGB(40, 90, 160));
        gs_fill_rect(24, 9 * GS_CHAR_H - 2, 400, GS_CHAR_H, UI_RGB(160, 60, 60));

        gs_print_at(4, 3, UI_RGB(255, 255, 255), "PS2 Memory Card Trader");
        gs_print_at(4, 5, UI_RGB(121, 50, 168), "Playstation 2 Card (purple)");
        gs_print_at(4, 6, UI_RGB(168, 111, 50), "Playstation 1 Card (brown)");
        gs_print_at(4, 8, UI_RGB(255, 255, 255), "abcdefghijklmnopqrstuvwxyz 0123456789");
        gs_print_at(4, 9, UI_RGB(255, 255, 255), "ABCDEFGHIJKLMNOPQRSTUVWXYZ !@#$%^&*()");

        gs_sprite_at(BTN_CROSS, 4, 12, 24);
        gs_sprite_at(BTN_CIRCLE, 8, 12, 24);
        gs_sprite_at(BTN_SQUARE, 12, 12, 24);
        gs_sprite_at(BTN_TRIANGLE, 16, 12, 24);
        gs_sprite_at(BTN_DPAD, 20, 12, 24);

        gs_render_end_frame();

        if (frame == 1) {
            printf("test_render: first frame flipped\n");
        }
    }

    return 0;
}
