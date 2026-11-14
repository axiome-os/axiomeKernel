/* Kernel-side boot splash.  Uses the framebuffer back-buffer, where the
   normal console also draws, and shares the renderer with axboot. */

#include "splash.h"
#include "framebuffer.h"
#include "debugmode.h"
#include "bootsplash.h"

static int g_visible;
static int g_surface_ready;
static unsigned int g_log_count;
static uint32_t g_progress;
static struct splash_surface g_surface;

static int surface_init(void)
{
    uint8_t *pixels;

    if (g_surface_ready)
        return 1;
    if (!fb_active())
        return 0;

    pixels = fb_back_buffer_for_gfx();
    if (!pixels)
        return 0;

    g_surface.pixels = pixels;
    g_surface.width = fb_width();
    g_surface.height = fb_height();
    g_surface.pitch = fb_pitch();
    g_surface_ready = (g_surface.width > 0 && g_surface.height > 0);
    return g_surface_ready;
}

void splash_show(void)
{
    struct splash_rect r;

    if (!kernel_splash_enabled() || kernel_debug_mode())
        return;
    if (!surface_init())
        return;

    g_visible = 1;
    
    g_progress = 250;
    g_log_count = 0;

    r = splash_render(&g_surface, g_progress, 1);
    (void)r; /* full-screen redraw */
    fb_mark_dirty(0, 0, fb_width(), fb_height());
    fb_flush();
}

void splash_set_progress(uint32_t permille)
{
    struct splash_rect r;

    if (!g_visible || permille > 1000)
        return;
    if (permille <= g_progress)
        return;

    g_progress = permille;
    r = splash_render(&g_surface, g_progress, 0);
    fb_mark_dirty((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h);
    fb_flush();
}

void splash_note_log(void)
{
    struct splash_rect r;

    if (!g_visible)
        return;

    if (++g_log_count < 6)
        return;
    g_log_count = 0;

    if (g_progress >= 980)
        return;
    g_progress += 2;
    if (g_progress > 980)
        g_progress = 980;

    r = splash_render(&g_surface, g_progress, 0);
    fb_mark_dirty((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h);
    fb_flush();
}

void splash_hide(void)
{
    g_visible = 0;
    fb_clear();
}
