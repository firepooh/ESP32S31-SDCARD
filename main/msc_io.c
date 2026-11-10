/*
 * MSC I/O engine: read-ahead and write merging between the TinyUSB MSC callbacks and the card.
 *
 * TinyUSB moves MSC data in chunks of CONFIG_TINYUSB_MSC_BUFSIZE (32 KiB) and calls READ(10) /
 * WRITE(10) once per chunk. esp_tinyusb serves every chunk with its own SD command in the TinyUSB
 * task: SD time and USB time add up, and every SD write pays the card's per-command programming
 * time (about 2.2 ms for a 32 KiB write on the tested card). This engine replaces that path:
 *
 *  - Read-ahead (RA, two buffers): once the host reads sequentially, the I/O task reads the next
 *    CONFIG_BENCH_MSC_RA_KB from the card while USB sends the current data. Chunks are then served
 *    with memcpy, so SD reads and USB transfers overlap.
 *  - Write merging (WB, two buffers): chunks with contiguous addresses are collected and the I/O
 *    task writes them as one SD command of up to CONFIG_BENCH_MSC_WB_KB. The host fills the other
 *    buffer meanwhile.
 *
 * Consistency
 *  - Every write drops the read-ahead data; a prefetch in flight is discarded by a generation
 *    counter.
 *  - A read writes back all merged data first.
 *  - Merged data is written back when the host is idle for CONFIG_BENCH_MSC_WB_IDLE_MS, on
 *    SYNCHRONIZE CACHE / PREVENT ALLOW MEDIUM REMOVAL (msc_ctrl.c), and before the application
 *    takes the card.
 *  - This is write-back caching: the host gets a good status before the data is on the card.
 *    Removing power within the idle time can lose up to 2 x CONFIG_BENCH_MSC_WB_KB. A failed
 *    background write is reported on the next WRITE(10) or SYNCHRONIZE CACHE.
 *
 * Threads: TinyUSB task (msc_io_read/write), console task (flush/invalidate), "msc_io" task (SD).
 * All segment state is protected by s_mutex; the I/O task owns a segment's buffer while BUSY.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "msc_io.h"

static const char *TAG = "msc_io";

#define SECT            512u
#define IO_TASK_PRIO    6
#define IO_TASK_CORE    0       // TinyUSB runs on core 1
#define WAIT_TICKS      pdMS_TO_TICKS(1000)
#define NO_SECTOR       UINT32_MAX

#if CONFIG_BENCH_MSC_RA_INTERNAL
#define RA_IN_SRAM      true
#else
#define RA_IN_SRAM      false
#endif

typedef enum {
    SEG_EMPTY = 0,
    SEG_FILL,       // WB: the host is adding chunks
    SEG_BUSY,       // RA: prefetch in flight, WB: write to the card in flight
    SEG_READY,      // RA: data valid
} seg_state_t;

typedef struct {
    uint8_t *buf;
    uint32_t start;         // first sector
    uint32_t count;         // sectors
    seg_state_t state;
    uint32_t gen;           // RA: s_ra_gen when the prefetch started
} seg_t;

typedef enum { JOB_WRITE, JOB_PREFETCH } job_type_t;

typedef struct {
    job_type_t type;
    uint8_t idx;
} job_t;

#define EV_WB(i)        BIT(i)
#define EV_RA(i)        BIT(2 + (i))

static sdmmc_card_t *s_card;
static uint32_t s_card_sectors;
static seg_t s_ra[2];
static seg_t s_wb[2];
static uint32_t s_ra_cap;           // sectors per RA buffer
static uint32_t s_wb_cap;           // sectors per WB buffer
static int s_wb_cur;                // WB buffer the host fills next
static uint32_t s_ra_gen;
static uint32_t s_last_rd_end = NO_SECTOR;
static int64_t s_last_wr_us;
static esp_err_t s_wb_err = ESP_OK; // first background write error not reported yet
static msc_io_counters_t s_cnt;
static char s_desc[96];

static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static QueueHandle_t s_jobs;

static inline void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static inline void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static inline bool ra_valid(const seg_t *s)
{
    return (s->state == SEG_READY || s->state == SEG_BUSY) && s->gen == s_ra_gen;
}

/* ------------------------------------------------------------------------------------------ */
/* Locked helpers                                                                             */
/* ------------------------------------------------------------------------------------------ */

static void wb_submit_locked(int i)
{
    s_wb[i].state = SEG_BUSY;
    xEventGroupClearBits(s_events, EV_WB(i));
    const job_t job = { .type = JOB_WRITE, .idx = (uint8_t)i };
    xQueueSend(s_jobs, &job, portMAX_DELAY);    // at most 4 jobs outstanding, the queue holds 8
}

static void ra_start_locked(int i, uint32_t start)
{
    if (start >= s_card_sectors || s_ra[i].state == SEG_BUSY) {
        return;
    }
    s_ra[i].start = start;
    s_ra[i].count = MIN(s_ra_cap, s_card_sectors - start);
    s_ra[i].state = SEG_BUSY;
    s_ra[i].gen = s_ra_gen;
    s_cnt.ra_prefetches++;
    xEventGroupClearBits(s_events, EV_RA(i));
    const job_t job = { .type = JOB_PREFETCH, .idx = (uint8_t)i };
    xQueueSend(s_jobs, &job, portMAX_DELAY);
}

static void ra_drop_locked(void)
{
    s_ra_gen++;
    for (int i = 0; i < 2; i++) {
        if (s_ra[i].state == SEG_READY) {
            s_ra[i].state = SEG_EMPTY;
        }
    }
    s_last_rd_end = NO_SECTOR;
}

// Write back the buffer being filled and wait for both buffers. Called and returns with the lock.
static void wb_flush_locked(void)
{
    if (s_wb[s_wb_cur].state == SEG_FILL) {
        wb_submit_locked(s_wb_cur);
        s_wb_cur ^= 1;
    }
    for (int i = 0; i < 2; i++) {
        while (s_wb[i].state == SEG_BUSY) {
            unlock();
            xEventGroupWaitBits(s_events, EV_WB(i), pdFALSE, pdTRUE, WAIT_TICKS);
            lock();
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* I/O task                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static void io_task(void *arg)
{
    (void)arg;
    const TickType_t poll = pdMS_TO_TICKS(MAX(1, CONFIG_BENCH_MSC_WB_IDLE_MS / 4));
    const int64_t idle_us = (int64_t)CONFIG_BENCH_MSC_WB_IDLE_MS * 1000;

    while (true) {
        job_t job;
        if (xQueueReceive(s_jobs, &job, poll) == pdTRUE) {
            if (job.type == JOB_WRITE) {
                seg_t *s = &s_wb[job.idx];     // start/count/buf are stable while BUSY
                const esp_err_t err = sdmmc_write_sectors(s_card, s->buf, s->start, s->count);
                lock();
                s_cnt.wb_cmds++;
                s_cnt.wb_bytes += (uint64_t)s->count * SECT;
                if (err != ESP_OK) {
                    s_cnt.wb_errors++;
                    if (s_wb_err == ESP_OK) {
                        s_wb_err = err;
                    }
                    ESP_LOGE(TAG, "write of sectors %" PRIu32 "+%" PRIu32 " failed: %s",
                             s->start, s->count, esp_err_to_name(err));
                }
                s->count = 0;
                s->state = SEG_EMPTY;
                unlock();
                xEventGroupSetBits(s_events, EV_WB(job.idx));
            } else {
                seg_t *s = &s_ra[job.idx];
                const esp_err_t err = sdmmc_read_sectors(s_card, s->buf, s->start, s->count);
                lock();
                if (err == ESP_OK && s->gen == s_ra_gen) {
                    s->state = SEG_READY;
                } else {
                    s->state = SEG_EMPTY;
                    if (err == ESP_OK) {
                        s_cnt.ra_dropped++;
                    }
                }
                unlock();
                xEventGroupSetBits(s_events, EV_RA(job.idx));
            }
            continue;
        }

        // No job: write back a partly filled merge buffer once the host is idle.
        lock();
        if (s_wb[s_wb_cur].state == SEG_FILL && esp_timer_get_time() - s_last_wr_us >= idle_us) {
            s_cnt.wb_idle_flushes++;
            wb_submit_locked(s_wb_cur);
            s_wb_cur ^= 1;
        }
        unlock();
    }
}

/* ------------------------------------------------------------------------------------------ */
/* API                                                                                        */
/* ------------------------------------------------------------------------------------------ */

int32_t msc_io_read(uint32_t sector, void *buf, uint32_t bytes)
{
    const uint32_t n = bytes / SECT;
    lock();
    if (s_wb[0].state != SEG_EMPTY || s_wb[1].state != SEG_EMPTY) {
        s_cnt.wb_read_flushes++;
        wb_flush_locked();
    }

    while (true) {
        int hit = -1;
        for (int i = 0; i < 2; i++) {
            const seg_t *s = &s_ra[i];
            if (ra_valid(s) && sector >= s->start && sector + n <= s->start + s->count) {
                hit = i;
                break;
            }
        }
        if (hit < 0) {
            break;
        }
        seg_t *s = &s_ra[hit];
        if (s->state == SEG_BUSY) {
            s_cnt.ra_waits++;
            unlock();
            xEventGroupWaitBits(s_events, EV_RA(hit), pdFALSE, pdTRUE, WAIT_TICKS);
            lock();
            continue;   // READY now, or dropped by a write
        }
        memcpy(buf, s->buf + (size_t)(sector - s->start) * SECT, bytes);
        s_cnt.ra_hits++;
        s_last_rd_end = sector + n;
        // Stay one buffer ahead: prefetch what follows this buffer into the other one.
        const uint32_t next = s->start + s->count;
        const seg_t *o = &s_ra[hit ^ 1];
        if (!(ra_valid(o) && o->start == next)) {
            ra_start_locked(hit ^ 1, next);
        }
        unlock();
        return (int32_t)bytes;
    }

    // Miss: read this chunk directly, then start read-ahead if the host reads sequentially.
    const bool sequential = (sector == s_last_rd_end);
    s_last_rd_end = sector + n;
    s_cnt.ra_misses++;
    unlock();

    const esp_err_t err = sdmmc_read_sectors(s_card, buf, sector, n);

    if (err == ESP_OK && sequential) {
        lock();
        for (int i = 0; i < 2; i++) {
            if (s_ra[i].state != SEG_BUSY) {
                ra_start_locked(i, sector + n);
                break;
            }
        }
        unlock();
    }
    return err == ESP_OK ? (int32_t)bytes : -1;
}

int32_t msc_io_write(uint32_t sector, const void *buf, uint32_t bytes)
{
    const uint32_t n = bytes / SECT;
    lock();
    if (s_wb_err != ESP_OK) {
        const esp_err_t err = s_wb_err;
        s_wb_err = ESP_OK;
        unlock();
        ESP_LOGE(TAG, "reporting an earlier background write error to the host: %s", esp_err_to_name(err));
        return -1;
    }
    ra_drop_locked();

    seg_t *s = &s_wb[s_wb_cur];
    if (s->state == SEG_FILL && (sector != s->start + s->count || s->count + n > s_wb_cap)) {
        if (sector != s->start + s->count) {
            s_cnt.wb_breaks++;
        }
        wb_submit_locked(s_wb_cur);
        s_wb_cur ^= 1;
        s = &s_wb[s_wb_cur];
    }
    while (s->state == SEG_BUSY) {
        // Both buffers are on their way to the card: the host waits.
        s_cnt.wb_waits++;
        unlock();
        xEventGroupWaitBits(s_events, EV_WB(s_wb_cur), pdFALSE, pdTRUE, WAIT_TICKS);
        lock();
        s = &s_wb[s_wb_cur];
    }
    if (s->state == SEG_EMPTY) {
        s->start = sector;
        s->count = 0;
        s->state = SEG_FILL;
    }
    memcpy(s->buf + (size_t)s->count * SECT, buf, bytes);
    s->count += n;
    s_last_wr_us = esp_timer_get_time();
    if (s->count + n > s_wb_cap) {
        // The next chunk would not fit: write this buffer now.
        wb_submit_locked(s_wb_cur);
        s_wb_cur ^= 1;
    }
    unlock();
    return (int32_t)bytes;
}

esp_err_t msc_io_flush(void)
{
    if (!s_mutex) {
        return ESP_OK;
    }
    lock();
    if (s_wb[0].state != SEG_EMPTY || s_wb[1].state != SEG_EMPTY) {
        s_cnt.wb_sync_flushes++;
    }
    wb_flush_locked();
    const esp_err_t err = s_wb_err;
    s_wb_err = ESP_OK;
    unlock();
    return err;
}

void msc_io_invalidate(void)
{
    if (!s_mutex) {
        return;
    }
    lock();
    ra_drop_locked();
    unlock();
}

void msc_io_get_counters(msc_io_counters_t *out)
{
    if (!s_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_cnt;
    unlock();
}

void msc_io_reset_counters(void)
{
    if (!s_mutex) {
        return;
    }
    lock();
    memset(&s_cnt, 0, sizeof(s_cnt));
    unlock();
}

const char *msc_io_describe(void)
{
    return s_desc;
}

static uint8_t *alloc_buf(size_t size, bool internal, const char **where)
{
    uint8_t *p = NULL;
    if (internal) {
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
        *where = "SRAM";
    }
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        *where = "PSRAM";
    }
    return p;
}

// memcpy cost of one 32 KiB chunk between a buffer and internal RAM (TinyUSB's buffer lives there)
static void log_copy_cost(const uint8_t *ra_buf, uint8_t *wb_buf)
{
    const size_t len = CONFIG_TINYUSB_MSC_BUFSIZE;
    uint8_t *tmp = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!tmp) {
        return;
    }
    memset(tmp, 0x5A, len);
    int64_t t0 = esp_timer_get_time();
    memcpy(tmp, ra_buf, len);
    const int64_t rd_us = esp_timer_get_time() - t0;
    t0 = esp_timer_get_time();
    memcpy(wb_buf, tmp, len);
    const int64_t wr_us = esp_timer_get_time() - t0;
    heap_caps_free(tmp);
    ESP_LOGI(TAG, "memcpy %u KiB: read-ahead -> TinyUSB %lld us, TinyUSB -> merge buffer %lld us",
             (unsigned)(len / 1024), (long long)rd_us, (long long)wr_us);
}

esp_err_t msc_io_init(sdmmc_card_t *card)
{
    ESP_RETURN_ON_FALSE(card, ESP_ERR_INVALID_ARG, TAG, "card is NULL");
    ESP_RETURN_ON_FALSE(card->csd.sector_size == SECT, ESP_ERR_NOT_SUPPORTED, TAG, "sector size must be 512");
    s_card = card;
    s_card_sectors = (uint32_t)card->csd.capacity;
    s_ra_cap = CONFIG_BENCH_MSC_RA_KB * 1024u / SECT;
    s_wb_cap = CONFIG_BENCH_MSC_WB_KB * 1024u / SECT;

    const char *ra_where = "?";
    const char *wb_where = "?";
    for (int i = 0; i < 2; i++) {
        s_ra[i].buf = alloc_buf((size_t)s_ra_cap * SECT, RA_IN_SRAM, &ra_where);
        s_wb[i].buf = alloc_buf((size_t)s_wb_cap * SECT, false, &wb_where);
        ESP_RETURN_ON_FALSE(s_ra[i].buf && s_wb[i].buf, ESP_ERR_NO_MEM, TAG, "no memory for MSC buffers");
    }
    snprintf(s_desc, sizeof(s_desc), "read-ahead 2x%d KiB %s, write merge 2x%d KiB %s, idle %d ms",
             CONFIG_BENCH_MSC_RA_KB, ra_where, CONFIG_BENCH_MSC_WB_KB, wb_where, CONFIG_BENCH_MSC_WB_IDLE_MS);

    s_mutex = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    s_jobs = xQueueCreate(8, sizeof(job_t));
    ESP_RETURN_ON_FALSE(s_mutex && s_events && s_jobs, ESP_ERR_NO_MEM, TAG, "no memory for sync objects");
    BaseType_t ok = xTaskCreatePinnedToCore(io_task, "msc_io", 4096, NULL, IO_TASK_PRIO, NULL, IO_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "no memory for the I/O task");

    ESP_LOGI(TAG, "%s", s_desc);
    log_copy_cost(s_ra[0].buf, s_wb[0].buf);
    return ESP_OK;
}
