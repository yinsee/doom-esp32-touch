/*
 * tdoom — timing and persistent storage for the Doom engine.
 *
 * Savegames and the config file need a real filesystem. The WAD does not (it is
 * memory-mapped from a raw partition, see port_wad.c), so the FAT volume here
 * exists purely so "save game" survives a power cycle -- which is part of
 * shipping the full game rather than a demo.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "../doomgeneric/doomgeneric.h"

static const char *TAG = "tdoom-sys";

#define STORAGE_LABEL "storage"
#define STORAGE_MOUNT "/doom"

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

/* Mount the FAT partition Doom writes its config and savegames into.
 * Called from tdoom.ino before the engine starts. Returns true on success;
 * a failure is not fatal -- the game still plays, it just cannot save. */
bool TD_MountStorage(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 8,                    /* config + a few savegame slots */
        .format_if_mount_failed = true,    /* first boot: the partition is blank */
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        STORAGE_MOUNT, STORAGE_LABEL, &cfg, &s_wl_handle);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "FAT mount failed: %s — saves will not persist",
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "storage mounted at %s", STORAGE_MOUNT);
    return true;
}

/* ---------------------------------------------------------------------------
 * doomgeneric platform hooks
 * ------------------------------------------------------------------------- */

uint32_t DG_GetTicksMs(void)
{
    /* esp_timer is monotonic since boot and unaffected by task scheduling,
     * which matters because Doom drives its 35 Hz game tic off this. */
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

void DG_SleepMs(uint32_t ms)
{
    if (ms == 0)
    {
        /* Still yield: Doom calls this in spin-ish waits and the watchdog on
         * this core needs a chance to be fed. */
        taskYIELD();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}
