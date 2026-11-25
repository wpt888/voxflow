#include "voxflow.h"
#include "layer-shell.h"

#include <cairo.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

enum {
    WIDTH = 226, HEIGHT = 60, SCALE = 2,
    PIXEL_WIDTH = WIDTH * SCALE, PIXEL_HEIGHT = HEIGHT * SCALE,
    STRIDE = PIXEL_WIDTH * 4, IMAGE_BYTES = STRIDE * PIXEL_HEIGHT,
    BUFFER_COUNT = 2, SHM_BYTES = BUFFER_COUNT * IMAGE_BYTES,
};
static const double pi = 3.14159265358979323846;

struct indicator_buffer {
    struct wl_buffer *proxy;
    bool busy;
};

struct indicator {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    unsigned char *pixels;
    struct indicator_buffer buffers[BUFFER_COUNT];
    bool configured;
    bool closed;
};

static void register_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *type, uint32_t version)
{
    struct indicator *indicator = data;
    if (!strcmp(type, wl_compositor_interface.name) && version >= 3)
        indicator->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 3);
    else if (!strcmp(type, wl_shm_interface.name))
        indicator->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(type, zwlr_layer_shell_v1_interface.name))
        indicator->shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
}

static void remove_global(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = register_global, .global_remove = remove_global,
};

static void configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
                      uint32_t width, uint32_t height)
{
    (void)width;
    (void)height;
    struct indicator *indicator = data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    indicator->configured = true;
}

static void closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
    (void)layer;
    ((struct indicator *)data)->closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = configure, .closed = closed,
};

static void synced(void *data, struct wl_callback *callback, uint32_t serial)
{
    (void)serial;
    *(bool *)data = true;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = { .done = synced };

static bool wait_for(struct indicator *indicator, const bool *ready)
{
    struct pollfd fd = { .fd = indicator_fd(indicator), .events = POLLIN };
    if (wl_display_flush(indicator->display) < 0)
        return false;
    while (!*ready) {
        if (poll(&fd, 1, 750) <= 0 || !indicator_dispatch(indicator))
            return false;
    }
    return true;
}

static void rounded_rect(cairo_t *draw, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(draw);
    cairo_arc(draw, x + w - r, y + r, r, -pi / 2, 0);
    cairo_arc(draw, x + w - r, y + h - r, r, 0, pi / 2);
    cairo_arc(draw, x + r, y + h - r, r, pi / 2, pi);
    cairo_arc(draw, x + r, y + r, r, pi, pi * 1.5);
    cairo_close_path(draw);
}

static bool draw_badge(void *pixels, const float levels[WAVEFORM_BARS])
{
    memset(pixels, 0, IMAGE_BYTES);
    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        pixels, CAIRO_FORMAT_ARGB32, PIXEL_WIDTH, PIXEL_HEIGHT, STRIDE);
    cairo_t *draw = cairo_create(surface);
    cairo_scale(draw, SCALE, SCALE);

    rounded_rect(draw, 1, 1, WIDTH - 2, HEIGHT - 2, 12);
    cairo_set_source_rgba(draw, 0.07, 0.08, 0.10, 0.96);
    cairo_fill(draw);
    cairo_set_line_cap(draw, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(draw, 2.25);

    double center_x = WIDTH / 2.0;
    double center_y = HEIGHT / 2.0;
    cairo_set_source_rgb(draw, 0.94, 0.95, 0.97);
    for (size_t bar = 0; bar < WAVEFORM_BARS; bar++) {
        double x = center_x + 6 * ((double)bar - (WAVEFORM_BARS - 1) / 2.0);
        double half_height = 0.4 + 22 * sqrt(levels[bar]);
        cairo_move_to(draw, x, center_y - half_height);
        cairo_line_to(draw, x, center_y + half_height);
    }
    cairo_stroke(draw);
    bool success = cairo_status(draw) == CAIRO_STATUS_SUCCESS;
    cairo_destroy(draw);
    cairo_surface_destroy(surface);
    return success;
}

static void buffer_released(void *data, struct wl_buffer *proxy)
{
    (void)proxy;
    ((struct indicator_buffer *)data)->busy = false;
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_released };

static bool make_buffers(struct indicator *indicator)
{
    int fd = memfd_create("voxflow-indicator", MFD_CLOEXEC);
    if (fd < 0)
        return false;
    if (ftruncate(fd, SHM_BYTES) < 0) {
        close(fd);
        return false;
    }
    void *pixels = mmap(NULL, SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return false;
    }
    indicator->pixels = pixels;
    struct wl_shm_pool *pool = wl_shm_create_pool(indicator->shm, fd, SHM_BYTES);
    close(fd);
    if (!pool)
        return false;
    bool ready = true;
    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct indicator_buffer *buffer = &indicator->buffers[i];
        buffer->proxy = wl_shm_pool_create_buffer(
            pool, i * IMAGE_BYTES, PIXEL_WIDTH, PIXEL_HEIGHT, STRIDE, WL_SHM_FORMAT_ARGB8888);
        if (!buffer->proxy || wl_buffer_add_listener(buffer->proxy, &buffer_listener, buffer) < 0) {
            ready = false;
            break;
        }
    }
    wl_shm_pool_destroy(pool);
    return ready;
}

struct indicator *indicator_open(void)
{
    struct indicator *indicator = calloc(1, sizeof *indicator);
    if (!indicator)
        return NULL;
    indicator->display = wl_display_connect(NULL);
    if (!indicator->display)
        goto fail;
    indicator->registry = wl_display_get_registry(indicator->display);
    if (!indicator->registry ||
        wl_registry_add_listener(indicator->registry, &registry_listener, indicator) < 0)
        goto fail;

    bool registry_ready = false;
    struct wl_callback *sync = wl_display_sync(indicator->display);
    if (!sync)
        goto fail;
    wl_callback_add_listener(sync, &sync_listener, &registry_ready);
    if (!wait_for(indicator, &registry_ready)) {
        if (!registry_ready)
            wl_callback_destroy(sync);
        goto fail;
    }
    if (!indicator->compositor || !indicator->shm || !indicator->shell || !make_buffers(indicator))
        goto fail;

    indicator->surface = wl_compositor_create_surface(indicator->compositor);
    if (!indicator->surface)
        goto fail;
    indicator->layer = zwlr_layer_shell_v1_get_layer_surface(
        indicator->shell, indicator->surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "voxflow");
    struct wl_region *input = wl_compositor_create_region(indicator->compositor);
    if (!indicator->layer || !input) {
        if (input)
            wl_region_destroy(input);
        goto fail;
    }

    wl_surface_set_input_region(indicator->surface, input);
    wl_region_destroy(input);
    wl_surface_set_buffer_scale(indicator->surface, SCALE);
    zwlr_layer_surface_v1_add_listener(indicator->layer, &layer_listener, indicator);
    zwlr_layer_surface_v1_set_size(indicator->layer, WIDTH, HEIGHT);
    zwlr_layer_surface_v1_set_anchor(indicator->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
    zwlr_layer_surface_v1_set_margin(indicator->layer, 0, 0, 16, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(indicator->layer, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        indicator->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(indicator->surface);
    if (wait_for(indicator, &indicator->configured))
        return indicator;

fail:
    indicator_close(indicator);
    return NULL;
}

static bool commit_buffer(struct indicator *indicator, struct indicator_buffer *buffer)
{
    buffer->busy = true;
    wl_surface_attach(indicator->surface, buffer->proxy, 0, 0);
    wl_surface_damage(indicator->surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(indicator->surface);
    return wl_display_flush(indicator->display) >= 0;
}

bool indicator_waveform(struct indicator *indicator, const float levels[WAVEFORM_BARS])
{
    /* Skip a frame if the compositor is still reading both recording buffers. */
    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct indicator_buffer *buffer = &indicator->buffers[i];
        if (!buffer->busy)
            return draw_badge(indicator->pixels + i * IMAGE_BYTES, levels) &&
                   commit_buffer(indicator, buffer);
    }
    return true;
}

int indicator_fd(struct indicator *indicator)
{
    return wl_display_get_fd(indicator->display);
}

bool indicator_dispatch(struct indicator *indicator)
{
    return wl_display_dispatch(indicator->display) >= 0 && !indicator->closed &&
           wl_display_flush(indicator->display) >= 0;
}

void indicator_close(struct indicator *indicator)
{
    if (!indicator)
        return;
    if (indicator->layer)
        zwlr_layer_surface_v1_destroy(indicator->layer);
    if (indicator->surface)
        wl_surface_destroy(indicator->surface);
    for (int i = 0; i < BUFFER_COUNT; i++)
        if (indicator->buffers[i].proxy)
            wl_buffer_destroy(indicator->buffers[i].proxy);
    if (indicator->pixels)
        munmap(indicator->pixels, SHM_BYTES);
    if (indicator->shell)
        zwlr_layer_shell_v1_destroy(indicator->shell);
    if (indicator->shm)
        wl_shm_destroy(indicator->shm);
    if (indicator->compositor)
        wl_compositor_destroy(indicator->compositor);
    if (indicator->registry)
        wl_registry_destroy(indicator->registry);
    if (indicator->display) {
        wl_display_flush(indicator->display);
        wl_display_disconnect(indicator->display);
    }
    free(indicator);
}
