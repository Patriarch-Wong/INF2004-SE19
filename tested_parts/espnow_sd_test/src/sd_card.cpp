/** @file sd_card.cpp
 * @brief ESP-IDF SDMMC/FAT backend; no Arduino C++ dependency.
 */
#include "sd_card.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <memory>
#include <new>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#define SD_CARD_MOUNT_PATH "/sdcard"
#define SD_CARD_PATH_BYTES (128U)
#define SD_CARD_FULL_PATH_BYTES \
    (SD_CARD_PATH_BYTES + sizeof(SD_CARD_MOUNT_PATH))
#define SD_CARD_MAX_FILES (3U)

struct sd_card_file
{
    int h_file = -1;
    bool b_writable = false;

    sd_card_file() = default;
    sd_card_file(const sd_card_file &) = delete;
    sd_card_file &operator=(const sd_card_file &) = delete;
    sd_card_file(sd_card_file &&) = delete;
    sd_card_file &operator=(sd_card_file &&) = delete;

    ~sd_card_file() noexcept
    {
        // Fallback only; normal close explicitly checks flush/close errors.
        if (h_file >= 0)
        {
            (void)::close(h_file);
        }
    }
};

/* Singleton host state. Callers must serialize module access. */
static sdmmc_card_t *gp_card = nullptr;
static uint32_t g_open_files = 0U;

/** Expand a validated path into the mounted namespace. */
static sd_card_status_t
sd_card_path(const char *p_path, char *p_full_path)
{
    if ((nullptr == p_path) || ('/' != p_path[0]) ||
        (strlen(p_path) >= SD_CARD_PATH_BYTES))
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    const char *p_component = p_path + 1;
    while ('\0' != *p_component)
    {
        const size_t length = strcspn(p_component, "/");
        if (((1U == length) && ('.' == p_component[0])) ||
            ((2U == length) && (0 == strncmp(p_component, "..", 2U))))
        {
            return (SD_CARD_INVALID_ARGUMENT);
        }
        p_component += length;
        if ('/' == *p_component)
        {
            ++p_component;
        }
    }
    const int32_t length = snprintf(p_full_path, SD_CARD_FULL_PATH_BYTES,
                                   "%s%s", SD_CARD_MOUNT_PATH, p_path);
    if ((length < 0) ||
        (static_cast<size_t>(length) >= SD_CARD_FULL_PATH_BYTES))
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    return (SD_CARD_OK);
}

/** Validate output-capable, distinct S3 GPIOs and a conservative clock. */
static bool
sd_card_config_valid(const sd_card_config_t *p_config)
{
    if (nullptr == p_config)
    {
        return (false);
    }
    return (GPIO_IS_VALID_OUTPUT_GPIO(p_config->cmd_gpio) &&
            GPIO_IS_VALID_OUTPUT_GPIO(p_config->clk_gpio) &&
            GPIO_IS_VALID_OUTPUT_GPIO(p_config->data_gpio) &&
            (p_config->cmd_gpio != p_config->clk_gpio) &&
            (p_config->cmd_gpio != p_config->data_gpio) &&
            (p_config->clk_gpio != p_config->data_gpio) &&
            (p_config->frequency_khz >= 400U) &&
            (p_config->frequency_khz <= 20000U));
}

sd_card_status_t
sd_card_mount(const sd_card_config_t *p_config)
{
    if (!sd_card_config_valid(p_config))
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    if (nullptr != gp_card)
    {
        return (SD_CARD_INVALID_STATE);
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = SD_CARD_MAX_FILES;
    mount.allocation_unit_size = 0U;
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = static_cast<int>(p_config->frequency_khz);
    slot.width = 1U;
    slot.cmd = static_cast<gpio_num_t>(p_config->cmd_gpio);
    slot.clk = static_cast<gpio_num_t>(p_config->clk_gpio);
    slot.d0 = static_cast<gpio_num_t>(p_config->data_gpio);
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    const esp_err_t result = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_PATH,
        &host, &slot, &mount, &gp_card);
    if (ESP_OK != result)
    {
        gp_card = nullptr;
        return ((ESP_ERR_NO_MEM == result) ? SD_CARD_NO_MEMORY :
                SD_CARD_IO_ERROR);
    }
    return (SD_CARD_OK);
}

sd_card_status_t
sd_card_unmount(void)
{
    if (0U != g_open_files)
    {
        return (SD_CARD_INVALID_STATE);
    }
    if (nullptr == gp_card)
    {
        return (SD_CARD_OK);
    }
    if (ESP_OK != esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_PATH, gp_card))
    {
        return (SD_CARD_IO_ERROR);
    }
    gp_card = nullptr;
    return (SD_CARD_OK);
}

sd_card_status_t
sd_card_capacity(uint64_t *p_bytes)
{
    if (nullptr == p_bytes)
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    *p_bytes = 0U;
    if (nullptr == gp_card)
    {
        return (SD_CARD_INVALID_STATE);
    }
    *p_bytes = static_cast<uint64_t>(gp_card->csd.capacity) *
               gp_card->csd.sector_size;
    return (SD_CARD_OK);
}

sd_card_status_t
sd_card_mkdir(const char *p_path)
{
    char full_path[SD_CARD_FULL_PATH_BYTES];
    if (SD_CARD_OK != sd_card_path(p_path, full_path))
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    if (nullptr == gp_card)
    {
        return (SD_CARD_INVALID_STATE);
    }
    if (0 == mkdir(full_path, 0775))
    {
        return (SD_CARD_OK);
    }
    struct stat info = {};
    if ((EEXIST == errno) && (0 == stat(full_path, &info)) &&
        S_ISDIR(info.st_mode))
    {
        return (SD_CARD_OK);
    }
    return (SD_CARD_IO_ERROR);
}

sd_card_status_t
sd_card_open(const char *p_path, sd_card_mode_t mode,
             sd_card_file_t **pp_file)
{
    char full_path[SD_CARD_FULL_PATH_BYTES];
    if ((nullptr == pp_file) ||
        ((SD_CARD_READ != mode) && (SD_CARD_CREATE_NEW != mode)) ||
        (SD_CARD_OK != sd_card_path(p_path, full_path)))
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    if ((nullptr != *pp_file) || (nullptr == gp_card) ||
        (g_open_files >= SD_CARD_MAX_FILES))
    {
        return (SD_CARD_INVALID_STATE);
    }
    std::unique_ptr<sd_card_file_t> p_file{
        new (std::nothrow) sd_card_file_t{}};
    if (nullptr == p_file)
    {
        return (SD_CARD_NO_MEMORY);
    }
    const bool b_create = (SD_CARD_CREATE_NEW == mode);
    const int flags = b_create ? (O_WRONLY | O_CREAT | O_EXCL) : O_RDONLY;
    // Use VFS descriptors directly: this FAT backend has no fcntl support
    // for newlib fdopen(). O_EXCL preserves existing files atomically.
    p_file->h_file = ::open(full_path, flags, 0664);
    if (p_file->h_file < 0)
    {
        const int saved_errno = errno;
        if (EEXIST == saved_errno)
        {
            return (SD_CARD_EXISTS);
        }
        return ((ENOENT == saved_errno) ? SD_CARD_NOT_FOUND :
                SD_CARD_IO_ERROR);
    }
    p_file->b_writable = b_create;
    ++g_open_files;
    *pp_file = p_file.release();
    return (SD_CARD_OK);
}

sd_card_status_t
sd_card_read(sd_card_file_t *p_file, void *p_data, size_t bytes)
{
    if ((nullptr == p_file) || ((nullptr == p_data) && (0U != bytes)) ||
        (bytes > static_cast<size_t>(INT32_MAX)))
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    if (p_file->b_writable)
    {
        return (SD_CARD_INVALID_STATE);
    }
    if (0U == bytes)
    {
        return (SD_CARD_OK);
    }
    const ssize_t count = ::read(p_file->h_file, p_data, bytes);
    return (((count >= 0) && (bytes == static_cast<size_t>(count))) ?
            SD_CARD_OK : SD_CARD_IO_ERROR);
}

sd_card_status_t
sd_card_write(sd_card_file_t *p_file, const void *p_data, size_t bytes)
{
    if ((nullptr == p_file) || ((nullptr == p_data) && (0U != bytes)) ||
        (bytes > static_cast<size_t>(INT32_MAX)))
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    if (!p_file->b_writable)
    {
        return (SD_CARD_INVALID_STATE);
    }
    if (0U == bytes)
    {
        return (SD_CARD_OK);
    }
    const ssize_t count = ::write(p_file->h_file, p_data, bytes);
    return (((count >= 0) && (bytes == static_cast<size_t>(count))) ?
            SD_CARD_OK : SD_CARD_IO_ERROR);
}

sd_card_status_t
sd_card_size(sd_card_file_t *p_file, size_t *p_bytes)
{
    if (nullptr == p_bytes)
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    *p_bytes = 0U;
    if (nullptr == p_file)
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    struct stat info = {};
    if ((0 != fstat(p_file->h_file, &info)) || (info.st_size < 0))
    {
        return (SD_CARD_IO_ERROR);
    }
    *p_bytes = static_cast<size_t>(info.st_size);
    return (SD_CARD_OK);
}

sd_card_status_t
sd_card_flush(sd_card_file_t *p_file)
{
    if (nullptr == p_file)
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    if (!p_file->b_writable)
    {
        return (SD_CARD_INVALID_STATE);
    }
    if (0 != fsync(p_file->h_file))
    {
        return (SD_CARD_IO_ERROR);
    }
    return (SD_CARD_OK);
}

sd_card_status_t
sd_card_close(sd_card_file_t **pp_file)
{
    if (nullptr == pp_file)
    {
        return (SD_CARD_INVALID_ARGUMENT);
    }
    if (nullptr == *pp_file)
    {
        return (SD_CARD_OK);
    }
    sd_card_file_t *p_file = *pp_file;
    sd_card_status_t status = SD_CARD_OK;
    if (p_file->b_writable)
    {
        status = sd_card_flush(p_file);
    }
    if (0 != ::close(p_file->h_file))
    {
        status = SD_CARD_IO_ERROR;
    }
    p_file->h_file = -1;
    delete p_file;
    *pp_file = nullptr;
    --g_open_files;
    return (status);
}

const char *
sd_card_status_string(sd_card_status_t status)
{
    switch (status)
    {
        case SD_CARD_OK:
            return ("OK");
        case SD_CARD_INVALID_ARGUMENT:
            return ("invalid argument or path");
        case SD_CARD_INVALID_STATE:
            return ("wrong mount/file state or open-file limit");
        case SD_CARD_NO_MEMORY:
            return ("out of memory");
        case SD_CARD_IO_ERROR:
            return ("SD or filesystem I/O failure");
        case SD_CARD_EXISTS:
            return ("file already exists");
        case SD_CARD_NOT_FOUND:
            return ("file or parent directory not found");
        default:
            return ("unknown SD status");
    }
}
