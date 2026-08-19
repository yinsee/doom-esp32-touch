/*
 * tdoom — PSRAM placement for Doom's large static arrays.
 *
 * Doom declares ~238 KB of static .bss, which overflows this chip's internal
 * DRAM once Arduino and the IDF have taken their share (the link failed by
 * 39616 bytes). ESP-IDF's usual answer, EXT_RAM_BSS_ATTR, is a no-op here:
 * Arduino's prebuilt libs ship with CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
 * unset, so .bss cannot be relocated by attribute.
 *
 * So the COLDEST large arrays are converted to runtime PSRAM allocations
 * instead. Only arrays touched a few times per tic are moved -- anything the
 * renderer walks per-pixel or per-column (visplanes, openings, drawsegs,
 * ylookup, columnofs) stays in internal SRAM, where its speed actually matters.
 *
 * TD_LAZY_ARRAY keeps every existing use site unchanged: it defines a macro
 * with the array's original name that allocates on first touch.
 */

#ifndef TDOOM_TD_ALLOC_H
#define TDOOM_TD_ALLOC_H

#include <stddef.h>

/* Allocate from PSRAM, zero-filled like the .bss it replaces. Halts with a
 * clear message rather than returning NULL -- a silent failure here would
 * surface as an inexplicable crash deep inside the renderer. */
void *TD_PsramAlloc(size_t bytes, const char *what);

/* Declare `name` as a lazily-allocated PSRAM array of `type[count]`, usable
 * exactly like the static array it replaces. File-scope only. */
#define TD_LAZY_ARRAY(type, name, count)                                      \
    static type *name##_td_ptr;                                               \
    static inline type *name##_td_get(void)                                   \
    {                                                                         \
        if (name##_td_ptr == NULL)                                            \
        {                                                                     \
            name##_td_ptr =                                                   \
                (type *)TD_PsramAlloc(sizeof(type) * (count), #name);         \
        }                                                                     \
        return name##_td_ptr;                                                 \
    }

#endif /* TDOOM_TD_ALLOC_H */
