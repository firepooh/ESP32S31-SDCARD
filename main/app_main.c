/*
 * ESP32-S31-Korvo-1: microSD card (FAT32/exFAT, 64 GB and larger) as a USB 2.0 High-Speed
 * Mass Storage device, with read/write throughput measurement (ESP-IDF v6.1).
 *
 * Board connections
 *   microSD   : SDMMC slot 0, 4-bit, CLK GPIO24 / CMD GPIO25 / D0-D3 GPIO20-23
 *   USB MSC   : USB 2.0 Type-A port (ESP32-S31 High-Speed OTG, UTMI PHY)
 *   Console   : USB-to-UART Type-C port (UART0, GPIO58/59), 115200 baud
 *
 * The PC measures the MSC throughput (tools/msc_bench.py or CrystalDiskMark). The device
 * prints its own view of the same traffic ([msc] lines) and can benchmark the card locally
 * (rawread, filebench) to separate the SD bus limit from the USB limit.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"
#include "sd_card.h"
#include "msc_ctrl.h"
#include "bench.h"
#include "cmd_bench.h"

static const char *TAG = "app";

static void print_banner(void)
{
    printf("\n"
           "==================================================================\n"
           " ESP32-S31-Korvo-1  microSD over USB High-Speed MSC benchmark\n"
           "------------------------------------------------------------------\n"
           " info                  card, bus mode, USB speed, filesystem\n"
           " stats [on|off|reset]  USB/SD counters, periodic [msc] report\n"
           " mount                 take the card from the PC (eject it first)\n"
           " expose                give the card back to the PC\n"
           " rawread               raw SD read speed sweep (after mount)\n"
           " filebench -s 1024     write/read/verify a 1 GiB file (after mount)\n"
           " format --yes          exFAT, 128 KiB clusters (after mount)\n"
           "==================================================================\n");
}

void app_main(void)
{
    print_banner();

    sdmmc_card_t *card = NULL;
    ESP_ERROR_CHECK(sd_card_init(&card));
    sd_card_print_info(card);
    // Read-only look at the filesystem before the PC can access the card.
    bench_print_fs_info(card);

    ESP_ERROR_CHECK(msc_ctrl_init(card, CONFIG_BENCH_EXPOSE_AT_BOOT));
    ESP_LOGI(TAG, "connect the USB 2.0 Type-A port to the PC; card owner: %s",
             msc_ctrl_host_owns() ? "USB host" : "application");

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "s31>";
    repl_config.max_cmdline_length = 128;
    repl_config.task_stack_size = 8192;
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    ESP_ERROR_CHECK(esp_console_register_help_command());
    ESP_ERROR_CHECK(cmd_bench_register(card));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
