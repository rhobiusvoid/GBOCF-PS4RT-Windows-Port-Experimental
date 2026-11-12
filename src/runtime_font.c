/* libSceFont / libSceFontFt host subset with no font data.
 * The console's system fonts are not part of game data and nothing is bundled,
 * so libraries and renderers can be created but no font can be opened:
 * OpenFontSet reports FONT_OPEN_FAILED and handle-taking calls report an invalid
 * handle. Signatures follow the shadPS4 font module (references/). */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR_INVALID_PARAMETER ((int32_t)0x80460002)
#define ERR_INVALID_LIBRARY   ((int32_t)0x80460004)
#define ERR_INVALID_FONT      ((int32_t)0x80460005)
#define ERR_INVALID_RENDERER  ((int32_t)0x80460007)
#define ERR_OPEN_FAILED       ((int32_t)0x80460036)
#define MAGIC_LIB  0x464f4e544c494231ull
#define MAGIC_REND 0x464f4e5452454e31ull

typedef struct { uint64_t magic; } Object;
typedef struct {
    void *buffer; int32_t width_byte; int8_t pixel_size; uint8_t pad0, style, pad2;
    int32_t width, height; uint32_t sc_x0, sc_y0, sc_x1, sc_y1; uint64_t reserved[11];
} Surface;
_Static_assert(sizeof(Surface)==0x80,"OrbisFontRenderSurface layout");

static unsigned libraries, renderers, open_attempts;

static int32_t create(void *out,uint64_t magic,unsigned *counter,int32_t invalid) {
    Object **slot=out;
    if (!slot) return invalid;
    Object *o=calloc(1,sizeof(*o)); if (!o) return ERR_INVALID_PARAMETER;
    o->magic=magic; *slot=o; ++*counter; return 0;
}
static int32_t destroy(Object **slot,uint64_t magic,int32_t invalid) {
    if (!slot || !*slot || (*slot)->magic!=magic) return invalid;
    free(*slot); *slot=NULL; return 0;
}
static ABI int32_t font_memory_init(void *mem,void *region,uint32_t size,const void *iface,void *mspace,void *cb,void *ctx) {
    (void)region; (void)size; (void)iface; (void)mspace; (void)cb; (void)ctx;
    return mem ? 0 : ERR_INVALID_PARAMETER;
}
static ABI int32_t font_memory_term(void *mem) { return mem ? 0 : ERR_INVALID_PARAMETER; }
static ABI int32_t create_library(const void *mem,const void *params,uint64_t edition,void *out) {
    (void)mem; (void)params; (void)edition; return create(out,MAGIC_LIB,&libraries,ERR_INVALID_PARAMETER);
}
static ABI int32_t destroy_library(Object **lib) { return destroy(lib,MAGIC_LIB,ERR_INVALID_PARAMETER); }
static ABI int32_t create_renderer(const void *mem,const void *params,uint64_t edition,void *out) {
    (void)mem; (void)params; (void)edition; return create(out,MAGIC_REND,&renderers,ERR_INVALID_PARAMETER);
}
static ABI int32_t destroy_renderer(Object **r) { return destroy(r,MAGIC_REND,ERR_INVALID_PARAMETER); }
static ABI int32_t support_system(Object *lib) { return lib && lib->magic==MAGIC_LIB ? 0 : ERR_INVALID_LIBRARY; }
static ABI int32_t support_external(Object *lib,uint32_t max,uint32_t formats) { (void)max; (void)formats; return support_system(lib); }
static ABI int32_t attach_cache(Object *lib,void *buffer,uint32_t size) { (void)buffer; (void)size; return support_system(lib); }
static ABI int32_t open_font_set(Object *lib,uint32_t type,uint32_t mode,const void *params,void *out) {
    (void)type; (void)mode; (void)params;
    if (!lib || lib->magic!=MAGIC_LIB) return ERR_INVALID_LIBRARY;
    if (!out) return ERR_INVALID_PARAMETER;
    if (!open_attempts++) fputs("Runtime: Font: no fonts available; OpenFontSet fails\n",stdout);
    *(void **)out=NULL; return ERR_OPEN_FAILED;
}
static ABI int32_t font_invalid(void) { return ERR_INVALID_FONT; }
static ABI void surface_init(Surface *s,void *buffer,int32_t width_byte,int32_t pixel_size,int32_t width,int32_t height) {
    if (!s) return;
    memset(s,0,sizeof(*s));
    s->buffer=buffer; s->width_byte=width_byte; s->pixel_size=(int8_t)pixel_size;
    s->width=width; s->height=height;
    s->sc_x1=width<0 ? 0 : (uint32_t)width; s->sc_y1=height<0 ? 0 : (uint32_t)height;
}
/* The Ft driver selection tables are opaque tokens handed back to library creation. */
static const uint64_t selection[2][32];
static ABI const void *select_library_ft(int32_t value) { return value==0 ? selection[0] : NULL; }
static ABI const void *select_renderer_ft(int32_t value) { return value==0 ? selection[1] : NULL; }

uintptr_t runtime_font_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"whrS4oksXc4#libSceFont",font_memory_init}, {"h6hIgxXEiEc#libSceFont",font_memory_term},
        {"n590hj5Oe-k#libSceFont",create_library}, {"FXP359ygujs#libSceFont",destroy_library},
        {"WaSFJoRWXaI#libSceFont",create_renderer}, {"exAxkyVLt0s#libSceFont",destroy_renderer},
        {"SsRbbCiWoGw#libSceFont",support_system}, {"mz2iTY0MK4A#libSceFont",support_external},
        {"CUKn5pX-NVY#libSceFont",attach_cache}, {"cKYtVmeSTcw#libSceFont",open_font_set},
        {"gdUCnU0gHdI#libSceFont",surface_init},
        {"oM+XCzVG3oM#libSceFontFt",select_library_ft}, {"Xx974EW-QFY#libSceFontFt",select_renderer_ft},
        {"3OdRkSjOcog#libSceFont",font_invalid}, {"vzHs3C8lWJk#libSceFont",font_invalid},
        {"L97d+3OgMlE#libSceFont",font_invalid}, {"imxVx8lm+KM#libSceFont",font_invalid},
        {"kAenWy1Zw5o#libSceFont",font_invalid}, {"v0phZwa4R5o#libSceFont",font_invalid},
        {"N1EBMeGhf7E#libSceFont",font_invalid}, {"6vGCkkQJOcI#libSceFont",font_invalid},
        {"1QjhKxrsOB8#libSceFont",font_invalid},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_font_report(void) {
    printf("Runtime: Font libraries=%u, renderers=%u, open attempts=%u\n",libraries,renderers,open_attempts);
}
