/* Code Fairy (CUSA28763): working fclose for the game's config files.
 *
 * In this eboot the PLT stub of the libc import fclose (0xda5c60) is "xor eax,eax; ret" followed by
 * the tail of the original "jmp [rip+0x69d892]", so every fclose returns 0 without closing anything.
 * The GOT slot it used to jump through (0x14434f8) is still bound to libc.prx's fclose.
 *
 * The game reads each character shading preset (/app0/shader_conf/toon_*.xml, outline_*.xml) with
 * fopen_s + fread + fclose on every scene change, so two FILE objects leak per change. After a full
 * mission (about 28 leaked files) libc's fopen fails, the next scene's preset is never applied
 * ("[bf::debug::menu::Config::analysis] file could not be opened"), and the battle preset
 * toon_battle01.xml stays active: toon shading off, battle shadow and rim colours, dim characters in
 * the cutscene that follows.
 *
 * Fix: put the original jump back, so fclose reaches libc again. Every fclose caller in the game
 * closes a FILE it opened itself (config XML loaders, a file stream object, a memory-mapped file
 * helper and the Lua file loader, which skips stdin). Applied only when the bytes match this build. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

#define OFF_FCLOSE_STUB 0xda5c60u
#define OFF_FOPEN_S_STUB 0xda5c70u

int runtime_fclose_apply(unsigned char *image, uint64_t image_size) {
    static const unsigned char disabled[] = {0x31, 0xc0, 0xc3, 0xd8, 0x69, 0x00};
    static const unsigned char original[] = {0xff, 0x25, 0x92, 0xd8, 0x69, 0x00}; /* jmp [rip+0x69d892] = GOT 0x14434f8 */
    static const unsigned char next[] = {0xff, 0x25, 0x8a, 0xd8, 0x69, 0x00};     /* fopen_s: jmp [rip+0x69d88a] */
    if (image_size < OFF_FOPEN_S_STUB + sizeof(next) || memcmp(image + OFF_FCLOSE_STUB, disabled, sizeof(disabled)) ||
        memcmp(image + OFF_FOPEN_S_STUB, next, sizeof(next))) {
        fputs("Runtime: fclose fix not applied (code differs from the reference build)\n", stderr);
        return 0;
    }
    memcpy(image + OFF_FCLOSE_STUB, original, sizeof(original));
    puts("Runtime: fclose import stub restored (game config files are closed again)");
    return 1;
}
