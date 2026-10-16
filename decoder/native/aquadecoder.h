// C API of the decoder for the Swift host.  Link libaquadecoder.dylib (+ libmlx.dylib, mlx.metallib).
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct aq_decoder aq_decoder;
// model_dir: a v2 export (decoder/v2/export_v2.py); metallib_path: path to mlx.metallib (or NULL for MLX's default
// lookup).  Returns NULL on failure (see aq_last_error).
aq_decoder* aq_open(const char* model_dir, const char* metallib_path);
void aq_close(aq_decoder* d);
// keys: the composition's key string; ctx_json: context tokens [{"kind":0|1,"chars":[char ids],"weights":[...]}] or NULL.
// Returns a malloc'ed JSON string {"candidates":[{text,score,char_ends,reading,prefix,...}],"timing_ms":{...},...};
// free with aq_free.  NULL on failure.
char* aq_decode(aq_decoder* d, const char* keys, const char* ctx_json);
// Thread-safe: the host has newer keys for this decoder, so a decode in progress on another thread is stale.  It stops
// at its next step boundary and returns {"aborted":true,"candidates":[]}; the next aq_decode is unaffected (and as fast
// as after a completed one, apart from the rare case where the aborted decode had compacted the table).
void aq_abort(aq_decoder* d);
// A sentence is over (a commit, or a new context): forget the state kept between its keystrokes.
void aq_reset(aq_decoder* d);
// Housekeeping between keystrokes: compact the K/V table for the next one.  The host calls it AFTER it has written the
// response, so the work does not sit inside the time the user waits for candidates; a decode that has not been
// preceded by one still works, it is just slower.
void aq_maintain(aq_decoder* d);
int aq_char_id(aq_decoder* d, const char* utf8_char);     // -1 when the char is outside the table
// The syllable ids a clean key string spells, as a malloc'ed JSON array, or "[]" when it does not segment exactly.
// The host's personal memory stores a remembered phrase by its reading so it can be found inside a sentence the
// model produced.
char* aq_readings(aq_decoder* d, const char* keys);
// The speed tier (CPU builds): 1 fast, 0 accurate.  The decoder picks it from its own decode times, between sentences;
// a host that stores aq_tier() when it closes and hands it to aq_set_tier() right after the next aq_open starts on the
// tier the machine needed last time.  aq_set_tier is ignored when the tier is fixed by AQ_TIER / AQ_BEAM_U / AQ_FINAL_U.
int aq_tier(aq_decoder* d);
void aq_set_tier(aq_decoder* d, int fast);
void aq_free(char* p);
const char* aq_last_error(void);
const char* aq_version(void);
#ifdef __cplusplus
}
#endif
