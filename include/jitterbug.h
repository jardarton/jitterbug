/*
 * jitterbug: a renderer that draws like the PlayStation GPU.
 *
 * The public API. C99, and it also compiles as C++. Every name starts with
 * jb_ or JB_. There is no global state.
 *
 * Three levels, each built on the one before:
 *
 *   - The console level: a context is one GPU with its 1024 x 512 VRAM of
 *     16-bit pixels, driven by the words a console program writes to the
 *     GPU's two ports (GP0 for drawing and VRAM transfers, GP1 for the
 *     display), read back through GPUREAD and GPUSTAT, and shown with
 *     jb_frame. Commands, their words and their limits are the console's,
 *     as the public hardware documentation (psx-spx, "GPU") describes
 *     them. The same commands can also be given as calls (VRAM, Drawing),
 *     kept in an ordering table (The ordering table), and a context has
 *     the console's geometry coprocessor, the GTE (Geometry). Calls here
 *     act at once and refuse what the console's words cannot carry.
 *   - Helpers for the console level: colour constants and the words of
 *     common commands, which spell out the console's model rather than
 *     hide it.
 *   - The friendly level: a frame that double-buffers in VRAM and draws
 *     what is recorded into it when it ends, in layers and an ordering
 *     table of its own, and 2D drawing that clips what the console would
 *     refuse.
 *
 * Sections, in order: Results, Contexts (with the options that can
 * change at run time); the console level: The GPU's ports, VRAM, Drawing
 * (with the structs the friendly level's 2D drawing takes too), The
 * ordering table, Geometry (the GTE, its helpers and fixed-point maths),
 * Output (the display as shown), Helpers; then the friendly level, where
 * a program that draws frames can start reading: The frame, Textures
 * (placing textures and palettes in VRAM, for any call that takes a
 * texture; texture files), 2D drawing, Sprites and text, Scene geometry
 * (float maths, camera, lights, fog, meshes) and 3D drawing (with mesh
 * files).
 *
 * Threads: a context is used from one thread at a time. Different contexts
 * are independent and may be used from different threads at once. A
 * context may run worker threads of its own while a call is inside it;
 * callbacks (log, allocator) are called on the thread that made that call.
 *
 * Errors: calls that can fail return a jb_result. Nothing aborts, and
 * nothing is printed; the log callback, when set, receives every error
 * message and a few warnings. jb_error_string gives the detail of the last
 * failed call on a context, and the context also keeps the first failure
 * until it is taken (jb_check, jb_end_frame), so a program can check once
 * a frame instead of after every call.
 */
#ifndef JITTERBUG_H
#define JITTERBUG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version. The build reads it from here. No ABI promise before 1.0. */
#define JB_VERSION_MAJOR 0
#define JB_VERSION_MINOR 1
#define JB_VERSION_PATCH 1

#define JB_VERSION_NUMBER(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define JB_VERSION JB_VERSION_NUMBER(JB_VERSION_MAJOR, JB_VERSION_MINOR, JB_VERSION_PATCH)

/* JB_API marks what a shared library exports. Define JB_SHARED when using
 * jitterbug as a DLL on Windows. */
#if defined(_WIN32) && defined(JB_BUILD_SHARED)
#define JB_API __declspec(dllexport)
#elif defined(_WIN32) && defined(JB_SHARED)
#define JB_API __declspec(dllimport)
#elif defined(__GNUC__)
#define JB_API __attribute__((visibility("default")))
#else
#define JB_API
#endif

/* The version of the library in use, as JB_VERSION_NUMBER packs it. Compare
 * it with JB_VERSION to find a header that doesn't match the library. */
JB_API unsigned jb_version(void);

/* The same as text, "major.minor.patch". */
JB_API const char *jb_version_string(void);

/* ------------------------------------------------------------------------ */
/* Results */

typedef enum jb_result {
    JB_OK = 0,
    JB_ERROR_INVALID_ARGUMENT = 1, /* a NULL or out-of-range argument */
    JB_ERROR_OUT_OF_MEMORY = 2,    /* an allocation failed; nothing changed */
    JB_ERROR_UNSUPPORTED = 3       /* a valid request this build can't do */
} jb_result;

/* A short constant description of a result code ("out of memory"), or
 * "unknown result" for a value that isn't one. Never NULL. */
JB_API const char *jb_result_string(jb_result result);

/* jb_context, below. */
typedef struct jb_context jb_context;

/*
 * The sticky error. Every call that fails on a context returns its result
 * and sets jb_error_string as always; the context also latches the first
 * such failure, its result and its detail, and keeps it (later failures do
 * not replace it) until it is taken. jb_check takes it: it returns that
 * result, or JB_OK if no call has failed since it was last taken, and
 * clears it. *detail (when detail is not NULL) gets its detail, "" with
 * JB_OK; the text belongs to the context and stays valid until the next
 * failing call or the next jb_check or jb_end_frame (which takes it too).
 * Calls that fail for jb NULL have no context to latch into.
 *
 * So code can make calls unchecked and look once:
 *     jb_gte_write(jb, ...); jb_prim_poly(jb, ...); ...
 *     if (jb_check(jb, &why) != JB_OK) report(why);
 *
 * For jb NULL it returns JB_ERROR_INVALID_ARGUMENT with *detail
 * "no context".
 */
JB_API jb_result jb_check(jb_context *jb, const char **detail);

/* ------------------------------------------------------------------------ */
/* Contexts */

/* A message from the library, without a trailing newline. */
typedef void (*jb_log_fn)(void *user, const char *message);

/* Memory for the library: size bytes (never 0) aligned for any object, as
 * malloc gives, or NULL. free is never called with NULL. */
typedef void *(*jb_alloc_fn)(void *user, size_t size);
typedef void (*jb_free_fn)(void *user, void *ptr);

/*
 * How a context is made. All zero means every default, so
 * jb_create(&(jb_config){ .scale = 2 }, NULL) in C99 changes one thing.
 */
typedef struct jb_config {
    /*
     * Internal resolution: 0 or 1 draws at the console's 1x; 2 to 8 draws
     * every primitive again at that multiple, and jb_frame shows that
     * picture. The 1x VRAM is still drawn at any scale, by the same
     * integer path a 1x context uses, so everything read back (GPUREAD)
     * and every 4- and 8-bit texture and palette is what a 1x context
     * would hold (docs/conformance.md says how far that path is checked
     * against a console). Negative is
     * JB_ERROR_INVALID_ARGUMENT; above 8 is JB_ERROR_UNSUPPORTED.
     */
    int scale;
    /*
     * The display size in 1x pixels, shown by jb_frame until a GP1(08h)
     * display mode sets one of the console's sizes: 1 to 1024 wide, 1 to
     * 512 high, any rectangle (a 427 x 240 display is a 16:9 one). 0 means
     * 320 wide, or 240 high. Out of range is JB_ERROR_INVALID_ARGUMENT.
     */
    int width;
    int height;
    /* Rasterising threads, 1 to 64; 0 for one per CPU, up to 16. The
     * pixels do not depend on it. Out of range is
     * JB_ERROR_INVALID_ARGUMENT. A single-threaded build (CMake
     * JB_THREADS=OFF, always so for emscripten) draws on the calling
     * thread alone: 0 means 1, and more than 1 is not an error but is
     * clamped to 1 with a message to the log, as when threads would not
     * start (jb_set_threads). */
    int threads;
    /*
     * Precise vertices, for scales above 1x. Not 0: the context's GTE
     * (jb_gte_command) remembers the unrounded screen position of every
     * vertex RTPS and RTPT project, and a polygon or line vertex at the
     * same integer position as one of those projections is drawn at its
     * sub-pixel position in the scaled picture, so geometry no longer snaps
     * to the 1x pixel grid (no vertex jitter). The match is by position
     * alone: a vertex the program placed itself on such a pixel moves by
     * up to a pixel too. Rectangles keep their integer positions; at 1x
     * nothing changes, and the 1x VRAM stays the console's at every scale.
     * Call jb_gte_begin_frame before each frame's projections. 0 (the
     * default) is the console.
     */
    int precise_vertices;
    /*
     * Perspective-correct textures. Not 0: a textured triangle whose three
     * vertices are at the positions of recent GTE projections (as for
     * precise_vertices, which need not be on) is textured with their
     * depths, perspective-correctly, rather than affinely as the console
     * does (no texture wobble); a quad is its two triangles. Other textured
     * triangles stay affine. Above 1x only the scaled picture changes and
     * the 1x VRAM stays the console's; at 1x the 1x VRAM is drawn this way
     * and is no longer the console's. 0 (the default) is the console.
     *
     * These two options are the only drawing that uses floating point
     * (done the same way on every platform); with both off it is integer
     * arithmetic only.
     *
     * The friendly level (The frame) does not match by position: what it
     * records carries each vertex's sub-pixel position and depth itself,
     * so with these options its geometry draws precisely and
     * perspective-correctly without the GTE's memory, and its 2D draws'
     * own vertices are never moved, nor drawn perspective-correctly (2D
     * vertices have no depth), even at positions where a jb_prim_* call
     * would find GTE depths. The new vertices where a 2D draw beyond the
     * console's limits is cut (2D drawing) lie between pixels and keep
     * their sub-pixel positions: with precise vertices the scaled picture
     * draws them there, so its edges near a cut can differ from the
     * picture without.
     */
    int perspective_correct;
    /* Where messages go; NULL drops them. */
    jb_log_fn log;
    void *log_user;
    /* Where every allocation the library makes goes (thread stacks and
     * the thread library's own memory excepted). Both set, or both NULL
     * for malloc and free; one without the other is
     * JB_ERROR_INVALID_ARGUMENT. */
    jb_alloc_fn alloc;
    jb_free_fn free;
    void *alloc_user;
    /* The fields below came after the ones above, and new fields go at the
       end, so a positional initialiser of the earlier fields keeps its
       meaning ({2, 320, 240, 0, 0, 0, log, user} still sets the log). */
    /*
     * Widescreen. Not 0: the frame's buffers (jb_begin_frame) are reserved
     * for a 16:9 display as well, round(height x 16 / 9) pixels wide (427
     * for 240), and the display starts at that width; jb_set_widescreen
     * switches between it and width. 0 (the default): buffers of width,
     * and jb_set_widescreen cannot turn it on. Textures
     * (jb_texture_create) are kept out of the buffers, which serve both
     * widths.
     */
    int widescreen;
    /*
     * The frame's capacity: records a frame holds (one for each triangle,
     * quad, line segment and rectangle the GPU is to draw, so a clipped
     * primitive can take several; a polygon cut where a vertex lies more
     * than a pixel from its whole-pixel position takes a second set of
     * pieces for the precise-vertices pass, at any scale, since the
     * switches may change before the frame ends), 0 for
     * JB_DEFAULT_MAX_PRIMITIVES (8192),
     * up to JB_LIMIT_MAX_PRIMITIVES; and the buckets of the frame's scene
     * ordering table, 0 for JB_DEFAULT_OT_SIZE (1024), up to
     * JB_LIMIT_OT_SIZE. Negative or above the limit is
     * JB_ERROR_INVALID_ARGUMENT. The memory (about 250 bytes a record and
     * 8 a bucket) is allocated by the first jb_begin_frame, never later,
     * so a context that draws no frames does not pay for it.
     */
    int max_primitives;
    int ot_size;
    /* The geometry path of the friendly level's 3D drawing, as
       jb_set_geometry: JB_GEOMETRY_GTE (0, the default) or
       JB_GEOMETRY_FLOAT; anything else is JB_ERROR_INVALID_ARGUMENT. */
    int geometry;
    /* Not 0: the CLUT cache off from the start (jb_set_clut_cache). 0, the
       default, is the console: the cache on. */
    int clut_cache_off;
} jb_config;

/* jb_config's capacities: the defaults (for 0) and the largest values. */
enum {
    JB_DEFAULT_MAX_PRIMITIVES = 8192,
    JB_LIMIT_MAX_PRIMITIVES = 1 << 22,
    JB_DEFAULT_OT_SIZE = 1024,
    JB_LIMIT_OT_SIZE = 1 << 16
};

/* The friendly level's geometry paths (jb_set_geometry). */
enum { JB_GEOMETRY_GTE = 0, JB_GEOMETRY_FLOAT = 1 };

/*
 * A new context, or NULL. config NULL means all defaults. *result (when
 * result isn't NULL) gets JB_OK, or why there is no context:
 * JB_ERROR_INVALID_ARGUMENT or JB_ERROR_UNSUPPORTED for a config field
 * (see jb_config), JB_ERROR_OUT_OF_MEMORY. The detail of a failure goes
 * to config->log.
 *
 * The context starts as the console's GPU does after GP1(00h) (see
 * jb_gp1): VRAM all zero, drawing state reset, display disabled (send
 * GP1(03h) with 0 to show it), display area at (0, 0) and of the
 * configured size, every GTE register 0. Its memory: 1 MB for the VRAM
 * and 1 MB for a buffer VRAM-to-CPU reads are copied into, 256 KB for the
 * GTE's table of projected vertices (precise_vertices), 128 KB for the
 * palettes the CLUT cache keeps (see jb_vram_load), 1.6 MB for a queue
 * of primitives when it draws on several threads or above 1x, 1 MB x
 * scale x scale for the scaled VRAM, 245 KB for the table of textures
 * and where they lie in VRAM (jb_texture_create), from the first jb_frame
 * the frame (4 bytes a pixel), and from the first jb_begin_frame the
 * frame's records (jb_config.max_primitives).
 */
JB_API jb_context *jb_create(const jb_config *config, jb_result *result);

/* Frees the context and everything it holds, after joining its worker
 * threads. Pointers it returned (jb_frame) become invalid. NULL is
 * ignored. */
JB_API void jb_destroy(jb_context *jb);

/*
 * The detail of the most recent call on this context that failed, such as
 * "jb_gp0: words is NULL but count is 3"; "" if none has. Calls that
 * succeed leave it as it was. The text belongs to the context and stays
 * valid until its next failing call or jb_destroy. For NULL, "no context".
 */
JB_API const char *jb_error_string(const jb_context *jb);

/*
 * jb_config's options, changed at run time (say, on a key press). Each
 * applies to what is sent after the call; what is already drawn stays.
 * jb NULL is JB_ERROR_INVALID_ARGUMENT for each.
 *
 * jb_set_scale: jb_config.scale, with its range and errors. The 1x VRAM is
 * kept; the scaled picture is made again from it, so what was drawn at
 * the old scale shows with 1x detail until it is drawn again, and
 * jb_frame's size becomes the display size times the new scale. The same
 * scale as now does nothing. Without memory for the new scale it is
 * JB_ERROR_OUT_OF_MEMORY and nothing changes: the context goes on at its
 * old scale with its picture as it was.
 *
 * jb_set_threads: jb_config.threads, with its range; the pixels do not
 * depend on it, and nothing drawn changes. The same value as now does
 * nothing. Fewer threads than asked for (without memory for them, or
 * threads that would not start) is logged, and drawing goes on with those
 * there are; no memory for the queue of primitives that more than one
 * thread at 1x needs is JB_ERROR_OUT_OF_MEMORY, the old threads kept. In a
 * single-threaded build every request is met with the calling thread
 * alone: more than 1 returns JB_OK and logs that fewer were started.
 *
 * jb_set_precise_vertices, jb_set_perspective_correct: the two options of
 * that name, on for any value but 0. A primitive takes them as they are
 * when it is sent; a frame's records (The frame) as they are when the
 * frame ends. Neither fails but for jb NULL.
 *
 * jb_set_clut_cache: the CLUT cache (see jb_vram_load), on for any value
 * but 0; on is the console and the default (jb_config.clut_cache_off).
 * Off, every 4- and 8-bit primitive reads its palette from VRAM as it is
 * drawn, so a palette rewritten in place draws at once with no GP0(01h).
 * It applies to primitives sent after it; turning it on starts with an
 * empty cache. Fails only for jb NULL.
 *
 * jb_set_widescreen: on for any value but 0, the 16:9 display (see
 * jb_config.widescreen) at round(height x 16 / 9) pixels wide; off,
 * jb_config.width. It takes effect with the next frame: frames are drawn
 * at the new width from the next jb_begin_frame, and the display (its
 * size, and what GP1(00h) returns it to) takes the new width at that
 * frame's jb_end_frame, as it shows the buffer drawn at that width; until
 * then the frame already shown stays as it is, at its own width. Before
 * the context's first frame nothing is shown yet, and the display width
 * changes at once. A display mode the program set with GP1(08h) since the
 * last GP1(00h) takes precedence: the display keeps that mode's size and
 * pixel format, and the new width is only what GP1(00h) returns the
 * display to. JB_ERROR_UNSUPPORTED, with nothing changed, to turn it on
 * in a context made without jb_config.widescreen: its buffers have no
 * room for the wider display. Turning it off there does nothing.
 *
 * jb_set_geometry: how the friendly level's 3D drawing transforms
 * vertices: JB_GEOMETRY_GTE (the default) through the context's own GTE
 * arithmetic, exactly as console code would; JB_GEOMETRY_FLOAT in single
 * precision floating point, rounded to the same screen formats. Anything
 * else is JB_ERROR_INVALID_ARGUMENT. It does not affect the console level.
 *
 * What each drawing switch changes, by scale:
 *   option               at 1x                        above 1x
 *   precise vertices     nothing                      the scaled picture:
 *                                                     polygons and lines
 *                                                     off the 1x grid
 *   perspective-correct  the 1x VRAM (no longer the   the scaled picture
 *                        console's)                   only
 *   dithering            the 1x VRAM                  the 1x VRAM; the
 *   (jb_set_dither)                                   scaled picture's own
 *                                                     drawing is never
 *                                                     dithered, but the 1x
 *                                                     VRAM it samples (4-
 *                                                     and 8-bit texels,
 *                                                     palettes) may have
 *                                                     been drawn dithered
 *   CLUT cache           both, the same way           both
 *   widescreen           the display's width          the same
 * Rectangles keep their integer positions with every option.
 */
JB_API jb_result jb_set_scale(jb_context *jb, int scale);
JB_API jb_result jb_set_threads(jb_context *jb, int threads);
JB_API jb_result jb_set_precise_vertices(jb_context *jb, int on);
JB_API jb_result jb_set_perspective_correct(jb_context *jb, int on);
JB_API jb_result jb_set_clut_cache(jb_context *jb, int on);
JB_API jb_result jb_set_widescreen(jb_context *jb, int on);
JB_API jb_result jb_set_geometry(jb_context *jb, int geometry);

/* ------------------------------------------------------------------------ */
/* The GPU's ports */

/*
 * GP0: count command words, as a console program would write them to the
 * GP0 port, in order. The stream may be cut anywhere: a command that is
 * not complete at the end of one call continues with the next, so any
 * split of the same words gives the same result. Lengths follow the
 * command: polygons, rectangles, lines, fills (02h), copies (80h) have
 * fixed lengths; a polyline (48h-4Fh, 58h-5Fh) ends with a word w where
 * (w & 0xF000F000) == 0x50005000 in the place of its third or a later
 * vertex; a CPU-to-VRAM transfer (A0h) is followed by its pixels, two per
 * word (the first in the low half, the last word padded), and writes each
 * row as it arrives; a VRAM-to-CPU transfer (C0h) makes the rectangle
 * readable through jb_gpuread. Unknown commands take one word and do
 * nothing. GP1(01h) or GP1(00h) drops a partial command. GP0(01h) clears
 * the CLUT cache (see jb_vram_load) and does nothing else here.
 *
 * Primitives may be drawn later than the call (on worker threads); every
 * call that reads pixels waits for them, so this ordering is never seen.
 *
 * Not as on the console: GP0(E1h) bit 10, which allows or forbids drawing
 * into the displayed area, is kept and reported by jb_gpustat but changes
 * nothing. Drawing there is always allowed.
 *
 * Errors: JB_ERROR_INVALID_ARGUMENT for jb NULL, or words NULL with count
 * above 0 (count 0 does nothing). Nothing else fails: no allocation
 * happens here.
 */
JB_API jb_result jb_gp0(jb_context *jb, const uint32_t *words, size_t count);

/*
 * GP1: one display-control word (the command in bits 24-31, as on the
 * console; commands 40h-FFh are 00h-3Fh again):
 *   00h  reset: as 01h and 02h, display disabled, DMA direction 0,
 *        display area start (0, 0), the display ranges to the console's
 *        reset values, display mode 0, the drawing state of GP0(E1h-E6h)
 *        to 0; the display size goes back to jb_config's, not mode 0's.
 *        VRAM is kept.
 *   01h  reset the command buffer: drops a partial GP0 command, the rest
 *        of a CPU-to-VRAM transfer and any unread VRAM-to-CPU data, and
 *        clears the CLUT cache (see jb_vram_load)
 *   02h  acknowledge the interrupt: clears GPUSTAT bit 24, set by GP0(1Fh)
 *   03h  display enable: bit 0 clear shows the display, set blanks it
 *   04h  DMA direction, bits 0-1: kept for GPUSTAT; nothing transfers
 *   05h  display area start: x in bits 0-9, y in bits 10-18
 *   06h, 07h  horizontal and vertical display ranges: kept, but jb_frame
 *        always shows the whole display area (a simplification: the
 *        console crops or borders it by these timings)
 *   08h  display mode: the display size becomes the mode's, 256, 320,
 *        368, 512 or 640 wide and 240 high, or 480 with bits 2 and 5
 *        (vertical resolution and interlace) both set; bit 4 shows the
 *        area as 24-bit pixels. Video mode, interlace and bit 7 only show
 *        in GPUSTAT.
 *   09h  bit 0 allows GP0(E1h) bit 11 to be written (psx-spx: "Set VRAM
 *        size (v2)"; ps1-tests calls it "allow texture disable"). Each
 *        GP0(E1h) sent through jb_gp0, and each textured polygon's page,
 *        writes the stored bit, which reads as GPUSTAT bit 15: while the
 *        gate is set it stores the word's bit 11; while it is clear it
 *        stores 0, whatever the word's bit 11. Clearing the gate keeps the
 *        stored bit until the next such write, and 00h does not change it
 *        (as on the console, ps1-tests' gp0-e1). Stored and reported only:
 *        the bit changes no drawing here. On a retail console it makes
 *        textures vanish (psx-spx: the VRAM chip select it selects is not
 *        wired), which no hardware dump shows, so it is not modelled.
 *        The helpers: a textured jb_prim_poly sends its page with bit 11
 *        clear, so it stores 0 with the gate set or clear, as that page
 *        sent through jb_gp0 would. The GP0(E1h) that jb_prim_rect and the
 *        blending jb_prim_* calls send is a helper-only update of the draw
 *        mode, not the raw command's semantics: its word has bit 11 clear,
 *        and the stored bit is put back after it, so GPUSTAT bit 15 stays
 *        as it was with the gate set or clear ("the draw mode as it is").
 *        The same word through jb_gp0 would store 0. jb_set_dither sends
 *        no GP0(E1h) and leaves the bit too.
 *   10h-1Fh  latch an internal register into GPUREAD, by bits 0-3: 2 the
 *        texture window (GP0(E2h) as sent, 20 bits), 3 and 4 the drawing
 *        area corners (E3h, E4h, 20 bits), 5 the drawing offset (E5h, 22
 *        bits), 7 the GPU version (2), 8 zero; the others leave GPUREAD as
 *        it was.
 * Everything else is ignored, as the console ignores it.
 *
 * jb_frame's pixels stay valid across every command except 00h.
 * Errors: JB_ERROR_INVALID_ARGUMENT for jb NULL.
 */
JB_API jb_result jb_gp1(jb_context *jb, uint32_t word);

/*
 * GPUREAD. After a GP0(C0h) command, each call returns the next word of
 * its rectangle: two 16-bit VRAM pixels, the first in the low half, rows
 * in order; the last word of an odd count has 0 in its high half. The
 * pixels are those of the 1x VRAM (the console's at any scale) when the
 * C0h command completed, with the console's sizes (0 means 1024 wide or
 * 512 high) and wrapping at the VRAM edges. A new C0h replaces unread
 * data. Otherwise it returns the last value latched: the last word read
 * or GP1(10h)'s answer, and 0 at first. GP0 commands sent while data
 * remains are carried out, and change only later reads. 0 for jb NULL.
 */
JB_API uint32_t jb_gpuread(jb_context *jb);

/*
 * GPUSTAT, from the drawing and display state as psx-spx lays it out:
 * bits 0-10 the draw mode (GP0(E1h), and a textured polygon's page),
 * 11-12 the mask settings (E6h), 14 and 16-22 the display mode (GP1(08h)),
 * 23 display disabled (GP1(03h)), 24 the interrupt flag (GP0(1Fh), cleared
 * by GP1(02h)), 29-30 the DMA direction (GP1(04h)). There is no timing:
 * the GPU is always ready for commands and DMA (bits 26 and 28 set), bit
 * 27 is set while VRAM-to-CPU data remains, bit 25 follows bits 29-30, and
 * the interlace bits read 13 set and 31 clear. Bit 15 is GP0(E1h) bit 11
 * as stored under GP1(09h) (see jb_gp1). After a reset it reads 0x14802000
 * (bit 15 clear, since the reset writes GP0(E1h) with 0). 0 for jb NULL.
 */
JB_API uint32_t jb_gpustat(const jb_context *jb);

/* ------------------------------------------------------------------------ */
/*
 * VRAM
 *
 * VRAM transfers as calls, each doing exactly what its GP0 command does. A
 * rectangle is given by its top-left corner, x 0..1023 and y 0..511, and
 * its size; one that runs past the right or bottom edge of VRAM wraps
 * round to the left or top, as on the console. Pixels are VRAM's 16-bit
 * ones: red in bits 0-4, green 5-9, blue 10-14, the mask bit 15.
 *
 * The range rule, for these calls and for every console-level drawing call
 * below (Drawing, The ordering table; the friendly level clips instead): each
 * argument must lie in the range given for it, which is what its field in
 * the command's words can carry, and anything else is
 * JB_ERROR_INVALID_ARGUMENT with nothing drawn or changed. (The console
 * keeps the low bits of each field instead; jb_gp0 does too.) A field the
 * command does not use is ignored and not checked.
 *
 * These calls and the drawing calls are each a whole command of their
 * own, not part of jb_gp0's word stream: a command jb_gp0 has received
 * only part of stays pending and continues with the next jb_gp0 words.
 * None of them allocates.
 */

/*
 * GP0(A0h), CPU to VRAM: w x h pixels (w 1..1024, h 1..512), rows top to
 * bottom with no padding, into the rectangle at (x, y), with the mask
 * settings of GP0(E6h) (jb_set_mask): with check on, a pixel whose mask
 * bit is set in VRAM is left as it is; with set on, every pixel written
 * gets the mask bit. Above 1x each pixel also becomes a scale x scale
 * block of the scaled picture.
 *
 * Palettes and the CLUT cache. The GPU keeps the last palette (CLUT) a 4-
 * or 8-bit primitive drew with, and reads a CLUT from VRAM again only when
 * a primitive names a different one, an 8-bit primitive follows a 4-bit
 * one with the same CLUT, or after GP0(01h), GP1(01h) or GP1(00h)
 * (psx-spx, and ps1-tests' clut-cache dump; jitterbug's model of it is
 * that a 4-bit primitive loads only 16 entries: docs/conformance.md,
 * "Assumptions beyond the evidence").
 * Nothing that writes VRAM clears it here: not this call, a fill, a copy,
 * nor a primitive drawn over the CLUT (the console's dump shows it for a
 * fill, and for a sprite drawn over its own CLUT while it draws; for
 * transfers, copies and the primitives after one drawn over the CLUT it
 * is jitterbug's assumption). So a program that rewrites a palette in
 * place (say, to animate it every frame) with a fill and draws with that
 * CLUT alone goes on drawing with the old colours, as the console's dump
 * shows. To have the new ones, send GP0(01h), "clear cache", after
 * rewriting it:
 *     uint32_t clear = 0x01000000;  jb_gp0(jb, &clear, 1);
 * or draw with another CLUT in between. The primitives keep the palette
 * in force when they were sent, at any scale and thread count.
 * Errors: jb or pixels NULL, an argument out of range.
 */
JB_API jb_result jb_vram_load(jb_context *jb, int x, int y, int w, int h, const uint16_t *pixels);

/*
 * GP0(C0h) and its GPUREAD words in one call: the w x h pixels (w 1..1024,
 * h 1..512) of the rectangle at (x, y) into pixels, rows top to bottom
 * with no padding, once everything sent before is drawn. They are the 1x
 * VRAM's, the console's at any scale. What jb_gpuread returns, and
 * GPUSTAT, are not affected.
 * Errors: jb or pixels NULL, an argument out of range.
 */
JB_API jb_result jb_vram_read(jb_context *jb, int x, int y, int w, int h, uint16_t *pixels);

/*
 * GP0(02h), fill: the rectangle at (x, y) in color (JB_RGB, 0..0xFFFFFF;
 * each channel keeps its top 5 bits) with the mask bit 0, ignoring the
 * drawing area, the drawing offset and the mask settings. As on the
 * console x is rounded down and w up to a multiple of 16, and w 0..1023
 * and h 0..511 are the command's fields: 0 fills nothing, and one fill
 * covers at most 1024 x 511.
 * Errors: jb NULL, an argument out of range.
 */
JB_API jb_result jb_vram_fill(jb_context *jb, int x, int y, int w, int h, uint32_t color);

/*
 * GP0(80h), VRAM to VRAM: the w x h rectangle (w 1..1024, h 1..512) at
 * (src_x, src_y) to (dst_x, dst_y), with the mask settings as for
 * jb_vram_load. Rows are copied top to bottom, each read whole before it
 * is written. Above 1x the scaled picture is copied too, with its detail.
 * Errors: jb NULL, an argument out of range.
 */
JB_API jb_result jb_vram_copy(jb_context *jb, int src_x, int src_y, int dst_x, int dst_y, int w,
                              int h);

/* ------------------------------------------------------------------------ */
/*
 * Drawing
 *
 * The console's primitives as calls. Each builds the GP0 words described
 * with it and has them carried out as jb_gp0 carries them out, so it draws
 * exactly the pixels those words draw; the range rule and the stream rule
 * of VRAM above apply. The console's model holds throughout:
 *   - Positions are in 1x pixels, x and y -1024..1023 each. The drawing
 *     offset (jb_set_draw_offset) is added to them, and pixels outside the
 *     drawing area (jb_set_draw_area) are not drawn. After a reset the
 *     area is the one pixel (0, 0), so set it before drawing.
 *   - A polygon or line with two vertices more than 1023 pixels apart in x
 *     or 511 in y is not drawn.
 *   - Colours are JB_RGB values, 0..0xFFFFFF. A textured primitive
 *     multiplies each texel by its colour / 128 (so 128 leaves it as it
 *     is), unless raw: then its colours are not used and not checked.
 *   - The blend mode (JB_BLEND_*) is the console's semi-transparency: B
 *     the pixel in VRAM, F the primitive's. A textured primitive blends only
 *     its texels with bit 15 set, and never draws texel 0x0000.
 *   - Dithering follows the draw mode (jb_set_dither). It applies to
 *     Gouraud untextured polygons (even with equal colours), to modulated
 *     textured polygons, flat or Gouraud, and to every line, flat ones
 *     too; never to flat untextured polygons, raw textured polygons or
 *     rectangles. The scaled picture above 1x is not dithered.
 *   - Colours and texture coordinates are interpolated as the console's
 *     GPU interpolates them (fitted to its VRAM dumps): over a triangle,
 *     or along a line, from the first vertex, with steps of 12 fractional
 *     bits and half a unit added, so a texture coordinate is rounded to
 *     the nearest texel rather than truncated.
 */

/* A colour for primitives and fills: r, g, b 0..255 each, as 0x00BBGGRR,
   the layout of the GP0 colour field and of the GTE's colour registers. */
#define JB_RGB(r, g, b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16)

/* Blend modes: the console's semi-transparency modes 0 to 3, plus none. */
enum {
    JB_BLEND_NONE = 0,   /* opaque */
    JB_BLEND_HALF = 1,   /* B / 2 + F / 2 (mode 0) */
    JB_BLEND_ADD = 2,    /* B + F (mode 1) */
    JB_BLEND_SUB = 3,    /* B - F (mode 2) */
    JB_BLEND_QUARTER = 4 /* B + F / 4 (mode 3) */
};

/* Texture colour depths: bits per texel. */
enum { JB_TEX_4BIT = 4, JB_TEX_8BIT = 8, JB_TEX_15BIT = 15 };

/*
 * A texture, named the console's way: the texture page its texels are in
 * and, for 4- and 8-bit texels, the colour lookup table (CLUT) that turns
 * them into 16-bit pixels: 16 or 256 VRAM pixels in a row (read through
 * the CLUT cache: see jb_vram_load). A page is 256 x
 * 256 texels from (page_x, page_y), 64, 128 or 256 VRAM pixels wide at 4,
 * 8 or 15 bits. A primitive's u and v (0..255 each) are texel coordinates
 * in the page, as the texture window (jb_set_texture_window) changes them.
 *
 * The fields after clut_y belong to the friendly level: jb_texture_create
 * fills them in (see Textures), and the console-level calls (jb_prim_* and
 * the rest above) ignore them and do not check them, so a struct made by
 * hand may leave them 0. id and owner say which texture of which context
 * it is (jb_texture_destroy). (Name the fields, {.page_x = 64, .depth =
 * JB_TEX_8BIT}: an initialiser that lists only the first five leaves the
 * rest 0 too, but compilers warn about it with -Wextra.)
 */
typedef struct jb_texture {
    int page_x; /* a multiple of 64, 0..960 */
    int page_y; /* 0 or 256 */
    int depth;  /* JB_TEX_4BIT, JB_TEX_8BIT or JB_TEX_15BIT */
    int clut_x; /* a multiple of 16, 0..1008; not used or checked at 15 bits */
    int clut_y; /* 0..511; likewise */
    int u, v;          /* the texture's top-left texel in its page, 0..255 each */
    int width, height; /* its size in texels, 1..256 each */
    int repeat;        /* not 0: it repeats (jb_texture_desc.repeat) */
    uint32_t id;       /* jb_texture_create's; 0 in a struct made by hand */
    const jb_context *owner; /* the context that made it; NULL by hand */
} jb_texture;

/* A vertex of a polygon or line. */
typedef struct jb_vertex {
    int x, y;       /* -1024..1023 each, before the drawing offset */
    uint32_t color; /* JB_RGB; each primitive says which vertices' it uses */
    int u, v;       /* texel coordinates, 0..255 each, when textured */
} jb_vertex;

/*
 * A polygon, GP0(20h-3Fh): a triangle (vertices 3) or a quad (4), drawn as
 * the triangles v[0] v[1] v[2] and v[1] v[2] v[3], so a quad's vertices go
 * in a Z (top left, top right, bottom left, bottom right). Flat (gouraud
 * 0) draws all of it in v[0]'s colour; Gouraud shades between its
 * vertices' colours. Textured when texture is not NULL.
 *
 * Words: the command, 20h + 10h Gouraud + 08h quad + 04h textured + 02h
 * blended + 01h raw, with v[0]'s colour; then each vertex's colour (from
 * the second, Gouraud only), position, and when textured its u and v with
 * in the high half the CLUT (the first vertex) or the texture page, blend
 * mode and depth (the second). As on the console, a textured polygon makes
 * its page and depth the draw mode's, and its blend mode when it blends
 * (one that does not keeps the draw mode's). An untextured polygon that
 * blends takes the mode from the draw mode, so it first sends GP0(E1h):
 * the draw mode as it is, with that blend mode (and GPUSTAT bit 15 kept:
 * jb_gp1, 09h).
 *
 * Errors: jb or poly NULL, an argument out of range (vertices, blend, the
 * texture, and each used position, colour and texel coordinate).
 */
typedef struct jb_poly {
    int vertices;              /* 3 or 4 */
    int gouraud;               /* 0 flat, else Gouraud */
    int blend;                 /* JB_BLEND_* */
    const jb_texture *texture; /* NULL for none */
    int raw;                   /* textured: texels not multiplied by the colour */
    jb_vertex v[4];
} jb_poly;

JB_API jb_result jb_prim_poly(jb_context *jb, const jb_poly *poly);

/*
 * A line from v[0] to v[1], GP0(40h-5Fh, without 08h): flat in v[0]'s
 * colour, or Gouraud; u and v are not used. Words: the command, 40h + 10h
 * Gouraud + 02h blended, with v[0]'s colour; v[0]'s position; v[1]'s
 * colour (Gouraud); v[1]'s position. One that blends first sends GP0(E1h)
 * with its mode, as an untextured polygon does.
 * Errors: jb or line NULL, an argument out of range.
 */
typedef struct jb_line {
    int gouraud; /* 0 flat, else Gouraud */
    int blend;   /* JB_BLEND_* */
    jb_vertex v[2];
} jb_line;

JB_API jb_result jb_prim_line(jb_context *jb, const jb_line *line);

/*
 * A polyline through count vertices (2 or more), GP0(48h-5Fh, with 08h):
 * sent as the line command from each vertex to the next, as jb_line
 * describes, which draws exactly what the polyline command and its
 * terminator draw (jb_gp0 also draws a polyline that way).
 * Errors: jb, line or line->v NULL, an argument out of range.
 */
typedef struct jb_polyline {
    int gouraud;        /* 0 flat (v[0]'s colour), else Gouraud */
    int blend;          /* JB_BLEND_* */
    size_t count;       /* vertices in v */
    const jb_vertex *v;
} jb_polyline;

JB_API jb_result jb_prim_polyline(jb_context *jb, const jb_polyline *line);

/*
 * A rectangle or sprite, GP0(60h-7Fh): w x h pixels from (x, y) at its top
 * left, in one colour, and textured from texel (u, v) at its top left, one
 * texel a pixel, mirrored by flip_x and flip_y. Mirrored in x, as on the
 * console, the top-left pixel takes texel u + 1 and each next pixel the one
 * before (with u 0, texels 1, 0, 255, ...); mirrored in y it starts at v.
 * It is drawn in whole 1x pixels: precise_vertices does not move it.
 *
 * Words: the command, 60h + 04h textured + 02h blended + 01h raw, plus
 * the size: 08h for 1 x 1, 10h for 8 x 8, 18h for 16 x 16 (used whenever
 * w and h are one of those; the pixels are the same as with a size word),
 * and otherwise 00h; with color; then the position; when textured u and v
 * with the CLUT in the high half; and the size word for 00h.
 *
 * A rectangle takes its texture page and depth, its blend mode and the
 * flips from the draw mode. So a textured one first sends GP0(E1h): the
 * draw mode as it is with the texture's page and depth, its blend mode
 * when it blends, and flip_x and flip_y (GPUSTAT bit 15 kept: jb_gp1,
 * 09h); an untextured one that blends
 * sends GP0(E1h) with its mode, as an untextured polygon does.
 *
 * Errors: jb or rect NULL, an argument out of range.
 */
typedef struct jb_rect {
    int x, y;                  /* -1024..1023 each, before the drawing offset */
    int w, h;                  /* 0..1023 and 0..511; 0 draws nothing */
    uint32_t color;            /* JB_RGB */
    int blend;                 /* JB_BLEND_* */
    const jb_texture *texture; /* NULL for none */
    int raw;                   /* textured: texels not multiplied by the colour */
    int u, v;                  /* textured: the top-left texel, 0..255 each */
    int flip_x, flip_y;        /* textured: mirrored, any value but 0 */
} jb_rect;

JB_API jb_result jb_prim_rect(jb_context *jb, const jb_rect *rect);

/*
 * The drawing state the console keeps, GP0(E1h-E6h). After jb_create and
 * GP1(00h) all of it is 0. jb_gpustat reports the draw mode and mask
 * settings, and GP1(10h) latches the area, offset and texture window.
 *
 * jb_set_draw_area: GP0(E3h) and GP0(E4h). Primitives draw only inside
 *   the rectangle from (left, top) to (right, bottom), both corners
 *   included: left and right 0..1023, top and bottom 0..511. With right
 *   less than left, or bottom than top, they draw nothing. Fills and
 *   transfers ignore it.
 * jb_set_draw_offset: GP0(E5h). x and y, -1024..1023 each, are added to
 *   every primitive's positions (not to fills or transfers).
 * jb_set_texture_window: GP0(E2h). mask_x, mask_y, offset_x and offset_y,
 *   0..31 each, in units of 8 texels: a texel coordinate u becomes
 *   (u & ~(mask_x * 8)) | ((offset_x & mask_x) * 8), and v likewise, so a
 *   texture repeats inside a window. All 0 is no window.
 * jb_set_mask: GP0(E6h). With set, every pixel drawn (by primitives,
 *   jb_vram_load and jb_vram_copy) gets the mask bit; with check, pixels
 *   whose mask bit is set are not drawn over. Any value but 0 is on.
 * jb_set_dither: dithering, GP0(E1h)'s bit 9 as the state keeps it (and
 *   GPUSTAT bit 9 reports it), on for any value but 0. Only that bit
 *   changes: no GP0(E1h) is sent, so the rest of the draw mode, the flips
 *   and GPUSTAT bit 15 (GP1(09h)) stay as they are.
 *
 * Errors: jb NULL, an argument out of range.
 */
JB_API jb_result jb_set_draw_area(jb_context *jb, int left, int top, int right, int bottom);
JB_API jb_result jb_set_draw_offset(jb_context *jb, int x, int y);
JB_API jb_result jb_set_texture_window(jb_context *jb, int mask_x, int mask_y, int offset_x,
                                       int offset_y);
JB_API jb_result jb_set_mask(jb_context *jb, int set, int check);
JB_API jb_result jb_set_dither(jb_context *jb, int on);

/* ------------------------------------------------------------------------ */
/*
 * The ordering table
 *
 * Primitives kept in buckets to be drawn later in one call, as console
 * programs sort a frame's primitives by depth. A table has buckets
 * numbered 0 to buckets - 1 and room for max_records records, all
 * allocated when it is made, so adding never allocates. jb_ot_draw draws
 * bucket buckets - 1 first and 0 last, so with the depth as the bucket the
 * farthest come first. Within a bucket records are drawn in the order
 * they were added: first in, first out. (The console's addPrim links each
 * primitive in front of its bucket, so its order is the reverse; this
 * table keeps the order of the calls on purpose.)
 *
 * Each add copies its struct, and the texture it points to, so neither
 * need outlive the call. It checks the struct as the jb_prim_* call of
 * that kind does (the same range rule and errors, with the add's name in
 * the detail), and a record that is added draws, at jb_ot_draw, exactly
 * the pixels that jb_prim_* call would draw if it were made then: with the
 * drawing state, the options and the precise-vertex matching as they are
 * at the draw. A polyline is kept as a record for each segment (count - 1
 * records), and draws what jb_prim_polyline draws.
 *
 * A table belongs to the context it was made with: it is drawn only on
 * that one, and must be destroyed before it. It is used from the
 * context's thread.
 *
 * jb_ot_create: a table, or NULL with *result (when result is not NULL)
 *   saying why: JB_ERROR_INVALID_ARGUMENT for jb NULL, buckets outside
 *   1..1048576 or max_records outside 1..16777216;
 *   JB_ERROR_OUT_OF_MEMORY. JB_OK with the table.
 * jb_ot_destroy: frees it; NULL is ignored.
 * jb_ot_clear: empties every bucket. Errors: ot NULL.
 * jb_ot_add_poly, jb_ot_add_line, jb_ot_add_polyline, jb_ot_add_rect: adds
 *   the primitive at the end of bucket (0..buckets - 1). Errors (nothing
 *   added): JB_ERROR_INVALID_ARGUMENT for ot or the struct NULL, the
 *   struct out of range, bucket out of range; JB_ERROR_OUT_OF_MEMORY when
 *   the table has no room (for a polyline, for all its segments).
 * jb_ot_draw: draws every record into jb, as above; the table is kept.
 *   Errors: jb or ot NULL, or ot made with another context.
 */
typedef struct jb_ot jb_ot;

JB_API jb_ot *jb_ot_create(jb_context *jb, int buckets, int max_records, jb_result *result);
JB_API void jb_ot_destroy(jb_ot *ot);
JB_API jb_result jb_ot_clear(jb_ot *ot);
JB_API jb_result jb_ot_add_poly(jb_ot *ot, int bucket, const jb_poly *poly);
JB_API jb_result jb_ot_add_line(jb_ot *ot, int bucket, const jb_line *line);
JB_API jb_result jb_ot_add_polyline(jb_ot *ot, int bucket, const jb_polyline *line);
JB_API jb_result jb_ot_add_rect(jb_ot *ot, int bucket, const jb_rect *rect);
JB_API jb_result jb_ot_draw(jb_context *jb, const jb_ot *ot);

/* ------------------------------------------------------------------------ */
/*
 * Geometry
 *
 * The GTE, the console's geometry coprocessor (COP2): one per context, and
 * independent of every other context's. It transforms, projects and
 * lights vertices in fixed point with the console's precision (psx-spx,
 * "Geometry Transformation Engine (GTE)"). A program writes inputs into
 * its registers, runs commands and reads the results, as console code
 * does with mtc2/ctc2, cop2 and mfc2/cfc2, and puts the results (SXY, RGB)
 * into primitives itself. jb_create clears every register; GP1(00h) does
 * not touch the GTE. Nothing here allocates.
 *
 * Registers are numbered as psx-spx numbers them: the data registers 0-31
 * (cop2r0-31) and the control registers 32-63 (cop2r32-63). What they keep
 * of what is written:
 *   - VZ0-2, IR0-3 and the 16-bit control registers RT33, L33, LB3, H,
 *     DQA, ZSF3 and ZSF4 keep 16 bits and read back sign-extended (H too,
 *     although the projection uses it unsigned).
 *   - OTZ and SZ0-3 keep 16 bits and read back zero-extended.
 *   - Writing SXYP pushes the screen XY FIFO (SXY0 takes SXY1, SXY1 takes
 *     SXY2, SXY2 the value); reading it reads SXY2.
 *   - Writing IRGB sets IR1-3 from a 5:5:5 colour; reading IRGB or ORGB
 *     gives IR1-3 as one (each IR >> 7, saturated to 0..31).
 *   - Writing LZCS sets LZCR to the count of its leading bits equal to bit
 *     31. ORGB and LZCR ignore writes.
 *   - FLAG keeps bits 12-30 of what is written, and bit 31 is the OR of
 *     bits 13-18 and 23-30.
 *   - The others keep all 32 bits. Vectors and matrices hold two signed
 *     16-bit values a register, the first in the low half (JB_GTE_PAIR).
 */
enum {
    /* Data registers. */
    JB_GTE_VXY0 = 0, JB_GTE_VZ0 = 1, JB_GTE_VXY1 = 2, JB_GTE_VZ1 = 3,
    JB_GTE_VXY2 = 4, JB_GTE_VZ2 = 5,
    JB_GTE_RGBC = 6, JB_GTE_OTZ = 7,
    JB_GTE_IR0 = 8, JB_GTE_IR1 = 9, JB_GTE_IR2 = 10, JB_GTE_IR3 = 11,
    JB_GTE_SXY0 = 12, JB_GTE_SXY1 = 13, JB_GTE_SXY2 = 14, JB_GTE_SXYP = 15,
    JB_GTE_SZ0 = 16, JB_GTE_SZ1 = 17, JB_GTE_SZ2 = 18, JB_GTE_SZ3 = 19,
    JB_GTE_RGB0 = 20, JB_GTE_RGB1 = 21, JB_GTE_RGB2 = 22, JB_GTE_RES1 = 23,
    JB_GTE_MAC0 = 24, JB_GTE_MAC1 = 25, JB_GTE_MAC2 = 26, JB_GTE_MAC3 = 27,
    JB_GTE_IRGB = 28, JB_GTE_ORGB = 29, JB_GTE_LZCS = 30, JB_GTE_LZCR = 31,
    /* Control registers. */
    JB_GTE_RT11RT12 = 32, JB_GTE_RT13RT21 = 33, JB_GTE_RT22RT23 = 34,
    JB_GTE_RT31RT32 = 35, JB_GTE_RT33 = 36,
    JB_GTE_TRX = 37, JB_GTE_TRY = 38, JB_GTE_TRZ = 39,
    JB_GTE_L11L12 = 40, JB_GTE_L13L21 = 41, JB_GTE_L22L23 = 42,
    JB_GTE_L31L32 = 43, JB_GTE_L33 = 44,
    JB_GTE_RBK = 45, JB_GTE_GBK = 46, JB_GTE_BBK = 47,
    JB_GTE_LR1LR2 = 48, JB_GTE_LR3LG1 = 49, JB_GTE_LG2LG3 = 50,
    JB_GTE_LB1LB2 = 51, JB_GTE_LB3 = 52,
    JB_GTE_RFC = 53, JB_GTE_GFC = 54, JB_GTE_BFC = 55,
    JB_GTE_OFX = 56, JB_GTE_OFY = 57, JB_GTE_H = 58, JB_GTE_DQA = 59,
    JB_GTE_DQB = 60, JB_GTE_ZSF3 = 61, JB_GTE_ZSF4 = 62,
    JB_GTE_FLAG = 63
};

/* Two signed 16-bit values as one register, lo in the low half: a
   vector's X and Y, or two matrix elements. */
#define JB_GTE_PAIR(lo, hi) (((uint32_t)(lo) & 0xffffu) | ((uint32_t)(hi) & 0xffffu) << 16)
/* The signed X (low half) and Y (high half) of an SXY value. */
#define JB_GTE_SX(sxy) ((int)(((uint32_t)(sxy) & 0xffffu) ^ 0x8000u) - 0x8000)
#define JB_GTE_SY(sxy) ((int)((((uint32_t)(sxy) >> 16) & 0xffffu) ^ 0x8000u) - 0x8000)

/*
 * Commands: the 25-bit immediate of the cop2 instruction, laid out as
 * psx-spx gives it. Bits 0-5 choose the command; sf (bit 19) shifts
 * results right by 12 bits (fractions in IR); lm (bit 10) saturates IR1-3
 * to 0..0x7FFF rather than -0x8000..0x7FFF; MVMVA also takes its matrix,
 * vector and translation from bits 13-18. Bits 20-24 and 6-9, 11-12 are
 * ignored, as by the console. The constants are psx-spx's words, so sf
 * and lm are set as it lists them: sf in all but NCLIP, OP, MVMVA, SQR,
 * GPF and GPL; lm in NCS, NCT, NCCS, NCCT, NCDS, NCDT, CC, CDP and SQR.
 * OR bits in, or clear them, to change them:
 *   JB_GTE_RTPS & ~JB_GTE_SF
 *   JB_GTE_MVMVA | JB_GTE_SF | JB_GTE_MX_LLM | JB_GTE_V_V0 | JB_GTE_CV_NONE
 */
#define JB_GTE_RTPS 0x0180001u  /* perspective transformation, V0 */
#define JB_GTE_RTPT 0x0280030u  /* perspective transformation, V0, V1, V2 */
#define JB_GTE_NCLIP 0x1400006u /* normal clipping: MAC0 = SXY winding */
#define JB_GTE_OP 0x170000Cu    /* outer product of IR and RT's diagonal */
#define JB_GTE_DPCS 0x0780010u  /* depth cueing, RGBC */
#define JB_GTE_INTPL 0x0980011u /* interpolation of IR and the far colour */
#define JB_GTE_MVMVA 0x0400012u /* matrix x vector + vector */
#define JB_GTE_NCDS 0x0E80413u  /* normal colour depth cue, V0 */
#define JB_GTE_CDP 0x1280414u   /* colour depth cue */
#define JB_GTE_NCDT 0x0F80416u  /* normal colour depth cue, V0, V1, V2 */
#define JB_GTE_NCCS 0x108041Bu  /* normal colour colour, V0 */
#define JB_GTE_CC 0x138041Cu    /* colour colour */
#define JB_GTE_NCS 0x0C8041Eu   /* normal colour, V0 */
#define JB_GTE_NCT 0x0D80420u   /* normal colour, V0, V1, V2 */
#define JB_GTE_SQR 0x0A00428u   /* square of IR */
#define JB_GTE_DCPL 0x0680029u  /* depth cue colour light */
#define JB_GTE_DPCT 0x0F8002Au  /* depth cueing, RGB0 three times */
#define JB_GTE_AVSZ3 0x158002Du /* OTZ = ZSF3 x (SZ1 + SZ2 + SZ3) */
#define JB_GTE_AVSZ4 0x168002Eu /* OTZ = ZSF4 x (SZ0 + SZ1 + SZ2 + SZ3) */
#define JB_GTE_GPF 0x190003Du   /* general purpose interpolation */
#define JB_GTE_GPL 0x1A0003Eu   /* general purpose interpolation with base */
#define JB_GTE_NCCT 0x118043Fu  /* normal colour colour, V0, V1, V2 */

/* Command fields. */
#define JB_GTE_SF (1u << 19)
#define JB_GTE_LM (1u << 10)
#define JB_GTE_MX_RT (0u << 17)  /* MVMVA matrix: rotation */
#define JB_GTE_MX_LLM (1u << 17) /* light */
#define JB_GTE_MX_LCM (2u << 17) /* light colour */
#define JB_GTE_V_V0 (0u << 15)   /* MVMVA vector: V0, V1, V2, or IR1-3 */
#define JB_GTE_V_V1 (1u << 15)
#define JB_GTE_V_V2 (2u << 15)
#define JB_GTE_V_IR (3u << 15)
#define JB_GTE_CV_TR (0u << 13)   /* MVMVA translation: TR, BK, FC, none */
#define JB_GTE_CV_BK (1u << 13)
#define JB_GTE_CV_FC (2u << 13)   /* the console's bugged FC form */
#define JB_GTE_CV_NONE (3u << 13)

/* FLAG bits, set by the command that overflowed or saturated. */
#define JB_GTE_FLAG_ERROR (1u << 31)   /* any of bits 13-18 and 23-30 */
#define JB_GTE_FLAG_MAC1_POS (1u << 30) /* MAC1-3 overflowed 44 bits */
#define JB_GTE_FLAG_MAC2_POS (1u << 29)
#define JB_GTE_FLAG_MAC3_POS (1u << 28)
#define JB_GTE_FLAG_MAC1_NEG (1u << 27)
#define JB_GTE_FLAG_MAC2_NEG (1u << 26)
#define JB_GTE_FLAG_MAC3_NEG (1u << 25)
#define JB_GTE_FLAG_IR1 (1u << 24)      /* IR1-3 saturated */
#define JB_GTE_FLAG_IR2 (1u << 23)
#define JB_GTE_FLAG_IR3 (1u << 22)
#define JB_GTE_FLAG_COLOR_R (1u << 21)  /* a colour FIFO channel saturated */
#define JB_GTE_FLAG_COLOR_G (1u << 20)
#define JB_GTE_FLAG_COLOR_B (1u << 19)
#define JB_GTE_FLAG_SZ_OTZ (1u << 18)   /* SZ3 or OTZ saturated */
#define JB_GTE_FLAG_DIVIDE (1u << 17)   /* the projection's divide overflowed */
#define JB_GTE_FLAG_MAC0_POS (1u << 16) /* MAC0 overflowed 32 bits */
#define JB_GTE_FLAG_MAC0_NEG (1u << 15)
#define JB_GTE_FLAG_SX2 (1u << 14)      /* SX2 or SY2 saturated to -0x400..0x3FF */
#define JB_GTE_FLAG_SY2 (1u << 13)
#define JB_GTE_FLAG_IR0 (1u << 12)      /* IR0 saturated */

/*
 * jb_gte_write: writes value into register reg (0..63), keeping what that
 * register keeps (above). Errors: jb NULL, reg out of range.
 *
 * jb_gte_read: register reg (0..63) as mfc2 or cfc2 reads it. 0 for jb
 * NULL or reg out of range (which is also recorded for jb_error_string).
 */
JB_API jb_result jb_gte_write(jb_context *jb, int reg, uint32_t value);
JB_API uint32_t jb_gte_read(jb_context *jb, int reg);

/*
 * Runs the command (JB_GTE_*, 0..0x1FFFFFF) as the console's GTE does:
 * FLAG is cleared first and then holds what the command overflowed or
 * saturated; read it as register JB_GTE_FLAG. With precise_vertices or
 * perspective_correct on, RTPS and RTPT also remember each vertex they
 * project (jb_config), which changes no register.
 * Errors: jb NULL; JB_ERROR_INVALID_ARGUMENT for a value above 25 bits
 * (pass the immediate, not the whole cop2 instruction);
 * JB_ERROR_UNSUPPORTED, with nothing changed, for a command number (bits
 * 0-5) the console has no documented command for.
 */
JB_API jb_result jb_gte_command(jb_context *jb, uint32_t command);

/*
 * Forgets the projections the GTE has remembered for precise_vertices and
 * perspective_correct. Call it once a frame, before that frame's RTPS and
 * RTPT: a primitive is matched with projections made since the last call,
 * by its vertices' positions, so last frame's could otherwise move this
 * frame's vertices that land on the same pixels. Projections are
 * remembered only while one of the two options is on; turning both off
 * stops that without forgetting what is there, and this call forgets it
 * whether or not an option is on. The memory is a fixed size, and a busy
 * frame can lose older projections before it is full. A vertex with no
 * remembered projection at its position is drawn the normal way, as with
 * the options off. jb_begin_frame calls it (The frame); a program that
 * draws at the console level without frames calls it itself, as the
 * display commands do not mark a frame.
 * Errors: jb NULL.
 */
JB_API jb_result jb_gte_begin_frame(jb_context *jb);

/*
 * GTE helpers
 *
 * Calls that do the register packing, the loops and the FIFO reads that
 * console code writes by hand. Each one is exactly the sequence of
 * jb_gte_write, jb_gte_command and jb_gte_read calls given with it, on the
 * context's GTE: the registers end as that sequence leaves them, so these
 * mix freely with a program's own register work, and with
 * precise_vertices or perspective_correct on jb_gte_project's projections
 * are remembered as jb_gte_command's are. A failing call writes nothing.
 * None of them allocates.
 *
 * Fixed point: a matrix element, a unit vector's component and a light
 * level are 4.12, so JB_GTE_ONE (4096) is 1.0.
 */
#define JB_GTE_ONE 4096

/* A 3 x 3 matrix, m[row][column], 4.12. A struct rather than an array, so
   that a const one can be passed from non-const data in C. */
typedef struct jb_gte_matrix {
    int16_t m[3][3];
} jb_gte_matrix;

/* A vector as V0-V2 hold one: a model-space vertex, or a normal (4.12). */
typedef struct jb_gte_vec {
    int16_t x, y, z;
} jb_gte_vec;

/* Matrix and vector registers, for jb_gte_set_matrix and jb_gte_set_vector. */
enum {
    JB_GTE_ROTATION = 0x01,    /* RT11-RT33, control registers 32-36 */
    JB_GTE_LIGHT = 0x02,       /* L11-L33: a light's direction a row (40-44) */
    JB_GTE_LIGHT_COLOR = 0x04, /* LR1-LB3: a light's colour a column (48-52) */
    JB_GTE_TRANSLATION = 0x08, /* TRX, TRY, TRZ (37-39) */
    JB_GTE_BACKGROUND = 0x10,  /* RBK, GBK, BBK: the ambient light (45-47) */
    JB_GTE_FAR_COLOR = 0x20    /* RFC, GFC, BFC: the depth-cue colour (53-55) */
};

/*
 * jb_gte_set_matrix: m into each matrix that which names (JB_GTE_ROTATION,
 * JB_GTE_LIGHT, JB_GTE_LIGHT_COLOR, ORed together), as five writes from
 * its first register (JB_GTE_RT11RT12, JB_GTE_L11L12 or JB_GTE_LR1LR2):
 *     JB_GTE_PAIR(m[0][0], m[0][1]), JB_GTE_PAIR(m[0][2], m[1][0]),
 *     JB_GTE_PAIR(m[1][1], m[1][2]), JB_GTE_PAIR(m[2][0], m[2][1]), m[2][2]
 * So m[r][c] is RT(r+1)(c+1) and L(r+1)(c+1); in the light colour matrix
 * it is channel r (red, green, blue) of light c+1: LR1 is m[0][0], LG1
 * m[1][0], LR2 m[0][1].
 *
 * jb_gte_set_vector: x, y and z into each vector that which names
 * (JB_GTE_TRANSLATION, JB_GTE_BACKGROUND, JB_GTE_FAR_COLOR, ORed), three
 * writes each, in that order. The background and far colours are 4.12 a
 * channel (JB_GTE_ONE is a light level of 1.0 for the background, and a
 * far colour channel c is c * 16 in the units of the depth cue).
 *
 * Errors: jb or m NULL; which 0 or with a bit that is not one of the
 * call's three.
 */
JB_API jb_result jb_gte_set_matrix(jb_context *jb, int which, const jb_gte_matrix *m);
JB_API jb_result jb_gte_set_vector(jb_context *jb, int which, int32_t x, int32_t y, int32_t z);

/*
 * Projects n model-space vertices with the GTE's registers as they are
 * (rotation, translation, H, OFX, OFY, DQA, DQB). For each three, from v[i]:
 *     VXY0, VZ0, VXY1, VZ1, VXY2, VZ2 written; JB_GTE_RTPT;
 *     sxy[i..i+2] = SXY0, SXY1, SXY2 and sz[i..i+2] = SZ1, SZ2, SZ3 read
 * (after RTPT the depths are in SZ1-3, one place behind the positions),
 * then for each of the one or two left: VXY0, VZ0; JB_GTE_RTPS; SXY2 and
 * SZ3. An SXY is the packed register (JB_GTE_SX, JB_GTE_SY take it
 * apart; x and y are saturated to -1024..1023, as the GTE does). sxy and sz
 * may each be NULL. *flag, when flag is not NULL, is the OR of FLAG after
 * every command, so it reports a saturation anywhere (JB_GTE_FLAG_SX2,
 * JB_GTE_FLAG_DIVIDE, ...), where FLAG itself holds only the last
 * command's. IR0 is left as the last command makes it: the depth cue of
 * the last vertex.
 * Errors: jb NULL; v NULL with n above 0 (n 0 does nothing).
 */
JB_API jb_result jb_gte_project(jb_context *jb, const jb_gte_vec *v, size_t n, uint32_t *sxy,
                                uint16_t *sz, uint32_t *flag);

/* What jb_gte_light computes for a normal N, as psx-spx gives it (each
   step truncated and saturated as the GTE does; light levels 4.12):
   L = BK + LCM x max(0, LLM x N), each channel saturated to 0..7.999. */
enum {
    JB_GTE_LIGHT_NC = 1,  /* NCT, NCS: the light alone, L x 256 (1.0 is 255) */
    JB_GTE_LIGHT_NCC = 2, /* NCCT, NCCS: color x L (1.0 leaves it unchanged) */
    JB_GTE_LIGHT_NCD = 3  /* NCDT, NCDS: color x L moved towards the far
                             colour by IR0 / 4096 (the depth cue) */
};

/*
 * Lights n normals (4.12, in the light matrix's space) with the light
 * matrix, light colour matrix and background colour as they are, in one of
 * the three ways above (kind). First RGBC is written with color (JB_RGB,
 * and command byte 0), then for each three from normals[i]: VXY0 ... VZ2
 * written; JB_GTE_NCT, JB_GTE_NCCT or JB_GTE_NCDT; rgb[i..i+2] = RGB0,
 * RGB1, RGB2 read, each & 0xFFFFFF (the colour without the command byte,
 * ready for a primitive); and for each one left: VXY0, VZ0; JB_GTE_NCS,
 * JB_GTE_NCCS or JB_GTE_NCDS; RGB2. The results are saturated to 0..255 a
 * channel (FLAG's colour bits say so). With JB_GTE_LIGHT_NCD every normal
 * takes the same IR0, which these commands do not change: write it (or
 * leave the one jb_gte_project left) before the call. rgb may be NULL;
 * *flag (flag not NULL) is the OR of FLAG after every command.
 * Errors: jb NULL; kind not one of the three; color above 0xFFFFFF;
 * normals NULL with n above 0.
 */
JB_API jb_result jb_gte_light(jb_context *jb, int kind, const jb_gte_vec *normals, size_t n,
                              uint32_t color, uint32_t *rgb, uint32_t *flag);

/* A directional light, for jb_gte_set_lights: its direction, the way its
   light travels (as jb_light's: a lamp shining down is (0, -4096, 0) in
   y-up space), as a unit vector in 4.12 in the space of the normals it will
   light, and its colour (JB_RGB). The light matrix's rows are the opposite
   direction, towards the lamp, which is what NC* multiply normals by. */
typedef struct jb_gte_lamp {
    int16_t x, y, z;
    uint32_t color;
} jb_gte_lamp;

/*
 * jb_gte_set_lights: the lighting registers from n lamps (0 to 3) and an
 * ambient colour (JB_RGB): row i of the light matrix is minus lamp i's
 * direction (towards the lamp; -(-32768) saturating to 32767), column i of
 * the light colour matrix its colour, the rows and columns of absent lamps
 * 0, and the background colour the ambient. A
 * colour channel c becomes the level (c * 4096 + 127) / 255, so 255 is
 * 1.0 (JB_GTE_ONE): a normal facing one white lamp, with no ambient, is lit
 * to 1.0, and JB_GTE_LIGHT_NCC then gives its colour unchanged
 * (JB_RGB(128, 128, 128), a texel unchanged, stays so). Lamps and ambient
 * together may light to more than 1.0, brightening. The writes are those
 * of jb_gte_set_matrix(JB_GTE_LIGHT), jb_gte_set_matrix(JB_GTE_LIGHT_COLOR)
 * and jb_gte_set_vector(JB_GTE_BACKGROUND) with those values.
 *
 * jb_gte_set_light_directions: the light matrix alone, as
 * jb_gte_set_matrix(JB_GTE_LIGHT) of D x rotation (jb_gte_mul_matrix),
 * where row i of D is minus lamp i's direction, as above (0 for absent
 * lamps); the colours
 * are not used. rotation NULL is the identity, so the directions are taken
 * as they are. Lighting needs the directions in the normals' space, which
 * is an object's own (its model space): with directions in world space and
 * rotation the object's rotation from model to world space, D x rotation
 * is the light matrix for that object's normals, since a light direction l
 * and a normal n turned into world space by R meet as l . (R n) =
 * (l R) . n. Call it for each object whose rotation differs.
 *
 * Errors: jb NULL; n outside 0..3; lamps NULL with n above 0; a colour
 * above 0xFFFFFF (jb_gte_set_lights).
 */
JB_API jb_result jb_gte_set_lights(jb_context *jb, const jb_gte_lamp *lamps, int n,
                                   uint32_t ambient);
JB_API jb_result jb_gte_set_light_directions(jb_context *jb, const jb_gte_lamp *lamps, int n,
                                             const jb_gte_matrix *rotation);

/*
 * The arithmetic of three commands as pure functions, with no context: the
 * same results as the commands give from the same register values, for
 * every input, so a program can cull and sort vertices it projected once
 * without writing them back into the registers.
 *
 * jb_gte_nclip: MAC0 as NCLIP leaves it with SXY0-2 = sxy0, sxy1, sxy2
 * (packed SXY values, each half signed): x0 y1 + x1 y2 + x2 y0 - x0 y2 -
 * x1 y0 - x2 y1, kept to its low 32 bits. Positive when the three go
 * clockwise on the screen (y down), negative anticlockwise, 0 for a line,
 * as long as the full sum fits 32 bits, which it always does for
 * positions the GTE projected (-1024..1023 each); for others whose sum
 * overflows (FLAG's MAC0 bits say so) the sign is the wrapped value's, as
 * the command's is, and says nothing about the winding.
 *
 * jb_gte_avsz3, jb_gte_avsz4: OTZ as AVSZ3 and AVSZ4 make it from ZSF3 or
 * ZSF4 (zsf, signed) and the depths SZ1-3 or SZ0-3: (zsf * the sum of the
 * depths) >> 12, saturated to 0..0xFFFF.
 *
 * *flag, when flag is not NULL, gets FLAG as the command leaves it:
 * JB_GTE_FLAG_MAC0_POS or JB_GTE_FLAG_MAC0_NEG when the full sum does not
 * fit 32 bits, JB_GTE_FLAG_SZ_OTZ when OTZ saturated, and
 * JB_GTE_FLAG_ERROR with any of those; else 0. None of them fails.
 */
JB_API int32_t jb_gte_nclip(uint32_t sxy0, uint32_t sxy1, uint32_t sxy2, uint32_t *flag);
JB_API uint32_t jb_gte_avsz3(int16_t zsf3, uint16_t sz1, uint16_t sz2, uint16_t sz3,
                             uint32_t *flag);
JB_API uint32_t jb_gte_avsz4(int16_t zsf4, uint16_t sz0, uint16_t sz1, uint16_t sz2, uint16_t sz3,
                             uint32_t *flag);

/*
 * Fixed-point maths in the GTE's conventions, in integer arithmetic only,
 * so every machine gives the same results. Angles are in 4096ths of a
 * turn (1024 is 90 degrees); any int is taken modulo 4096, negative ones
 * too. A NULL pointer argument makes a call write nothing.
 *
 * jb_isin, jb_icos: round(4096 x sin(2 pi angle / 4096)), and the cosine
 * likewise, exactly: 4.12 values from -4096 to 4096, from a table of the
 * quarter turn (every one of the 1025 values the nearest integer).
 *
 * jb_gte_rotation_matrix: the rotation by x about the x axis, then y about
 * the y axis, then z about the z axis, for column vectors (v' = R v):
 * R = Rz x (Ry x Rx), the products by jb_gte_mul_matrix, with
 *     Rx = [1 0 0; 0 c -s; 0 s c]   Ry = [c 0 s; 0 1 0; -s 0 c]
 *     Rz = [c -s 0; s c 0; 0 0 1]
 * (c and s the jb_icos and jb_isin of that axis's angle, 1 is 4096): a
 * positive angle turns y towards z, z towards x, and x towards y. On the
 * console's y-down screen, with z going into it, a positive z angle turns
 * clockwise.
 *
 * jb_gte_mul_matrix: out = a x b, each element (r, c) the sum over k of
 * a[r][k] x b[k][c], shifted right by 12 (rounding down, as the GTE does)
 * and saturated to -32768..32767: what MVMVA with sf, a as its matrix,
 * column c of b as its vector and no translation leaves in IR1-3. out may
 * be a or b.
 *
 * jb_gte_mul_vector: out = m x v the same way: MVMVA's IR1-3 for v. out
 * may be v.
 */
JB_API int jb_isin(int angle);
JB_API int jb_icos(int angle);
JB_API void jb_gte_rotation_matrix(int x, int y, int z, jb_gte_matrix *out);
JB_API void jb_gte_mul_matrix(const jb_gte_matrix *a, const jb_gte_matrix *b,
                              jb_gte_matrix *out);
JB_API void jb_gte_mul_vector(const jb_gte_matrix *m, const jb_gte_vec *v, jb_gte_vec *out);

/* ------------------------------------------------------------------------ */
/* Output */

/*
 * The display area as the console shows it: width x height pixels at the
 * context's scale (the display size times jb_config.scale), each 4 bytes
 * R, G, B, A with A 255, rows top to bottom with no padding (width x 4
 * bytes apart). The area starts where GP1(05h) put it and wraps at the
 * VRAM edges. 15-bit pixels widen by repeating their top bits
 * (c << 3 | c >> 2); in 24-bit mode (GP1(08h) bit 4) the VRAM bytes are
 * the R, G, B bytes. A disabled display gives a black frame of the same
 * size.
 *
 * The pixels belong to the context and stay valid until the next jb_frame,
 * GP1(00h) or jb_destroy on it. Only a frame larger than any before it
 * allocates, and every call makes sure the buffer also holds the frame's
 * reserved size at the current scale (jb_config.width and height, and the
 * 16:9 width with jb_config.widescreen), so that once jb_frame has been
 * called at a scale, switching widescreen does not allocate.
 *
 * Returns NULL, with *width and *height 0, for jb NULL or without memory
 * (JB_ERROR_OUT_OF_MEMORY, see jb_error_string). width and height may be
 * NULL.
 */
JB_API const uint8_t *jb_frame(jb_context *jb, int *width, int *height);

/* ------------------------------------------------------------------------ */
/*
 * Helpers
 *
 * Constants and word builders for the console level. Each is a macro that
 * gives the value the comment says, so they work in constant expressions;
 * an argument outside its range keeps only the bits the field has, as the
 * console would. Being macros, some evaluate an argument more than once:
 * JB_RGB_TO_15 its rgb (three times), JB_GP1_DISPLAY_MODE its w and h,
 * JB_GP0_DRAW_MODE its blend and depth. Give them no argument with side
 * effects (not JB_RGB_TO_15(*src++)); convert in a loop with the
 * increment apart.
 */

/*
 * Colours. JB_RGB (Drawing) is a primitive's colour, 8 bits a channel.
 * VRAM, textures and palettes hold 16-bit pixels: red in bits 0-4, green
 * 5-9, blue 10-14, and bit 15, which means two things:
 *   - in VRAM it is the mask bit (jb_set_mask);
 *   - in a texel (a 15-bit texture's, or a palette entry a 4- or 8-bit
 *     texel names) it makes the texel semi-transparent when the primitive
 *     blends, and changes nothing when it does not.
 * The texel 0x0000 is never drawn: it is the transparent texel. So black
 * that should draw is 0x8000 (JB_TEXEL_BLACK), which a blending primitive
 * blends as black (adding nothing in JB_BLEND_ADD).
 *
 * JB_RGB15: a 16-bit pixel from 5-bit channels, r, g, b 0..31, bit 15 0.
 * JB_RGB_TO_15: the same from a JB_RGB colour, each channel's top 5 bits,
 *   as a fill and a flat rectangle keep them.
 * JB_NEUTRAL: the primitive colour that draws texels as they are (a
 *   texel is multiplied by colour / 128, each channel).
 * JB_TINT_BLACK: black, for the friendly fields in which 0 stands for
 *   JB_NEUTRAL (jb_material.color, and the tint of jb_mesh_draw,
 *   jb_billboard and jb_sprite), so that a fade to black can end at black
 *   rather than jump back to JB_NEUTRAL at 0. Only those fields take it;
 *   every other colour is a JB_RGB value, and where it is checked (not,
 *   say, in a raw primitive) refuses it as above 0xFFFFFF.
 */
#define JB_RGB15(r, g, b)                                                                     \
    ((uint16_t)(((uint32_t)(r) & 31u) | ((uint32_t)(g) & 31u) << 5 | ((uint32_t)(b) & 31u) << 10))
#define JB_RGB_TO_15(rgb)                                                                     \
    ((uint16_t)(((uint32_t)(rgb) >> 3 & 31u) | ((uint32_t)(rgb) >> 11 & 31u) << 5 |           \
                ((uint32_t)(rgb) >> 19 & 31u) << 10))
#define JB_NEUTRAL JB_RGB(128, 128, 128)
#define JB_TINT_BLACK 0xFF000000u
#define JB_TEXEL_TRANSPARENT ((uint16_t)0x0000u) /* never drawn */
#define JB_TEXEL_BLACK ((uint16_t)0x8000u)       /* opaque black (blended when blending) */
#define JB_TEXEL_SEMI ((uint16_t)0x8000u)        /* OR in: semi-transparent texel */
#define JB_MASK_BIT ((uint16_t)0x8000u)          /* the same bit in VRAM: the mask bit */

/*
 * GP1 words (jb_gp1 describes each command).
 * JB_GP1_DISPLAY_ENABLE(on): 03h, the display shown for on not 0, blanked
 *   for 0 (the console's bit is "disable": this takes care of it).
 * JB_GP1_DISPLAY_AREA(x, y): 05h, the display area's top-left at x
 *   0..1023, y 0..511 of VRAM.
 * JB_GP1_DISPLAY_MODE(w, h): 08h for one of the console's sizes: w 256,
 *   320, 368, 512 or 640 (any other w gives 256's code), h 240 or 480
 *   (480 sets the vertical resolution and interlace bits; any other h
 *   gives 240). OR in JB_GP1_MODE_24BIT for 24-bit pixels.
 */
#define JB_GP1_RESET 0x00000000u
#define JB_GP1_RESET_COMMANDS 0x01000000u
#define JB_GP1_ACK_IRQ 0x02000000u
#define JB_GP1_DISPLAY_ENABLE(on) (0x03000000u | ((on) ? 0u : 1u))
#define JB_GP1_DMA(direction) (0x04000000u | ((uint32_t)(direction) & 3u))
#define JB_GP1_DISPLAY_AREA(x, y)                                                             \
    (0x05000000u | ((uint32_t)(x) & 0x3ffu) | ((uint32_t)(y) & 0x1ffu) << 10)
#define JB_GP1_DISPLAY_MODE(w, h)                                                             \
    (0x08000000u |                                                                            \
     ((w) == 320 ? 1u : (w) == 512 ? 2u : (w) == 640 ? 3u : (w) == 368 ? 0x40u : 0u) |       \
     ((h) == 480 ? 0x24u : 0u))
#define JB_GP1_MODE_24BIT 0x10u

/*
 * GP0 words for the state commands (jb_set_* in Drawing are the same as
 * calls) and the one-word commands.
 * JB_GP0_XY(x, y): a position or size word, x in the low half.
 * JB_GP0_DRAW_MODE(page_x, page_y, blend, depth, dither): E1h with the
 *   texture page at (page_x, page_y) (VRAM pixels, multiples of 64 and
 *   256), the semi-transparency mode of blend (JB_BLEND_*; NONE gives mode
 *   0, B / 2 + F / 2), depth JB_TEX_*, dithering for dither not 0; flips
 *   and bits 10-11 clear.
 * JB_GP0_TEXTURE_WINDOW(mask_x, mask_y, offset_x, offset_y): E2h, as
 *   jb_set_texture_window.
 * JB_GP0_DRAW_AREA_TOP_LEFT(x, y), JB_GP0_DRAW_AREA_BOTTOM_RIGHT(x, y):
 *   E3h and E4h, as jb_set_draw_area.
 * JB_GP0_DRAW_OFFSET(x, y): E5h, as jb_set_draw_offset (x, y signed).
 * JB_GP0_MASK(set, check): E6h, as jb_set_mask.
 * JB_GP0_CLEAR_CACHE: 01h, which empties the CLUT cache (jb_vram_load).
 * JB_GP0_IRQ: 1Fh, which sets GPUSTAT bit 24.
 */
#define JB_GP0_XY(x, y) (((uint32_t)(x) & 0xffffu) | ((uint32_t)(y) & 0xffffu) << 16)
#define JB_GP0_DRAW_MODE(page_x, page_y, blend, depth, dither)                                \
    (0xe1000000u | ((uint32_t)(page_x) / 64u & 15u) | ((uint32_t)(page_y) / 256u & 1u) << 4 | \
     (((blend) > 0 ? (uint32_t)(blend) - 1u : 0u) & 3u) << 5 |                                \
     ((depth) == 4 ? 0u : (depth) == 8 ? 1u : 2u) << 7 | ((dither) ? 1u : 0u) << 9)
#define JB_GP0_TEXTURE_WINDOW(mask_x, mask_y, offset_x, offset_y)                             \
    (0xe2000000u | ((uint32_t)(mask_x) & 31u) | ((uint32_t)(mask_y) & 31u) << 5 |             \
     ((uint32_t)(offset_x) & 31u) << 10 | ((uint32_t)(offset_y) & 31u) << 15)
#define JB_GP0_DRAW_AREA_TOP_LEFT(x, y)                                                       \
    (0xe3000000u | ((uint32_t)(x) & 0x3ffu) | ((uint32_t)(y) & 0x1ffu) << 10)
#define JB_GP0_DRAW_AREA_BOTTOM_RIGHT(x, y)                                                   \
    (0xe4000000u | ((uint32_t)(x) & 0x3ffu) | ((uint32_t)(y) & 0x1ffu) << 10)
#define JB_GP0_DRAW_OFFSET(x, y)                                                              \
    (0xe5000000u | ((uint32_t)(x) & 0x7ffu) | ((uint32_t)(y) & 0x7ffu) << 11)
#define JB_GP0_MASK(set, check) (0xe6000000u | ((set) ? 1u : 0u) | ((check) ? 2u : 0u))
#define JB_GP0_CLEAR_CACHE 0x01000000u
#define JB_GP0_IRQ 0x1f000000u

/* ------------------------------------------------------------------------ */
/*
 * The frame
 *
 * The friendly level draws a frame at a time:
 *     jb_begin_frame(jb, JB_RGB(16, 24, 48));
 *     ... jb_draw_* and the rest of the friendly level ...
 *     if (jb_end_frame(jb) != JB_OK) report(jb_error_string(jb));
 *     if ((rgba = jb_frame(jb, &w, &h)) != NULL) present(rgba, w, h);
 * The draws need no checks of their own: the first failure of the frame,
 * jb_begin_frame's too, is what jb_end_frame returns (jb_check).
 *
 * What the friendly level also uses from before it: the structs of
 * Drawing (jb_poly, jb_line, jb_polyline, jb_rect, jb_texture) and its
 * JB_RGB and JB_BLEND_*, jb_frame (Output), and the colour constants of
 * Helpers. Colours come in three scales, all the console's: JB_RGB with
 * 255 full for fills, untextured primitives, text, lights, ambient and
 * fog; JB_RGB with 128 leaving texels as they are (JB_NEUTRAL) for the
 * colour a textured draw multiplies its texels by (material colours,
 * tints); and 15-bit VRAM colours (JB_RGB15) for texels and palettes,
 * which jb_palette_from_rgba makes from RGBA. Dithering, the console's
 * most visible pattern at 1x, starts off, as GP1(00h) leaves it:
 * jb_set_dither(jb, 1) (Drawing) turns it on, and a frame's records take
 * it as it is at jb_end_frame.
 *
 * Buffers. The frame double-buffers inside VRAM, as console games did: it
 * draws into the back buffer while the other is shown. The buffers are
 * fixed when the context is made, from the reserved display size (w x h:
 * jb_config.width and height, w the larger of width and the 16:9 width
 * with jb_config.widescreen): two at (0, 0) and (0, h) when 2h <= 512;
 * else two side by side at (0, 0) and (w, 0) when 2w <= 1024; else one at
 * (0, 0), drawn while it is shown (as 640 x 480 games did). The rest of
 * VRAM is free for textures and palettes. The frame draws, and clears,
 * at jb_frame_size: the configured height h and the width each
 * jb_begin_frame takes (jb_config.width, or the 16:9 width while
 * widescreen is on), whatever display mode the program sets with
 * GP1(08h): such a mode changes only what jb_frame shows, which for a
 * mode wider or taller than the frame includes VRAM beyond the buffer
 * (the other buffer, textures).
 *
 * jb_begin_frame: starts a frame. It chooses the back buffer (in turn,
 *   the first frame the first buffer), sets the drawing area to it and the
 *   drawing offset to its corner (GP0(E3h-E5h)), so that positions are
 *   pixels of the frame, clears it to clear_color (JB_RGB) with the mask
 *   bit 0, empties the frame's records, makes the 2D layer the overlay,
 *   and forgets the GTE's remembered projections (jb_gte_begin_frame).
 *   The clear writes the buffer's pixels and nothing else: fills
 *   (GP0(02h)) where its columns lie on 16-pixel boundaries, and opaque
 *   flat rectangles, with the mask settings off for them and restored, at
 *   the ends. The first call allocates the records (jb_config).
 *   Errors, with nothing done: jb NULL, a frame already begun or
 *   clear_color above 0xFFFFFF (JB_ERROR_INVALID_ARGUMENT); no memory for
 *   the records (JB_ERROR_OUT_OF_MEMORY).
 *
 * During a frame the friendly level records what it is asked to draw:
 * nothing reaches VRAM until jb_end_frame, but for the font: the first
 * text that shows, once its first character has a record, places the
 * font in VRAM at once (outside the display buffers, so nothing drawn
 * changes; Sprites and text). Console-level calls (jb_gp0,
 * jb_prim_*, jb_vram_*, ...) still act at once, so they land over the
 * clear and under everything recorded. So do the texture calls
 * (jb_texture_create, jb_texture_write, jb_palette_write,
 * jb_texture_destroy): records keep where a texture is, not its texels,
 * so every draw of the frame shows the texels and palette as they are at
 * jb_end_frame. A palette rewritten between two draws shows its last
 * colours in both (make palette variants textures of their own), and a
 * texture destroyed and its place reused during a frame shows the new
 * texels in the draws recorded before. A call that would change the
 * drawing area or offset changes them for the console-level calls after
 * it; jb_end_frame sets them back to the buffer before it draws.
 *
 * jb_end_frame: draws the frame's records into the back buffer, in this
 *   order: the background layer, in the order of the calls; the scene
 *   (the friendly 3D drawing), from its farthest bucket to its nearest,
 *   each bucket in the order of the calls; the overlay layer, in the
 *   order of the calls. Then it shows the buffer (GP1(05h)), and after the
 *   first frame enables the display (GP1(03h), so nothing need be sent
 *   to see it; a display disabled later stays disabled). Records draw with
 *   the console's rules and the context's state when jb_end_frame runs:
 *   the dithering (jb_set_dither), the mask settings, the CLUT cache and
 *   the options as they are then (in a frame more than 511 pixels high
 *   the pieces of a clipped polygon are drawn with the drawing area set to
 *   the half of the buffer they belong to, GP0(E3h) and GP0(E4h), and
 *   back to the whole buffer after); a textured record sets the draw mode's
 *   page as a textured primitive does, and the texture window it needs,
 *   which is put back as it was afterwards. Afterwards the drawing area
 *   and offset are the buffer's.
 *   Returns the sticky error (see jb_check), taking it: the first failure
 *   on the context since it was last taken, JB_OK if none; when it is a
 *   failure, jb_error_string gives that failure's detail afterwards (so
 *   the detail and the result returned always match). Errors of its own: jb NULL, no frame begun
 *   (JB_ERROR_INVALID_ARGUMENT, and latched).
 *
 * Capacity: a frame holds jb_config.max_primitives records; a draw that
 * finds it full is dropped (all of it, or what did not fit of a clipped
 * one) and fails with JB_ERROR_OUT_OF_MEMORY, whose detail names
 * jb_config.max_primitives.
 *
 * Layers: jb_set_layer chooses where the 2D draws after it go,
 * JB_LAYER_OVERLAY (in front of the scene, the default, and again at each
 * jb_begin_frame) or JB_LAYER_BACKGROUND (behind it). 3D drawing always
 * goes into the scene. Errors: jb NULL, any other layer.
 */
enum { JB_LAYER_OVERLAY = 0, JB_LAYER_BACKGROUND = 1 };

JB_API jb_result jb_begin_frame(jb_context *jb, uint32_t clear_color);
JB_API jb_result jb_end_frame(jb_context *jb);
JB_API jb_result jb_set_layer(jb_context *jb, int layer);

/*
 * The size frames are drawn at, in 1x pixels: the frame's width and
 * height, for laying out 2D draws (positions are pixels of the frame). In
 * a frame, the size it is drawn at: a widescreen change made during it
 * applies from the next frame, as jb_set_widescreen says. Outside a frame,
 * the size the next jb_begin_frame will draw at, so a widescreen change
 * shows here at once, although the display takes it only at that frame's
 * jb_end_frame. The width is jb_config.width, or the 16:9 width while
 * widescreen is on; the height is jb_config.height. It is not the size
 * jb_frame shows, which is at the context's scale and follows a display
 * mode the program sets (GP1(08h)). width and height may be NULL. Errors:
 * jb NULL (JB_ERROR_INVALID_ARGUMENT, *width and *height 0).
 */
JB_API jb_result jb_frame_size(const jb_context *jb, int *width, int *height);

/* ------------------------------------------------------------------------ */
/*
 * Textures
 *
 * The friendly level's textures: the context finds room in VRAM for a
 * texture's texels and its palette, uploads them and gives back a
 * jb_texture naming where they are, ready for any call that takes one.
 *
 * Where things go (the placer). VRAM holds the display buffers, the
 * textures and their palettes, and nothing is placed over another:
 *   - The display buffers jb_begin_frame draws into are never used, whether
 *     or not the program draws frames: exactly the frame's (The frame),
 *     for the reserved size w x h (w the larger of jb_config.width and,
 *     with jb_config.widescreen, the 16:9 width), two buffers at (0, 0)
 *     and (0, h) when 2h <= 512, else at (0, 0) and (w, 0) when 2w <= 1024,
 *     else one at (0, 0). The rest of VRAM is free for textures.
 *   - Nor are the regions given to jb_vram_reserve.
 *   - A texture's texels lie within one texture page (a 4-bit texture in
 *     64 VRAM pixels from its page_x, 8-bit 128, 15-bit 256) and within
 *     rows 0..255 or 256..511, so that a primitive can reach every texel
 *     from its page: a 4-bit texture w texels wide takes (w + 3) / 4 VRAM
 *     pixels a row, 8-bit (w + 1) / 2, 15-bit w. Its u is then a multiple
 *     of 4 at 4 bits and of 2 at 8 bits.
 *   - A repeat texture's width and height are powers of two, 8 to 256,
 *     and its u and v multiples of them, which the texture window
 *     (jb_set_texture_window) needs to wrap texel coordinates within it:
 *     mask (256 - width) / 8 and offset u / 8 in x, the same in y.
 *   - A palette is one row of 16 (4-bit) or 256 (8-bit) VRAM pixels with x
 *     a multiple of 16, as a CLUT must be.
 * The search is first fit: texels top-down (row 0 downwards, each row
 * left to right), palettes column by column (x 0 rightwards in steps of
 * 16, each column from row 511 upwards). With the display buffers at the
 * left, the texels fill the page columns to their right and the palettes
 * the space below or beside the buffers, before they take the bottom of a
 * page column a 256-row texture would need. When the palette finds no
 * room around the texels' first place (they may sit on its only places),
 * every pair is tried: the palette's places in its order, each with the
 * texels' first place around it, the first pair that fits winning; so a
 * texture is refused only when no texels and palette fit together. All of
 * it depends only on the calls made: the same calls in the same order give
 * the same places on every platform. A texture freed gives its space back
 * to the textures after it.
 *
 * Texels and palettes are written with jb_vram_load's transfers, but with
 * the mask settings (GP0(E6h), jb_set_mask) off, so that each texel and
 * entry is written exactly as given: GP0(E6h) is sent with both off before
 * and with the program's settings after, when either was on (the set bit
 * would add bit 15 to whole VRAM pixels, which at 4 and 8 bits are packed
 * texels, and the check bit would keep old pixels). Texel indices are
 * packed as the GPU reads them: at 4 bits four to a VRAM pixel, the
 * leftmost in the low 4 bits; at 8 bits two, the leftmost in the low byte;
 * a row's last pixel is padded with index 0.
 *
 * Colours in VRAM (palette entries and 15-bit texels) are 15-bit: red in
 * bits 0-4, green 5-9, blue 10-14, and bit 15 the semi-transparency bit.
 * A texel or entry of 0x0000 is never drawn (transparent); one with bit 15
 * set blends when the primitive blends, and is drawn like any other when
 * it does not. So black that should show is 0x8000 (or a near black such
 * as 0x0421 when it must stay opaque under blending).
 *
 * The library's text font is a 4-bit texture placed like any other, at
 * first use. It is an original 8 x 8 bitmap font made for jitterbug, not
 * taken from any other font, under the library's licence.
 *
 * None of these calls allocates but jb_texture_load, which reads its file
 * into memory for the call: a context has room for 1024 textures (the
 * font among them, once used) from jb_create. Textures can also come from
 * .jbt files (below, after jb_palette_from_rgba).
 */

/*
 * A texture to make. texels holds width x height texels, rows top to
 * bottom with no padding: one byte a texel at 4 bits (palette indices
 * 0..15) and 8 bits (0..255), and one uint16_t at 15 bits (the 15-bit
 * colour itself); NULL makes every texel 0: at 15 bits the transparent
 * 0x0000, at 4 and 8 bits index 0, which shows palette entry 0 (transparent
 * only when that entry is 0x0000, as with a NULL palette). palette holds the
 * 16 (4-bit) or 256 (8-bit) palette entries, or is NULL for all 0x0000 (set
 * it later with jb_palette_write); 15-bit textures have none, and it is
 * ignored. repeat asks for a texture that repeats under the texture window
 * (see above); its width and height must then be powers of two, 8..256.
 * From an RGBA image (a decoded PNG): jb_palette_from_rgba turns its
 * pixels into 15-bit texels, or a palette's RGBA into entries.
 */
typedef struct jb_texture_desc {
    int width, height;        /* texels, 1..256 each */
    int depth;                /* JB_TEX_4BIT, JB_TEX_8BIT or JB_TEX_15BIT */
    const void *texels;       /* uint8_t or uint16_t, as above; NULL for 0 */
    const uint16_t *palette;  /* 16 or 256 entries; NULL for 0x0000 */
    int repeat;               /* not 0: placed so that it can repeat */
} jb_texture_desc;

/*
 * Places a texture in VRAM (see Where things go), writes its texels and
 * palette there, and fills *out: page_x, page_y, depth, clut_x and clut_y
 * (0 at 15 bits) as the console-level calls name it, u, v, width and
 * height where it lies in its page, repeat (1 or 0), id, the count of
 * textures the context has made (1 for the first, so the same calls give
 * the same ids), and owner, the context (jb_texture_destroy). Primitives
 * drawn with it from then on show it; a primitive's u from out->u to
 * out->u + width - 1 (and v likewise) covers it. A 4- or 8-bit texture is
 * followed by GP0(01h), clear cache, so that its palette is drawn even
 * where the CLUT cache held one read from the same pixels before (a
 * destroyed texture's, say).
 *
 * Errors, each with nothing placed or written and *out unchanged:
 * JB_ERROR_INVALID_ARGUMENT for jb, desc or out NULL, a size outside
 * 1..256, a depth not 4, 8 or 15, a repeat texture whose sizes are not
 * powers of two 8..256, or a 4-bit texel above 15 (the detail names its
 * position); JB_ERROR_OUT_OF_MEMORY when there is no room in VRAM for
 * the texels or the palette ("no room in VRAM for a 64 x 64 8-bit
 * texture"), when the context already holds 1024 textures, or when it has
 * made 4294967295 (every id there is).
 */
JB_API jb_result jb_texture_create(jb_context *jb, const jb_texture_desc *desc, jb_texture *out);

/*
 * Frees what jb_texture_create placed for texture, for later textures; the
 * pixels in VRAM stay as they are until something is written over them.
 * texture must be equal, field for field, to one jb_texture_create returned
 * on this context that has not been destroyed (a copy is fine). id and
 * owner tell them apart: the id is new for every texture the context
 * makes, so a copy of a destroyed texture's handle does not name a texture
 * made later in the same place; owner is the context's address, so another
 * live context's texture is never taken for one of this context's. A
 * handle names a texture only while its context lives: a context made
 * after jb_destroy at the same address, with the same calls, would take
 * the old context's handles for its own. (Ids are the same from run to
 * run; owner is not.)
 * Errors: JB_ERROR_INVALID_ARGUMENT for jb or texture NULL, or a texture
 * that is not such a one (made by hand, destroyed already, from another
 * context, or the library's font).
 */
JB_API jb_result jb_texture_destroy(jb_context *jb, const jb_texture *texture);

/*
 * Writes count palette entries of texture (4- or 8-bit, one
 * jb_texture_create returned, as jb_texture_destroy takes it) from entry
 * first (with the mask settings off, as jb_texture_create writes them),
 * and then sends GP0(01h), clear
 * cache. The clear is needed for the new colours to show: the GPU keeps the
 * palette it last drew with in its CLUT cache, and a primitive with the
 * same CLUT would go on drawing with the cached, old colours (see
 * jb_vram_load). first 0..entries-1 and count 0..entries-first, entries
 * 16 or 256; count 0 does nothing.
 * Errors: JB_ERROR_INVALID_ARGUMENT for jb or texture NULL, colors NULL
 * with count above 0, a texture that is not one of the context's (or the
 * library's font), a 15-bit one, or first or count out of range.
 */
JB_API jb_result jb_palette_write(jb_context *jb, const jb_texture *texture, int first, int count,
                                  const uint16_t *colors);

/*
 * Writes the w x h texels at (x, y) within texture (one jb_texture_create
 * returned, as jb_texture_destroy takes it), in jb_texture_desc's texel
 * format, rows top to bottom with no padding; x 0..width-1, w
 * 1..width-x, and y, h likewise. Texels that share a VRAM pixel with the
 * rectangle but lie outside it keep their values (the pixel is read back
 * first). Written with the mask settings off, as jb_texture_create writes
 * them.
 * Errors: JB_ERROR_INVALID_ARGUMENT for jb, texture or texels NULL, a
 * texture that is not one of the context's (or the library's font), the
 * rectangle out of range, or a 4-bit texel above 15.
 */
JB_API jb_result jb_texture_write(jb_context *jb, const jb_texture *texture, int x, int y, int w,
                                  int h, const void *texels);

/*
 * Keeps the placer out of the rectangle at (x, y), w x h VRAM pixels, for
 * the rest of the context's life: for VRAM the program manages itself
 * (its own textures loaded with jb_vram_load, a render target). x
 * 0..1023, y 0..511, w 1..1024-x, h 1..512-y (it does not wrap). It may
 * overlap the display buffers and other reserved regions. It writes no
 * pixel.
 * Errors: JB_ERROR_INVALID_ARGUMENT for jb NULL, an argument out of range,
 * or a rectangle that overlaps a texture's texels or palette already
 * placed (the detail names it); nothing is reserved then.
 */
JB_API jb_result jb_vram_reserve(jb_context *jb, int x, int y, int w, int h);

/*
 * 15-bit colours from count RGBA colours (4 bytes each, R, G, B, A, 0..255),
 * for a palette or for 15-bit texels; rgba and out may not overlap. Each
 * channel keeps its top 5 bits (c >> 3), as a fill does. Alpha:
 *   0         0x0000, transparent (whatever the colour)
 *   1..127    the colour with bit 15: it blends when the primitive blends
 *   128..255  the colour without bit 15: opaque; but a colour that is 0 in
 *             15 bits (black) becomes 0x8000, since 0x0000 would be
 *             transparent (0x8000 blends when the primitive blends)
 * Needs no context. Errors: JB_ERROR_INVALID_ARGUMENT for rgba or out NULL
 * with count above 0.
 */
JB_API jb_result jb_palette_from_rgba(const uint8_t *rgba, size_t count, uint16_t *out);

/*
 * Texture files: .jbt, as tools/jb-tex writes them from a PNG, one texture
 * as jb_texture_desc takes it (its size, depth, texels and repeat flag)
 * with one or more palettes at 4 and 8 bits; docs/formats.md gives the
 * format byte for byte. The loaders read untrusted input: a file that is
 * not exactly a valid .jbt (truncated, longer than its header says, sizes
 * or offsets that do not fit the file, overlapping sections, a field out
 * of range, a 4-bit texel above 15, a version other than 1) is refused
 * with JB_ERROR_INVALID_ARGUMENT and a detail (jb_error_string), with
 * nothing placed or written and *out (or *desc) unchanged.
 *
 * jb_texture_parse: checks the size bytes at data as a .jbt and fills
 *   *desc for jb_texture_create, its texels and palette pointing into data
 *   (which must then stay as it is while desc is used), and *palettes
 *   (palettes not NULL) with the file's count of palettes: 0 at 15 bits,
 *   else 1 or more, one after another, so palette k (k from 0) is the
 *   entries (16 or 256) from desc->palette + k * entries; desc->palette is
 *   the first. The other palettes are for jb_palette_write, as frames of
 *   palette animation. It places nothing and needs no memory. data must be
 *   aligned to 2 bytes (as memory from malloc is), since desc points into
 *   it as uint16_t; jb_texture_load_mem takes data at any address.
 *   Errors: as above, and jb NULL, desc NULL, data NULL or not aligned to
 *   2 bytes.
 * jb_texture_load_mem: the same check, then jb_texture_create with the
 *   file's texels, its first palette and its repeat flag: the same places,
 *   VRAM, *out and errors as jb_texture_create would give for the desc that
 *   jb_texture_parse fills (its errors named jb_texture_load_mem). It
 *   allocates nothing. Errors: as above and as jb_texture_create's, and
 *   out NULL.
 * jb_texture_load: jb_texture_load_mem on the file at path, read whole
 *   into memory from the context's allocator first (freed before it
 *   returns). Errors: as jb_texture_load_mem's (named jb_texture_load); a
 *   path NULL, or a file that cannot be opened or read or is larger than
 *   64 MB, is JB_ERROR_INVALID_ARGUMENT, and no memory to read it into
 *   JB_ERROR_OUT_OF_MEMORY.
 */
JB_API jb_result jb_texture_parse(jb_context *jb, const void *data, size_t size,
                                  jb_texture_desc *desc, int *palettes);
JB_API jb_result jb_texture_load_mem(jb_context *jb, const void *data, size_t size,
                                     jb_texture *out);
JB_API jb_result jb_texture_load(jb_context *jb, const char *path, jb_texture *out);

/* ------------------------------------------------------------------------ */
/*
 * 2D drawing
 *
 * The structs of Drawing, recorded into the frame's current layer and
 * drawn at jb_end_frame, with their meaning there: so a quad's vertices go
 * in the console's Z (jb_poly: top left, top right, bottom left, bottom
 * right), not round it as a mesh face's do (jb_mesh_face); a textured
 * one's u and v are texels of the page, as the console's are, so for a
 * texture from jb_texture_create add its u and v (jb_sprite counts from
 * the texture's corner itself); and colours are the console's, so 0 is
 * black, and JB_NEUTRAL (or raw) shows texels as they are. Positions are
 * pixels of the frame, (0, 0) its top-left, and any int: what the console
 * would refuse or not draw is clipped instead.
 *   - A primitive within the console's limits (positions -1024..1023, a
 *     polygon's triangles and a line no more than 1023 x 511 pixels across)
 *     is recorded as it is, and draws exactly the pixels the jb_prim_*
 *     call would draw in the buffer, at any scale and thread count, with
 *     three exceptions, all from the friendly level's vertices being its
 *     own rather than matched with the GTE's projections
 *     (jb_config.perspective_correct): precise vertices never move it; it
 *     is never drawn perspective-correctly, as 2D vertices have no depth,
 *     even where the jb_prim_* call at the same positions finds GTE
 *     depths for them; and a textured one draws with its texture's window
 *     (below), not the window the console level set
 *     (jb_set_texture_window), which jb_end_frame puts back afterwards.
 *   - A polygon that can draw no pixel of the frame (judged exactly, by
 *     its triangles, at 1x and in the scaled picture), or that has no
 *     area, is dropped, with JB_OK, and takes no record; so is a line
 *     whose pixels' bounds miss the frame (its ends' pixels at 1x, a pixel
 *     beyond its ends' positions in the scaled picture: a diagonal line
 *     whose bounds meet the frame, though none of its pixels does, still
 *     takes one).
 *   - One beyond the limits is clipped and cut into pieces the console
 *     draws. A rectangle is clipped to the frame exactly: the pixels drawn
 *     and their texels are those of the whole rectangle. A line is
 *     clipped to the frame (and a few pixels around it), and is always one
 *     piece. A polygon (a quad as its two triangles, v0 v1 v2 and v1 v2
 *     v3, unless both fit) is clipped to a rectangle around the frame as
 *     large as the console's limits allow (1023 x 511, the frame in its
 *     middle; around each half of a frame more than 511 pixels high, each
 *     half then drawn with the drawing area set to it), and drawn as
 *     triangles of what is left: new vertices lie only on that
 *     rectangle's edges, outside the frame where there is room. An edge
 *     within the console's limits is drawn whole, exactly as the console
 *     draws it, even where it leaves the rectangle. Colours and texture
 *     coordinates are interpolated linearly in screen space at the new
 *     vertices, so a clipped polygon looks as the console would draw it
 *     up to rounding: at 1x an edge beyond the limits can move by up to
 *     half a pixel near where it leaves the rectangle, and colours and
 *     texels step a little differently; texture coordinates are whole
 *     texels, so a texture stretched over many pixels shows its rounding
 *     (by up to half a texel). (Where depths are known, as for 3D
 *     drawing, new vertices also carry their attributes interpolated
 *     perspective-correctly, which the scaled picture uses when
 *     perspective_correct is on at jb_end_frame; the 1x VRAM above 1x
 *     always gets the linear ones.) No pixel of one primitive is drawn
 *     twice, unless its own triangles overlap: a crossed or concave quad
 *     whose two triangles overlap draws twice where they do, as
 *     jb_prim_poly draws it. Pieces leave no gaps, also where separate
 *     draws share an edge exactly (one of them cut, the other not, or
 *     both): an edge within the limits is drawn whole by both, and one
 *     beyond them is cut at the same points by both. Where a polygon is
 *     less than about a pixel wide where it leaves the rectangle, rounding
 *     can leave some of its pixels out, or draw a few of its neighbour's.
 *     In a frame 1024 pixels wide no polygon reaches the last column: the
 *     console's positions end at 1023 and its fill rule leaves a polygon's
 *     right edge undrawn (lines and rectangles reach it).
 * Everything else is as jb_prim_* says (blend modes, textures, raw,
 * dithering), and so are the errors, without the range rule for positions:
 * JB_ERROR_INVALID_ARGUMENT for jb or the struct NULL, no frame begun, a
 * value no primitive can have (vertices not 3 or 4, a blend mode, a
 * texture field, a colour above 0xFFFFFF, a texel coordinate outside
 * 0..255, a rectangle's w or h below 0, a polyline's v NULL or count below
 * 2), with nothing recorded; JB_ERROR_OUT_OF_MEMORY with the frame full.
 * Each call copies the struct and its texture. The texture window: a
 * textured primitive is drawn with no texture window, whatever the
 * console level set, except with a repeat texture (jb_texture_create):
 * then with the window that tiles it (Textures), so that texel
 * coordinates anywhere in 0..255 repeat the texture, the tile from the
 * page's (0, 0). A rectangle's flips apply only when it is textured.
 */
JB_API jb_result jb_draw_poly(jb_context *jb, const jb_poly *poly);
JB_API jb_result jb_draw_line(jb_context *jb, const jb_line *line);
JB_API jb_result jb_draw_polyline(jb_context *jb, const jb_polyline *line);
JB_API jb_result jb_draw_rect(jb_context *jb, const jb_rect *rect);

/* ------------------------------------------------------------------------ */
/*
 * Sprites and text
 *
 * 2D draws of textures and of the library's font, recorded into the
 * frame's current layer (jb_set_layer) as the 2D drawing calls are, with
 * their positions in pixels of the frame (any int), clipped the same way,
 * and drawn at jb_end_frame. Errors outside a frame and the frame being
 * full are as for 2D drawing, and every failure is latched (jb_check).
 */

/*
 * A sprite: the source, src_w x src_h texels at (u, v) within texture,
 * shown w x h pixels at (x, y) of the frame.
 *   - Sizes: src_w, src_h 0 are the rest of the texture from (u, v) (as
 *     jb_billboard's tw, th), and w, h 0 the source's size, 1:1. So
 *     {texture, x, y} draws the whole texture 1:1; w, h alone scale it;
 *     src_w, src_h alone pick a frame of a sheet 1:1; all four scale that
 *     frame. A size of 0 is never empty: a sprite shrinking to nothing
 *     must not be drawn at 0.
 *   - The source lies within the texture: u 0..width-1, src_w
 *     0..width-u, and v, src_h likewise. For a repeat texture it may run on
 *     past the texture's edge to the page's last texel, src_w up to 256 -
 *     u and src_h up to 256 - v: the texture repeats.
 *   - The texture's own u, v are 0..255 and its width and height 0..256,
 *     the rectangle within its page; a texture made by hand with width or
 *     height 0 is taken as its page from its u, v. A repeat texture, made
 *     by hand too, needs power-of-two sizes 8..256 with u and v multiples of
 *     them, as jb_texture_create makes it (the texture window can wrap no
 *     other).
 *   - tint is the primitive colour (JB_RGB), 0 for JB_NEUTRAL, the texels
 *     as they are, and JB_TINT_BLACK for black; raw ignores it. blend is
 *     JB_BLEND_*: texels with bit 15 blend, as on the console (opaque ones
 *     never do: jb_palette_from_rgba gives bit 15 to alpha 1..127). flip_x, flip_y (any value but 0) mirror it.
 * 1:1 it is drawn as a textured rectangle: exactly what jb_prim_rect draws
 * with the same texture, colour, blend mode, raw and flips at (x, y), w x
 * h, u = texture->u + u and v = texture->v + v (with flip_x, u + src_w - 2,
 * and with flip_y, v + src_h - 1: a mirrored rectangle starts from texel
 * u + 1 in x and from v in y, so the sprite shows the same texels
 * mirrored); not dithered.
 * Scaled, it is a textured quad from (x, y) to (x + w, y + h), its corners
 * carrying the source's first and last texels (u and u + src_w - 1, and
 * v likewise; swapped when flipped), so that no pixel takes a texel past
 * the source; the GPU steps between them as it does over any quad, with
 * its uneven steps, and dithers it when it is modulated and dithering is
 * on.
 * Errors (JB_ERROR_INVALID_ARGUMENT, nothing recorded): jb, sprite or
 * texture NULL, no frame begun, a texture field out of range, a
 * texture whose u, v and size leave its page, a repeat texture the window
 * cannot wrap (above), u or v outside the texture,
 * a source size outside its range, w or h negative, tint above 0xFFFFFF
 * but JB_TINT_BLACK (unless raw), a blend mode not JB_BLEND_*. The frame full:
 * JB_ERROR_OUT_OF_MEMORY.
 */
typedef struct jb_sprite {
    const jb_texture *texture; /* copied when recorded */
    int x, y;                  /* the top-left pixel, any int */
    int w, h;                  /* pixels; 0 the source's size */
    int u, v;                  /* the source's top-left texel within the texture */
    uint32_t tint;             /* JB_RGB; 0 for JB_NEUTRAL; or JB_TINT_BLACK */
    int blend;                 /* JB_BLEND_* */
    int flip_x, flip_y;        /* mirrored, any value but 0 */
    int raw;                   /* texels as they are, without tint */
    int src_w, src_h;          /* texels shown; 0 the rest of the texture */
} jb_sprite;

JB_API jb_result jb_draw_sprite(jb_context *jb, const jb_sprite *sprite);

/*
 * Text in the library's font (Textures: an original 8 x 8 bitmap font),
 * its first character's cell's top-left at (x, y). Each character takes an
 * 8 x 8 cell and moves on 8 pixels; the glyphs leave the cell's right
 * column empty, so characters never touch, but lines are 8 pixels apart
 * and the cell's rows are all used (capitals from the top row, descenders
 * to the bottom one), so a descender can touch a capital on the line
 * below. '\n' starts a new line at x, 8 pixels down. A space draws
 * nothing. Every other byte outside 32..126 (other control characters,
 * and bytes 127-255, so UTF-8 beyond ASCII) is drawn as '?'.
 * color (JB_RGB) is the colour the text shows: each channel's top 5 bits,
 * as a fill of that colour gives (a glyph's texels are 16 in each 5-bit
 * channel, drawn modulated by the colour: 16 x c / 128 is c >> 3), on a
 * cell that is otherwise transparent. Each character that shows is one 8 x 8 textured
 * rectangle,
 * not dithered, clipped exactly at the frame's edges.
 * jb_draw_text draws it opaque. jb_draw_text_blended blends it with blend
 * (JB_BLEND_*: JB_BLEND_NONE is jb_draw_text), the glyphs' texels having
 * the semi-transparency bit (the font's second palette).
 * A character shows when a set texel of its glyph lands in the frame; one
 * that does not (a space, a cell off the frame, or one whose part in the
 * frame is blank) records nothing. The first character a context draws
 * that shows places the font in VRAM for the rest of its life: a 128 x 48 4-bit texture and two 16-entry palettes (side by side
 * when they fit, else apart), written at once, outside the display
 * buffers. Text that shows nothing places nothing.
 * Errors: JB_ERROR_INVALID_ARGUMENT for jb or text NULL, no frame begun,
 * color above 0xFFFFFF, a blend mode not JB_BLEND_*, with nothing
 * recorded; JB_ERROR_OUT_OF_MEMORY, with nothing recorded and the font not
 * placed, when the font finds no room in VRAM, or no place in the table of
 * textures (jb_texture_create's limits: 1024 textures held at once, and
 * the count of ids), or when the frame is full before its first character
 * shows; and JB_ERROR_OUT_OF_MEMORY when the frame fills later (the
 * characters that did not fit are dropped).
 */
JB_API jb_result jb_draw_text(jb_context *jb, int x, int y, uint32_t color, const char *text);
JB_API jb_result jb_draw_text_blended(jb_context *jb, int x, int y, uint32_t color, int blend,
                                      const char *text);

/*
 * The size text takes when drawn (above), in pixels: width 8 x the longest
 * line's characters, height 8 x the lines (one more than the '\n's; a text
 * ending in '\n' has an empty last line), 0 x 0 for "". Needs no context.
 * width and height may be NULL. JB_ERROR_INVALID_ARGUMENT, with both sizes
 * 0, for text NULL, or a text whose width or height (asked for or not)
 * would pass the largest int.
 */
JB_API jb_result jb_text_size(const char *text, int *width, int *height);

/* ------------------------------------------------------------------------ */
/*
 * Scene geometry
 *
 * The friendly level's 3D maths and its scene state: float vectors and
 * matrices, a camera, lights and fog, and the mesh a frame draws.
 *
 * The world is right-handed with y up (glTF's convention), and in view
 * space the camera looks down -z with y up; the library turns that into
 * the console's screen, y down. Distances are in world units, the units of
 * a model's positions after its model matrix.
 *
 * Determinism: the float maths here, and the float geometry path, give the
 * same bits on every machine and compiler the library is checked on. They
 * use only + - x / and sqrtf (each correctly rounded in IEEE arithmetic),
 * the library is built with no contraction into fused multiply-adds
 * (-ffp-contract=off) and refuses to build where float expressions are
 * evaluated at a wider precision (FLT_EVAL_METHOD not 0) or float and
 * double are not IEEE binary32 and binary64, and nothing calls the C
 * library's sin, cos or tan, whose results differ between platforms. The
 * guarantee assumes the floating-point environment a C program starts
 * with: rounding to nearest, ties to even, and subnormal numbers kept (not
 * flushed to zero). A program that changes the rounding mode (fesetround)
 * or turns on flush-to-zero while it calls the library gets results that
 * may differ in the last bit. It covers the results the calls document;
 * the one NaN they return is the canonical one (below).
 */

/* A point or direction. */
typedef struct jb_vec3 {
    float x, y, z;
} jb_vec3;

/*
 * A 4 x 4 matrix, column-major as in OpenGL and glTF: element (row r,
 * column c) is m[c * 4 + r]. Points are column vectors, p' = M p, so the
 * translation is m[12], m[13], m[14], and jb_mat4_mul(a, b) applies b
 * first, then a.
 */
typedef struct jb_mat4 {
    float m[16];
} jb_mat4;

/*
 * jb_sinf, jb_cosf, jb_tanf: of x in radians, within one unit in the last
 * place (nearly always the correctly rounded float) for every finite x: x
 * is reduced by multiples of pi/2 (with a three-part constant below 2^23,
 * and exactly from the bits of 2/pi above), then a polynomial is evaluated
 * in double precision. Infinite or NaN x gives NaN, always the same one
 * (bits 0x7FC00000: quiet, positive, payload 0), whatever the hardware
 * would make.
 */
JB_API float jb_sinf(float x);
JB_API float jb_cosf(float x);
JB_API float jb_tanf(float x);

/*
 * jb_vec3_dot, jb_vec3_cross: the dot and cross products (right-handed:
 * x cross y is z). jb_vec3_normalize: v scaled to length 1 (divided by
 * sqrtf of the dot product; first by its largest component when the dot
 * product would overflow or underflow, so very long and very short
 * vectors work too); a zero vector, or one with an infinite or NaN
 * component, comes back as it was.
 */
JB_API float jb_vec3_dot(jb_vec3 a, jb_vec3 b);
JB_API jb_vec3 jb_vec3_cross(jb_vec3 a, jb_vec3 b);
JB_API jb_vec3 jb_vec3_normalize(jb_vec3 v);

/*
 * Matrices. Angles are in radians; a positive one turns anticlockwise
 * looking from the positive axis towards the origin (the right-hand rule).
 *
 * jb_mat4_identity, jb_mat4_translate, jb_mat4_scale, jb_mat4_rotate_x,
 * jb_mat4_rotate_y, jb_mat4_rotate_z: those transforms.
 * jb_mat4_mul: a x b; NULL stands for the identity.
 * jb_mat4_trs: translate(t) x rotate_z(r.z) x rotate_y(r.y) x rotate_x(r.x)
 *   x scale(s): scaled, turned about x, then y, then z, then moved by t.
 * jb_mat4_look_at: the view matrix of a camera at eye looking at target
 *   with up as its up direction: world to view space, where the camera is
 *   at the origin looking down -z, y up. eye equal to target, a
 *   difference target - eye that overflows, or up parallel to the view
 *   direction (judged in double from the values given, so exactly parallel
 *   directions are found so: an angle below about 1e-6 radians), gives the
 *   identity.
 * jb_mat4_transform_point: m x (p, 1), the w row ignored. m NULL is the
 *   identity.
 */
JB_API jb_mat4 jb_mat4_identity(void);
JB_API jb_mat4 jb_mat4_translate(float x, float y, float z);
JB_API jb_mat4 jb_mat4_scale(float x, float y, float z);
JB_API jb_mat4 jb_mat4_rotate_x(float radians);
JB_API jb_mat4 jb_mat4_rotate_y(float radians);
JB_API jb_mat4 jb_mat4_rotate_z(float radians);
JB_API jb_mat4 jb_mat4_mul(const jb_mat4 *a, const jb_mat4 *b);
JB_API jb_mat4 jb_mat4_trs(jb_vec3 t, jb_vec3 r, jb_vec3 s);
JB_API jb_mat4 jb_mat4_look_at(jb_vec3 eye, jb_vec3 target, jb_vec3 up);
JB_API jb_vec3 jb_mat4_transform_point(const jb_mat4 *m, jb_vec3 p);

/*
 * The camera: where it is (eye), what it looks at (target; with eye, both
 * all zero is the default view, from the origin down -z) and which way
 * is up (up; all zero is +y), its vertical field of view in degrees (fov_y,
 * more than 0 and less than 180; 0 is 60), and the distances of its near
 * and far planes in world units (z_near and z_far, 0 < z_near < z_far; 0
 * for either is its default, 32 and 4096). Nothing nearer than z_near or
 * farther than z_far is drawn; faces that cross those planes are cut at
 * them (at the planes' exact depths, not rounded). projection, when not 0,
 * is the distance of the projection plane in pixels (the GTE's H, as
 * console code sets it: 256 is a common choice for a 240-line display) and
 * takes the place of fov_y. The horizontal extent follows from the
 * display's size, with square pixels. So a camera all zero is the default
 * one, and {.projection = 256} that camera with H 256. The defaults suit
 * the console's own units, objects tens to hundreds of units across (a
 * mesh's int16 positions, as first-light's 256-unit cube); a scene in
 * metres needs a model matrix that scales it up, or z_near and z_far of
 * its own. Settings that apply
 * from the next 3D draw on.
 */
typedef struct jb_camera {
    jb_vec3 eye, target, up;
    float fov_y;
    float z_near, z_far;
    float projection;
} jb_camera;

/*
 * Sets the camera (NULL: the default, which is also a new context's: at
 * the origin looking down -z, y up, 60 degrees, near 32, far 4096).
 * Errors: jb NULL; a value that is not finite; fov_y outside 0..180 (or
 * 180 itself); projection negative; z_near negative, z_far negative, or
 * z_far not more than z_near once the defaults are applied; eye equal to
 * target (but for both all zero, the default view), or so far from it
 * that the difference is not finite; up
 * parallel to the view direction (judged from the values given; any
 * length of up works); a view matrix that is not finite in float (an eye
 * too far from the origin). Nothing changes on an error. A field of view
 * so narrow that its tangent is 0 in float (below about 1e-37 degrees) is
 * refused by the 3D draws, as is, in the GTE path, one whose H is outside
 * 1..65535.
 *
 * In the GTE geometry path (jb_set_geometry) depths are the GTE's own: the
 * camera's view space is scaled by a power of two, the largest that puts
 * z_far at 32767 GTE units or less (4 for the default 4096), and since
 * the GTE's divide overflows at a depth of H / 2 or less (H, the distance
 * of the projection plane in pixels, is 208 for a 240-line display at 60
 * degrees), the near plane there is at depth H / 2 + 1 when z_near would
 * be nearer. For the default camera that is 105 GTE units, 26.25 world
 * units, nearer than its z_near of 32, so z_near holds; a z_near of 10
 * would draw from 26.25 on. The GTE path draws while its near plane is
 * nearer than the far one by any amount (planes within one integer GTE
 * depth draw at that depth); a z_far at H / 2 + 1 GTE units or nearer
 * leaves it none, and its 3D draws are refused. The float path clips at
 * z_near as given, but whenever the GTE path can draw, both paths sort
 * over the same depths (from the GTE path's near plane to the far one,
 * however close), so switching paths within a frame keeps the order.
 */
JB_API jb_result jb_set_camera(jb_context *jb, const jb_camera *camera);

/*
 * Lights for 3D drawing: an ambient light and up to three directional
 * ones, in world space, kept by the context until changed. The lit
 * colour of a vertex is its material colour times the light reaching it:
 * the ambient plus, for each light, its colour times the cosine of the
 * angle between the vertex normal and the direction the light comes from
 * (none from behind). Light colours are JB_RGB values in which 255 is a
 * level of 1.0, and a level of 1.0 leaves a colour unchanged, so a
 * material colour of JB_RGB(128, 128, 128) lit by a white light straight
 * on, with no ambient, draws its texels unchanged; brighter light (up to
 * the console's 2x, more saturating) brightens them. A new context has
 * ambient JB_RGB(255, 255, 255) and no light on, so lit meshes show their
 * colours unchanged until lights are set; lower the ambient when turning
 * lights on, or they add to its full level.
 *
 * jb_set_ambient: the ambient light's colour. Errors: jb NULL, color above
 *   0xFFFFFF.
 * jb_set_light: light index (0, 1 or 2): direction is the way its light
 *   travels in world space (so a light with direction (0, -1, 0) shines
 *   down from above; jb_gte_lamp takes the same convention), any length
 *   but 0. intensity scales its colour: 0 is 1.0, and up to 7.99 (the
 *   GTE's largest light level for a channel of 255), so a light can be
 *   brighter than white, as console lights often were (1.6 times full with
 *   a material of JB_RGB(128, 128, 128) shows texels 1.6 times as bright).
 *   light NULL, or its color 0, turns the light off (so a light fades out
 *   by its colour: an intensity falling to 0 is 1.0 there). Errors: jb
 *   NULL; index
 *   outside 0..2; color above 0xFFFFFF; for a light that is on, a direction
 *   that is 0 or not finite, or intensity not finite or outside 0..7.99.
 *
 * The GTE path lights with the GTE's NC* commands and its own light matrix
 * for each object (jb_gte_set_light_directions); an object's model matrix
 * is taken as a rotation and a scale, and its normals are turned by the
 * rotation alone, so a non-uniform scale does not bend them.
 */
typedef struct jb_light {
    jb_vec3 direction;
    uint32_t color;
    float intensity;
} jb_light;

JB_API jb_result jb_set_ambient(jb_context *jb, uint32_t color);
JB_API jb_result jb_set_light(jb_context *jb, int index, const jb_light *light);

/*
 * Fog, the console's depth cue: from distance start in front of the camera
 * (world units, along its view direction) the colour of 3D draws moves
 * towards color (JB_RGB), and it is color from distance end on. The amount
 * is linear in 1 / depth, as the GTE's depth cue is: (1/start - 1/z) /
 * (1/start - 1/end). end 0 turns fog off (the default), whatever start and
 * color. Errors: jb NULL; color above 0xFFFFFF; for fog on, start and end
 * not finite or not 0 < start < end. In the GTE path the depth cue's
 * registers limit how sharp fog can be. With zn and zf the start and end
 * in GTE units (world units times the camera's scale) and H the
 * projection distance the GTE is given: DQA = -256 zn zf / ((zf - zn) H)
 * must round into 16 bits, so zn zf / (zf - zn) at most about 128 H, and
 * DQB = 2^24 zf / (zf - zn) into 32 bits, so zf / (zf - zn) below 128 (end
 * at least about 1.008 x start). Fog beyond either is refused, with a
 * detail naming DQA and DQB, by the 3D draws when they make the registers.
 * For the default camera (scale 4, H 208), fog ending at the far plane,
 * 4096, can start up to about 2500.
 */
JB_API jb_result jb_set_fog(jb_context *jb, uint32_t color, float start, float end);

/*
 * The 3D geometry path, for every 3D draw from the next on:
 *   JB_GEOMETRY_GTE (the default): the model and view matrices are
 *     multiplied in float and turned into the GTE's registers (a 4.12
 *     rotation with the scale folded in, a 32-bit translation, H from the
 *     field of view and the display height, the screen centre, the
 *     ordering-table scale, the light matrix in the object's space, the
 *     depth cue), and every vertex goes through the GTE's own arithmetic
 *     (RTPT and RTPS, NCLIP, AVSZ3 and AVSZ4, the NC* lighting), on a GTE
 *     inside the context kept for this alone: the registers jb_gte_* use
 *     are never touched. A face that needs no clipping gives exactly the
 *     screen positions, colours and depth order that console code would
 *     get from those registers. The console's look, its jitter and its
 *     limits included.
 *   JB_GEOMETRY_FLOAT: the same matrices, with projection, lighting and
 *     depth in float, rounded to the same screen formats at the end.
 * Errors: jb NULL; mode not one of the two.
 * jb_set_geometry is declared with the other options, under Contexts.
 */

/*
 * A material, for a mesh's faces.
 *   texture: NULL for an untextured face; otherwise its texture, copied
 *     when the face is drawn, with u and v (each vertex's) within it.
 *   color: JB_RGB, the colour a face's vertices have without vertex
 *     colours (0 means JB_NEUTRAL, JB_RGB(128, 128, 128), which leaves
 *     texels as they are; JB_TINT_BLACK is black). Any other value above
 *     0xFFFFFF is refused by jb_draw_mesh.
 *   blend: JB_BLEND_*.
 *   unlit: not 0 to take the colour as it is, without the lights.
 *   raw: textured faces only: texels not multiplied by the colour.
 *   double_sided: not 0 to draw a face seen from behind too; otherwise it
 *     is culled.
 */
typedef struct jb_material {
    const jb_texture *texture;
    uint32_t color;
    int blend;
    int unlit;
    int raw;
    int double_sided;
} jb_material;

/* A face: 3 or 4 (vertices) indices into the mesh's vertices, going round
   the face anticlockwise as seen from its front (outside), as mesh files
   keep them, and its material (an index into the mesh's materials). A
   quad should be flat and convex; it becomes the console's quad (its
   vertices in the order v[0] v[1] v[3] v[2]), so the GPU draws it as the
   triangles v[0] v[1] v[3] and v[1] v[3] v[2]. (A 2D quad, jb_poly, is
   given in that Z order itself, not round the face.) */
typedef struct jb_mesh_face {
    uint16_t v[4];
    uint16_t vertices;
    uint16_t material;
} jb_mesh_face;

/*
 * A mesh in memory, as jb_draw_mesh (3D drawing) takes it. Per vertex, each
 * array vertex_count long (1..65536): positions in the console's model
 * space (int16, the model matrix places them in the world); optionally
 * normals (4.12, length 4096; NULL to light each face by its own flat
 * normal, from its edges or, for a quad, its diagonals, their cross
 * product exact however thin the face), colours (JB_RGB,
 * used in place of the material colour; NULL for none) and texel
 * coordinates (uv: u then v, 0..255 each, texels of the material's
 * texture, within its width and height but for a repeat texture's, which
 * may run on to the page's 255 and repeat it; NULL for 0, 0). Then
 * face_count faces (0 or more) and, when there are faces, material_count
 * materials (1..65536). The mesh is read
 * when it is drawn and may change between draws. In the GTE path a mesh's
 * coordinates are multiplied by a power of two before the GTE when the
 * model and view matrices (in GTE units) are too large for the GTE's 4.12
 * rotation, whose elements stay below 8, and the rotation is divided by it; a
 * mesh whose largest coordinate times that would pass 32767 is refused by
 * jb_draw_mesh, with a detail, as is a face whose u, v lie outside its
 * material's texture.
 */
typedef struct jb_mesh {
    int vertex_count;
    const jb_gte_vec *positions;
    const jb_gte_vec *normals;
    const uint32_t *colors;
    const uint8_t *uv;
    int face_count;
    const jb_mesh_face *faces;
    int material_count;
    const jb_material *materials;
} jb_mesh;

/* ------------------------------------------------------------------------ */
/*
 * 3D drawing
 *
 * Meshes, billboards and lines in the world, with the scene's camera,
 * lights and fog as they are at the call (Scene geometry), recorded into
 * the frame's scene and drawn at jb_end_frame from its farthest bucket to
 * its nearest (The frame), each bucket in the order of the calls. The
 * scene has jb_config.ot_size buckets over the camera's near to far
 * distance; a face, billboard or line goes in the bucket of the average of
 * its vertices' depths (a billboard's centre's), each a whole number of
 * the GTE's depth units rounded down (SZ on the GTE path, the same
 * rounding of the unrounded depth on the float path), one scale for all of
 * them in both paths: a deeper average never sorts nearer, also where the
 * path changes within a frame, and equal ones share a bucket. A draw's
 * depth bias is added on the float path to each depth before it is
 * rounded down (so a bias of any size orders the draw as if it were that
 * much nearer or farther; a biased depth nearer than the eye counts in
 * the average as it is, negative), and on the GTE path to OTZ on its
 * scale, rounded, as console code adds an offset (so there a bias smaller
 * than an OTZ step does nothing). (ZSF3 and ZSF4 in the ratio 4 : 3, so
 * AVSZ3 and AVSZ4 give one OTZ scale, OTZ = 12 k x average / 4096, and
 * OTZ is mapped linearly onto
 * the buckets between the near and far planes' OTZ.) Depth is sorted, not
 * tested, as on the console: within a bucket, and between faces that
 * cross, the later call is drawn over the earlier.
 *
 * What a face becomes:
 *   - Culled when it faces away (unless its material is double-sided) or
 *     lies wholly nearer than the near plane, beyond the far plane, or off
 *     the frame. Facing is NCLIP's sign for its first three vertices (the
 *     GPU's first triangle), or for a quad whose first triangle has no area
 *     on the screen, its second's, judged in both paths on the positions
 *     each output draws: for the 1x VRAM (and the scaled frame without
 *     precise vertices) on the whole-pixel positions, as the console
 *     culls; for the scaled frame with precise vertices on the sub-pixel
 *     positions it rasterises. So a face seen almost edge on is drawn or
 *     culled in each output by where its corners land there, not by its
 *     exact orientation, a neighbour turned the other way by a hair does
 *     not leave a gap between them, and such a face may be drawn in one
 *     output and not the other. (A clipped face's triangles are judged
 *     otherwise: below.)
 *   - Exact (GTE path): a face with every vertex between the near and far
 *     planes and within the GTE's -1024..1023 pixels of the frame's corner
 *     each way (by the exact projection), and nothing the GTE saturated,
 *     is the console's own triangle
 *     or quad (a quad in the GPU's order, v[0] v[1] v[3] v[2]) with the
 *     GTE's screen positions, colours and ordering, so it draws exactly
 *     the pixels console code would draw from the same registers with
 *     jb_prim_poly and an ordering table, at 1x and at every scale with
 *     each option. When it spans more than the GPU draws (1023 x 511
 *     pixels), it is cut as the 2D draws cut a polygon.
 *   - Clipped: any other face is cut as its GPU triangles (a quad's two,
 *     v[0] v[1] v[3] and v[1] v[2] v[3], so the diagonal the GPU draws
 *     along stays where it is), each culled by its own orientation in view
 *     space (its corners are not all on the screen to judge by) and cut at
 *     the near and far planes and at a band around the screen past which
 *     the GTE cannot reach. Colours, normals and u, v are interpolated along a cut
 *     edge in view space at the near and far planes (where the cut is a
 *     real point of the surface) and along the screen at the band (which
 *     stands for cutting the screen, so the affine look is kept); the new
 *     vertices are then lit and projected as its own vertices are (a
 *     vertex cut at the band lies on its edge on the screen exactly; one
 *     more than 32767 GTE units off the axis, which only a great depth
 *     near the frame's edge reaches, is projected with IR saturated, as
 *     the GTE saturates it, and drawn where the GTE puts it), and the
 *     pieces cut to the frame. Neighbouring faces cut at the same edge
 *     get the same points, and a face's own vertices keep their
 *     projection, cut or not, so a mesh shows no cracks and draws no
 *     pixel of a shared edge twice. A face that covers the frame's edge
 *     draws its every pixel there, but in a frame 1024 wide the last
 *     column, x 1023, which no polygon reaches (The frame: the console's
 *     positions end at 1023), and, on the GTE path, the column before it
 *     where the face's own edge lies on the band's right edge at x 1023:
 *     the GTE projects such a vertex to 1022 (its divide and whole IR),
 *     and a clipped face keeps that so as to meet an exact neighbour on
 *     their shared edge (a known limit).
 *   - Flat (one colour) when all its vertices' colours are equal,
 *     Gouraud otherwise; this matters only for dithering (Drawing).
 *   - Textured with its material's texture, the vertices' u, v counted
 *     from the texture's corner (u, v of the jb_texture; a coordinate past
 *     the page's 255 is 255), or for a repeat texture as they are, with the
 *     texture window that wraps them within the texture set for it (and
 *     put back after the frame).
 * The records carry the vertices' sub-pixel positions and depths, so
 * precise vertices (above 1x) and perspective-correct texturing work for
 * them with no GTE table: for exact faces the same positions and depths
 * the GTE's own records give console code (jb_config.precise_vertices),
 * wherever its table gives each vertex its own record. The table is found
 * by integer screen position alone (a primitive's words carry nothing
 * else), so it cannot always: where two of a face's vertices land on one
 * integer position, whatever their depths, console code gets one record
 * for both (records there within 1/16 in depth are merged into one, and
 * of separate ones the lookup takes the same for both); and a vertex whose
 * record was evicted from a crowded frame's table, or seems another
 * object's, gets none, or no depth. Console code then draws the vertex
 * with another vertex's sub-pixel position and depth, or without them,
 * while the friendly record keeps each vertex its own. So such a face can
 * differ from console code, in which pixels it covers above 1x with
 * precise vertices (the positions), and in its texels and shading at
 * every scale, 1x included, with perspective-correct texturing (the
 * depths). At 1x with affine texturing it never differs.
 * Where the band around the screen cut a face, its new vertices carry both
 * the affine colours and texel coordinates (along the screen, as the
 * console's rasteriser interpolates) and the perspective-correct ones (in
 * view space), so each option draws them its own way.
 *
 * Errors, each with nothing recorded and the failure latched (jb_check):
 * JB_ERROR_INVALID_ARGUMENT for jb or the struct NULL, no frame begun, a
 * value out of range (below, and the mesh's, Scene geometry), a texture
 * with a field no primitive can have or a friendly field (u, v, width,
 * height, repeat) out of range for its page, or (GTE path) a model matrix,
 * mesh or fog beyond what the GTE's registers hold (the detail says
 * which; billboards and lines use no rotation or translation register,
 * so a camera far from the origin or a tiny depth range does not refuse
 * them);
 * JB_ERROR_OUT_OF_MEMORY when the frame is full, with what was recorded
 * before kept (jb_config.max_primitives). Drawing nothing (all of it
 * culled) is JB_OK.
 */

/* A mesh, placed in the world by model (NULL: the identity, the mesh's
   coordinates are world ones): jb_draw_mesh_ex with those and the rest 0. */
JB_API jb_result jb_draw_mesh(jb_context *jb, const jb_mesh *mesh, const jb_mat4 *model);

/*
 * A mesh with per-draw settings:
 *   mesh, model: as jb_draw_mesh's.
 *   depth_bias: world units added to the depth each of its faces is sorted
 *     by (its bucket), negative for nearer; its geometry, clipping and fog
 *     are unchanged. A shadow or decal lying on a surface, biased towards
 *     the camera by a little more than the depths the two can differ by,
 *     is drawn after the surface. The same units in both geometry paths:
 *     on the float path added to each depth before it is rounded down (3D
 *     drawing), on the GTE path added to OTZ on its scale, rounded.
 *   tint: JB_RGB multiplying the colour of each vertex (the material's or
 *     its vertex colour) by tint / 128 a channel, saturated at 255, before
 *     lighting; 0 (or JB_NEUTRAL) for none, JB_TINT_BLACK for black.
 * Errors: as jb_draw_mesh's (with this call's name), and draw NULL,
 * depth_bias not finite, tint above 0xFFFFFF (but JB_TINT_BLACK).
 */
typedef struct jb_mesh_draw {
    const jb_mesh *mesh;
    const jb_mat4 *model;
    float depth_bias;
    uint32_t tint;
} jb_mesh_draw;

JB_API jb_result jb_draw_mesh_ex(jb_context *jb, const jb_mesh_draw *draw);

/*
 * Mesh files: .jbm, as tools/jb-mesh writes them from OBJ or glTF, one
 * jb_mesh (positions, optional normals, vertex colours and texel u, v,
 * faces and materials) with numbered texture slots; docs/formats.md gives
 * the format byte for byte. A file holds no texture: each material names a
 * slot or none, and the program binds a texture to each slot when it loads
 * the mesh. Each slot records the size of the texture its u, v were made
 * for (jb-mesh maps u, v to texels of that size), and the bound texture
 * must be that size.
 *
 * jb_mesh_load_mem: checks the size bytes at data (any address) as a .jbm
 *   and makes *out, a jb_mesh with its arrays in memory of its own from
 *   the context's allocator (one allocation, kept until jb_mesh_destroy or
 *   jb_destroy), for jb_draw_mesh. textures holds texture_count textures,
 *   one for each slot in order (texture_count must be the mesh's count of
 *   slots; textures may be NULL when it is 0), each copied: a material
 *   with slot k draws with the copy of textures[k] (the texture itself
 *   must stay live, as for any material). The mesh is checked as
 *   jb_draw_mesh checks one, so a loaded mesh is never refused for its
 *   indices, colours or u, v (a draw can still refuse it for the GTE's
 *   limits, or for a texture destroyed since). The file is not needed
 *   after the call.
 * jb_mesh_load: the same for the file at path, read whole into memory
 *   from the context's allocator first (freed before it returns).
 * Errors, each with nothing allocated (no memory kept) and *out unchanged:
 *   JB_ERROR_INVALID_ARGUMENT for jb or out NULL, path NULL (jb_mesh_load),
 *   data NULL, texture_count negative, textures NULL with texture_count
 *   above 0, texture_count not the mesh's count of slots, a bound
 *   texture's width or height not its slot's; and for a file that is not
 *   exactly a valid .jbm: truncated or longer than its header says, sizes
 *   or offsets that do not fit the file, overlapping sections, a count out
 *   of range (vertices 1..65536, faces up to 1048576, materials up to
 *   65536), a face that is not 3 or 4 vertices or indexes a vertex or
 *   material that does not exist, a material with a blend mode not
 *   JB_BLEND_*, a colour above 0xFFFFFF, a slot that does not exist, a
 *   normal component outside -4096..4096, u, v outside the bound texture
 *   (but for a repeat texture), a reserved bit or field not 0, a version
 *   other than 1; jb_mesh_load also for a file that cannot be opened or
 *   read or is larger than 64 MB. Each has a detail (jb_error_string).
 *   JB_ERROR_OUT_OF_MEMORY when the allocator gives no memory.
 * jb_mesh_destroy: frees the memory of a mesh jb_mesh_load(_mem) made on
 *   this context (mesh equal to its *out, or a copy with the same
 *   positions pointer). Errors: JB_ERROR_INVALID_ARGUMENT for jb or mesh
 *   NULL, or a mesh that is not such a one (made by hand, from another
 *   context, destroyed already). jb_destroy frees those not destroyed.
 */
JB_API jb_result jb_mesh_load_mem(jb_context *jb, const void *data, size_t size,
                                  const jb_texture *textures, int texture_count, jb_mesh *out);
JB_API jb_result jb_mesh_load(jb_context *jb, const char *path, const jb_texture *textures,
                              int texture_count, jb_mesh *out);
JB_API jb_result jb_mesh_destroy(jb_context *jb, const jb_mesh *mesh);

/*
 * A billboard: a rectangle facing the camera (parallel to the screen)
 * centred at position in the world, width x height world units (height 0:
 * width x th / tw, the texels' own shape), showing the texture's texels
 * from (u, v), tw x th of them (0: to the texture's edge; within its
 * width and height, or for a hand-made jb_texture with width 0 within the
 * page). texture NULL draws a flat rectangle of tint. tint (0 for
 * JB_NEUTRAL, which leaves texels as they are; JB_TINT_BLACK for black)
 * multiplies the texels unless raw, and is fogged at the billboard's depth. When it would show its
 * texels one to a pixel (its size on screen rounds to tw x th pixels) it is
 * a rectangle (GP0(60h-7Fh)): whole pixels, crisp, and precise vertices do
 * not move it; otherwise a textured quad whose corners have sub-pixel
 * positions and the texel coordinates u .. u + tw - 1 and v .. v + th - 1
 * (the GPU rounds an interpolated coordinate to the nearest texel, so this
 * samples the crop and nothing beside it, the first and last texels taking
 * half the width of the others), clipped to the frame first, so a
 * billboard of any size shows the part of it in view, each texel where
 * it would be uncut (the cut is moved out to a whole texel, or for a
 * texel more than 16 pixels across made at the texels' boundaries, a quad
 * for each texel in view). Its texture's
 * friendly fields (u, v, width, height, repeat) must be as
 * jb_texture_create makes them, or 0 for a hand-made texture. Sorted by
 * the depth of its centre plus depth_bias (world units, as
 * jb_mesh_draw's). Culled when its centre is nearer than the near plane,
 * beyond the far one, or far off the screen. Errors (above): position,
 * width, height or depth_bias not finite, width not above 0, height
 * negative, tint above 0xFFFFFF (but JB_TINT_BLACK), a blend mode, the
 * texels outside the
 * texture (checked so that no size overflows), a texture's friendly field
 * out of range.
 */
typedef struct jb_billboard {
    const jb_texture *texture; /* NULL: untextured */
    jb_vec3 position;          /* its centre, world space */
    float width, height;       /* world units */
    int u, v, tw, th;          /* the texels shown, within the texture */
    uint32_t tint;             /* JB_RGB; 0 is JB_NEUTRAL; or JB_TINT_BLACK */
    int blend;                 /* JB_BLEND_* */
    int raw;                   /* textured: texels not multiplied by tint */
    float depth_bias;          /* world units added to its sorting depth */
} jb_billboard;

JB_API jb_result jb_draw_billboard(jb_context *jb, const jb_billboard *billboard);

/*
 * A line in the world from a to b, Gouraud from color_a to color_b
 * (JB_RGB), blended by blend (JB_BLEND_*), each end fogged at its depth:
 * cut at the near and far planes (the colour interpolated there) and to
 * the frame, and drawn as the console draws a line (lines are always
 * dithered when dithering is on). Its bucket is that of its two ends'
 * average depth plus depth_bias (world units, as jb_mesh_draw's). Ends
 * any distance away work, up to the largest floats, on either side and
 * unequally far: the line is kept as its direction and moment (a x b),
 * exact products of the given floats, so its visible part is found to
 * the view's own precision, and a cut point is placed on the plane it
 * lies on exactly. Errors
 * (above): a position or depth_bias not finite, a colour above 0xFFFFFF,
 * a blend mode.
 */
typedef struct jb_line3d {
    jb_vec3 a, b;
    uint32_t color_a, color_b;
    int blend;
    float depth_bias; /* world units added to its sorting depth */
} jb_line3d;

JB_API jb_result jb_draw_line3d(jb_context *jb, const jb_line3d *line);

#ifdef __cplusplus
}
#endif

#endif /* JITTERBUG_H */
