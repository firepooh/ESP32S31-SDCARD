# Project copy of ESP-IDF v6.1 components/fatfs with exFAT enabled (src/ffconf.h: FF_FS_EXFAT 1).
# It overrides the IDF fatfs component because the project components directory has priority.
if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" STREQUAL "6.1")
    message(WARNING "components/fatfs was copied from ESP-IDF v6.1 but the build uses "
                    "v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}. Re-copy it from $IDF_PATH/components/fatfs "
                    "and set FF_FS_EXFAT to 1 in src/ffconf.h.")
endif()

idf_build_get_property(target IDF_TARGET)

set(srcs "diskio/diskio.c"
        "diskio/diskio_rawflash.c"
        "diskio/diskio_wl.c"
        "diskio/diskio_bdl.c"
        "src/ff.c"
        "src/ffunicode.c")

set(include_dirs "diskio" "src")

set(requires "wear_levelling" "esp_blockdev")

# for linux, we do not have support for sdmmc, for real targets, add respective sources
if(${target} STREQUAL "linux")
    list(APPEND srcs "port/linux/ffsystem.c"
                  "vfs/vfs_fat.c"
                  "vfs/vfs_fat_bdl.c")
    list(APPEND include_dirs "vfs")
    list(APPEND priv_requires "vfs" "linux")
else()
    list(APPEND srcs "port/freertos/ffsystem.c"
            "diskio/diskio_sdmmc.c"
            "vfs/vfs_fat.c"
            "vfs/vfs_fat_sdmmc.c"
            "vfs/vfs_fat_spiflash.c"
            "vfs/vfs_fat_bdl.c")

    list(APPEND include_dirs "vfs")

    list(APPEND requires "sdmmc" "esp_driver_sdmmc" "esp_driver_sdspi")

    list(APPEND priv_requires "vfs" "esp_driver_gpio" "esptool_py")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires}
                       PRIV_REQUIRES ${priv_requires}
                      )
