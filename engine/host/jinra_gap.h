#pragma once
#include <stdint.h>

extern int jinra_gap_enabled;
void jinra_gap_init(void);
void jinra_gap_flush(void);
void jinra_gap_note(const char *kind, int64_t begin, int64_t end, int64_t signal,
                    uint32_t signal_tid, uint64_t key, uint64_t lr);

#define JINRA_GAP_NOW() (jinra_gap_enabled ? hos_now_ns() : 0)
#define JINRA_GAP_LONG(kind, begin, end, signal, tid, key, lr) do { \
    if ((begin) && (end) - (begin) >= 2000000) \
        jinra_gap_note((kind), (begin), (end), (signal), (tid), (key), (lr)); \
} while (0)
