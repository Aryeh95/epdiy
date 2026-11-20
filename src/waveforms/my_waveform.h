#pragma once
#include "epd_internals.h"
#ifdef __cplusplus
extern "C" {
#endif
// Clear: complete GC/GL (48 phases) and DU (20 phases).
extern const EpdWaveform MY_WAVEFORM;
// Initialize both immutable profiles before epd_hl_init().
void my_waveform_init(void);
// Balanced: trimmed GC/GL (36/37 phases), unchanged DU; NULL if unavailable.
const EpdWaveform* my_waveform_fast(void);
// Clean-room cross-fade reader waveform: right-aligned transitions (to == 0 skips white
// pre-erase, to == 15 skips black pre-flash, to in 1..14 preserves pre-erase, 5+5 0->0
// black retention; 21 active phases on B/W pages via leading-skip). Do NOT use in Night Mode.
#define MY_WAVEFORM_HAS_READER 1
const EpdWaveform* my_waveform_reader(void);
// Tunable 16-level GC, for calibrating gray levels on the glass. Every pixel
// that changes is first cleared to white and then driven to its level from
// white, so each level is defined by its own three numbers. levels[] is
// indexed by gray level, 0 = black .. 15 = white:
//   black: frames pushed toward black from white
//   pull:  frames pushed back toward white after that
//   clear: frames pushed toward white to clear a pixel at this level
// Unchanged gray pixels are not driven. levels[15] is white and holds the
// top-up for unchanged pixels instead: `pull` white frames for one that stays
// white, `black` black frames for one that stays black, so that untouched
// white and black keep up with freshly driven white and black. Its `clear`
// chooses the timing: 0 draws every level only after the clearing has ended;
// nonzero draws a pixel that was white while the others clear, the two ending
// together, so the old screen fades as the new one appears.
#define MY_WAVEFORM_HAS_TUNED 1
typedef struct {
    uint8_t black;
    uint8_t pull;
    uint8_t clear;
} my_tuned_level_t;
// Builds the table; returns its phase count, or 0 (table unchanged) when it
// would exceed MY_TUNED_MAX_PHASES or cannot be allocated. Never call while a
// frame is being drawn.
#define MY_TUNED_MAX_PHASES 64
int my_waveform_tuned_set(const my_tuned_level_t levels[16]);
// The balanced GC's own numbers: its pushes from white to each level, and the
// white pushes of its transition from each level to white (it also pushes
// toward black before those, which this table has no number for).
void my_waveform_tuned_defaults(my_tuned_level_t levels[16]);
// NULL until my_waveform_tuned_set() has succeeded.
const EpdWaveform* my_waveform_tuned(void);
// Sleep settling: two black-only phases and a hold; white/gray targets are untouched.
const EpdWaveform* my_waveform_black_settle(void);
// Extreme: 30-phase, 8-gray GL only; GC uses the 16-gray profiles.
const EpdWaveform* my_waveform_gray8(void);
const EpdWaveform* my_waveform_short_du(void);
/**
 * @brief 获取快速档普通局刷使用的十四相 DU 波形
 */
const EpdWaveform* my_waveform_fast_du(void);
// Binary wipe + reveal: band in target index, old/new binary flags in source.
const EpdWaveform* my_waveform_wipe_reveal(void);
const EpdWaveform* my_waveform_wipe_reveal_short(void);
// Profile DU white conditioning (20/8 phases) + profile GL from white.
// The renderer adds spatial band delays without consuming gray indices.
const EpdWaveform* my_waveform_wipe_gl(int profile);
#ifdef __cplusplus
}
#endif
