/*
 * Console commands for the SD/MSC benchmark.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "argtable3/argtable3.h"
#include "sd_card.h"
#include "msc_ctrl.h"
#if CONFIG_BENCH_MSC_FAST_IO
#include "msc_io.h"
#endif
#include "bench.h"
#include "cmd_bench.h"

static const char *TAG = "cmd";
static sdmmc_card_t *s_card;

/* ---------------------------------------- info ---------------------------------------- */

static int cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("---- SD card ----\n");
    sd_card_print_info(s_card);
    printf("---- USB ----\n");
    printf("USB device : %s\n", msc_ctrl_usb_speed_name());
    printf("Card owner : %s\n", msc_ctrl_host_owns() ? "USB host (PC)" : "application (" MSC_BASE_PATH ")");
    printf("---- Filesystem ----\n");
    if (msc_ctrl_host_owns()) {
        printf("Filesystem: owned by the USB host, run 'mount' to inspect it here\n");
    } else {
        bench_print_fs_info(s_card);
    }
    printf("---- Memory ----\n");
    printf("free internal %u KiB, free PSRAM %u KiB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return 0;
}

/* ------------------------------------- mount/expose ------------------------------------- */

static int cmd_mount(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bool fs = false;
    if (msc_ctrl_take(&fs) != ESP_OK) {
        return 1;
    }
    if (fs) {
        bench_print_fs_info(s_card);
    }
    return 0;
}

static int cmd_expose(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return msc_ctrl_expose() == ESP_OK ? 0 : 1;
}

/* ---------------------------------------- stats ---------------------------------------- */

static struct {
    struct arg_str *action;
    struct arg_end *end;
} s_stats_args;

static void print_dir(const char *name, uint64_t bytes, uint64_t us, uint32_t cmds, uint32_t err)
{
    printf("  SD %-5s: %10.1f MB in %8" PRIu32 " cmds (avg %6.1f KiB/cmd), busy %7.2f s",
           name, bytes / 1e6, cmds, cmds ? bytes / 1024.0 / cmds : 0.0, us / 1e6);
    if (us) {
        printf(" -> %6.2f MB/s while busy", (double)bytes / (double)us);
    }
    printf(", errors %" PRIu32 "\n", err);
}

static int cmd_stats(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_stats_args) != 0) {
        arg_print_errors(stderr, s_stats_args.end, argv[0]);
        return 1;
    }
    if (s_stats_args.action->count) {
        const char *a = s_stats_args.action->sval[0];
        if (strcmp(a, "on") == 0) {
            msc_ctrl_set_report(true);
        } else if (strcmp(a, "off") == 0) {
            msc_ctrl_set_report(false);
        } else if (strcmp(a, "reset") == 0) {
            msc_ctrl_reset_counters();
            printf("counters cleared\n");
            return 0;
        } else {
            printf("unknown action '%s' (on | off | reset)\n", a);
            return 1;
        }
    }

    msc_counters_t c;
    msc_ctrl_get_counters(&c);
    printf("  USB      : read %.1f MB, write %.1f MB (host traffic)\n", c.usb_rd_bytes / 1e6, c.usb_wr_bytes / 1e6);
    print_dir("read", c.sd_rd_bytes, c.sd_rd_us, c.sd_rd_cmds, c.sd_rd_err);
    print_dir("write", c.sd_wr_bytes, c.sd_wr_us, c.sd_wr_cmds, c.sd_wr_err);
    printf("  bounce buffer %u KiB: %" PRIu32 " reads, %" PRIu32 " writes copied\n",
           (unsigned)(msc_ctrl_bounce_size() / 1024), c.bounce_rd, c.bounce_wr);
    printf("  host requests refused while the application owned the card: %" PRIu32 "\n", c.host_rejects);
#if CONFIG_BENCH_MSC_FAST_IO
    msc_io_counters_t io;
    msc_io_get_counters(&io);
    printf("  MSC engine: %s\n", msc_io_describe());
    printf("    read : %" PRIu32 " hits, %" PRIu32 " misses, %" PRIu32 " waits, %" PRIu32
           " prefetches (%" PRIu32 " dropped)\n",
           io.ra_hits, io.ra_misses, io.ra_waits, io.ra_prefetches, io.ra_dropped);
    printf("    write: %" PRIu32 " SD commands (avg %.1f KiB), %" PRIu32 " breaks, %" PRIu32
           " idle, %" PRIu32 " read, %" PRIu32 " sync write-backs, %" PRIu32 " host waits, %" PRIu32 " errors\n",
           io.wb_cmds, io.wb_cmds ? io.wb_bytes / 1024.0 / io.wb_cmds : 0.0, io.wb_breaks,
           io.wb_idle_flushes, io.wb_read_flushes, io.wb_sync_flushes, io.wb_waits, io.wb_errors);
#endif
    printf("  periodic report: %s\n", msc_ctrl_get_report() ? "on" : "off");
    return 0;
}

/* --------------------------------------- rawread --------------------------------------- */

static struct {
    struct arg_int *size;
    struct arg_int *chunk;
    struct arg_int *offset;
    struct arg_end *end;
} s_raw_args;

static int cmd_rawread(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_raw_args) != 0) {
        arg_print_errors(stderr, s_raw_args.end, argv[0]);
        return 1;
    }
    const int size = s_raw_args.size->count ? s_raw_args.size->ival[0] : 64;
    const int chunk = s_raw_args.chunk->count ? s_raw_args.chunk->ival[0] : 0;
    const int offset = s_raw_args.offset->count ? s_raw_args.offset->ival[0] : -1;
    if (size <= 0 || chunk < 0 || chunk > 4096) {
        printf("invalid argument (size > 0, 0 <= chunk <= 4096 KiB)\n");
        return 1;
    }
    return bench_raw_read(s_card, (uint32_t)size, (uint32_t)chunk, offset) == ESP_OK ? 0 : 1;
}

/* -------------------------------------- filebench -------------------------------------- */

static struct {
    struct arg_int *size;
    struct arg_int *buf;
    struct arg_lit *no_verify;
    struct arg_lit *keep;
    struct arg_end *end;
} s_file_args;

static int cmd_filebench(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_file_args) != 0) {
        arg_print_errors(stderr, s_file_args.end, argv[0]);
        return 1;
    }
    const int size = s_file_args.size->count ? s_file_args.size->ival[0] : 256;
    const int buf = s_file_args.buf->count ? s_file_args.buf->ival[0] : 256;
    if (size <= 0 || size > 65536 || buf < 1 || buf > 4096) {
        printf("invalid argument (1..65536 MiB, buffer 1..4096 KiB)\n");
        return 1;
    }
    return bench_file(s_card, (uint32_t)size, (uint32_t)buf,
                      s_file_args.no_verify->count == 0, s_file_args.keep->count > 0) == ESP_OK ? 0 : 1;
}

/* --------------------------------------- format --------------------------------------- */

static struct {
    struct arg_str *type;
    struct arg_int *cluster;
    struct arg_lit *yes;
    struct arg_end *end;
} s_fmt_args;

static int cmd_format(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_fmt_args) != 0) {
        arg_print_errors(stderr, s_fmt_args.end, argv[0]);
        return 1;
    }
    const char *type = s_fmt_args.type->count ? s_fmt_args.type->sval[0] : "exfat";
    bool exfat;
    if (strcmp(type, "exfat") == 0) {
        exfat = true;
    } else if (strcmp(type, "fat32") == 0) {
        exfat = false;
    } else {
        printf("unknown type '%s' (exfat | fat32)\n", type);
        return 1;
    }
    const int cluster = s_fmt_args.cluster->count ? s_fmt_args.cluster->ival[0] : (exfat ? 128 : 32);
    const int max_cluster = exfat ? 32768 : 64;
    if (cluster < 4 || cluster > max_cluster || (cluster & (cluster - 1)) != 0) {
        printf("cluster must be a power of two, 4..%d KiB for %s\n", max_cluster, type);
        return 1;
    }
    if (!s_fmt_args.yes->count) {
        printf("This erases the whole card. Repeat with --yes to format.\n");
        return 1;
    }
    if (msc_ctrl_host_owns()) {
        printf("The USB host owns the card. Eject it on the PC and run 'mount' first.\n");
        return 1;
    }
    if (bench_format(s_card, exfat, (uint32_t)cluster) != ESP_OK) {
        return 1;
    }
    bool fs = false;
    msc_ctrl_remount(&fs);
    if (fs) {
        bench_print_fs_info(s_card);
    }
    return fs ? 0 : 1;
}

/* --------------------------------------- restart --------------------------------------- */

static int cmd_restart(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_restart();
    return 0;
}

/* -------------------------------------- register -------------------------------------- */

esp_err_t cmd_bench_register(sdmmc_card_t *card)
{
    s_card = card;

    s_stats_args.action = arg_str0(NULL, NULL, "<on|off|reset>", "periodic report on/off, or clear counters");
    s_stats_args.end = arg_end(2);

    s_raw_args.size = arg_int0("s", "size", "<MiB>", "bytes per run (default 64; a run also ends after 5 s)");
    s_raw_args.chunk = arg_int0("c", "chunk", "<KiB>", "KiB per SD command (default: sweep 4..1024)");
    s_raw_args.offset = arg_int0("o", "offset", "<MiB>", "start offset (default: middle of the card)");
    s_raw_args.end = arg_end(3);

    s_file_args.size = arg_int0("s", "size", "<MiB>", "test file size (default 256)");
    s_file_args.buf = arg_int0("b", "buf", "<KiB>", "bytes per write()/read() call (default 256)");
    s_file_args.no_verify = arg_lit0("n", "no-verify", "skip the data check");
    s_file_args.keep = arg_lit0("k", "keep", "keep " MSC_BASE_PATH "/s31bench.bin");
    s_file_args.end = arg_end(4);

    s_fmt_args.type = arg_str0("t", "type", "<exfat|fat32>", "filesystem (default exfat)");
    s_fmt_args.cluster = arg_int0("c", "cluster", "<KiB>", "cluster size (default 128 exFAT, 32 FAT32)");
    s_fmt_args.yes = arg_lit0(NULL, "yes", "confirm: erase the whole card");
    s_fmt_args.end = arg_end(3);

    const esp_console_cmd_t cmds[] = {
        {
            .command = "info",
            .help = "SD card registers and bus mode, USB link speed, card owner, filesystem",
            .func = &cmd_info,
        },
        {
            .command = "mount",
            .help = "Take the card from the USB host and mount it at " MSC_BASE_PATH
                    " (eject the drive on the PC first)",
            .func = &cmd_mount,
        },
        {
            .command = "expose",
            .help = "Unmount " MSC_BASE_PATH " and hand the card to the USB host",
            .func = &cmd_expose,
        },
        {
            .command = "stats",
            .help = "USB/SD transfer counters; 'stats off' stops the periodic [msc] lines",
            .func = &cmd_stats,
            .argtable = &s_stats_args,
        },
        {
            .command = "rawread",
            .help = "Raw sequential SD read speed (read only, needs 'mount')",
            .func = &cmd_rawread,
            .argtable = &s_raw_args,
        },
        {
            .command = "filebench",
            .help = "Write, read back and verify a test file on FAT/exFAT (needs 'mount')",
            .func = &cmd_filebench,
            .argtable = &s_file_args,
        },
        {
            .command = "format",
            .help = "Format the whole card (MBR, one partition, 4 MiB aligned); needs 'mount' and --yes",
            .func = &cmd_format,
            .argtable = &s_fmt_args,
        },
        {
            .command = "restart",
            .help = "Restart the chip",
            .func = &cmd_restart,
        },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    return ESP_OK;
}
