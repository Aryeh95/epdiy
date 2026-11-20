/**
 * @brief MY_WAVEFORM：均衡完整波形与快速裁剪波形
 */

#include "my_waveform.h"

#include "my_waveform_du.h"
#include "my_waveform_gc.h"
#include "my_waveform_gl.h"
#include "my_waveform_trim.h"
#include "my_gray8_gc16.h"
#include "my_gray8_gl16.h"

#include <assert.h>
#include <string.h>

#include <esp_log.h>
#include <esp_heap_caps.h>

#define TAG "my_wf"

#define MY_FULL_DU_FRAMES 20
#define MY_FULL_GC16_FRAMES 48
#define MY_FULL_GL16_FRAMES 48
#define MY_TRIM_ERASE_MAX 11
#define MY_TRIM_SAT_CUT 5
#define MY_TRIM_WHITE_SAT_CUT 0
#define MY_TRIM_HOLD 3
#define MY_GC16_FRAMES MY_FULL_GC16_FRAMES
#define MY_GL16_FRAMES MY_FULL_GL16_FRAMES

static const EpdWaveformTempInterval my_waveform_intervals[] = {
    { .min = 0, .max = 50 },
};

static inline void lut_or(uint8_t (*data)[16][4], int f, int to, int from, int action) {
    data[f][to][from / 4] |= (uint8_t)(action << (6 - 2 * (from % 4)));
}

static inline int lut_get(const uint8_t (*data)[16][4], int f, int to, int from) {
    return (data[f][to][from / 4] >> (6 - 2 * (from % 4))) & 3;
}

// 源 DU 只认目标 0/15；中间灰 50/50，暗→黑、亮→白
static uint8_t s_du_data[MY_FULL_DU_FRAMES][16][4];
static const EpdWaveformPhases s_du_phases = {
    .phases = MY_FULL_DU_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_du_data[0],
};
static const EpdWaveformPhases* s_du_ranges[] = { &s_du_phases };
static const EpdWaveformMode s_du_mode = {
    .type = 1,
    .temp_ranges = 1,
    .range_data = &s_du_ranges[0],
};

static void complete_du_build(void) {
    memset(s_du_data, 0, sizeof(s_du_data));
    for (int to = 0; to < 16; to++) {
        const int to_bin = to < 8 ? 0 : 15;
        for (int from = 0; from < 16; from++) {
            for (int f = 0; f < MY_FULL_DU_FRAMES; f++) {
                const int action = lut_get(my_du_25_0_data, f, to_bin, from);
                if (action != 0) {
                    lut_or(s_du_data, f, to, from, action);
                }
            }
        }
    }
    // 在末尾四相为黑→黑补黑，不增加总相数。
    for (int f = MY_FULL_DU_FRAMES - 4; f < MY_FULL_DU_FRAMES; ++f) {
        lut_or(s_du_data, f, 0, 0, 1);
    }
}

// 白底 15→15 源表全保持：挂在已有往白推的一相上再推 1 帧
static void gl16_white_tick(uint8_t (*data)[16][4], int frames) {
    int tick = -1;
    for (int f = frames - 1; f >= 0; f--) {
        for (int from = 0; from < 15; from++) {
            if (lut_get(data, f, 15, from) == 2) {
                tick = f;
                break;
            }
        }
        if (tick >= 0) {
            break;
        }
    }
    if (tick < 0) {
        tick = frames > 2 ? frames - 3 : 0;
    }
    lut_or(data, tick, 15, 15, 2);
}

static uint8_t s_gc16_data[MY_FULL_GC16_FRAMES][16][4];
static uint8_t s_gl16_data[MY_FULL_GL16_FRAMES][16][4];
static const EpdWaveformPhases s_gc16_phases = {
    .phases = MY_GC16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_gc16_data[0],
};
static const EpdWaveformPhases s_gl16_phases = {
    .phases = MY_GL16_FRAMES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&s_gl16_data[0],
};
static const EpdWaveformPhases* s_gc16_ranges[] = { &s_gc16_phases };
static const EpdWaveformPhases* s_gl16_ranges[] = { &s_gl16_phases };
static const EpdWaveformMode s_gc16_mode = {
    .type = 2,
    .temp_ranges = 1,
    .range_data = &s_gc16_ranges[0],
};
static const EpdWaveformMode s_gl16_mode = {
    .type = 5,
    .temp_ranges = 1,
    .range_data = &s_gl16_ranges[0],
};

static const EpdWaveformMode* s_modes[] = {
    &s_du_mode,
    &s_gc16_mode,
    &s_gl16_mode,
};

const EpdWaveform MY_WAVEFORM = {
    .num_modes = 3,
    .num_temp_ranges = 1,
    .mode_data = s_modes,
    .temp_intervals = my_waveform_intervals,
};

static EpdWaveformPhases s_fast_gc_phases = { .phases = 36 };
static EpdWaveformPhases s_fast_gl_phases = { .phases = 37 };
static const EpdWaveformPhases* s_fast_gc_ranges[] = { &s_fast_gc_phases };
static const EpdWaveformPhases* s_fast_gl_ranges[] = { &s_fast_gl_phases };
static const EpdWaveformMode s_fast_gc_mode = {
    .type = 2, .temp_ranges = 1, .range_data = s_fast_gc_ranges,
};
static const EpdWaveformMode s_fast_gl_mode = {
    .type = 5, .temp_ranges = 1, .range_data = s_fast_gl_ranges,
};
static const EpdWaveformMode* s_fast_modes[] = {
    &s_du_mode, &s_fast_gc_mode, &s_fast_gl_mode,
};
static const EpdWaveform s_fast_waveform = {
    .num_modes = 3, .num_temp_ranges = 1,
    .mode_data = s_fast_modes, .temp_intervals = my_waveform_intervals,
};

const EpdWaveform* my_waveform_fast(void) {
    return s_fast_gc_phases.luts ? &s_fast_waveform : NULL;
}

static EpdWaveformPhases s_reader_gl_phases = { .phases = 0 };
static const EpdWaveformPhases* s_reader_gl_ranges[] = { &s_reader_gl_phases };
static const EpdWaveformMode s_reader_gl_mode = {
    .type = 5, .temp_ranges = 1, .range_data = s_reader_gl_ranges,
};
static const EpdWaveformMode s_reader_gc_mode = {
    .type = 2, .temp_ranges = 1, .range_data = s_reader_gl_ranges,
};
static const EpdWaveformMode* s_reader_modes[] = {
    &s_du_mode, &s_reader_gc_mode, &s_reader_gl_mode,
};
static const EpdWaveform s_reader_waveform = {
    .num_modes = 3, .num_temp_ranges = 1,
    .mode_data = s_reader_modes, .temp_intervals = my_waveform_intervals,
};

const EpdWaveform* my_waveform_reader(void) {
    return s_reader_gl_phases.luts && s_reader_gl_phases.phases > 0 ? &s_reader_waveform : NULL;
}

// Reinforce both unchanged endpoints twice to reduce transition residue.
static void gc16_reinforce_same_color(uint8_t (*data)[16][4], int frames) {
    assert(frames >= 3);
    for (int f = 0; f < frames; ++f) {
        for (int gray = 0; gray < 16; ++gray) {
            const int shift = 6 - 2 * (gray % 4);
            data[f][gray][gray / 4] &= (uint8_t)~(3u << shift);
        }
    }
    // Keep the final hold phase; intermediate grays receive no reinforcement.
    lut_or(data, frames - 3, 0, 0, 1);
    lut_or(data, frames - 2, 0, 0, 1);
    lut_or(data, frames - 3, 15, 15, 2);
    lut_or(data, frames - 2, 15, 15, 2);
}

// Shorten white-to-black by one phase and extend black-to-white by one phase.
static void gc16_adjust_endpoint_transitions(uint8_t (*data)[16][4], int frames) {
    for (int f = frames - 2; f >= 0; --f) {
        if (lut_get(data, f, 0, 15) == 1) {
            data[f][0][3] &= (uint8_t)~3u;
            break;
        }
    }
    for (int f = frames - 3; f >= 0; --f) {
        if (lut_get(data, f, 15, 0) == 2 && lut_get(data, f + 1, 15, 0) == 0) {
            lut_or(data, f + 1, 15, 0, 2);
            break;
        }
    }
}

// --- tunable 16-level GC (see my_waveform.h) ---
static EpdWaveformPhases s_tuned_phases = { .phases = 0 };
static const EpdWaveformPhases* s_tuned_ranges[] = { &s_tuned_phases };
static const EpdWaveformMode s_tuned_mode = {
    .type = 2, .temp_ranges = 1, .range_data = s_tuned_ranges,
};
static const EpdWaveformMode* s_tuned_modes[] = { &s_tuned_mode };
static const EpdWaveform s_tuned_waveform = {
    .num_modes = 1, .num_temp_ranges = 1,
    .mode_data = s_tuned_modes, .temp_intervals = my_waveform_intervals,
};
static uint8_t (*s_tuned_data)[16][4];

const EpdWaveform* my_waveform_tuned(void) {
    return s_tuned_phases.phases > 0 ? &s_tuned_waveform : NULL;
}

#define MY_TUNED_HOLD 2

int my_waveform_tuned_set(const my_tuned_level_t levels[16]) {
    if (levels == NULL) return 0;
    // Stage one clears to white, stage two draws from white; each is as long
    // as its longest transition.
    const int white_top_up = levels[15].pull;
    const int black_top_up = levels[15].black;
    int clear_len = white_top_up > black_top_up ? white_top_up : black_top_up;
    int draw_len = black_top_up;
    for (int level = 0; level < 15; ++level) {
        if (levels[level].clear > clear_len) clear_len = levels[level].clear;
        if (levels[level].black + levels[level].pull > draw_len) {
            draw_len = levels[level].black + levels[level].pull;
        }
    }
    const int phases = clear_len + draw_len + MY_TUNED_HOLD;
    if (phases > MY_TUNED_MAX_PHASES) return 0;
    if (!s_tuned_data) {
        s_tuned_data = heap_caps_malloc(
            (size_t)MY_TUNED_MAX_PHASES * 16 * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_tuned_data) return 0;
    }
    memset(s_tuned_data, 0, (size_t)MY_TUNED_MAX_PHASES * 16 * 4);
    const bool overlap = levels[15].clear != 0;
    // Top-ups run with the stage that drives the same way, ending with it.
    // Overlapped, a pixel that was white is drawn inside the clearing stage.
    for (int i = 0; i < white_top_up; ++i) lut_or(s_tuned_data, clear_len - 1 - i, 15, 15, 2);
    for (int i = 0; i < black_top_up; ++i) {
        lut_or(s_tuned_data, (overlap ? clear_len : clear_len + draw_len) - 1 - i, 0, 0, 1);
    }
    for (int to = 0; to < 16; ++to) {
        for (int from = 0; from < 16; ++from) {
            if (from == to) continue;
            if (from != 15) {
                // Clear, ending with the stage so everything turns white together.
                int f = clear_len - levels[from].clear;
                for (int i = 0; i < levels[from].clear; ++i) lut_or(s_tuned_data, f++, to, from, 2);
            }
            if (to != 15) {
                const int length = levels[to].black + levels[to].pull;
                int f = clear_len;
                if (overlap && from == 15) {
                    f = clear_len > length ? clear_len - length : 0;
                } else if (overlap) {
                    f = clear_len + draw_len - length;
                }
                for (int i = 0; i < levels[to].black; ++i) lut_or(s_tuned_data, f++, to, from, 1);
                for (int i = 0; i < levels[to].pull; ++i) lut_or(s_tuned_data, f++, to, from, 2);
            }
        }
    }
    s_tuned_phases.luts = (const uint8_t*)s_tuned_data;
    s_tuned_phases.phases = phases;
    return phases;
}

void my_waveform_tuned_defaults(my_tuned_level_t levels[16]) {
    const EpdWaveformPhases* src = s_fast_gc_phases.luts ? &s_fast_gc_phases : &s_gc16_phases;
    const uint8_t (*data)[16][4] = (const uint8_t (*)[16][4])src->luts;
    memset(levels, 0, 16 * sizeof(levels[0]));
    for (int level = 0; level < 15; ++level) {
        int black = 0, pull = 0, clear = 0;
        for (int f = 0; f < src->phases; ++f) {
            // From white: every black push, and the white pushes that follow them.
            const int draw = lut_get(data, f, level, 15);
            if (draw == 1) black++;
            if (draw == 2 && black > 0) pull++;
            if (lut_get(data, f, 15, level) == 2) clear++;
        }
        levels[level].black = (uint8_t)black;
        levels[level].pull = (uint8_t)pull;
        levels[level].clear = (uint8_t)clear;
    }
    // Its unchanged white and black each get two frames (gc16_reinforce_same_color).
    levels[15].pull = 2;
    levels[15].black = 2;
}

// Two black-only settling phases and a final hold; other targets are untouched.
static uint8_t s_black_settle_data[3][16][4];
static const EpdWaveformPhases s_black_settle_phases = {
    .phases = 3, .phase_times = NULL, .luts = (const uint8_t*)s_black_settle_data,
};
static const EpdWaveformPhases* s_black_settle_ranges[] = { &s_black_settle_phases };
static const EpdWaveformMode s_black_settle_mode = {
    .type = 2, .temp_ranges = 1, .range_data = s_black_settle_ranges,
};
static const EpdWaveformMode* s_black_settle_modes[] = { &s_black_settle_mode };
static const EpdWaveform s_black_settle_waveform = {
    .num_modes = 1, .num_temp_ranges = 1,
    .mode_data = s_black_settle_modes, .temp_intervals = my_waveform_intervals,
};
static void black_settle_build(void) {
    memset(s_black_settle_data, 0, sizeof(s_black_settle_data));
    for (int f = 0; f < 2; ++f) {
        for (int from = 0; from < 16; ++from) lut_or(s_black_settle_data, f, 0, from, 1);
    }
}
const EpdWaveform* my_waveform_black_settle(void) { return &s_black_settle_waveform; }

static void short_du_build(void);
static void wipe_build(void);

void my_waveform_init(void) {
    static bool initialized;
    if (initialized) return;
    initialized = true;
    black_settle_build();
    complete_du_build();
    short_du_build();
    wipe_build();
    memcpy(s_gc16_data, my_gc_25_0_data, sizeof(s_gc16_data));
    gc16_adjust_endpoint_transitions(s_gc16_data, MY_FULL_GC16_FRAMES);
    memcpy(s_gl16_data, my_gl_25_0_data, sizeof(s_gl16_data));
    gl16_white_tick(s_gl16_data, MY_GL16_FRAMES);

    // Build once in PSRAM. Switching profiles never modifies an active LUT.
    uint8_t (*data)[16][4] = heap_caps_malloc(
        sizeof(s_gc16_data) + sizeof(s_gl16_data), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        ESP_LOGW(TAG, "fast waveform allocation failed; balanced retained");
        return;
    }
    const my_waveform_trim_t trim = {
        .erase_max = MY_TRIM_ERASE_MAX, .sat_cut = MY_TRIM_SAT_CUT,
        .white_sat_cut = MY_TRIM_WHITE_SAT_CUT, .hold = MY_TRIM_HOLD,
    };
    const int gc = my_waveform_trim(&my_gc_25_0, &trim, data);
    const int gl = my_waveform_trim(&my_gl_25_0, &trim, data + MY_FULL_GC16_FRAMES);
    assert(gc == s_fast_gc_phases.phases);
    gc16_reinforce_same_color(data, gc);
    gc16_adjust_endpoint_transitions(data, gc);
    assert(gl == s_fast_gl_phases.phases);
    gl16_white_tick(data + MY_FULL_GC16_FRAMES, gl);
    s_fast_gc_phases.luts = (const uint8_t*)data;
    s_fast_gl_phases.luts = (const uint8_t*)(data + MY_FULL_GC16_FRAMES);

    // Build clean-room cross-fade reader table in PSRAM from trimmed GL16.
    uint8_t (*reader)[16][4] = heap_caps_calloc(
        (size_t)gl, sizeof(reader[0]), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reader) {
        ESP_LOGW(TAG, "reader waveform allocation failed; fast GC16 retained");
        return;
    }
    const uint8_t (*src_gl)[16][4] = (const uint8_t (*)[16][4])s_fast_gl_phases.luts;
    int max_active = 0;
    for (int to = 0; to < 16; ++to) {
        for (int from = 0; from < 16; ++from) {
            if (to == from) continue;
            uint8_t seq[MY_FULL_GL16_FRAMES];
            for (int f = 0; f < gl; ++f) {
                seq[f] = (uint8_t)lut_get(src_gl, f, to, from);
            }
            int a = 0;
            while (a < gl && seq[a] == 0) ++a;
            int z = gl;
            while (z > a && seq[z - 1] == 0) --z;
            if (to < 15) {
                // Dark/gray targets darken directly without pre-erasing to white.
                while (a < z && seq[a] == 2) ++a;
            }
            const int len = z - a;
            for (int i = 0; i < len; ++i) {
                if (seq[a + i] != 0) {
                    lut_or(reader, i, to, from, seq[a + i]);
                }
            }
            if (len > max_active) max_active = len;
        }
    }
    // Retained black (0 -> 0): 5 erase + 5 darken to prevent fading across page turns.
    for (int f = 0; f < 5 && f < gl; ++f) lut_or(reader, f, 0, 0, 2);
    for (int f = 5; f < 10 && f < gl; ++f) lut_or(reader, f, 0, 0, 1);
    if (max_active < 10) max_active = 10;
    int reader_phases = max_active + MY_TRIM_HOLD;
    if (reader_phases > gl) reader_phases = gl;
    s_reader_gl_phases.phases = reader_phases;
    s_reader_gl_phases.luts = (const uint8_t*)reader;
    ESP_LOGI(TAG, "reader waveform initialized (%d phases)", reader_phases);
}

// Extreme GL uses eight optical grays; GC always uses the 16-gray profiles.
static const EpdWaveformPhases s_gray8_gl_phases = {
    .phases = 30, .phase_times = NULL, .luts = (const uint8_t*)e0470_gray8_gl16_data,
};
static const EpdWaveformPhases* s_gray8_gl_ranges[] = { &s_gray8_gl_phases };
static const EpdWaveformMode s_gray8_gl_mode = { .type=5, .temp_ranges=1, .range_data=s_gray8_gl_ranges };
static const EpdWaveformMode* s_gray8_modes[] = { &s_gray8_gl_mode };
static const EpdWaveform s_gray8_waveform = {
    .num_modes=1, .num_temp_ranges=1, .mode_data=s_gray8_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_gray8(void) { return &s_gray8_waveform; }

// 黑白转换推黑七相、推白八相；黑→黑在末尾四相补黑，其余同色保持。
static uint8_t s_short_du_data[8][16][4];
static const EpdWaveformPhases s_short_du_phases = {
    .phases=8, .phase_times=NULL, .luts=(const uint8_t*)s_short_du_data,
};
static const EpdWaveformPhases* s_short_du_ranges[] = { &s_short_du_phases };
static const EpdWaveformMode s_short_du_mode = { .type=1, .temp_ranges=1, .range_data=s_short_du_ranges };
static const EpdWaveformMode* s_short_du_modes[] = { &s_short_du_mode };
static const EpdWaveform s_short_du_waveform = {
    .num_modes=1, .num_temp_ranges=1, .mode_data=s_short_du_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_short_du(void) { return &s_short_du_waveform; }

static uint8_t s_fast_du_data[14][16][4];
static const EpdWaveformPhases s_fast_du_phases = {
    .phases=14, .phase_times=NULL, .luts=(const uint8_t*)s_fast_du_data,
};
static const EpdWaveformPhases* s_fast_du_ranges[] = { &s_fast_du_phases };
static const EpdWaveformMode s_fast_du_mode = {
    .type=1, .temp_ranges=1, .range_data=s_fast_du_ranges,
};
static const EpdWaveformMode* s_fast_du_modes[] = { &s_fast_du_mode };
static const EpdWaveform s_fast_du_waveform = {
    .num_modes=1, .num_temp_ranges=1,
    .mode_data=s_fast_du_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_fast_du(void) { return &s_fast_du_waveform; }
// Binary page navigation: each band starts one phase later. The target nibble
// holds the band, and the source nibble holds old/new binary flags. Index 15
// keeps existing white pixels and the border undriven.
static uint8_t (*s_reveal_data)[16][4];
static EpdWaveformPhases s_reveal_phases = {
    .phases=2*MY_FULL_DU_FRAMES+9, .phase_times=NULL, .luts=NULL,
};
static const EpdWaveformPhases* s_reveal_ranges[] = { &s_reveal_phases };
static const EpdWaveformMode s_reveal_mode = { .type=1, .temp_ranges=1, .range_data=s_reveal_ranges };
static const EpdWaveformMode* s_reveal_modes[] = { &s_reveal_mode };
static const EpdWaveform s_reveal_waveform = {
    .num_modes=1, .num_temp_ranges=1, .mode_data=s_reveal_modes, .temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_wipe_reveal(void) { return s_reveal_data ? &s_reveal_waveform : NULL; }
static uint8_t (*s_reveal_short_data)[16][4];
static EpdWaveformPhases s_reveal_short_phases = {
    .phases=25,.phase_times=NULL,.luts=NULL,
};
static const EpdWaveformPhases* s_reveal_short_ranges[] = { &s_reveal_short_phases };
static const EpdWaveformMode s_reveal_short_mode = { .type=1,.temp_ranges=1,.range_data=s_reveal_short_ranges };
static const EpdWaveformMode* s_reveal_short_modes[] = { &s_reveal_short_mode };
static const EpdWaveform s_reveal_short_waveform = {
    .num_modes=1,.num_temp_ranges=1,.mode_data=s_reveal_short_modes,.temp_intervals=my_waveform_intervals,
};
const EpdWaveform* my_waveform_wipe_reveal_short(void) { return s_reveal_short_data ? &s_reveal_short_waveform : NULL; }
const EpdWaveform* my_waveform_wipe_gl(int profile) {
    static uint8_t (*data[4])[16][4];
    static EpdWaveformPhases phases[4];
    static const EpdWaveformPhases* ranges[4];
    static EpdWaveformMode modes[4];
    static const EpdWaveformMode* mode_ptrs[4];
    static EpdWaveform waves[4];
    const int slot=profile>=0 && profile<4 ? profile : 0;
    if(waves[slot].num_modes) return &waves[slot];
    const EpdWaveform* source=slot==3 ? my_waveform_gray8() :
        (slot>=1 && my_waveform_fast() ? my_waveform_fast() : &MY_WAVEFORM);
    const int erase=slot>=2 ? 8 : MY_FULL_DU_FRAMES;
    const EpdWaveformPhases* gl=NULL;
    for(int m=0;m<source->num_modes;++m)
        if(source->mode_data[m]->type==5) gl=source->mode_data[m]->range_data[0];
    if(!gl || gl->phases>MY_FULL_GL16_FRAMES) return NULL;
    data[slot]=heap_caps_malloc((erase+gl->phases)*16*4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!data[slot]) return NULL;
    memset(data[slot],0,(erase+gl->phases)*16*4);
    for(int to=0;to<16;++to) for(int old=0;old<16;++old) {
        for(int f=0;f<erase;++f)
            lut_or(data[slot],f,to,old,lut_get(slot>=2 ? s_short_du_data : my_du_25_0_data,f,15,old));
        for(int f=0;f<gl->phases;++f) {
            const int action=(gl->luts[(f*16+to)*4+3])&3; // source white (15)
            lut_or(data[slot],erase+f,to,old,action);
        }
    }
    phases[slot]=(EpdWaveformPhases){.phases=erase+gl->phases,
        .phase_times=NULL,.luts=(const uint8_t*)data[slot]};
    ranges[slot]=&phases[slot];
    modes[slot]=(EpdWaveformMode){.type=5,.temp_ranges=1,.range_data=&ranges[slot]};
    mode_ptrs[slot]=&modes[slot];
    waves[slot]=(EpdWaveform){.num_modes=1,.num_temp_ranges=1,
        .mode_data=&mode_ptrs[slot],.temp_intervals=my_waveform_intervals};
    return &waves[slot];
}
static void wipe_build(void) {
    // Waveform data is read before each scan into the INTERNAL conversion LUT.
    // Keep the large immutable source tables out of DMA-capable internal RAM.
    s_reveal_data=heap_caps_malloc((2*MY_FULL_DU_FRAMES+9)*16*4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_reveal_short_data=heap_caps_malloc(25*16*4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(s_reveal_data) memset(s_reveal_data,0,(2*MY_FULL_DU_FRAMES+9)*16*4);
    if(s_reveal_short_data) memset(s_reveal_short_data,0,25*16*4);
    s_reveal_phases.luts=(const uint8_t*)s_reveal_data;
    s_reveal_short_phases.luts=(const uint8_t*)s_reveal_short_data;
    for (int band=0;band<10;++band) for (int code=0;code<4;++code) {
        const int old=(code&1) ? 15 : 0;
        for (int f=0;f<MY_FULL_DU_FRAMES;++f) {
            if(s_reveal_data) lut_or(s_reveal_data,band+f,band,code,lut_get(my_du_25_0_data,f,15,old));
            if (s_reveal_data && (code&2)) lut_or(s_reveal_data,band+MY_FULL_DU_FRAMES+f,band,code,
                lut_get(my_du_25_0_data,f,0,15));
        }
        for(int f=0;f<8;++f) {
            if(s_reveal_short_data) lut_or(s_reveal_short_data,band+f,band,code,lut_get(s_short_du_data,f,15,old));
            if(s_reveal_short_data && (code&2)) lut_or(s_reveal_short_data,band+8+f,band,code,lut_get(s_short_du_data,f,0,15));
        }
    }
}
static void short_du_build(void) {
    for (int to=0;to<16;++to) for (int from=0;from<16;++from) {
        if (to==from) continue;
        const int endpoint=to<8 ? 0 : 15;
        const int action=endpoint==0 ? 1 : 2;
        const int frames=endpoint==0 ? 7 : 8;
        for (int f=0;f<frames;++f) lut_or(s_short_du_data,f,to,from,action);
        const int fast_frames=endpoint==0 ? 13 : 14;
        for (int f=0;f<fast_frames;++f) lut_or(s_fast_du_data,f,to,from,action);
    }
    for (int f=4;f<8;++f) lut_or(s_short_du_data,f,0,0,1);
    for (int f=10;f<14;++f) lut_or(s_fast_du_data,f,0,0,1);
}
