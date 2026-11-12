/* Code Fairy (CUSA28763) internal render resolution.
 *
 * The title's renderer setup (FUN_007ec5a0) calls the target allocator FUN_007f3540 with a
 * constant 1920x1080; every full-size scene target and the screen-size globals follow from it.
 * Replacing the two immediates resizes the scene. PS4RT_CF_RESOLUTION=<w>x<h> selects the size
 * (default and 1920x1080 leave the game untouched). Larger scene targets also need a
 * larger game memory pool: the startup configuration (FUN_008e5b00) requests 3601 MiB, sized for
 * 1080p, so its 32-bit size field is raised to the 4095 MiB maximum and the host direct-memory
 * pool is enlarged to hold it. Each patch is applied only when the bytes there match this build
 * exactly. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OFF_SETUP_CALL 0x7ec644u
#define DEFAULT_WIDTH 1920u
#define DEFAULT_HEIGHT 1080u
#define OFF_POOL_SIZE 0x8e5d0cu
#define POOL_BYTES 0xfff00000u
#define HOST_POOL_MB "9216"
/* Graphics init FUN_009fff50 widens the 32-bit type-3 pool size and passes it to the region
 * creator FUN_00a47ef0 as a 64-bit value; everything after the config field is 64-bit. */
#define OFF_POOL_PASS 0xa000d8u
#define HOST_POOL_DOUBLE_MB "10240"

/* mov esi, imm32 ; mov edx, imm32 (width, height) */
static const unsigned char original[] = {0xbe, 0x80, 0x07, 0x00, 0x00, 0xba, 0x38, 0x04, 0x00, 0x00};
/* movabs rdx, 0xe110000000a00000: 10 MiB in the low dword, the 3601 MiB pool in the high dword */
static const unsigned char pool_original[] = {0x48, 0xba, 0x00, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x10, 0xe1};
/* mov eax,[r14+4] ; lea rdi,[rbp-0x3c0] ; mov [rbp-0x3c0],rax */
static const unsigned char pass_original[] = {0x41, 0x8b, 0x46, 0x04, 0x48, 0x8d, 0xbd, 0x40, 0xfc, 0xff, 0xff,
                                              0x48, 0x89, 0x85, 0x40, 0xfc, 0xff, 0xff};
/* mov eax,[r14+4] ; shl rax,1 ; lea rdi,[rbp-0x3c0] ; mov [rdi],rax ; nop: twice the configured size */
static const unsigned char pass_double[] = {0x41, 0x8b, 0x46, 0x04, 0x48, 0xd1, 0xe0, 0x48, 0x8d, 0xbd, 0x40,
                                            0xfc, 0xff, 0xff, 0x48, 0x89, 0x07, 0x90};
_Static_assert(sizeof(pass_double) == sizeof(pass_original), "pool patch size");

int runtime_resolution_apply(unsigned char *image, uint64_t image_size) {
    uint32_t width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT;
    const char *request = getenv("PS4RT_CF_RESOLUTION");
    if (request && *request) {
        char *end;
        const unsigned long w = strtoul(request, &end, 10);
        const unsigned long h = *end == 'x' ? strtoul(end + 1, &end, 10) : 0;
        if (*end || w < 640 || w > 8192 || h < 360 || h > 8192) {
            fprintf(stderr, "Runtime: PS4RT_CF_RESOLUTION '%s' is not <w>x<h> (640..8192); using %ux%u\n", request,
                    width, height);
        } else {
            width = (uint32_t)w;
            height = (uint32_t)h;
        }
    }
    if (width == 1920 && height == 1080) return 0;
    if (image_size < OFF_SETUP_CALL + sizeof(original) || memcmp(image + OFF_SETUP_CALL, original, sizeof(original))) {
        fputs("Runtime: render resolution not changed (code differs from the reference build)\n", stderr);
        return 0;
    }
    memcpy(image + OFF_SETUP_CALL + 1, &width, 4);
    memcpy(image + OFF_SETUP_CALL + 6, &height, 4);
    printf("Runtime: render resolution %ux%u\n", width, height);
    if (height % 8u)
        fprintf(stderr, "Runtime: height %u is not a multiple of 8; the game drops three scene targets "
                        "and particle effects disappear (use e.g. 1024x576 instead of 960x540)\n", height);
    /* Above 1080p the enlarged scene targets come out of the game's type-3 graphics pool. Its
     * 32-bit config field (3601 MiB) caps it below 4 GiB, which 4K exhausted (a 2 MiB video
     * texture failed in the chapter 3 tutorial). Doubling the size where it is passed on keeps the
     * stock config value for every other reader and gives a 7202 MiB pool. */
    int pool_doubled = 0;
    if (width * height > 1920u * 1080u && image_size >= OFF_POOL_PASS + sizeof(pass_original) &&
        !memcmp(image + OFF_POOL_PASS, pass_original, sizeof(pass_original))) {
        memcpy(image + OFF_POOL_PASS, pass_double, sizeof(pass_double));
        setenv("PS4RT_DMEM_MB", HOST_POOL_DOUBLE_MB, 0);
        printf("Runtime: game memory pool doubled (7202 MiB)\n");
        pool_doubled = 1;
    }
    if (width * height > 1920u * 1080u && !pool_doubled) {
        if (image_size < OFF_POOL_SIZE + sizeof(pool_original) ||
            memcmp(image + OFF_POOL_SIZE, pool_original, sizeof(pool_original))) {
            fputs("Runtime: game memory pool not enlarged (code differs from the reference build)\n", stderr);
        } else {
            const uint32_t pool = POOL_BYTES;
            memcpy(image + OFF_POOL_SIZE + 6, &pool, 4);
            setenv("PS4RT_DMEM_MB", HOST_POOL_MB, 0);
            printf("Runtime: game memory pool %u MiB\n", pool >> 20);
        }
    }
    /* Ultrawide: camera parameter blocks store a constant 16:9 aspect (imm32 of
     * mov dword [mem], 0x3fe38e39). Replace it with the render aspect when that is not 16:9. */
    if (width * 9u != height * 16u) {
        static const uint32_t aspect_sites[] = {0x7f881cu, 0x7f8e01u, 0xb8c62bu};
        const float aspect = (float)width / (float)height;
        const uint32_t sixteen_nine = 0x3fe38e39u;
        for (unsigned i = 0; i < sizeof(aspect_sites) / sizeof(aspect_sites[0]); ++i) {
            const uint32_t at = aspect_sites[i];
            if (image_size < at + 4 || memcmp(image + at, &sixteen_nine, 4)) {
                fprintf(stderr, "Runtime: aspect site %#x differs from the reference build\n", at);
                continue;
            }
            memcpy(image + at, &aspect, 4);
        }
        printf("Runtime: camera aspect %.4f\n", aspect);
        /* Display buffer: startup config FUN_008e5b00 movabs packs (w,h) u16 at +6/+8. The HUD
         * canvas stays 1920x1080 centred, so keep the height and widen the display. */
        static const unsigned char display_original[] = {0x48, 0xb8, 0, 0, 0, 0, 0x80, 0x07, 0x38, 0x04};
        const uint32_t display_at = 0x8e5c1fu;
        if (image_size < display_at + sizeof(display_original) ||
            memcmp(image + display_at, display_original, sizeof(display_original))) {
            fputs("Runtime: display size not changed (code differs from the reference build)\n", stderr);
        } else {
            const uint16_t dw = (uint16_t)(((uint32_t)(1080.0f * aspect + 0.5f) + 7u) & ~7u), dh = 1080;
            memcpy(image + display_at + 6, &dw, 2);
            memcpy(image + display_at + 8, &dh, 2);
            printf("Runtime: display %ux%u\n", dw, dh);
        }
    }
    /* High-res display: above 1080 lines the display buffer is the render size (e.g. 3840x2160,
     * 5120x1440) instead of 1080 tall; the renderer scales the HUD canvas (fixed 1920x1080 pixels
     * in the game) by height/1080. PS4RT_HIRES_DISPLAY=0 keeps the 1080-line display. */
    {
        const char *hires = getenv("PS4RT_HIRES_DISPLAY");
        static const unsigned char display_original[] = {0x48, 0xb8, 0, 0, 0, 0, 0x80, 0x07, 0x38, 0x04};
        const uint32_t display_at = 0x8e5c1fu;
        if (!(hires && *hires == '0') && height > 1080) {
            const uint16_t dw = (uint16_t)((width + 7u) & ~7u), dh = (uint16_t)height;
            if (image_size < display_at + sizeof(display_original) ||
                (memcmp(image + display_at, display_original, 6) != 0)) {
                fputs("Runtime: high-res display not applied (code differs from the reference build)\n", stderr);
            } else {
                memcpy(image + display_at + 6, &dw, 2);
                memcpy(image + display_at + 8, &dh, 2);
                printf("Runtime: high-res display %ux%u\n", dw, dh);
            }
        }
    }
    /* Marker tracking (FUN_00348a20, FUN_003630f0, FUN_00363b10, FUN_00653ab0; the last places
     * enemy pilot icons): a marker projected in display
     * pixels is drawn on its unit while within +-960 x +-540 of the centre, otherwise replaced by
     * an arrow on a 725x375 ellipse. Each function reads its own rodata block (eps, b, a^2, b^2,
     * a*b, rad->deg, 360, -960, 960, -540, 540), and every value is read only there. Scale the
     * bounds and the ellipse to the actual display size. */
    {
        if (image_size < 0x8e5c1fu + 10) return 1;
        const unsigned char *disp = image + 0x8e5c1fu;
        const uint32_t disp_w = (uint32_t)disp[6] | ((uint32_t)disp[7] << 8);
        const uint32_t disp_h = (uint32_t)disp[8] | ((uint32_t)disp[9] << 8);
        if (disp_w != 1920 || disp_h != 1080) {
            static const unsigned char track_original[] = {
                0xac, 0xc5, 0x27, 0x37, 0x00, 0x80, 0xbb, 0x43, 0x90, 0x53, 0x00, 0x49, 0x40, 0x54, 0x09,
                0x48, 0x60, 0xc0, 0x84, 0x48, 0xe1, 0x2e, 0x65, 0x42, 0x00, 0x00, 0xb4, 0x43, 0x00, 0x00,
                0x70, 0xc4, 0x00, 0x00, 0x70, 0x44, 0x00, 0x00, 0x07, 0xc4, 0x00, 0x00, 0x07, 0x44};
            static const uint32_t track_blocks[] = {0xde1880u, 0xde39b0u, 0xde39e8u, 0xe2e4c8u};
            const float hw = (float)disp_w * 0.5f, hh = (float)disp_h * 0.5f;
            const float a = 725.0f * hw / 960.0f, b = 375.0f * hh / 540.0f;
            const float vals[] = {b, a * a, b * b, a * b};
            const float bounds[] = {-hw, hw, -hh, hh};
            for (unsigned i = 0; i < sizeof(track_blocks) / sizeof(track_blocks[0]); ++i) {
                unsigned char *blk = image + track_blocks[i];
                if (image_size < track_blocks[i] + sizeof(track_original) ||
                    memcmp(blk, track_original, sizeof(track_original))) {
                    fprintf(stderr, "Runtime: marker tracking block %#x differs from the reference build\n",
                            track_blocks[i]);
                    continue;
                }
                memcpy(blk + 4, vals, sizeof(vals));
                memcpy(blk + 0x1c, bounds, sizeof(bounds));
            }
            printf("Runtime: marker tracking %.0fx%.0f, arrow ellipse %.0fx%.0f\n", hw * 2, hh * 2, a, b);
        }
    }
    return 1;
}
