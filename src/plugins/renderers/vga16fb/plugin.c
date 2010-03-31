/* plugin.c - vga16fb renderer plugin
 *
 * Copyright (C) 2010 Canonical Ltd.
 *               2006-2009 Red Hat, Inc.
 *               2008 Charlie Brej <cbrej@cs.man.ac.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Scott James Remnant <scott@ubuntu.com>
 *             Charlie Brej <cbrej@cs.man.ac.uk>
 *             Kristian Høgsberg <krh@redhat.com>
 *             Peter Jones <pjones@redhat.com>
 *             Ray Strode <rstrode@redhat.com>
 */
#include "config.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <values.h>
#include <unistd.h>
#include <sys/io.h>

#include <linux/fb.h>

#include "ply-buffer.h"
#include "ply-event-loop.h"
#include "ply-list.h"
#include "ply-logger.h"
#include "ply-rectangle.h"
#include "ply-region.h"
#include "ply-terminal.h"

#include "ply-renderer.h"
#include "ply-renderer-plugin.h"

#include "vga.h"

#ifndef PLY_FRAME_BUFFER_DEFAULT_FB_DEVICE_NAME
#define PLY_FRAME_BUFFER_DEFAULT_FB_DEVICE_NAME "/dev/fb0"
#endif

struct _ply_renderer_head
{
  ply_pixel_buffer_t *pixel_buffer;
  ply_rectangle_t area;
  char *map_address;
  size_t size;

  uint16_t red[16];
  uint16_t green[16];
  uint16_t blue[16];
  uint32_t palette_size;
};

struct _ply_renderer_input_source
{
  ply_renderer_backend_t *backend;
  ply_fd_watch_t *terminal_input_watch;

  ply_buffer_t   *key_buffer;

  ply_renderer_input_source_handler_t handler;
  void           *user_data;
};

struct _ply_renderer_backend
{
  ply_event_loop_t *loop;
  ply_terminal_t *terminal;

  char *device_name;
  int   device_fd;

  ply_renderer_input_source_t input_source;
  ply_renderer_head_t head;
  ply_list_t *heads;

  unsigned int row_stride;

  uint32_t is_active : 1;
};

ply_renderer_plugin_interface_t *ply_renderer_backend_get_interface (void);
static void ply_renderer_head_redraw (ply_renderer_backend_t *backend,
                                      ply_renderer_head_t    *head);
static bool open_input_source (ply_renderer_backend_t      *backend,
                               ply_renderer_input_source_t *input_source);

static ply_renderer_backend_t *
create_backend (const char *device_name,
                ply_terminal_t *terminal)
{
  ply_renderer_backend_t *backend;

  backend = calloc (1, sizeof (ply_renderer_backend_t));

  if (device_name != NULL)
    backend->device_name = strdup (device_name);
  else if (getenv ("FRAMEBUFFER") != NULL)
    backend->device_name = strdup (getenv ("FRAMEBUFFER"));
  else
    backend->device_name =
      strdup (PLY_FRAME_BUFFER_DEFAULT_FB_DEVICE_NAME);

  backend->loop = ply_event_loop_get_default ();
  backend->head.map_address = MAP_FAILED;
  backend->heads = ply_list_new ();
  backend->input_source.key_buffer = ply_buffer_new ();
  backend->terminal = terminal;

  return backend;
}

static void
initialize_head (ply_renderer_backend_t *backend,
                 ply_renderer_head_t    *head)
{
  head->pixel_buffer = ply_pixel_buffer_new (head->area.width,
                                             head->area.height);
  ply_pixel_buffer_fill_with_color (backend->head.pixel_buffer, NULL,
                                    0.0, 0.0, 0.0, 1.0);

  memset (head->red, 0, sizeof head->red);
  memset (head->green, 0, sizeof head->green);
  memset (head->blue, 0, sizeof head->blue);

  head->palette_size = 0;

  ply_list_append_data (backend->heads, head);
}

static void
uninitialize_head (ply_renderer_backend_t *backend,
                   ply_renderer_head_t    *head)
{
  if (head->pixel_buffer != NULL)
    {
      ply_pixel_buffer_free (head->pixel_buffer);
      head->pixel_buffer = NULL;

      ply_list_remove_data (backend->heads, head);
    }
}

static void
destroy_backend (ply_renderer_backend_t *backend)
{

  free (backend->device_name);
  uninitialize_head (backend, &backend->head);

  ply_list_free (backend->heads);

  free (backend);
}

static void
set_palette (ply_renderer_backend_t *backend,
             ply_renderer_head_t    *head)
{
  struct fb_cmap cmap;

  if (backend->device_fd < 0)
    return;
  if (!head->palette_size)
    return;

  cmap.start = 0;
  cmap.len = head->palette_size;;
  cmap.red = head->red;
  cmap.green = head->green;
  cmap.blue = head->blue;
  cmap.transp = NULL;

  ioctl (backend->device_fd, FBIOPUTCMAP, &cmap);
}

static void
activate (ply_renderer_backend_t *backend)
{
  backend->is_active = true;

  if (backend->head.map_address != MAP_FAILED)
    ply_renderer_head_redraw (backend, &backend->head);
}

static void
deactivate (ply_renderer_backend_t *backend)
{
  backend->is_active = false;
}

static void
on_active_vt_changed (ply_renderer_backend_t *backend)
{
  if (ply_terminal_is_active (backend->terminal))
    {
      activate (backend);
    }
  else
    {
      deactivate (backend);
    }
}

static bool
open_device (ply_renderer_backend_t *backend)
{
  backend->device_fd = open (backend->device_name, O_RDWR);

  if (backend->device_fd < 0)
    {
      ply_trace ("could not open '%s': %m", backend->device_name);
      return false;
    }

  if (!ply_terminal_open (backend->terminal))
    {
      ply_trace ("could not open terminal: %m");
      return false;
    }

  if (!ply_terminal_is_vt (backend->terminal))
    {
      ply_trace ("terminal is not a VT");
      ply_terminal_close (backend->terminal);
      return false;
    }

  ply_terminal_watch_for_active_vt_change (backend->terminal,
                                           (ply_terminal_active_vt_changed_handler_t)
                                           on_active_vt_changed,
                                           backend);

  return true;
}

static void
close_device (ply_renderer_backend_t *backend)
{

  ply_terminal_stop_watching_for_active_vt_change (backend->terminal,
                                                   (ply_terminal_active_vt_changed_handler_t)
                                                   on_active_vt_changed,
                                                   backend);
  uninitialize_head (backend, &backend->head);

  close (backend->device_fd);
  backend->device_fd = -1;

  backend->head.area.x = 0;
  backend->head.area.y = 0;
  backend->head.area.width = 0;
  backend->head.area.height = 0;
}

static bool
query_device (ply_renderer_backend_t *backend)
{
  struct fb_var_screeninfo variable_screen_info;
  struct fb_fix_screeninfo fixed_screen_info;

  assert (backend != NULL);
  assert (backend->device_fd >= 0);

  if (ioctl (backend->device_fd, FBIOGET_VSCREENINFO, &variable_screen_info) < 0)
    return false;

  if (ioctl (backend->device_fd, FBIOGET_FSCREENINFO, &fixed_screen_info) < 0)
    return false;

  /* We only support the vga16fb with its own kooky planar colour mode. */
  if ((fixed_screen_info.type != FB_TYPE_VGA_PLANES)
      || (fixed_screen_info.type_aux != FB_AUX_VGA_PLANES_VGA4)
      || (fixed_screen_info.visual != FB_VISUAL_PSEUDOCOLOR)
      || (variable_screen_info.bits_per_pixel != 4))
    {
      ply_trace ("Doesn't look like vga16fb\n");
      return false;
    }

  backend->head.area.x = variable_screen_info.xoffset;
  backend->head.area.y = variable_screen_info.yoffset;
  backend->head.area.width = variable_screen_info.xres;
  backend->head.area.height = variable_screen_info.yres;

  backend->row_stride = fixed_screen_info.line_length;
  backend->head.size = backend->head.area.height * backend->row_stride;

  initialize_head (backend, &backend->head);

  return true;

}

static bool
map_to_device (ply_renderer_backend_t *backend)
{
  ply_renderer_head_t *head;

  assert (backend != NULL);
  assert (backend->device_fd >= 0);

  head = &backend->head;
  assert (head->size > 0);

  if (ioperm (VGA_REGS_BASE, VGA_REGS_LEN, 1) < 0) {
    ply_trace ("could not obtain permission to write to VGA regs: %m");
    return false;
  }

  head->map_address = mmap (NULL, head->size, PROT_WRITE,
                            MAP_SHARED, backend->device_fd, 0);

  if (head->map_address == MAP_FAILED) {
    ply_trace ("could not map VGA memory: %m");
    return false;
  }

  if (ply_terminal_is_active (backend->terminal))
      activate (backend);
  else
      ply_terminal_activate_vt (backend->terminal);

  return true;
}

static void
unmap_from_device (ply_renderer_backend_t *backend)
{
  ply_renderer_head_t *head;

  head = &backend->head;

  if (head->map_address != MAP_FAILED)
    {
      munmap (head->map_address, head->size);
      head->map_address = MAP_FAILED;
    }
}

static unsigned int
argb32_pixel_value_to_color_index (ply_renderer_backend_t *backend,
                                   ply_renderer_head_t    *head,
                                   uint32_t                pixel_value)
{
  uint16_t red, green, blue;
  unsigned int shift, index;

  red = (pixel_value >> 16) & 0xff;
  green = (pixel_value >> 8) & 0xff;
  blue = pixel_value & 0xff;

  /* The 6 here is entirely arbitrary; that means we keep the top two bits
   * of each colour when comparing against existing colors in the palette;
   * in theory meaning a maximum of 64 -- that's still too many, so we
   * then try again with 7 bits and a maximum of 8 -- in between those two
   * is the 16 we actually have room for.
   */
  for (shift = 6; shift < 8; shift++)
    {
      for (index = 0; index < head->palette_size; index++)
        if (   ((head->red[index] >> (8 + shift)) == (red >> shift))
            && ((head->green[index] >> (8 + shift)) == (green >> shift))
            && ((head->blue[index] >> (8 + shift)) == (blue >> shift)))
          return index;

      if (head->palette_size < 16)
        {
          index = head->palette_size++;

          head->red[index] = red << 8;
          head->green[index] = green << 8;
          head->blue[index] = blue << 8;

          set_palette (backend, head);
          ply_trace ("palette now has %d colours (added %06x)\n",
                     head->palette_size, pixel_value & 0xffffff);

          return index;
        }
    }

  /* Didn't find a colour, so just return the last
   * (first is probably background colour so a bad choice)
   */
  return head->palette_size - 1;;
}

static void
flush_area (ply_renderer_backend_t *backend,
            ply_renderer_head_t    *head,
            ply_rectangle_t        *area_to_flush)
{
  unsigned char *mask;
  uint32_t *shadow_buffer;
  unsigned long x1, x2, y1, y2, x, y;
  unsigned int c, b;

  mask = malloc (backend->row_stride * 16);

  shadow_buffer = ply_pixel_buffer_get_argb32_data (backend->head.pixel_buffer);

  x1 = area_to_flush->x;
  y1 = area_to_flush->y;
  x2 = x1 + area_to_flush->width;
  y2 = y1 + area_to_flush->height;

  for (y = y1; y < y2; y++)
    {
      memset (mask, 0, backend->row_stride * 16);

      for (x = x1; x < x2; x++)
        {
          unsigned int index;
          uint32_t pixel;

          pixel = shadow_buffer[x + y * head->area.width];
          index = argb32_pixel_value_to_color_index (backend, head, pixel);

          mask[index * backend->row_stride + x / 8] |= (0x80 >> (x % 8));
        }

      for (c = 0; c < 16; c++)
        {
          for (b = x1 / 8; b < x2 / 8 + 1; b++)
            {
              char *p;

              if (!mask[c * backend->row_stride + b])
                continue;

              vga_set_reset (c);
              vga_bit_mask (mask[c * backend->row_stride + b]);

              p = head->map_address + y * backend->row_stride + b;
              *p |= 1;
            }
        }
    }

  free (mask);
}

static void
flush_head (ply_renderer_backend_t *backend,
            ply_renderer_head_t    *head)
{
  ply_region_t *updated_region;
  ply_list_t *areas_to_flush;
  ply_list_node_t *node;
  ply_pixel_buffer_t *pixel_buffer;

  assert (backend != NULL);
  assert (&backend->head == head);

  if (!backend->is_active)
    return;

  ply_terminal_set_mode (backend->terminal, PLY_TERMINAL_MODE_GRAPHICS);
  ply_terminal_set_unbuffered_input (backend->terminal);

  /* Reset to basic values; enable use of the Set/Reset register for all
   * planes.
   */
  vga_enable_set_reset (0xf);
  vga_mode (0);
  vga_data_rotate (0);
  vga_map_mask (0xff);

  set_palette (backend, &backend->head);

  pixel_buffer = head->pixel_buffer;
  updated_region = ply_pixel_buffer_get_updated_areas (pixel_buffer);
  areas_to_flush = ply_region_get_sorted_rectangle_list (updated_region);

  node = ply_list_get_first_node (areas_to_flush);
  while (node != NULL)
    {
      ply_list_node_t *next_node;
      ply_rectangle_t *area_to_flush;

      area_to_flush = (ply_rectangle_t *) ply_list_node_get_data (node);

      next_node = ply_list_get_next_node (areas_to_flush, node);

      flush_area (backend, head, area_to_flush);

      node = next_node;
    }

  ply_region_clear (updated_region);
}

static void
ply_renderer_head_redraw (ply_renderer_backend_t *backend,
                          ply_renderer_head_t    *head)
{
  ply_region_t *region;

  region = ply_pixel_buffer_get_updated_areas (head->pixel_buffer);

  ply_region_add_rectangle (region, &head->area);

  flush_head (backend, head);
}

static ply_list_t *
get_heads (ply_renderer_backend_t *backend)
{
  return backend->heads;
}

static ply_pixel_buffer_t *
get_buffer_for_head (ply_renderer_backend_t *backend,
                     ply_renderer_head_t    *head)
{

  if (head != &backend->head)
    return NULL;

  return backend->head.pixel_buffer;
}

static bool
has_input_source (ply_renderer_backend_t      *backend,
                  ply_renderer_input_source_t *input_source)
{
  return input_source == &backend->input_source;
}

static ply_renderer_input_source_t *
get_input_source (ply_renderer_backend_t *backend)
{
  return &backend->input_source;
}

static void
on_key_event (ply_renderer_input_source_t *input_source,
              int                          terminal_fd)
{
  ply_buffer_append_from_fd (input_source->key_buffer,
                             terminal_fd);

  if (input_source->handler != NULL)
    input_source->handler (input_source->user_data, input_source->key_buffer, input_source);

}

static void
on_input_source_disconnected (ply_renderer_input_source_t *input_source)
{
  ply_trace ("input source disconnected, reopening");
  open_input_source (input_source->backend, input_source);
}

static bool
open_input_source (ply_renderer_backend_t      *backend,
                   ply_renderer_input_source_t *input_source)
{
  int terminal_fd;

  assert (backend != NULL);
  assert (has_input_source (backend, input_source));

  terminal_fd = ply_terminal_get_fd (backend->terminal);

  input_source->backend = backend;
  input_source->terminal_input_watch = ply_event_loop_watch_fd (backend->loop, terminal_fd, PLY_EVENT_LOOP_FD_STATUS_HAS_DATA,
                                                                (ply_event_handler_t) on_key_event,
                                                                (ply_event_handler_t) on_input_source_disconnected,
                                                                input_source);
  return true;
}

static void
set_handler_for_input_source (ply_renderer_backend_t      *backend,
                              ply_renderer_input_source_t *input_source,
                              ply_renderer_input_source_handler_t handler,
                              void                        *user_data)
{
  assert (backend != NULL);
  assert (has_input_source (backend, input_source));

  input_source->handler = handler;
  input_source->user_data = user_data;
}

static void
close_input_source (ply_renderer_backend_t      *backend,
                    ply_renderer_input_source_t *input_source)
{
  assert (backend != NULL);
  assert (has_input_source (backend, input_source));

  ply_event_loop_stop_watching_fd (backend->loop, input_source->terminal_input_watch);
  input_source->terminal_input_watch = NULL;
  input_source->backend = NULL;
}

ply_renderer_plugin_interface_t *
ply_renderer_backend_get_interface (void)
{
  static ply_renderer_plugin_interface_t plugin_interface =
    {
      .create_backend = create_backend,
      .destroy_backend = destroy_backend,
      .open_device = open_device,
      .close_device = close_device,
      .query_device = query_device,
      .map_to_device = map_to_device,
      .unmap_from_device = unmap_from_device,
      .activate = activate,
      .deactivate = deactivate,
      .flush_head = flush_head,
      .get_heads = get_heads,
      .get_buffer_for_head = get_buffer_for_head,
      .get_input_source = get_input_source,
      .open_input_source = open_input_source,
      .set_handler_for_input_source = set_handler_for_input_source,
      .close_input_source = close_input_source
    };

  return &plugin_interface;
}
/* vim: set ts=4 sw=4 et ai ci cino={.5s,^-2,+.5s,t0,g0,e-2,n-2,p2s,(0,=.5s,:.5s */
