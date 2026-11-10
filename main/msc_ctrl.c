/*
 * USB MSC for the microSD card on the ESP32-S31 High-Speed OTG port.
 *
 * esp_tinyusb provides the SCSI callbacks and the SD/MMC storage backend. A few symbols are
 * wrapped with the linker (--wrap, see main/CMakeLists.txt):
 *
 *   tud_msc_test_unit_ready_cb / tud_msc_read10_cb / tud_msc_write10_cb
 *     - count the bytes moved per direction and time each transfer session,
 *     - refuse host access ("medium not present") while the application owns the card,
 *     - report "medium may have changed" once when the card is handed back to the host.
 *
 *   sdmmc_read_sectors / sdmmc_write_sectors
 *     - measure the time the SD bus is busy (SD busy % = SD time / wall time),
 *     - copy unaligned buffers through one cache-aligned DMA buffer. The esp_tinyusb write
 *       buffer sits at a 4-byte aligned offset inside its storage object; without the copy the
 *       driver would split a 32 KB WRITE(10) into several SD commands.
 *
 * Card ownership is switched only by msc_ctrl_take() / msc_ctrl_expose() (esp_tinyusb auto
 * mount on USB connect/disconnect is turned off), and the card is never formatted implicitly.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "soc/soc_caps.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tusb.h"
#include "msc_ctrl.h"
#if CONFIG_BENCH_MSC_FAST_IO
#include "msc_io.h"
#endif

static const char *TAG = "msc";

#define SCSI_ASC_MEDIUM_NOT_PRESENT     0x3A
#define SCSI_ASC_MEDIUM_MAY_HAVE_CHANGED 0x28   // "not ready to ready change, medium may have changed"
#define SCSI_ASC_WRITE_ERROR            0x0C
#define SCSI_OP_PREVENT_ALLOW_REMOVAL   0x1E
#define SCSI_OP_SYNCHRONIZE_CACHE_10    0x35
#define SCSI_OP_SYNCHRONIZE_CACHE_16    0x91
#define MSC_BLOCK                       512u
#define TAKE_DRAIN_MS                   100     // let a deferred WRITE(10) in the TinyUSB task finish
#define HOST_RECENT_IO_US               (3 * 1000 * 1000LL)
#define SESSION_MIN_REPORT_BYTES        (1000 * 1000ULL)

enum { DIR_RD = 0, DIR_WR = 1 };

typedef struct {
    int64_t t_first;    // start of the session [us], 0 = idle
    int64_t t_last;     // end of the last transfer [us]
    uint64_t bytes;
} session_t;

static sdmmc_card_t *s_card;
static tinyusb_msc_storage_handle_t s_storage;
static volatile bool s_host_access;     // true: the USB host may access the card
static volatile bool s_media_changed;   // report UNIT ATTENTION once after expose
static bool s_fs_mounted;               // filesystem mounted at MSC_BASE_PATH
static volatile bool s_report = true;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static msc_counters_t s_cnt;
static session_t s_sess[2];
static int64_t s_last_host_io_us;

static uint8_t *s_bounce;
static size_t s_bounce_size;
static SemaphoreHandle_t s_bounce_mutex;

/* ------------------------------------------------------------------------------------------ */
/* Linker wraps                                                                               */
/* ------------------------------------------------------------------------------------------ */

bool __real_tud_msc_test_unit_ready_cb(uint8_t lun);
int32_t __real_tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
int32_t __real_tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
int32_t __real_tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize);
int32_t __wrap_tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize);
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start_sector, size_t sector_count);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start_sector, size_t sector_count);

bool __wrap_tud_msc_test_unit_ready_cb(uint8_t lun);
int32_t __wrap_tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
int32_t __wrap_tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start_sector, size_t sector_count);
esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start_sector, size_t sector_count);

static inline void session_add(session_t *s, uint64_t bytes, int64_t t_start, int64_t t_end)
{
    if (s->t_first == 0) {
        s->t_first = t_start;
        s->bytes = 0;
    }
    s->bytes += bytes;
    s->t_last = t_end;
}

static void host_refused(uint8_t lun)
{
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, SCSI_ASC_MEDIUM_NOT_PRESENT, 0x00);
    portENTER_CRITICAL(&s_lock);
    s_cnt.host_rejects++;
    portEXIT_CRITICAL(&s_lock);
}

bool __wrap_tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!s_host_access) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, SCSI_ASC_MEDIUM_NOT_PRESENT, 0x00);
        return false;
    }
    if (s_media_changed) {
        s_media_changed = false;
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, SCSI_ASC_MEDIUM_MAY_HAVE_CHANGED, 0x00);
        return false;
    }
    return __real_tud_msc_test_unit_ready_cb(lun);
}

int32_t __wrap_tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    if (!s_host_access) {
        host_refused(lun);
        return -1;
    }
    const int64_t t0 = esp_timer_get_time();
    int32_t ret;
#if CONFIG_BENCH_MSC_FAST_IO
    if (offset == 0 && bufsize % MSC_BLOCK == 0) {
        ret = msc_io_read(lba, buffer, bufsize);
        if (ret < 0) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);   // unrecovered read error
        }
    } else {
        msc_io_flush();     // unusual partial-block access: use the esp_tinyusb path
        ret = __real_tud_msc_read10_cb(lun, lba, offset, buffer, bufsize);
    }
#else
    ret = __real_tud_msc_read10_cb(lun, lba, offset, buffer, bufsize);
#endif
    const int64_t t1 = esp_timer_get_time();
    if (ret > 0) {
        portENTER_CRITICAL(&s_lock);
        s_cnt.usb_rd_bytes += (uint32_t)ret;
        session_add(&s_sess[DIR_RD], (uint32_t)ret, t0, t1);
        s_last_host_io_us = t1;
        portEXIT_CRITICAL(&s_lock);
    }
    return ret;
}

int32_t __wrap_tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    if (!s_host_access) {
        host_refused(lun);
        return -1;
    }
    const int64_t t0 = esp_timer_get_time();
    int32_t ret;
#if CONFIG_BENCH_MSC_FAST_IO
    if (offset == 0 && bufsize % MSC_BLOCK == 0) {
        ret = msc_io_write(lba, buffer, bufsize);
        if (ret < 0) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, SCSI_ASC_WRITE_ERROR, 0x00);
        }
    } else {
        msc_io_flush();     // unusual partial-block access: use the esp_tinyusb path
        msc_io_invalidate();
        ret = __real_tud_msc_write10_cb(lun, lba, offset, buffer, bufsize);
    }
#else
    ret = __real_tud_msc_write10_cb(lun, lba, offset, buffer, bufsize);
#endif
    const int64_t t1 = esp_timer_get_time();
    if (ret > 0) {
        portENTER_CRITICAL(&s_lock);
        s_cnt.usb_wr_bytes += (uint32_t)ret;
        session_add(&s_sess[DIR_WR], (uint32_t)ret, t0, t1);
        s_last_host_io_us = t1;
        portEXIT_CRITICAL(&s_lock);
    }
    return ret;
}

// SCSI commands without a TinyUSB built-in handler. With write merging, SYNCHRONIZE CACHE must
// put all accepted data on the card before the good status. Windows sends PREVENT ALLOW MEDIUM
// REMOVAL when the drive is ejected, so write back there as well.
int32_t __wrap_tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
#if CONFIG_BENCH_MSC_FAST_IO
    const uint8_t op = scsi_cmd[0];
    if (op == SCSI_OP_SYNCHRONIZE_CACHE_10 || op == SCSI_OP_SYNCHRONIZE_CACHE_16 ||
            op == SCSI_OP_PREVENT_ALLOW_REMOVAL) {
        const esp_err_t err = msc_io_flush();
        if (err != ESP_OK) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, SCSI_ASC_WRITE_ERROR, 0x00);
            return -1;
        }
        if (op != SCSI_OP_PREVENT_ALLOW_REMOVAL) {
            return 0;
        }
    }
#endif
    return __real_tud_msc_scsi_cb(lun, scsi_cmd, buffer, bufsize);
}

static inline bool sd_buffer_dma_ok(const sdmmc_card_t *card, const void *buf, size_t len)
{
    bool ok = card->host.check_buffer_alignment(card->host.slot, buf, len);
#if !SOC_SDMMC_PSRAM_DMA_CAPABLE
    ok = ok && !esp_ptr_external_ram(buf);
#endif
    return ok;
}

static inline bool use_bounce(const sdmmc_card_t *card, const void *buf, size_t len)
{
    return s_bounce && len > 0 && len <= s_bounce_size && !sd_buffer_dma_ok(card, buf, len);
}

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start_sector, size_t sector_count)
{
    const size_t len = sector_count * card->csd.sector_size;
    const bool bounce = use_bounce(card, dst, len);
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err;
    if (bounce) {
        xSemaphoreTake(s_bounce_mutex, portMAX_DELAY);
        err = __real_sdmmc_read_sectors(card, s_bounce, start_sector, sector_count);
        if (err == ESP_OK) {
            memcpy(dst, s_bounce, len);
        }
        xSemaphoreGive(s_bounce_mutex);
    } else {
        err = __real_sdmmc_read_sectors(card, dst, start_sector, sector_count);
    }
    const int64_t t1 = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    s_cnt.sd_rd_us += (uint64_t)(t1 - t0);
    s_cnt.sd_rd_cmds++;
    if (err == ESP_OK) {
        s_cnt.sd_rd_bytes += len;
    } else {
        s_cnt.sd_rd_err++;
    }
    if (bounce) {
        s_cnt.bounce_rd++;
    }
    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start_sector, size_t sector_count)
{
    const size_t len = sector_count * card->csd.sector_size;
    const bool bounce = use_bounce(card, src, len);
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err;
    if (bounce) {
        xSemaphoreTake(s_bounce_mutex, portMAX_DELAY);
        memcpy(s_bounce, src, len);
        err = __real_sdmmc_write_sectors(card, s_bounce, start_sector, sector_count);
        xSemaphoreGive(s_bounce_mutex);
    } else {
        err = __real_sdmmc_write_sectors(card, src, start_sector, sector_count);
    }
    const int64_t t1 = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    s_cnt.sd_wr_us += (uint64_t)(t1 - t0);
    s_cnt.sd_wr_cmds++;
    if (err == ESP_OK) {
        s_cnt.sd_wr_bytes += len;
    } else {
        s_cnt.sd_wr_err++;
    }
    if (bounce) {
        s_cnt.bounce_wr++;
    }
    // Host writes reach the card later (deferred in the TinyUSB task): extend the session.
    if (s_host_access && s_sess[DIR_WR].t_first != 0) {
        s_sess[DIR_WR].t_last = t1;
    }
    portEXIT_CRITICAL(&s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD write failed at sector %u (+%u): %s",
                 (unsigned)start_sector, (unsigned)sector_count, esp_err_to_name(err));
    }
    return err;
}

/* ------------------------------------------------------------------------------------------ */
/* Statistics task                                                                            */
/* ------------------------------------------------------------------------------------------ */

static void print_session(const char *name, const session_t *s)
{
    const double secs = (s->t_last - s->t_first) / 1e6;
    if (s->bytes < SESSION_MIN_REPORT_BYTES || secs <= 0) {
        return;
    }
    printf("[msc] %s session: %.1f MB in %.2f s -> %.2f MB/s\n",
           name, s->bytes / 1e6, secs, s->bytes / secs / 1e6);
}

static void stats_task(void *arg)
{
    (void)arg;
    msc_counters_t prev;
    msc_ctrl_get_counters(&prev);
    int64_t t_prev = esp_timer_get_time();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BENCH_STATS_PERIOD_MS));

        msc_counters_t now;
        session_t done[2];
        bool closed[2] = { false, false };
        const int64_t t_now = esp_timer_get_time();

        portENTER_CRITICAL(&s_lock);
        now = s_cnt;
        for (int d = 0; d < 2; d++) {
            if (s_sess[d].t_first != 0 &&
                    (t_now - s_sess[d].t_last) > (int64_t)CONFIG_BENCH_STATS_IDLE_MS * 1000) {
                done[d] = s_sess[d];
                closed[d] = true;
                memset(&s_sess[d], 0, sizeof(s_sess[d]));
            }
        }
        portEXIT_CRITICAL(&s_lock);

        // Counters were reset in between: start over.
        if (now.usb_rd_bytes < prev.usb_rd_bytes || now.usb_wr_bytes < prev.usb_wr_bytes ||
                now.sd_rd_us < prev.sd_rd_us || now.sd_wr_us < prev.sd_wr_us) {
            prev = now;
            t_prev = t_now;
            continue;
        }

        const double dt = (t_now - t_prev) / 1e6;
        const uint64_t d_rd = now.usb_rd_bytes - prev.usb_rd_bytes;
        const uint64_t d_wr = now.usb_wr_bytes - prev.usb_wr_bytes;
        if (s_report && (d_rd || d_wr) && dt > 0) {
            printf("[msc] RD %7.2f MB/s (SD busy %3.0f%%) | WR %7.2f MB/s (SD busy %3.0f%%)\n",
                   d_rd / dt / 1e6, (now.sd_rd_us - prev.sd_rd_us) / (dt * 1e4),
                   d_wr / dt / 1e6, (now.sd_wr_us - prev.sd_wr_us) / (dt * 1e4));
        }
        if (closed[DIR_RD]) {
            print_session("READ ", &done[DIR_RD]);
        }
        if (closed[DIR_WR]) {
            print_session("WRITE", &done[DIR_WR]);
        }
        prev = now;
        t_prev = t_now;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Ownership                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static bool fs_probe_mounted(void)
{
    DIR *dir = opendir(MSC_BASE_PATH);
    if (!dir) {
        return false;
    }
    closedir(dir);
    return true;
}

// esp_tinyusb keeps the "application" state after a failed mount (no filesystem). Leaving that
// state logs unmount errors that mean nothing here, so silence them for the transition.
static void storage_set_usb_quiet(void)
{
    const esp_log_level_t lvl_storage = esp_log_level_get("storage_sdmmc");
    const esp_log_level_t lvl_msc = esp_log_level_get("tinyusb_msc_storage");
    esp_log_level_set("storage_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("tinyusb_msc_storage", ESP_LOG_NONE);
    tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_USB);
    esp_log_level_set("storage_sdmmc", lvl_storage);
    esp_log_level_set("tinyusb_msc_storage", lvl_msc);
}

static void storage_mount_app(void)
{
    tinyusb_msc_set_storage_mount_point(s_storage, TINYUSB_MSC_STORAGE_MOUNT_APP);
    s_fs_mounted = fs_probe_mounted();
    if (s_fs_mounted) {
        ESP_LOGI(TAG, "filesystem mounted at %s", MSC_BASE_PATH);
    } else {
        ESP_LOGW(TAG, "no FAT/exFAT filesystem on the card: use 'format' or format it on the PC");
    }
}

esp_err_t msc_ctrl_take(bool *fs_mounted)
{
    ESP_RETURN_ON_FALSE(s_storage, ESP_ERR_INVALID_STATE, TAG, "MSC not initialized");

    if (s_host_access) {
        int64_t last_io;
        portENTER_CRITICAL(&s_lock);
        last_io = s_last_host_io_us;
        portEXIT_CRITICAL(&s_lock);
        if (tud_mounted() && last_io != 0 && esp_timer_get_time() - last_io < HOST_RECENT_IO_US) {
            ESP_LOGW(TAG, "the host accessed the card %lld ms ago; eject the drive on the PC first",
                     (long long)((esp_timer_get_time() - last_io) / 1000));
        }
        s_host_access = false;  // from now on host commands get "medium not present"
        vTaskDelay(pdMS_TO_TICKS(TAKE_DRAIN_MS));
#if CONFIG_BENCH_MSC_FAST_IO
        const esp_err_t werr = msc_io_flush();      // merged host data goes to the card first
        if (werr != ESP_OK) {
            ESP_LOGE(TAG, "write-back of host data failed: %s", esp_err_to_name(werr));
        }
        msc_io_invalidate();
#endif
        ESP_LOGI(TAG, "card taken from the USB host");
    }

    tinyusb_msc_mount_point_t mp = TINYUSB_MSC_STORAGE_MOUNT_USB;
    tinyusb_msc_get_storage_mount_point(s_storage, &mp);
    if (mp != TINYUSB_MSC_STORAGE_MOUNT_APP || !s_fs_mounted) {
        if (mp == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            storage_set_usb_quiet();
        }
        storage_mount_app();
    }
    if (fs_mounted) {
        *fs_mounted = s_fs_mounted;
    }
    return ESP_OK;
}

esp_err_t msc_ctrl_remount(bool *fs_mounted)
{
    ESP_RETURN_ON_FALSE(s_storage, ESP_ERR_INVALID_STATE, TAG, "MSC not initialized");
    ESP_RETURN_ON_FALSE(!s_host_access, ESP_ERR_INVALID_STATE, TAG, "the USB host owns the card");

    // The host stays blocked (s_host_access == false) while esp_tinyusb passes the USB state.
    storage_set_usb_quiet();
    s_fs_mounted = false;
    storage_mount_app();
    if (fs_mounted) {
        *fs_mounted = s_fs_mounted;
    }
    return ESP_OK;
}

esp_err_t msc_ctrl_expose(void)
{
    ESP_RETURN_ON_FALSE(s_storage, ESP_ERR_INVALID_STATE, TAG, "MSC not initialized");
    if (s_host_access) {
        return ESP_OK;
    }
    // Unmounts the filesystem from /sdcard; all files must be closed (the benchmarks close theirs).
    storage_set_usb_quiet();
    s_fs_mounted = false;
#if CONFIG_BENCH_MSC_FAST_IO
    msc_io_invalidate();    // the application may have changed the card
#endif
    s_media_changed = true;
    s_host_access = true;
    ESP_LOGI(TAG, "card exposed to the USB host");
    return ESP_OK;
}

bool msc_ctrl_host_owns(void)
{
    return s_host_access;
}

bool msc_ctrl_fs_mounted(void)
{
    return !s_host_access && s_fs_mounted;
}

bool msc_ctrl_usb_connected(void)
{
    return tud_mounted();
}

const char *msc_ctrl_usb_speed_name(void)
{
    if (!tud_mounted()) {
        return "not connected";
    }
    switch (tud_speed_get()) {
    case TUSB_SPEED_HIGH:
        return "High-Speed (480 Mbit/s)";
    case TUSB_SPEED_FULL:
        return "Full-Speed (12 Mbit/s)";
    default:
        return "unknown speed";
    }
}

void msc_ctrl_get_counters(msc_counters_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_cnt;
    portEXIT_CRITICAL(&s_lock);
}

void msc_ctrl_reset_counters(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_cnt, 0, sizeof(s_cnt));
    memset(s_sess, 0, sizeof(s_sess));
    portEXIT_CRITICAL(&s_lock);
#if CONFIG_BENCH_MSC_FAST_IO
    msc_io_reset_counters();
#endif
}

void msc_ctrl_set_report(bool on)
{
    s_report = on;
}

bool msc_ctrl_get_report(void)
{
    return s_report;
}

size_t msc_ctrl_bounce_size(void)
{
    return s_bounce ? s_bounce_size : 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Init                                                                                       */
/* ------------------------------------------------------------------------------------------ */

static void storage_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;
    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
    case TINYUSB_MSC_EVENT_FORMAT_FAILED:
        ESP_LOGD(TAG, "storage event %d", event->id);
        break;
    default:
        break;
    }
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "USB host connected, %s", msc_ctrl_usb_speed_name());
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "USB host disconnected");
        break;
    default:
        break;
    }
}

esp_err_t msc_ctrl_init(sdmmc_card_t *card, bool expose)
{
    ESP_RETURN_ON_FALSE(card, ESP_ERR_INVALID_ARG, TAG, "card is NULL");
    ESP_RETURN_ON_FALSE(!s_storage, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    s_card = card;
    s_host_access = false;

    s_bounce_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_bounce_mutex, ESP_ERR_NO_MEM, TAG, "no memory for mutex");
    s_bounce_size = CONFIG_TINYUSB_MSC_BUFSIZE;
    s_bounce = heap_caps_malloc(s_bounce_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_bounce) {
        ESP_LOGW(TAG, "no internal DMA memory for the %u B bounce buffer", (unsigned)s_bounce_size);
    }
#if CONFIG_BENCH_MSC_FAST_IO
    ESP_RETURN_ON_ERROR(msc_io_init(card), TAG, "MSC I/O engine");
#endif

    const tinyusb_msc_driver_config_t drv_cfg = {
        .user_flags = {
            .auto_mount_off = 1,    // ownership changes only through msc_ctrl_take/expose
        },
        .callback = storage_event_cb,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&drv_cfg), TAG, "MSC driver install");

    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium.card = card,
        .fat_fs = {
            .base_path = (char *)MSC_BASE_PATH,
            .config = {
                .format_if_mount_failed = false,
                .max_files = 4,
                .allocation_unit_size = 0,
            },
            .do_not_format = true,  // never format the card on a mount failure
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage), TAG, "MSC storage");

    // ESP32-S31: port 0 is the High-Speed OTG port (UTMI PHY). Default descriptors from
    // esp_tinyusb (strings from menuconfig), MSC bulk endpoints 512 B in High-Speed.
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB driver install");

    BaseType_t ok = xTaskCreate(stats_task, "msc_stats", 4096, NULL, 2, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "stats task");

    if (expose) {
        s_host_access = true;
        ESP_LOGI(TAG, "card exposed to the USB host");
    } else {
        ESP_RETURN_ON_ERROR(msc_ctrl_take(NULL), TAG, "take card");
    }
    return ESP_OK;
}
