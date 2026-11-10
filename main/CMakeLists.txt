idf_component_register(
    SRCS "app_main.c"
         "sd_card.c"
         "msc_ctrl.c"
         "msc_io.c"
         "bench.c"
         "cmd_bench.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio console esp_timer heap
)

# Linker wraps (implemented in msc_ctrl.c):
#  - TinyUSB MSC callbacks of esp_tinyusb: count USB bytes, block the host while the
#    application owns the card, report "medium changed" when the card is handed back,
#    route READ(10)/WRITE(10) through the read-ahead / write-merge engine (msc_io.c) and
#    write back merged data on SYNCHRONIZE CACHE.
#  - SD sector I/O: measure SD busy time and copy unaligned buffers (esp_tinyusb write
#    buffer) into one cache-aligned DMA buffer so a 32 KB request stays one SD command.
set(wrapped_symbols
    tud_msc_test_unit_ready_cb
    tud_msc_read10_cb
    tud_msc_write10_cb
    tud_msc_scsi_cb
    sdmmc_read_sectors
    sdmmc_write_sectors
)
foreach(sym ${wrapped_symbols})
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${sym}")
endforeach()
