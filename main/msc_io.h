/*
 * MSC I/O engine: read-ahead and write merging between the TinyUSB MSC callbacks and the card.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t ra_hits;           /*!< chunks served from read-ahead data */
    uint32_t ra_misses;         /*!< chunks read from the card directly */
    uint32_t ra_waits;          /*!< chunks that waited for a read-ahead in flight */
    uint32_t ra_prefetches;     /*!< read-ahead SD commands started */
    uint32_t ra_dropped;        /*!< read-ahead results dropped because data was written meanwhile */
    uint32_t wb_cmds;           /*!< merged SD write commands */
    uint64_t wb_bytes;          /*!< bytes written by merged commands */
    uint32_t wb_breaks;         /*!< merges ended early by a non-contiguous write */
    uint32_t wb_idle_flushes;   /*!< merges written back because the host went idle */
    uint32_t wb_waits;          /*!< host chunks that waited for a free merge buffer */
    uint32_t wb_read_flushes;   /*!< write-backs forced by a read */
    uint32_t wb_sync_flushes;   /*!< write-backs forced by SYNCHRONIZE CACHE / PREVENT ALLOW / take */
    uint32_t wb_errors;         /*!< failed background writes */
} msc_io_counters_t;

/** @brief Allocate the buffers and start the I/O task. */
esp_err_t msc_io_init(sdmmc_card_t *card);

/**
 * @brief Serve one READ(10) chunk.
 * @return bytes copied, or -1 on error
 */
int32_t msc_io_read(uint32_t sector, void *buf, uint32_t bytes);

/**
 * @brief Accept one WRITE(10) chunk (written back later, merged with contiguous chunks).
 * @return bytes accepted, or -1 if a previous background write failed
 */
int32_t msc_io_write(uint32_t sector, const void *buf, uint32_t bytes);

/** @brief Write back all merged data and wait. Returns the first background write error. */
esp_err_t msc_io_flush(void);

/** @brief Forget read-ahead data (the card was changed by someone else). */
void msc_io_invalidate(void);

void msc_io_get_counters(msc_io_counters_t *out);
void msc_io_reset_counters(void);

/** @brief Buffer description for the stats output, e.g. "RA 2x256 KiB PSRAM, WB 2x512 KiB PSRAM". */
const char *msc_io_describe(void);

#ifdef __cplusplus
}
#endif
