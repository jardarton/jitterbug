/*
 * first_light_scene_errors: fl_scene_draw when a frame cannot begin. The
 * test opens a frame itself, so the scene's jb_begin_frame fails (a frame
 * already begun); fl_scene_draw must return that failure at once, without
 * drawing into or ending a frame that is not its own, and take it, so that
 * the frame the test opened still ends cleanly and the next draw succeeds.
 * (A first jb_begin_frame without memory for its records fails the same
 * way.) And a switch the context refuses is reported once, by
 * fl_scene_set_switches, not again by the next frame. Strict C99 against
 * <jitterbug.h> and scene.h.
 */
#include <stdio.h>

#include "scene.h"

static int failures;

static void check(const char *what, long got, long want)
{
    if (got == want)
        return;
    printf("FAIL %s: got %ld, want %ld\n", what, got, want);
    ++failures;
}

int main(void)
{
    fl_switches sw = FL_SWITCHES_CONSOLE;
    fl_scene s;

    if (fl_scene_init(&s, &sw, 1) != JB_OK) {
        printf("FAIL fl_scene_init\n");
        return 1;
    }
    check("a frame of the scene's own", fl_scene_draw(&s, 0), JB_OK);
    check("the test's frame begins", jb_begin_frame(s.jb, 0), JB_OK);
    check("the scene's frame cannot begin", fl_scene_draw(&s, 1), JB_ERROR_INVALID_ARGUMENT);
    check("the test's frame was not ended by the scene, and nothing is latched",
          jb_end_frame(s.jb), JB_OK);
    check("the next frame draws", fl_scene_draw(&s, 2), JB_OK);
    /* A switch the context refuses (scale 9: JB_ERROR_UNSUPPORTED, as a
       scale without memory is JB_ERROR_OUT_OF_MEMORY) is reported by
       fl_scene_set_switches alone: the next frame draws at the old
       settings and returns JB_OK, not the refusal again. */
    {
        fl_switches big = sw;
        big.scale = 9;
        check("a refused switch", fl_scene_set_switches(&s, &big), JB_ERROR_UNSUPPORTED);
        check("the frame after it", fl_scene_draw(&s, 3), JB_OK);
        check("at the old scale", (long)(fl_scene_frame(&s, NULL, NULL) != NULL), 1);
    }
    fl_scene_free(&s);
    printf("first_light_scene_errors: %d failures\n", failures);
    return failures != 0;
}
