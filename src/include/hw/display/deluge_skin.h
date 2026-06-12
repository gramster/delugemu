/*
 * Deluge composited front-panel skin view
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DELUGE_SKIN_H
#define HW_DISPLAY_DELUGE_SKIN_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_DELUGE_SKIN "deluge-skin"
OBJECT_DECLARE_SIMPLE_TYPE(DelugeSkinState, DELUGE_SKIN)

struct DelugeOledState;
struct DelugePadGridState;

struct DelugeSkinState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuConsole *con;

    struct DelugeOledState *oled;
    struct DelugePadGridState *padgrid;

    /* PIC backend, read for indicator-LED and gold-knob state (may be NULL). */
    struct Chardev *pic;

    /* GPIO device, read for firmware-driven panel LEDs (Synced). May be NULL. */
    DeviceState *gpio;

    /*
     * SSIF audio device, read for the host monitor OUTPUT LEVEL so the skin can
     * draw the level arc on the master volume knob. May be NULL (headless).
     */
    DeviceState *ssif;

    /* RGBA skin background, converted to 32-bit ARGB pixels. */
    uint32_t *bg_argb;
    bool bg_loaded;

    /* Optional image path; defaults to Delugemu_Normal.png in cwd. */
    char *image_path;

    /*
     * Inverse (dark) theme. The default skin is the light "Normal" panel, on
     * which an unlit pad slot is filled white; with inverse set (the original
     * dark panel) an unlit pad slot is filled black. Lit pads blend their LED
     * colour over the slot identically in both themes.
     */
    bool inverse;

    /*
     * Display down-scale. The native panel is 2256x1584, which is larger than
     * many monitors; opening the host window at native size overflows the
     * screen. scale_percent (10..100, default 100) renders the composited panel
     * at that fraction of native size so the window opens small enough to fit,
     * while zoom-to-fit still scales it to any later window size. When < 100 the
     * panel is composited at native resolution into comp[] and box-filtered down
     * to the (smaller) display surface; out_w/out_h are the scaled dimensions.
     */
    uint32_t scale_percent;
    int out_w;
    int out_h;
    uint32_t *comp;

    bool dirty;
    QEMUTimer *refresh_timer;

    /*
     * Periodic refresh interval, in milliseconds (the "refresh-ms" property).
     * A longer interval lowers the host UI frame rate, reducing the BQL/main-
     * loop contention that competes with the host audio voice during a live
     * performance, at the cost of a less smooth panel. Clamped to a sane range
     * in realize(); safety_ticks is the matching number of intervals that
     * approximates a ~1s forced full refresh regardless of the chosen rate.
     */
    uint32_t refresh_ms;
    uint32_t safety_ticks;

    /*
     * Idle-skip state for the periodic refresh. The static skin (background,
     * encoder affordances, power LED) is only recomposited when the dynamic
     * overlays actually change, so a quiescent panel costs no full-frame
     * memcpy or display upload. The dynamic sources are digested in three
     * groups so the periodic refresh can restrict the (expensive) downscale
     * and display upload to just the part of the panel that moved:
     *   - oled_hash : the OLED framebuffer + mode flags (the high-frequency
     *                 animator), a small contiguous rectangle;
     *   - pad_hash  : the 18x8 RGB pad grid (main + sidebar rectangles);
     *   - misc_hash : the scattered indicator/gold-knob/SYNCED LEDs and the
     *                 master-volume meter, which fall back to a full-panel
     *                 update since they span the whole front panel.
     * idle_ticks bounds staleness with an occasional forced full refresh in
     * case a source is ever missed.
     */
    uint64_t oled_hash;
    uint64_t pad_hash;
    uint64_t misc_hash;
    bool have_region_hashes;
    uint32_t idle_ticks;

    /*
     * Audio-only mode. When set, the periodic refresh skips compositing and the
     * full-frame display upload entirely, so the host UI does no per-frame
     * surface scaling/blit. This frees the QEMU main loop (which also services
     * the host audio voice on some backends) during a performance, at the cost
     * of a frozen panel. Toggled live from the input layer.
     */
    bool render_suspended;
};

void deluge_skin_set_oled(DeviceState *dev, struct DelugeOledState *oled);
void deluge_skin_set_padgrid(DeviceState *dev,
                             struct DelugePadGridState *padgrid);
void deluge_skin_set_pic(DeviceState *dev, struct Chardev *pic);
void deluge_skin_set_gpio(DeviceState *dev, DeviceState *gpio);
void deluge_skin_set_ssif(DeviceState *dev, DeviceState *ssif);
void deluge_skin_set_render_suspended(DeviceState *dev, bool suspended);

#endif /* HW_DISPLAY_DELUGE_SKIN_H */
