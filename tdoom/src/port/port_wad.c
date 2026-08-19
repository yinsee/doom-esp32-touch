/*
 * tdoom — WAD access from memory-mapped flash.
 *
 * Replaces upstream w_file_stdc.c. It deliberately keeps the same symbol name
 * (`stdc_wad_file`) so w_file.c needs no changes at all.
 *
 * The win here is w_file.h's `mapped` field: when it is non-NULL, w_wad.c
 * (see W_CacheLumpNum, w_wad.c:399) returns a pointer straight into the mapped
 * region instead of Z_Malloc'ing a copy. So on this board the 4.2 MB WAD costs
 * ZERO bytes of RAM — lumps are read through the flash cache in place, and the
 * whole zone heap stays available for the game.
 *
 * The WAD lives in a dedicated raw partition (see partitions.csv) written with:
 *   esptool --port /dev/cu.usbmodem11201 write-flash 0x310000 doom1.wad
 */

#include <stdio.h>
#include <string.h>

#include "esp_partition.h"
#include "esp_log.h"

#include "../doomgeneric/config.h"
#include "../doomgeneric/doomtype.h"
#include "../doomgeneric/i_system.h"
#include "../doomgeneric/m_misc.h"
#include "../doomgeneric/w_file.h"
#include "../doomgeneric/z_zone.h"

static const char *TAG = "tdoom-wad";

/* Label must match the `wad` row in partitions.csv. */
#define WAD_PARTITION_LABEL "wad"

/* An IWAD always starts with one of these. Used to reject an unwritten
 * (0xFF-filled) partition with a clear message instead of letting the engine
 * die deep inside W_AddFile with something cryptic. */
#define IWAD_MAGIC "IWAD"
#define PWAD_MAGIC "PWAD"

typedef struct {
    wad_file_t wad;
    esp_partition_mmap_handle_t handle;
} mmap_wad_file_t;

extern wad_file_class_t stdc_wad_file;

static wad_file_t *MmapWad_Open(char *path)
{
    const esp_partition_t *part;
    const void *mapped = NULL;
    esp_partition_mmap_handle_t handle = 0;
    mmap_wad_file_t *result;
    esp_err_t err;
    uint32_t wad_len;

    /* The engine passes whatever path d_iwad.c settled on; there is only one
     * WAD on this device, so the path is informational. */
    ESP_LOGI(TAG, "opening WAD (engine asked for '%s')", path ? path : "(null)");

    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY,
                                    WAD_PARTITION_LABEL);
    if (part == NULL)
    {
        ESP_LOGE(TAG, "no '%s' partition — check partitions.csv is in use",
                 WAD_PARTITION_LABEL);
        return NULL;
    }

    err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                             &mapped, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_partition_mmap failed: %s", esp_err_to_name(err));
        return NULL;
    }

    /* Validate before handing it to the engine. An erased partition reads as
     * all 0xFF, which would otherwise produce a baffling failure later. */
    if (memcmp(mapped, IWAD_MAGIC, 4) != 0 && memcmp(mapped, PWAD_MAGIC, 4) != 0)
    {
        ESP_LOGE(TAG, "partition '%s' does not begin with IWAD/PWAD — "
                      "flash doom1.wad to offset 0x%08x first",
                 WAD_PARTITION_LABEL, (unsigned)part->address);
        esp_partition_munmap(handle);
        return NULL;
    }

    /* The partition is larger than the WAD. Recover the real length from the
     * directory header (numlumps at +4, dir offset at +8) so W_Read cannot be
     * talked into reading past the end of the real file. */
    {
        const unsigned char *p = (const unsigned char *)mapped;
        uint32_t numlumps = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                            ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        uint32_t infotableofs = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                                ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
        wad_len = infotableofs + numlumps * 16;

        if (wad_len > part->size)
        {
            ESP_LOGE(TAG, "WAD claims %u bytes but partition is only %u — truncated flash?",
                     (unsigned)wad_len, (unsigned)part->size);
            esp_partition_munmap(handle);
            return NULL;
        }
        ESP_LOGI(TAG, "%.4s, %u lumps, %u bytes, mapped at %p",
                 (const char *)mapped, (unsigned)numlumps, (unsigned)wad_len, mapped);
    }

    result = Z_Malloc(sizeof(mmap_wad_file_t), PU_STATIC, NULL);
    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = (byte *)mapped;   /* the whole point: lumps read in place */
    result->wad.length = wad_len;
    result->handle = handle;

    return &result->wad;
}

static void MmapWad_Close(wad_file_t *wad)
{
    mmap_wad_file_t *f = (mmap_wad_file_t *)wad;

    if (f->handle != 0)
    {
        esp_partition_munmap(f->handle);
        f->handle = 0;
    }
    Z_Free(f);
}

static size_t MmapWad_Read(wad_file_t *wad, unsigned int offset,
                           void *buffer, size_t buffer_len)
{
    /* Reached only for lumps the engine chooses to copy; most go through the
     * `mapped` pointer and never land here. Clamp so a bad offset can't read
     * off the end of the mapping. */
    if (offset >= wad->length)
    {
        return 0;
    }
    if (offset + buffer_len > wad->length)
    {
        buffer_len = wad->length - offset;
    }

    memcpy(buffer, wad->mapped + offset, buffer_len);
    return buffer_len;
}

wad_file_class_t stdc_wad_file =
{
    MmapWad_Open,
    MmapWad_Close,
    MmapWad_Read,
};
