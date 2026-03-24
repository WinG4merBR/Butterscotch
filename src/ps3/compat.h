#pragma once

#include <stdbool.h>
#include <time.h>
#include <sys/types.h>

// --- C23 Polyfills ---
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ <= 201710L
    #ifndef nullptr
        #define nullptr ((void*)0)
    #endif
#endif

// --- PS3 Timing Fixes ---
#ifdef __PPU__
    #include <sys/systime.h>
    #ifndef CLOCK_MONOTONIC
        #define CLOCK_MONOTONIC 1
    #endif

    static inline int ps3_clock_gettime(struct timespec* ts) {
        u64 sec = 0, nsec = 0;
        sysGetCurrentTime(&sec, &nsec);
        if (ts) {
            ts->tv_sec  = (time_t)sec;
            ts->tv_nsec = (long)nsec;
        }
        return 0;
    }
    #define clock_gettime(id, ts) ps3_clock_gettime(ts)
#endif