#include <cogl/cogl.h>
#include <glib.h>
#include <stdio.h>
#include <sys/time.h>
#include <string.h>

#include <wayland-server.h>

CoglColor black;

typedef struct _CoglandCompositor CoglandCompositor;

typedef struct
{
  struct wl_buffer *wayland_buffer;
  CoglTexture2D *texture;
  GList *surfaces_attached_to;
} CoglandBuffer;

typedef struct
{
  CoglandCompositor *compositor;

  struct wl_surface wayland_surface;
  int x;
  int y;
  CoglandBuffer *buffer;
} CoglandSurface;

typedef struct
{
  struct wl_object wayland_output;

  int x;
  int y;
  int width;
  int height;
  CoglOnscreen *onscreen;
} CoglandOutput;

typedef struct
{
  GSource source;
  GPollFD pfd;
  struct wl_event_loop *loop;
} WaylandEventSource;

struct _CoglandCompositor
{
  struct wl_display *wayland_display;
  struct wl_compositor wayland_compositor;
  struct wl_shm *wayland_shm;
  struct wl_event_loop *wayland_loop;

  CoglDisplay *cogl_display;
  CoglContext *cogl_context;

  int virtual_width;
  int virtual_height;
  GList *outputs;

  CoglPrimitive *triangle;

  GSource *wayland_event_source;

  GList *surfaces;
};

static guint32
get_time (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static gboolean
wayland_event_source_prepare (GSource *base, int *timeout)
{
  *timeout = -1;

  return FALSE;
}

static gboolean
wayland_event_source_check (GSource *base)
{
  WaylandEventSource *source = (WaylandEventSource *)base;
  return source->pfd.revents;
}

static gboolean
wayland_event_source_dispatch (GSource *base,
                                GSourceFunc callback,
                                void *data)
{
  WaylandEventSource *source = (WaylandEventSource *)base;
  wl_event_loop_dispatch (source->loop, 0);
  return TRUE;
}

static GSourceFuncs wayland_event_source_funcs =
{
  wayland_event_source_prepare,
  wayland_event_source_check,
  wayland_event_source_dispatch,
  NULL
};

GSource *
wayland_event_source_new (struct wl_event_loop *loop)
{
  WaylandEventSource *source;

  source = (WaylandEventSource *) g_source_new (&wayland_event_source_funcs,
                                                sizeof (WaylandEventSource));
  source->loop = loop;
  source->pfd.fd = wl_event_loop_get_fd (loop);
  source->pfd.events = G_IO_IN | G_IO_ERR;
  g_source_add_poll (&source->source, &source->pfd);

  return &source->source;
}

static CoglandBuffer *
cogland_buffer_new (struct wl_buffer *wayland_buffer)
{
  CoglandBuffer *buffer = g_slice_new (CoglandBuffer);

  buffer->wayland_buffer = wayland_buffer;
  buffer->texture = NULL;
  buffer->surfaces_attached_to = NULL;

  return buffer;
}

static void
cogland_buffer_free (CoglandBuffer *buffer)
{
  GList *l;

  buffer->wayland_buffer->user_data = NULL;

  for (l = buffer->surfaces_attached_to; l; l = l->next)
    {
      CoglandSurface *surface = l->data;
      surface->buffer = NULL;
    }

  if (buffer->texture)
    cogl_object_unref (buffer->texture);

  g_list_free (buffer->surfaces_attached_to);
  g_slice_free (CoglandBuffer, buffer);
}

static void
shm_buffer_created (struct wl_buffer *wayland_buffer)
{
  wayland_buffer->user_data = cogland_buffer_new (wayland_buffer);
}

static CoglPixelFormat
get_buffer_format (struct wl_buffer *wayland_buffer)
{
  struct wl_compositor *compositor = wayland_buffer->compositor;
  struct wl_visual *visual = wayland_buffer->visual;
#if G_BYTE_ORDER == G_BIG_ENDIAN
  if (visual == &compositor->premultiplied_argb_visual)
    return COGL_PIXEL_FORMAT_ARGB_8888_PRE;
  else if (visual == &compositor->argb_visual)
    return COGL_PIXEL_FORMAT_ARGB_8888;
  else if (visual == &compositor->rgb_visual)
    return COGL_PIXEL_FORMAT_RGB_888;
#elif G_BYTE_ORDER == G_LITTLE_ENDIAN
  if (visual == &compositor->premultiplied_argb_visual)
    return COGL_PIXEL_FORMAT_BGRA_8888_PRE;
  else if (visual == &compositor->argb_visual)
    return COGL_PIXEL_FORMAT_BGRA_8888;
  else if (visual == &compositor->rgb_visual)
    return COGL_PIXEL_FORMAT_BGR_888;
#endif
  else
    g_return_val_if_reached (COGL_PIXEL_FORMAT_ANY);
}

static void
shm_buffer_damaged (struct wl_buffer *wayland_buffer,
		    gint32 x,
                    gint32 y,
                    gint32 width,
                    gint32 height)
{
  CoglandBuffer *buffer = wayland_buffer->user_data;

  if (buffer->texture)
    {
      cogl_texture_set_region (buffer->texture,
                               x, y,
                               x, y,
                               width, height,
                               width, height,
                               get_buffer_format (wayland_buffer),
                               wl_shm_buffer_get_stride (wayland_buffer),
                               wl_shm_buffer_get_data (wayland_buffer));
    }
}

static void
shm_buffer_destroyed (struct wl_buffer *wayland_buffer)
{
  if (wayland_buffer->user_data)
    cogland_buffer_free ((CoglandBuffer *)wayland_buffer->user_data);
}

const static struct wl_shm_callbacks shm_callbacks = {
  shm_buffer_created,
  shm_buffer_damaged,
  shm_buffer_destroyed
};

static void
cogland_surface_destroy (struct wl_client *wayland_client,
                         struct wl_surface *wayland_surface)
{
  wl_resource_destroy (&wayland_surface->resource, wayland_client, get_time ());
}

static void
cogland_surface_detach_buffer (CoglandSurface *surface)
{
  CoglandBuffer *buffer = surface->buffer;

  if (buffer)
    {
      buffer->surfaces_attached_to =
        g_list_remove (buffer->surfaces_attached_to, surface);
      if (buffer->surfaces_attached_to == NULL)
        cogland_buffer_free (buffer);
      surface->buffer = NULL;
    }
}

static void
cogland_surface_attach_buffer (struct wl_client *wayland_client,
                               struct wl_surface *wayland_surface,
                               struct wl_buffer *wayland_buffer,
                               gint32 dx, gint32 dy)
{
  CoglandBuffer *buffer = wayland_buffer->user_data;
  CoglandSurface *surface = container_of (wayland_surface,
                                          CoglandSurface,
                                          wayland_surface);
  CoglandCompositor *compositor = surface->compositor;

  cogland_surface_detach_buffer (surface);

  /* XXX: it seems like for shm buffers we will have been notified of
   * the buffer already via the callbacks, but for drm buffers I guess
   * this will be the first we know of them? */
  if (!buffer)
    {
      buffer = cogland_buffer_new (wayland_buffer);
      wayland_buffer->user_data = buffer;
    }

  /* wayland-drm.c: drm_create_buffer doesn't fill this in for us...*/
  if (!wayland_buffer->compositor)
    wayland_buffer->compositor = &compositor->wayland_compositor;

  g_return_if_fail (g_list_find (buffer->surfaces_attached_to, surface) == NULL);

  buffer->surfaces_attached_to = g_list_prepend (buffer->surfaces_attached_to,
                                                 surface);

  if (!buffer->texture)
    {
      GError *error = NULL;

      buffer->texture =
        cogl_wayland_texture_2d_new_from_buffer (compositor->cogl_context,
                                                 wayland_buffer,
                                                 &error);
      if (!buffer->texture)
        g_error ("Failed to create texture_2d from wayland buffer: %s",
                 error->message);
    }

  surface->buffer = buffer;
}

static void
cogland_surface_map_toplevel (struct wl_client *client,
                              struct wl_surface *surface)
{
}

static void
cogland_surface_map_transient (struct wl_client *client,
                               struct wl_surface *surface,
                               struct wl_surface *parent,
                               gint32 dx,
                               gint32 dy,
                               guint32 flags)
{
}

static void
cogland_surface_map_fullscreen (struct wl_client *client,
                                struct wl_surface *surface)
{
}

static void
cogland_surface_damage (struct wl_client *client,
                        struct wl_surface *surface,
                        gint32 x,
                        gint32 y,
                        gint32 width,
                        gint32 height)
{
}

const struct wl_surface_interface cogland_surface_interface = {
  cogland_surface_destroy,
  cogland_surface_attach_buffer,
  cogland_surface_map_toplevel,
  cogland_surface_map_transient,
  cogland_surface_map_fullscreen,
  cogland_surface_damage
};

static void
cogland_surface_free (CoglandSurface *surface)
{
  CoglandCompositor *compositor = surface->compositor;
  compositor->surfaces = g_list_remove (compositor->surfaces, surface);
  cogland_surface_detach_buffer (surface);
  g_slice_free (CoglandSurface, surface);
}

static void
cogland_surface_resource_destroy_cb (struct wl_resource *wayland_resource,
                                     struct wl_client *wayland_client)
{
  CoglandSurface *surface =
    container_of (wayland_resource, CoglandSurface, wayland_surface.resource);
  cogland_surface_free (surface);
}

static void
cogland_compositor_create_surface (struct wl_client *wayland_client,
                                   struct wl_compositor *wayland_compositor,
                                   guint32 wayland_id)
{
  CoglandCompositor *compositor = container_of (wayland_compositor,
                                                CoglandCompositor,
                                                wayland_compositor);
  CoglandSurface *surface = g_slice_new0 (CoglandSurface);
  surface->compositor = compositor;

  surface->wayland_surface.resource.destroy =
    cogland_surface_resource_destroy_cb;

  surface->wayland_surface.resource.object.id = wayland_id;
  surface->wayland_surface.resource.object.interface = &wl_surface_interface;
  surface->wayland_surface.resource.object.implementation =
          (void (**)(void)) &cogland_surface_interface;
  surface->wayland_surface.client = wayland_client;

  wl_client_add_resource (wayland_client, &surface->wayland_surface.resource);

  compositor->surfaces = g_list_prepend (compositor->surfaces,
                                         surface);
}

const static struct wl_compositor_interface cogland_compositor_interface =
{
  cogland_compositor_create_surface,
};

static void
cogland_output_post_geometry (struct wl_client *wayland_client,
                              struct wl_object *wayland_output,
                              guint32 version)
{
  CoglandOutput *output =
    container_of (wayland_output, CoglandOutput, wayland_output);

  wl_client_post_event (wayland_client,
                        wayland_output,
                        WL_OUTPUT_GEOMETRY,
                        output->x, output->y,
                        output->width, output->height);
}

static void
cogland_compositor_create_output (CoglandCompositor *compositor,
                                  int x,
                                  int y,
                                  int width,
                                  int height)
{
  CoglandOutput *output = g_slice_new0 (CoglandOutput);
  CoglFramebuffer *fb;
  GError *error = NULL;

  output->wayland_output.interface = &wl_output_interface;

  wl_display_add_object (compositor->wayland_display, &output->wayland_output);
  wl_display_add_global (compositor->wayland_display, &output->wayland_output,
                         cogland_output_post_geometry);

  output->onscreen = cogl_onscreen_new (compositor->cogl_context,
                                        width, height);
  /* Eventually there will be an implicit allocate on first use so this
   * will become optional... */
  fb = COGL_FRAMEBUFFER (output->onscreen);
  if (!cogl_framebuffer_allocate (fb, &error))
    g_error ("Failed to allocate framebuffer: %s\n", error->message);

  cogl_onscreen_show (output->onscreen);
#if 0
  cogl_framebuffer_set_viewport (fb, x, y, width, height);
#else
  cogl_push_framebuffer (fb);
  cogl_set_viewport (-x, -y,
                     compositor->virtual_width,
                     compositor->virtual_height);
  cogl_pop_framebuffer ();
#endif

  compositor->outputs = g_list_prepend (compositor->outputs, output);
}

static gboolean
paint_cb (void *user_data)
{
  CoglandCompositor *compositor = user_data;
  GList *l;

  for (l = compositor->outputs; l; l = l->next)
    {
      CoglandOutput *output = l->data;
      CoglFramebuffer *fb = COGL_FRAMEBUFFER (output->onscreen);
      GList *l2;

      cogl_push_framebuffer (fb);

#if 0
      cogl_framebuffer_clear (fb, COGL_BUFFER_BIT_COLOR);
#else
      cogl_clear (&black, COGL_BUFFER_BIT_COLOR);
#endif
      cogl_primitive_draw (compositor->triangle);

      for (l2 = compositor->surfaces; l2; l2 = l2->next)
        {
          CoglandSurface *surface = l2->data;

          if (surface->buffer)
            {
              CoglTexture2D *texture = surface->buffer->texture;
              cogl_set_source_texture (texture);
              cogl_rectangle (-1, 1, 1, -1);
            }
          wl_display_post_frame (compositor->wayland_display,
                                 &surface->wayland_surface, get_time ());
        }
      cogl_framebuffer_swap_buffers (fb);


      cogl_pop_framebuffer ();

    }

  return TRUE;
}

int
main (int argc, char **argv)
{
  CoglandCompositor compositor;
  GMainLoop *loop;
  GError *error = NULL;
  CoglVertexP2C4 triangle_vertices[] = {
      {0, 0.7, 0xff, 0x00, 0x00, 0x80},
      {-0.7, -0.7, 0x00, 0xff, 0x00, 0xff},
      {0.7, -0.7, 0x00, 0x00, 0xff, 0xff}
  };

  memset (&compositor, 0, sizeof (compositor));

  compositor.wayland_display = wl_display_create ();
  if (compositor.wayland_display == NULL)
    g_error ("failed to create wayland display");

  if (wl_compositor_init (&compositor.wayland_compositor,
                          &cogland_compositor_interface,
                          compositor.wayland_display) < 0)
    g_error ("Failed to init wayland compositor");

  compositor.wayland_shm = wl_shm_init (compositor.wayland_display,
                                        &shm_callbacks);
  if (!compositor.wayland_shm)
    g_error ("Failed to allocate setup wayland shm callbacks");

  loop = g_main_loop_new (NULL, FALSE);
  compositor.wayland_loop =
    wl_display_get_event_loop (compositor.wayland_display);
  compositor.wayland_event_source =
    wayland_event_source_new (compositor.wayland_loop);
  g_source_attach (compositor.wayland_event_source, NULL);

  compositor.cogl_display = cogl_display_new (NULL, NULL);
  cogl_wayland_display_set_compositor_display (compositor.cogl_display,
                                               compositor.wayland_display);

  compositor.cogl_context = cogl_context_new (compositor.cogl_display, &error);
  if (!compositor.cogl_context)
    g_error ("Failed to create a Cogl context: %s\n", error->message);

  compositor.virtual_width = 640;
  compositor.virtual_height = 480;

  /* Emulate compositing with multiple monitors... */
  cogland_compositor_create_output (&compositor, 0, 0, 320, 240);
  cogland_compositor_create_output (&compositor, 320, 0, 320, 240);
  cogland_compositor_create_output (&compositor, 0, 240, 320, 240);
  cogland_compositor_create_output (&compositor, 320, 240, 320, 240);

  if (wl_display_add_socket (compositor.wayland_display, "wayland-0"))
    g_error ("Failed to create socket");

  compositor.triangle = cogl_primitive_new_p2c4 (COGL_VERTICES_MODE_TRIANGLES,
                                                 3, triangle_vertices);

  g_timeout_add (16, paint_cb, &compositor);

  g_main_loop_run (loop);

  return 0;
}
