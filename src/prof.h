#ifndef __MC_PROF_H__
#define __MC_PROF_H__

// tiny timing for the log: PROF_START(t) ... PROF_END(t, "name") prints when
// the part took longer than 2 ms (the n64 count register runs at 46.875 MHz)
#include "recomputils.h"
extern u64 osGetTime(void);
#define PROF_START(t) u64 t = osGetTime()
#define PROF_END(t, name) do { \
        u32 _us = (u32)(osGetTime() - (t)) / 47u; /* ~46.875 counts a us */ \
        if (_us > 2000) recomp_printf("[mc] slow: %s %d us\n", name, (s32)_us); \
    } while (0)

#endif
