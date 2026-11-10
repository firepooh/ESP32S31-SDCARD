/*
 * On-device SD card benchmarks and filesystem helpers.
 * All functions need the card owned by the application (msc_ctrl_take()).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Raw sequential read with sdmmc_read_sectors() (read-only, no filesystem involved).
 *
 * @param total_mib  bytes per run (each run also stops after 5 s)
 * @param chunk_kib  sectors per SD command in KiB, 0 = sweep 4 KiB .. 1 MiB
 * @param offset_mib start offset, negative = middle of the card
 */
esp_err_t bench_raw_read(sdmmc_card_t *card, uint32_t total_mib, uint32_t chunk_kib, int32_t offset_mib);

/**
 * @brief Write a test file through the filesystem (/sdcard), read it back and verify.
 *
 * @param size_mib  file size
 * @param buf_kib   size of each write()/read() call
 * @param verify    check the data read back
 * @param keep      keep the test file
 */
esp_err_t bench_file(sdmmc_card_t *card, uint32_t size_mib, uint32_t buf_kib, bool verify, bool keep);

/**
 * @brief Create a new FAT32 or exFAT volume (MBR, one partition) on the whole card.
 *
 * The data area starts on a 4 MiB boundary. All data on the card is lost.
 */
esp_err_t bench_format(sdmmc_card_t *card, bool exfat, uint32_t cluster_kib);

/**
 * @brief Print filesystem type, cluster size, size and free space.
 *
 * If the filesystem is not mounted by the application, the card is probed read-only
 * on a temporary FatFs drive (use only while the USB host does not write to the card).
 */
void bench_print_fs_info(sdmmc_card_t *card);

#ifdef __cplusplus
}
#endif
