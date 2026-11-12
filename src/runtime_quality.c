/* Code Fairy (CUSA28763) graphics quality: sun shadow map size.
 *
 * The render target setup FUN_007f3540 creates the sun shadow atlas with a constant 4096x4096
 * (mov edi,0x1000 ; mov esi,0x1000 before the target allocator FUN_009fe360). The atlas holds
 * four 2048x2048 cascades and the game derives the cascade viewports from the texture size, so a
 * smaller atlas only lowers the shadow resolution. On a Radeon 610M at 540p a 1024 atlas saves
 * about 18 ms per combat frame (82 -> 64 ms GPU time), 2048 about 15 ms.
 * PS4RT_CF_SHADOW_MAP=<1024|2048|4096> selects the size (4096 and unset leave the game untouched).
 * The patch is applied only when the bytes match this build exactly. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OFF_SHADOW_WIDTH 0x7f397du
#define OFF_SHADOW_HEIGHT 0x7f3982u
#define GAME_SHADOW_SIZE 4096u

int runtime_quality_apply(unsigned char *image, uint64_t image_size) {
    const char *request = getenv("PS4RT_CF_SHADOW_MAP");
    if (!request || !*request) return 0;
    const unsigned long size = strtoul(request, NULL, 10);
    if (size != 1024 && size != 2048 && size != GAME_SHADOW_SIZE) {
        fprintf(stderr, "Runtime: PS4RT_CF_SHADOW_MAP '%s' is not 1024, 2048 or 4096; keeping 4096\n", request);
        return 0;
    }
    if (size == GAME_SHADOW_SIZE) return 0;
    /* mov edi, 0x1000 ; mov esi, 0x1000 */
    static const unsigned char width_original[] = {0xbf, 0x00, 0x10, 0x00, 0x00};
    static const unsigned char height_original[] = {0xbe, 0x00, 0x10, 0x00, 0x00};
    if (image_size < OFF_SHADOW_HEIGHT + sizeof(height_original) ||
        memcmp(image + OFF_SHADOW_WIDTH, width_original, sizeof(width_original)) ||
        memcmp(image + OFF_SHADOW_HEIGHT, height_original, sizeof(height_original))) {
        fputs("Runtime: shadow map size not changed (code differs from the reference build)\n", stderr);
        return 0;
    }
    const uint32_t value = (uint32_t)size;
    memcpy(image + OFF_SHADOW_WIDTH + 1, &value, 4);
    memcpy(image + OFF_SHADOW_HEIGHT + 1, &value, 4);
    printf("Runtime: shadow map %lux%lu\n", size, size);
    return 1;
}
