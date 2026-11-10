/*
 * On-device SD card benchmarks: raw sector read, file write/read/verify through FAT/exFAT,
 * filesystem information and format.
 *
 * Rates are MB/s with 1 MB = 10^6 bytes (same unit as CrystalDiskMark), sizes are MiB/KiB.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "soc/soc_caps.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "msc_ctrl.h"
#include "bench.h"

static const char *TAG = "bench";

#define MIB                 (1024u * 1024u)
#define RAW_RUN_MAX_US      (5 * 1000 * 1000LL)
#define PROGRESS_US         (5 * 1000 * 1000LL)
#define BENCH_FILE          MSC_BASE_PATH "/s31bench.bin"
#define FS_ALIGN_SECTORS    8192u           // 4 MiB: typical SDXC allocation unit
#define VERIFY_BLOCK        512u
#define NO_DRIVE            0xFF

typedef struct {
    uint8_t *p;
    size_t size;
    const char *where;
    bool dma_ok;
} bench_buf_t;

static esp_err_t buf_alloc(const sdmmc_card_t *card, size_t size, bench_buf_t *b)
{
    memset(b, 0, sizeof(*b));
    b->p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
    b->where = "internal RAM";
    if (!b->p) {
        b->p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        b->where = "PSRAM";
    }
    ESP_RETURN_ON_FALSE(b->p, ESP_ERR_NO_MEM, TAG, "no memory for a %u KiB buffer", (unsigned)(size / 1024));
    b->size = size;
    b->dma_ok = card->host.check_buffer_alignment(card->host.slot, b->p, size);
#if !SOC_SDMMC_PSRAM_DMA_CAPABLE
    b->dma_ok = b->dma_ok && !esp_ptr_external_ram(b->p);
#endif
    return ESP_OK;
}

static void buf_free(bench_buf_t *b)
{
    heap_caps_free(b->p);
    memset(b, 0, sizeof(*b));
}

// bytes per microsecond == MB/s (10^6 B/s)
static inline double rate_mbps(uint64_t bytes, int64_t us)
{
    return us > 0 ? (double)bytes / (double)us : 0.0;
}

/* ------------------------------------------------------------------------------------------ */
/* Raw read                                                                                   */
/* ------------------------------------------------------------------------------------------ */

esp_err_t bench_raw_read(sdmmc_card_t *card, uint32_t total_mib, uint32_t chunk_kib, int32_t offset_mib)
{
    static const uint32_t sweep_kib[] = { 4, 16, 32, 64, 128, 256, 512, 1024 };
    ESP_RETURN_ON_FALSE(!msc_ctrl_host_owns(), ESP_ERR_INVALID_STATE, TAG,
                        "the USB host owns the card, run 'mount' first");
    ESP_RETURN_ON_FALSE(total_mib > 0, ESP_ERR_INVALID_ARG, TAG, "size must be > 0");

    const uint32_t ss = card->csd.sector_size;
    const uint64_t cap = (uint64_t)card->csd.capacity * ss;
    const uint32_t *list = chunk_kib ? &chunk_kib : sweep_kib;
    const size_t n_list = chunk_kib ? 1 : sizeof(sweep_kib) / sizeof(sweep_kib[0]);

    uint32_t max_kib = 0;
    for (size_t i = 0; i < n_list; i++) {
        ESP_RETURN_ON_FALSE((list[i] * 1024u) % ss == 0, ESP_ERR_INVALID_ARG, TAG,
                            "chunk must be a multiple of %u B", (unsigned)ss);
        max_kib = MAX(max_kib, list[i]);
    }

    uint64_t total = MIN((uint64_t)total_mib * MIB, cap);
    uint64_t start = (offset_mib < 0) ? cap / 2 : (uint64_t)offset_mib * MIB;
    start &= ~(uint64_t)(4 * MIB - 1);
    if (start + total > cap) {
        start = (cap - total) & ~(uint64_t)(4 * MIB - 1);
    }

    bench_buf_t buf;
    ESP_RETURN_ON_ERROR(buf_alloc(card, (size_t)max_kib * 1024, &buf), TAG, "buffer");

    printf("Raw sequential read (sdmmc_read_sectors), card clock %d kHz\n", card->real_freq_khz);
    printf("  start %.1f MiB (sector %" PRIu64 "), up to %" PRIu64 " MiB or 5 s per run\n",
           start / (double)MIB, start / ss, total / MIB);
    printf("  buffer %u KiB in %s, %s\n", (unsigned)max_kib, buf.where,
           buf.dma_ok ? "DMA direct" : "not DMA aligned (driver copies)");
    printf("  chunk        MB/s      IO/s    us/IO\n");

    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < n_list && ret == ESP_OK; i++) {
        const uint32_t cs = list[i] * 1024u;
        const size_t secs = cs / ss;
        uint64_t done = 0;
        uint32_t ios = 0;
        const int64_t t0 = esp_timer_get_time();
        int64_t elapsed = 0;
        while (done + cs <= total && elapsed < RAW_RUN_MAX_US) {
            ret = sdmmc_read_sectors(card, buf.p, (size_t)((start + done) / ss), secs);
            if (ret != ESP_OK) {
                printf("  read error at sector %" PRIu64 ": %s\n", (start + done) / ss, esp_err_to_name(ret));
                break;
            }
            done += cs;
            ios++;
            elapsed = esp_timer_get_time() - t0;
        }
        if (ret == ESP_OK && ios > 0) {
            printf("  %4u KiB  %8.2f  %8.0f  %7.0f\n", (unsigned)list[i], rate_mbps(done, elapsed),
                   ios / (elapsed / 1e6), (double)elapsed / ios);
        }
    }
    buf_free(&buf);
    return ret;
}

/* ------------------------------------------------------------------------------------------ */
/* File write / read / verify                                                                 */
/* ------------------------------------------------------------------------------------------ */

// Every 512 B block = 8-byte file offset + 504 bytes of a fixed pseudo-random pattern.
static void make_ref_block(uint8_t *ref)
{
    uint32_t x = 0x2545F491u;
    for (size_t i = 0; i < VERIFY_BLOCK; i += sizeof(x)) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        memcpy(ref + i, &x, sizeof(x));
    }
}

static void stamp_blocks(uint8_t *buf, size_t len, uint64_t file_off)
{
    for (size_t o = 0; o < len; o += VERIFY_BLOCK) {
        const uint64_t v = file_off + o;
        memcpy(buf + o, &v, sizeof(v));
    }
}

static uint32_t check_blocks(const uint8_t *buf, size_t len, uint64_t file_off, const uint8_t *ref,
                             uint64_t *first_bad)
{
    uint32_t bad = 0;
    for (size_t o = 0; o < len; o += VERIFY_BLOCK) {
        uint64_t v;
        memcpy(&v, buf + o, sizeof(v));
        if (v != file_off + o || memcmp(buf + o + sizeof(v), ref + sizeof(v), VERIFY_BLOCK - sizeof(v)) != 0) {
            if (bad == 0) {
                *first_bad = file_off + o;
            }
            bad++;
        }
    }
    return bad;
}

static void progress(const char *what, uint64_t done, uint64_t total, int64_t *t_next)
{
    const int64_t now = esp_timer_get_time();
    if (now >= *t_next) {
        printf("  ... %s %" PRIu64 " / %" PRIu64 " MiB\n", what, done / MIB, total / MIB);
        *t_next = now + PROGRESS_US;
    }
}

esp_err_t bench_file(sdmmc_card_t *card, uint32_t size_mib, uint32_t buf_kib, bool verify, bool keep)
{
    ESP_RETURN_ON_FALSE(msc_ctrl_fs_mounted(), ESP_ERR_INVALID_STATE, TAG,
                        "no filesystem mounted at %s, run 'mount' first", MSC_BASE_PATH);
    ESP_RETURN_ON_FALSE(size_mib > 0 && buf_kib > 0, ESP_ERR_INVALID_ARG, TAG, "size and buffer must be > 0");
    ESP_RETURN_ON_FALSE((buf_kib * 1024u) % VERIFY_BLOCK == 0, ESP_ERR_INVALID_ARG, TAG, "bad buffer size");

    const uint64_t total = (uint64_t)size_mib * MIB;
    const size_t bsz = (size_t)buf_kib * 1024;

    unlink(BENCH_FILE);     // leftover from an earlier run with -k
    uint64_t fs_total = 0, fs_free = 0;
    ESP_RETURN_ON_ERROR(esp_vfs_fat_info(MSC_BASE_PATH, &fs_total, &fs_free), TAG, "filesystem info");
    ESP_RETURN_ON_FALSE(fs_free >= total + MIB, ESP_ERR_INVALID_SIZE, TAG,
                        "not enough free space (%.1f MB free)", fs_free / 1e6);

    bench_buf_t buf;
    ESP_RETURN_ON_ERROR(buf_alloc(card, bsz, &buf), TAG, "buffer");
    uint8_t ref[VERIFY_BLOCK];
    make_ref_block(ref);
    for (size_t o = 0; o < bsz; o += VERIFY_BLOCK) {
        memcpy(buf.p + o, ref, VERIFY_BLOCK);
    }

    printf("File benchmark: %s, %u MiB, %u KiB per call, buffer in %s (%s)\n", BENCH_FILE,
           (unsigned)size_mib, (unsigned)buf_kib, buf.where,
           buf.dma_ok ? "DMA direct" : "not DMA aligned");

    esp_err_t ret = ESP_OK;
    int64_t t_next = esp_timer_get_time() + PROGRESS_US;

    // ---- write (timed: write() calls, fsync() and close()) ----
    int fd = open(BENCH_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("  open for write failed: %s\n", strerror(errno));
        buf_free(&buf);
        return ESP_FAIL;
    }
    uint64_t done = 0;
    int64_t io_us = 0;
    while (done < total) {
        const size_t n = (size_t)MIN((uint64_t)bsz, total - done);
        stamp_blocks(buf.p, n, done);
        const int64_t t0 = esp_timer_get_time();
        const ssize_t w = write(fd, buf.p, n);
        io_us += esp_timer_get_time() - t0;
        if (w != (ssize_t)n) {
            printf("  write failed at %" PRIu64 " B: %s\n", done, w < 0 ? strerror(errno) : "short write");
            ret = ESP_FAIL;
            break;
        }
        done += n;
        progress("written", done, total, &t_next);
    }
    int64_t t0 = esp_timer_get_time();
    if (fsync(fd) != 0 && ret == ESP_OK) {
        printf("  fsync failed: %s\n", strerror(errno));
        ret = ESP_FAIL;
    }
    close(fd);
    io_us += esp_timer_get_time() - t0;
    if (ret == ESP_OK) {
        printf("  write : %8.2f MB/s  (%.1f MiB in %.2f s)\n", rate_mbps(done, io_us),
               done / (double)MIB, io_us / 1e6);
    }

    // ---- read (timed: read() calls only), then verify ----
    if (ret == ESP_OK) {
        fd = open(BENCH_FILE, O_RDONLY);
        if (fd < 0) {
            printf("  open for read failed: %s\n", strerror(errno));
            ret = ESP_FAIL;
        }
    }
    if (ret == ESP_OK) {
        uint32_t bad = 0;
        uint64_t first_bad = 0;
        done = 0;
        io_us = 0;
        t_next = esp_timer_get_time() + PROGRESS_US;
        while (done < total) {
            const size_t n = (size_t)MIN((uint64_t)bsz, total - done);
            t0 = esp_timer_get_time();
            const ssize_t r = read(fd, buf.p, n);
            io_us += esp_timer_get_time() - t0;
            if (r != (ssize_t)n) {
                printf("  read failed at %" PRIu64 " B: %s\n", done, r < 0 ? strerror(errno) : "short read");
                ret = ESP_FAIL;
                break;
            }
            if (verify) {
                uint64_t fb = 0;
                const uint32_t b = check_blocks(buf.p, n, done, ref, &fb);
                if (b && bad == 0) {
                    first_bad = fb;
                }
                bad += b;
            }
            done += n;
            progress("read", done, total, &t_next);
        }
        close(fd);
        if (ret == ESP_OK) {
            printf("  read  : %8.2f MB/s  (%.1f MiB in %.2f s)\n", rate_mbps(done, io_us),
                   done / (double)MIB, io_us / 1e6);
            if (verify) {
                if (bad == 0) {
                    printf("  verify: OK\n");
                } else {
                    printf("  verify: FAILED, %" PRIu32 " bad 512 B blocks, first at byte %" PRIu64 "\n",
                           bad, first_bad);
                    ret = ESP_FAIL;
                }
            }
        }
    }

    if (keep) {
        printf("  test file kept: %s\n", BENCH_FILE);
    } else {
        unlink(BENCH_FILE);
    }
    buf_free(&buf);
    return ret;
}

/* ------------------------------------------------------------------------------------------ */
/* Filesystem info / format                                                                   */
/* ------------------------------------------------------------------------------------------ */

static const char *fs_type_name(BYTE t)
{
    switch (t) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "exFAT";
    default:       return "unknown";
    }
}

/*
 * FatFs drive of the card. While the application has the filesystem mounted, esp_tinyusb owns
 * the drive. Otherwise the card is not registered and a temporary drive is used.
 *
 * ff_diskio_get_pdrv_card() is trusted only while mounted: ff_diskio_unregister() keeps the
 * card pointer in the diskio_sdmmc table, so the lookup can return a drive that is no longer
 * registered. temp_drive_release() clears that entry as well.
 */
static esp_err_t card_drive_get(sdmmc_card_t *card, BYTE *pdrv, bool *temp)
{
    if (msc_ctrl_fs_mounted()) {
        *pdrv = ff_diskio_get_pdrv_card(card);
        *temp = false;
        ESP_RETURN_ON_FALSE(*pdrv != NO_DRIVE, ESP_ERR_INVALID_STATE, TAG, "mounted card has no FatFs drive");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ff_diskio_get_drive(pdrv), TAG, "no free FatFs drive");
    ff_diskio_register_sdmmc(*pdrv, card);
    *temp = true;
    return ESP_OK;
}

static void temp_drive_release(BYTE pdrv)
{
    ff_diskio_register_sdmmc(pdrv, NULL);   // forget the card pointer
    ff_diskio_unregister(pdrv);
}

void bench_print_fs_info(sdmmc_card_t *card)
{
    BYTE pdrv = NO_DRIVE;
    bool temp_drive = false;
    if (card_drive_get(card, &pdrv, &temp_drive) != ESP_OK) {
        printf("Filesystem: no FatFs drive available\n");
        return;
    }
    const bool app_mounted = !temp_drive;
    FATFS *tmp_fs = NULL;
    if (temp_drive) {
        // Read-only probe on the temporary drive
        tmp_fs = heap_caps_calloc(1, sizeof(FATFS), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!tmp_fs) {
            printf("Filesystem: no memory for a probe\n");
            temp_drive_release(pdrv);
            return;
        }
    }

    const char drv[3] = { (char)('0' + pdrv), ':', 0 };
    FRESULT fr = FR_OK;
    if (tmp_fs) {
        fr = f_mount(tmp_fs, drv, 1);
    }
    if (fr == FR_OK) {
        FATFS *fs = NULL;
        DWORD free_clst = 0;
        fr = f_getfree(drv, &free_clst, &fs);
        if (fr == FR_OK) {
            const uint64_t clst_bytes = (uint64_t)fs->csize * 512u;
            const uint64_t total = (uint64_t)(fs->n_fatent - 2) * clst_bytes;
            const uint64_t free_b = (uint64_t)free_clst * clst_bytes;
            const uint32_t au_sect = card->ssr.alloc_unit_kb * 2u;
            const uint64_t data_lba = (uint64_t)fs->database;
            printf("Filesystem: %s, cluster %" PRIu64 " KiB, size %.2f GB, free %.2f GB%s\n",
                   fs_type_name(fs->fs_type), clst_bytes / 1024, total / 1e9, free_b / 1e9,
                   app_mounted ? " (mounted at " MSC_BASE_PATH ")" : " (probe)");
            if (au_sect) {
                printf("            data area at sector %" PRIu64 ", %s the %" PRIu32 " KiB AU boundary\n",
                       data_lba, (data_lba % au_sect) == 0 ? "on" : "NOT on", (uint32_t)card->ssr.alloc_unit_kb);
            }
        }
    }
    if (fr != FR_OK) {
        printf("Filesystem: no FAT/exFAT volume found (FRESULT %d)\n", (int)fr);
    }

    if (tmp_fs) {
        f_mount(NULL, drv, 0);
        heap_caps_free(tmp_fs);
    }
    if (temp_drive) {
        temp_drive_release(pdrv);
    }
}

esp_err_t bench_format(sdmmc_card_t *card, bool exfat, uint32_t cluster_kib)
{
    ESP_RETURN_ON_FALSE(!msc_ctrl_host_owns(), ESP_ERR_INVALID_STATE, TAG,
                        "the USB host owns the card, run 'mount' first");

    BYTE pdrv = NO_DRIVE;
    bool temp_drive = false;
    ESP_RETURN_ON_ERROR(card_drive_get(card, &pdrv, &temp_drive), TAG, "FatFs drive");
    const char drv[3] = { (char)('0' + pdrv), ':', 0 };

    // f_mkfs writes the FAT/bitmap through this buffer: keep it DMA capable and large.
    bench_buf_t work;
    esp_err_t ret = buf_alloc(card, 64 * 1024, &work);
    if (ret == ESP_OK) {
        const MKFS_PARM opt = {
            .fmt = exfat ? FM_EXFAT : FM_FAT32,
            .n_fat = exfat ? 1 : 2,
            .align = FS_ALIGN_SECTORS,
            .n_root = 0,
            .au_size = cluster_kib * 1024u,
        };
        printf("Formatting the whole card as %s, cluster %u KiB, data area on a 4 MiB boundary...\n",
               exfat ? "exFAT" : "FAT32", (unsigned)cluster_kib);
        const int64_t t0 = esp_timer_get_time();
        const FRESULT fr = f_mkfs(drv, &opt, work.p, work.size);
        if (fr == FR_OK) {
            printf("Format done in %.1f s\n", (esp_timer_get_time() - t0) / 1e6);
        } else {
            printf("f_mkfs failed (FRESULT %d)\n", (int)fr);
            ret = ESP_FAIL;
        }
        buf_free(&work);
    }

    if (temp_drive) {
        temp_drive_release(pdrv);
    }
    return ret;
}
