/** @file codec_test.h
 * @brief Integration test using only the codec and SD card public APIs.
 */
#ifndef CODEC_TEST_H
#define CODEC_TEST_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*codec_test_log_t)(const char *p_message);

/** Requires mounted sd_card and a task with a 64 KiB stack.
 * The logger is synchronous and must not retain the message pointer.
 * Releases all file/codec handles and buffers before returning.
 */
bool codec_test_run(codec_test_log_t pf_log);

#ifdef __cplusplus
}
#endif
#endif /* CODEC_TEST_H */
