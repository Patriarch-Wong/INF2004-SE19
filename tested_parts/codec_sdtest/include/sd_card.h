/** @file sd_card.h
 * @brief Single-card SDMMC storage API for ESP32-S3, usable from C or C++.
 */
#ifndef SD_CARD_H
#define SD_CARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sd_card_file sd_card_file_t;

typedef enum
{
    SD_CARD_OK = 0,
    SD_CARD_INVALID_ARGUMENT,
    SD_CARD_INVALID_STATE,
    SD_CARD_NO_MEMORY,
    SD_CARD_IO_ERROR,
    SD_CARD_EXISTS,
    SD_CARD_NOT_FOUND
} sd_card_status_t;

typedef enum
{
    SD_CARD_READ = 0,
    SD_CARD_CREATE_NEW
} sd_card_mode_t;

typedef struct
{
    int32_t cmd_gpio;
    int32_t clk_gpio;
    int32_t data_gpio;
    uint32_t frequency_khz; /**< 400..20000; 1-bit bus. */
} sd_card_config_t;

/** Mount at /sdcard without formatting; already mounted returns STATE.
 * This module owns the SDMMC host. Do not also use Arduino SD_MMC.
 * Serialize ALL calls from one task or an external lock; no ISR calls.
 */
sd_card_status_t sd_card_mount(const sd_card_config_t *p_config);

/** Idempotent when unmounted; refuses to unmount while API files are open. */
sd_card_status_t sd_card_unmount(void);
sd_card_status_t sd_card_capacity(uint64_t *p_bytes);

/** Paths are card-relative absolute paths, e.g. /audio/file.bin, max 127
 * characters, without . or .. components. mkdir accepts an existing directory.
 */
sd_card_status_t sd_card_mkdir(const char *p_path);

/** Open for read or exclusively create a new file, never truncate/overwrite.
 * *pp_file must be NULL. On failure it remains NULL. Up to 3 files may be open.
 * Caller owns handle until close; do not copy handles or use after close.
 */
sd_card_status_t sd_card_open(const char *p_path, sd_card_mode_t mode,
                            sd_card_file_t **pp_file);

/** Exact-length I/O; short reads (including EOF) and writes return IO_ERROR.
 * Failure may advance the file position or partially modify a newly made file.
 * A zero-length operation is allowed; its data pointer may be NULL.
 * Each call accepts at most INT32_MAX bytes (the SDK's signed I/O limit).
 */
sd_card_status_t sd_card_read(sd_card_file_t *p_file, void *p_data,
                            size_t bytes);
sd_card_status_t sd_card_write(sd_card_file_t *p_file, const void *p_data,
                             size_t bytes);
sd_card_status_t sd_card_size(sd_card_file_t *p_file, size_t *p_bytes);

/** Flush buffered output and sync FAT storage; write handles only. */
sd_card_status_t sd_card_flush(sd_card_file_t *p_file);

/** Close and release handle, even on flush/close failure. Sets it to NULL.
 * Closing an already NULL handle succeeds. Write handles are synced first.
 */
sd_card_status_t sd_card_close(sd_card_file_t **pp_file);
const char *sd_card_status_string(sd_card_status_t status);

#ifdef __cplusplus
}
#endif
#endif /* SD_CARD_H */
