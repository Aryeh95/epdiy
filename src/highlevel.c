/**
 * High-level API implementation for epdiy.
 */

#include <assert.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_types.h>
#include <string.h>

#include "epd_highlevel.h"
#include "epdiy.h"

#ifndef _swap_int
#define _swap_int(a, b) \
    {                   \
        int t = a;      \
        a = b;          \
        b = t;          \
    }
#endif

static bool already_initialized = 0;

EpdiyHighlevelState epd_hl_init(const EpdWaveform* waveform) {
    assert(!already_initialized);
    if (waveform == NULL) {
        waveform = epd_get_display()->default_waveform;
    }

    int fb_size = epd_width() / 2 * epd_height();

#if !(defined(CONFIG_ESP32_SPIRAM_SUPPORT) || defined(CONFIG_ESP32S3_SPIRAM_SUPPORT) || \
      defined(CONFIG_SPIRAM))
    ESP_LOGW(
        "EPDiy", "Please enable PSRAM (menuconfig → Component config → ESP PSRAM)"
    );
#endif
    /* S31 LCD 走 AXI-GDMA，PSRAM 传输对齐 64B；16B 会对齐失败并 abort */
#if defined(CONFIG_IDF_TARGET_ESP32S31) || defined(CONFIG_IDF_TARGET_ESP32P4)
    const size_t fb_align = 64;
#else
    const size_t fb_align = 16;
#endif
    EpdiyHighlevelState state;
    state.back_fb = heap_caps_aligned_alloc(fb_align, fb_size, MALLOC_CAP_SPIRAM);
    assert(state.back_fb != NULL);
    state.front_fb = heap_caps_aligned_alloc(fb_align, fb_size, MALLOC_CAP_SPIRAM);
    assert(state.front_fb != NULL);
    state.difference_fb = heap_caps_aligned_alloc(fb_align, 2 * fb_size, MALLOC_CAP_SPIRAM);
    assert(state.difference_fb != NULL);
    state.dirty_lines = malloc(epd_height() * sizeof(bool));
    assert(state.dirty_lines != NULL);
    state.dirty_columns
        = heap_caps_aligned_alloc(16, epd_width() / 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(state.dirty_columns != NULL);
    state.waveform = waveform;

    ESP_LOGW("epd_dram",
             "hl FB in PSRAM: front+back+diff=%u KB; dirty_cols INTERNAL=%u B",
             (unsigned)((fb_size * 4) / 1024), (unsigned)(epd_width() / 2));

    memset(state.front_fb, 0xFF, fb_size);
    memset(state.back_fb, 0xFF, fb_size);

    already_initialized = true;
    return state;
}

uint8_t* epd_hl_get_framebuffer(EpdiyHighlevelState* state) {
    assert(state != NULL);
    return state->front_fb;
}

uint8_t* epd_hl_get_back_framebuffer(EpdiyHighlevelState* state) {
    assert(state != NULL);
    return state->back_fb;
}

enum EpdDrawError epd_hl_update_screen(EpdiyHighlevelState* state, enum EpdDrawMode mode, int temperature) {
    return epd_hl_update_area(state, mode, temperature, epd_full_screen());
}

enum EpdDrawError epd_hl_update_screen_full(
    EpdiyHighlevelState* state, enum EpdDrawMode mode, int temperature
) {
    assert(state != NULL);
    // Full/recovery draws intentionally retain same-color conditioning pulses.
    mode &= ~MODE_SKIP_UNCHANGED;

    EpdRect area = epd_full_screen();
    uint32_t ts = esp_timer_get_time() / 1000;

    epd_difference_image_cropped(
        state->front_fb,
        state->back_fb,
        area,
        state->difference_fb,
        state->dirty_lines,
        state->dirty_columns
    );

    int fb_height = epd_height();
    int col_bytes = epd_width() / 2;
    for (int y = 0; y < fb_height; y++) {
        state->dirty_lines[y] = true;
    }
    memset(state->dirty_columns, 0xFF, col_bytes);

    uint32_t t1 = esp_timer_get_time() / 1000;

    EpdRect full = epd_full_screen();
    ESP_LOGI(
        "epd_path",
        "[upd] full draw_base packing=1PPB|mode=0x%X area=%dx%d diff_fb=%p",
        (unsigned)mode,
        full.width,
        full.height,
        (void*)state->difference_fb
    );
    enum EpdDrawError err = epd_draw_base(
        full,
        state->difference_fb,
        full,
        MODE_PACKING_1PPB_DIFFERENCE | mode,
        temperature,
        state->dirty_lines,
        state->dirty_columns,
        state->waveform
    );

    uint32_t t2 = esp_timer_get_time() / 1000;

    int fb_size = col_bytes * fb_height;
    if (err == EPD_DRAW_SUCCESS) memcpy(state->back_fb, state->front_fb, fb_size);

    uint32_t t3 = esp_timer_get_time() / 1000;

    ESP_LOGI(
        "epdiy",
        "full diff: %dms, draw: %dms, buffer update: %dms, total: %dms",
        t1 - ts,
        t2 - t1,
        t3 - t2,
        t3 - ts
    );
    return err;
}

enum EpdDrawError epd_hl_update_screen_from_white(
    EpdiyHighlevelState* state, enum EpdDrawMode mode, int temperature
) {
    assert(state != NULL);
    int fb_size = epd_width() / 2 * epd_height();
    ESP_LOGI(
        "epd_path",
        "[upd] from_white mode=0x%X temp=%d fb=%dx%d/%2 wf=%p front=%p back=%p",
        (unsigned)mode,
        temperature,
        epd_width(),
        epd_height(),
        (void*)state->waveform,
        (void*)state->front_fb,
        (void*)state->back_fb
    );
    memset(state->back_fb, 0xFF, fb_size);
    return epd_hl_update_screen_full(state, mode, temperature);
}

EpdRect _inverse_rotated_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    // If partial update uses full screen do not rotate anything
    if (!(x == 0 && y == 0 && epd_width() == w && epd_height() == h)) {
        // invert the current display rotation
        switch (epd_get_rotation()) {
            // 0 landscape: Leave it as is
            case EPD_ROT_LANDSCAPE:
                break;
            // 1 90 ° clockwise
            case EPD_ROT_PORTRAIT:
                _swap_int(x, y);
                _swap_int(w, h);
                x = epd_width() - x - w;
                break;

            case EPD_ROT_INVERTED_LANDSCAPE:
                // 3 180°
                x = epd_width() - x - w;
                y = epd_height() - y - h;
                break;

            case EPD_ROT_INVERTED_PORTRAIT:
                // 3 270 °
                _swap_int(x, y);
                _swap_int(w, h);
                y = epd_height() - y - h;
                break;
        }
    }

    EpdRect rotated = { x, y, w, h };
    return rotated;
}

static enum EpdDrawError update_region(
    EpdiyHighlevelState* state, enum EpdDrawMode mode, int temperature, EpdRect area, bool cleanup
) {
    assert(state != NULL);
    const bool equalize_black = !cleanup && (mode & MODE_EQUALIZE_BLACK) != 0;

    uint32_t ts = esp_timer_get_time() / 1000;
    const int fb_width = epd_width();
    const int fb_height = epd_height();
    const int col_bytes = fb_width / 2;

    int x0 = 0;
    int x1 = fb_width;
    int ry0 = 0;
    int ry1 = fb_height;

    if (equalize_black) {
        epd_difference_image_cropped(
            state->front_fb,
            state->back_fb,
            epd_full_screen(),
            state->difference_fb,
            state->dirty_lines,
            state->dirty_columns
        );
    } else {
        EpdRect rotated_area = _inverse_rotated_area(area.x, area.y, area.width, area.height);
        int rx0 = rotated_area.x < 0 ? 0 : rotated_area.x;
        ry0 = rotated_area.y < 0 ? 0 : rotated_area.y;
        int rx1 = rotated_area.x + rotated_area.width;
        ry1 = rotated_area.y + rotated_area.height;
        if (rx1 > fb_width) rx1 = fb_width;
        if (ry1 > fb_height) ry1 = fb_height;
        if (rx0 >= rx1 || ry0 >= ry1) {
            epd_leading_skip_discard();
            return EPD_DRAW_SUCCESS;
        }
        x0 = rx0 & ~31;
        x1 = (rx1 + 31) & ~31;
        if (x1 > fb_width) x1 = fb_width;
        EpdRect clamped = { .x = x0, .y = ry0, .width = x1 - x0, .height = ry1 - ry0 };

        EpdRect diff_area = epd_difference_image_cropped(
            state->front_fb,
            state->back_fb,
            clamped,
            state->difference_fb,
            state->dirty_lines,
            state->dirty_columns
        );

        // Enforce horizontal column isolation even outside crop_to.
        if (x0 > 0) memset(state->dirty_columns, 0, (size_t)(x0 / 2));
        if (x1 < fb_width) memset(state->dirty_columns + (x1 / 2), 0, (size_t)(col_bytes - x1 / 2));

        if (cleanup) {
            memset(state->dirty_columns + (x0 / 2), 0xFF, (size_t)((x1 - x0) / 2));
            for (int y = 0; y < fb_height; ++y) {
                state->dirty_lines[y] = (y >= ry0 && y < ry1);
            }
            mode &= ~MODE_SKIP_UNCHANGED;
        } else {
            bool any_col = false;
            for (int c = x0 / 2; c < x1 / 2; ++c) {
                if (state->dirty_columns[c] != 0) {
                    any_col = true;
                    break;
                }
            }
            if (!any_col || diff_area.height == 0 || diff_area.width == 0) {
                epd_leading_skip_discard();
                return EPD_DRAW_SUCCESS;
            }
        }
    }

    uint32_t t1 = esp_timer_get_time() / 1000;

    enum EpdDrawMode draw_mode = MODE_PACKING_1PPB_DIFFERENCE | mode;
    if (!cleanup) {
        draw_mode |= MODE_SKIP_UNCHANGED;
    }

    enum EpdDrawError err = epd_draw_base(
        epd_full_screen(),
        state->difference_fb,
        epd_full_screen(),
        draw_mode,
        temperature,
        equalize_black ? NULL : state->dirty_lines,
        equalize_black ? NULL : state->dirty_columns,
        state->waveform
    );

    uint32_t t2 = esp_timer_get_time() / 1000;

    if (err == EPD_DRAW_SUCCESS) {
        const int copy_off = x0 / 2;
        const int copy_bytes = (x1 - x0) / 2;
        for (int l = ry0; l < ry1; ++l) {
            if (state->dirty_lines[l]) {
                memcpy(state->back_fb + col_bytes * l + copy_off,
                       state->front_fb + col_bytes * l + copy_off,
                       (size_t)copy_bytes);
            }
        }
    }

    uint32_t t3 = esp_timer_get_time() / 1000;

    ESP_LOGI(
        "epdiy",
        "diff: %dms, draw: %dms, buffer update: %dms, total: %dms",
        t1 - ts,
        t2 - t1,
        t3 - t2,
        t3 - ts
    );
    return err;
}

enum EpdDrawError epd_hl_update_area(EpdiyHighlevelState* state, enum EpdDrawMode mode, int temperature, EpdRect area) {
    return update_region(state, mode, temperature, area, false);
}

enum EpdDrawError epd_hl_update_area_full(EpdiyHighlevelState* state, enum EpdDrawMode mode, int temperature, EpdRect area) {
    return update_region(state, mode, temperature, area, true);
}

void epd_hl_set_all_white(EpdiyHighlevelState* state) {
    assert(state != NULL);
    int fb_size = epd_width() / 2 * epd_height();
    memset(state->front_fb, 0xFF, fb_size);
}

void epd_fullclear(EpdiyHighlevelState* state, int temperature) {
    assert(state != NULL);
    epd_hl_set_all_white(state);
    enum EpdDrawError err = epd_hl_update_screen(state, MODE_GC16, temperature);
    assert(err == EPD_DRAW_SUCCESS);
    epd_clear();
}

void epd_hl_waveform(EpdiyHighlevelState* state, const EpdWaveform* waveform) {
    if (waveform == NULL) {
        waveform = epd_get_display()->default_waveform;
    }
    state->waveform = waveform;
}
