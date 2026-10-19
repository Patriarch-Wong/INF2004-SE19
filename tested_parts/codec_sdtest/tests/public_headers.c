/** @file public_headers.c
 * @brief Compile-only check that module interfaces remain usable from C99.
 */
#include "app_config.h"
#include "codec.h"
#include "codec.h"
#include "codec_test.h"
#include "sd_card.h"
#include "sd_card.h"

void public_headers_check(void);

void
public_headers_check(void)
{
    codec_t *p_codec = NULL;
    sd_card_file_t *p_file = NULL;
    codec_destroy(&p_codec);
    (void)sd_card_close(&p_file);
}
