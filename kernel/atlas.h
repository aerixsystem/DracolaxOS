/* kernel/atlas.h — Boot animation atlas system
 * Parses the XML-like atlas format and sequences frames in the framebuffer.
 */
#ifndef ATLAS_H
#define ATLAS_H

#include "types.h"

#define ATLAS_MAX_ANIMS   16
#define ATLAS_MAX_FRAMES  64
#define ATLAS_NAME_LEN    32

typedef struct {
    uint32_t x, y, w, h;
} atlas_frame_t;

typedef struct {
    char         name[ATLAS_NAME_LEN];   /* e.g. "boot/appear"     */
    atlas_frame_t frames[ATLAS_MAX_FRAMES];
    int          nframes;
    uint8_t      loop;
    int          current;                 /* playback position      */
} atlas_anim_t;

typedef struct {
    atlas_anim_t  anims[ATLAS_MAX_ANIMS];
    int           nanim;
    uint32_t      img_w, img_h;           /* atlas image dimensions */
} atlas_t;

extern atlas_t g_atlas;

/* Parse the embedded atlas XML; img is the raw atlas image pixels (may be NULL) */
void atlas_init(void);

/* Get animation by name; returns NULL if not found */
atlas_anim_t *atlas_get(const char *name);

/* Advance an animation by one frame; returns current frame */
const atlas_frame_t *atlas_next_frame(atlas_anim_t *anim);

/* Play a named animation for n frames at given screen position (FB only).
 * Falls back to a procedural spinner on VGA. */
void atlas_play(const char *name, uint32_t screen_x, uint32_t screen_y,
                int nframes_to_play, uint32_t frame_delay_ms);

/* Boot animation sequence: appear → loading → finish */
void atlas_boot_sequence(void);

/* Progressive boot: call these instead of atlas_boot_sequence() when
 * the boot screen should track actual init time.
 *   atlas_boot_start()           — draw bg + logo, begin at 0%
 *   atlas_boot_set_progress(pct) — advance bar to pct (0-100)
 *   atlas_boot_finish()          — fill to 100%, fade to black */
void atlas_boot_start(void);
void atlas_boot_set_progress(int pct);
void atlas_boot_finish(void);

/** Print a short status/error string in the bottom debug strip during boot.
 *  Safe to call while animation is running — writes only to the reserved strip. */
void atlas_boot_log(const char *msg, int is_error);

#endif /* ATLAS_H */
