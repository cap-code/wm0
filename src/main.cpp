#include "xconnection.h"
#include <X11/keysym.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xproto.h>

struct Client {
  xcb_window_t frame;
  xcb_window_t titlebar;
  xcb_window_t window;
  xcb_gcontext_t titlebar_gc;
  int x, y;
  int width, height;
  std::string title;
};

struct DragState {
  bool active = false;
  xcb_window_t frame = XCB_NONE;
  int start_root_x = 0;
  int start_root_y = 0;
  int start_x = 0;
  int start_y = 0;
};

enum ResizeEdge {
  RESIZE_NONE = 0,
  RESIZE_LEFT = 1 << 0,
  RESIZE_RIGHT = 1 << 1,
  RESIZE_TOP = 1 << 2,
  RESIZE_BOTTOM = 1 << 3
};

struct WMCursors {
  xcb_cursor_t normal;
  xcb_cursor_t move;
  xcb_cursor_t resize_h;
  xcb_cursor_t resize_v;
  xcb_cursor_t resize_diag1;
  xcb_cursor_t resize_diag2;
};

struct ResizeState {
  bool active = false;
  xcb_window_t frame = XCB_NONE;
  int edges = RESIZE_NONE;
  int start_root_x = 0;
  int start_root_y = 0;
  int start_x = 0;
  int start_y = 0;
  int start_w = 0;
  int start_h = 0;
};

static const int RESIZE_BORDER = 10;
static const int MIN_WIDTH = 100;
static const int MIN_HEIGHT = 80;
static const int TITLE_HEIGHT = 24;
static const int gap = 20;

static const uint32_t COLOR_ACTIVE = 0x005577FF;
static const uint32_t COLOR_INACTIVE = 0x333333FF;
static const uint32_t COLOR_TEXT = 0xFFFFFFFF;

static int next_x = 50;
static int next_y = 50;
static int row_height = 0;

std::string get_window_title(xcb_connection_t *conn, xcb_window_t win) {

  xcb_intern_atom_cookie_t name_cookie =
      xcb_intern_atom(conn, 0, 12, "_NET_WM_NAME");

  xcb_intern_atom_reply_t *name_reply =
      xcb_intern_atom_reply(conn, name_cookie, nullptr);

  if (!name_reply) {
    name_cookie = xcb_intern_atom(conn, 0, 7, "WM_NAME");
    name_reply = xcb_intern_atom_reply(conn, name_cookie, nullptr);
    if (!name_reply)
      return "";
  }

  xcb_atom_t name_atom = name_reply->atom;
  free(name_reply);

  xcb_get_property_cookie_t prop_cookie = xcb_get_property(
      conn, 0, win, name_atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 1024);
  xcb_get_property_reply_t *prop =
      xcb_get_property_reply(conn, prop_cookie, nullptr);

  if (!prop)
    return "";

  std::string title;

  if (xcb_get_property_value_length(prop) > 0) {
    title.assign(static_cast<char *>(xcb_get_property_value(prop)),
                 xcb_get_property_value_length(prop));
  }

  free(prop);

  std::cout << "window title: " << title << "\n" << std::endl;
  return title;
}

void draw_titlebar(xcb_connection_t *conn, xcb_window_t win, xcb_gcontext_t gc,
                   uint32_t color, const std::string &title, int width) {
  std::cout << "Drawing titlebar for window: " << win << "\n" << std::endl;
  uint32_t vals[] = {color, COLOR_TEXT};

  xcb_change_gc(conn, gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, vals);

  xcb_rectangle_t rect = {0, 0, static_cast<uint16_t>(width), TITLE_HEIGHT};

  xcb_poly_fill_rectangle(conn, win, gc, 1, &rect);

  if (!title.empty()) {
    xcb_image_text_8(conn, title.size(), win, gc, 8, 16, title.c_str());
  }
}

bool position_hints(xcb_connection_t *conn, xcb_window_t win, int &x, int &y,
                    bool &user_specified) {
  xcb_size_hints_t hints;

  if (!xcb_icccm_get_wm_normal_hints_reply(
          conn, xcb_icccm_get_wm_normal_hints(conn, win), &hints, nullptr)) {
    return false;
  }

  if (hints.flags & XCB_ICCCM_SIZE_HINT_US_POSITION) {
    x = hints.x;
    y = hints.y;
    user_specified = true;
    return true;
  }

  if (hints.flags & XCB_ICCCM_SIZE_HINT_P_POSITION) {
    x = hints.x;
    y = hints.y;
    user_specified = false;
    return true;
  }

  return false;
}

void set_cursor(xcb_connection_t *conn, xcb_window_t win, xcb_cursor_t cursor) {
  uint32_t cursor_vals[] = {cursor};
  xcb_change_window_attributes(conn, win, XCB_CW_CURSOR, cursor_vals);
}

xcb_cursor_t cursor_for_edges(const WMCursors &cursors, int edges) {
  if ((edges & RESIZE_LEFT) && (edges & RESIZE_TOP)) {
    return cursors.resize_diag1;
  }
  if ((edges & RESIZE_RIGHT) && (edges & RESIZE_TOP)) {
    return cursors.resize_diag2;
  }
  if ((edges & RESIZE_LEFT) || (edges & RESIZE_RIGHT)) {
    return cursors.resize_h;
  }
  if ((edges & RESIZE_TOP) || (edges & RESIZE_BOTTOM)) {
    return cursors.resize_v;
  }
  return cursors.normal;
}

int main() {
  XConnection conn;

  if (!conn.is_valid()) {
    return 1;
  }

  std::cout << "XConnection established \n" << std::endl;

  const xcb_setup_t *setup = xcb_get_setup(conn.get());
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
  xcb_screen_t *screen = it.data;

  std::unordered_map<xcb_window_t, Client> clients;
  std::vector<xcb_window_t> client_order;
  DragState drag;
  ResizeState resize;
  xcb_window_t focused_window = XCB_NONE;
  WMCursors cursors;

  std::cout << "Screen size: " << screen->width_in_pixels << "x"
            << screen->height_in_pixels << "\n"
            << std::endl;

  std::cout << "Root winow id: " << screen->root << "\n" << std::endl;

  uint32_t root_events[] = {XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                            XCB_EVENT_MASK_KEY_PRESS};

  xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(
      conn.get(), screen->root, XCB_CW_EVENT_MASK, root_events);

  xcb_generic_error_t *error = xcb_request_check(conn.get(), cookie);

  if (error) {
    std::cerr << "Failed to get the ownership of root windown\n" << std::endl;
    free(error);
    return 1;
  }

  xcb_cursor_context_t *cursor_ctx;
  xcb_cursor_context_new(conn.get(), screen, &cursor_ctx);

  cursors.normal = xcb_cursor_load_cursor(cursor_ctx, "left_ptr");
  cursors.move = xcb_cursor_load_cursor(cursor_ctx, "fleur");
  cursors.resize_h = xcb_cursor_load_cursor(cursor_ctx, "sb_h_double_arrow");
  cursors.resize_v = xcb_cursor_load_cursor(cursor_ctx, "sb_v_double_arrow");
  cursors.resize_diag1 = xcb_cursor_load_cursor(cursor_ctx, "top_left_corner");
  cursors.resize_diag2 = xcb_cursor_load_cursor(cursor_ctx, "top_right_corner");

  xcb_cursor_context_free(cursor_ctx);

  xcb_key_symbols_t *keysyms = xcb_key_symbols_alloc(conn.get());

  auto grab_key = [&](xcb_keysym_t sym, uint16_t mod) {
    xcb_keycode_t *codes = xcb_key_symbols_get_keycode(keysyms, sym);
    for (int i = 0; codes[i] != XCB_NO_SYMBOL; i++) {
      xcb_grab_key(conn.get(), 1, screen->root, mod, codes[i],
                   XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
    free(codes);
  };

  grab_key(XK_F4, XCB_MOD_MASK_1);
  grab_key(XK_Tab, XCB_MOD_MASK_1);

  xcb_flush(conn.get());

  std::cout << "WM running ...\n" << std::endl;

  while (true) {
    xcb_generic_event_t *event = xcb_wait_for_event(conn.get());
    if (!event)
      break;

    uint8_t type = event->response_type & ~0x80;

    switch (type) {
    case XCB_CONFIGURE_REQUEST: {
      auto *e = reinterpret_cast<xcb_configure_request_event_t *>(event);

      std::cout << "Configure request received for window: " << e->window
                << "\n"
                << std::endl;

      auto it = clients.find(e->window);
      if (it == clients.end()) {
        uint32_t values[7];
        int i = 0;

        if (e->value_mask & XCB_CONFIG_WINDOW_X)
          values[i++] = e->x;
        if (e->value_mask & XCB_CONFIG_WINDOW_Y)
          values[i++] = e->y;
        if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
          values[i++] = e->width;
        if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
          values[i++] = e->height;
        if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
          values[i++] = e->border_width;
        if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING)
          values[i++] = e->sibling;
        if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
          values[i++] = e->stack_mode;

        xcb_configure_window(conn.get(), e->window, e->value_mask, values);
        break;
      }

      Client &c = it->second;

      if (e->value_mask & XCB_CONFIG_WINDOW_X)
        c.x = e->x;
      if (e->value_mask & XCB_CONFIG_WINDOW_Y)
        c.y = e->y;
      if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
        c.width = e->width;
      if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
        c.height = e->height;

      uint32_t frame_vals[] = {
          static_cast<uint32_t>(c.x), static_cast<uint32_t>(c.y),
          static_cast<uint32_t>(c.width), static_cast<uint32_t>(c.height)};

      xcb_configure_window(conn.get(), c.frame,
                           XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                               XCB_CONFIG_WINDOW_WIDTH |
                               XCB_CONFIG_WINDOW_HEIGHT,
                           frame_vals);
      xcb_configure_window(conn.get(), c.window,
                           XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                           &frame_vals[2]);

      break;
    }
    case XCB_MAP_REQUEST: {
      std::cout << "Map request received\n" << std::endl;
      auto *e = reinterpret_cast<xcb_map_request_event_t *>(event);

      xcb_get_window_attributes_cookie_t attr_cookie =
          xcb_get_window_attributes(conn.get(), e->window);
      xcb_get_window_attributes_reply_t *attr =
          xcb_get_window_attributes_reply(conn.get(), attr_cookie, nullptr);

      if (!attr)
        break;

      if (attr->override_redirect) {
        free(attr);
        xcb_map_window(conn.get(), e->window);
        break;
      }

      free(attr);

      if (clients.find(e->window) != clients.end()) {
        break;
      }

      auto geom_cookie = xcb_get_geometry(conn.get(), e->window);
      auto *geom = xcb_get_geometry_reply(conn.get(), geom_cookie, nullptr);

      int x = 0, y = 0;
      bool user_pos = false;
      int width = geom ? geom->width : 800;
      int height = geom ? geom->height : 400;

      if (geom)
        free(geom);

      if (!position_hints(conn.get(), e->window, x, y, user_pos)) {

        x = next_x;
        y = next_y;

        next_x += width + gap;
        row_height = std::max(row_height, height + TITLE_HEIGHT);
        if (next_x + width > screen->width_in_pixels) {
          next_x = 50;
          next_y += row_height + gap;
          row_height = 0;
        }
        if (next_y + row_height + TITLE_HEIGHT > screen->height_in_pixels) {
          next_y = 50;
          next_x += width + gap;
          row_height = 0;
        }
      }
      xcb_window_t frame = xcb_generate_id(conn.get());

      xcb_window_t titlebar = xcb_generate_id(conn.get());

      uint32_t frame_events[] = {
          XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
          XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
          XCB_EVENT_MASK_PROPERTY_CHANGE};

      xcb_create_window(conn.get(), XCB_COPY_FROM_PARENT, frame, screen->root,
                        x, y, width, height, 10, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                        screen->root_visual, XCB_CW_EVENT_MASK, frame_events);

      uint32_t titlebar_events[] = {
          XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE |
          XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION};

      xcb_create_window(conn.get(), XCB_COPY_FROM_PARENT, titlebar, frame, 0, 0,
                        width, TITLE_HEIGHT, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                        screen->root_visual, XCB_CW_EVENT_MASK,
                        titlebar_events);

      xcb_reparent_window(conn.get(), e->window, frame, 0, TITLE_HEIGHT);

      xcb_grab_button(conn.get(), 0, e->window,
                      XCB_EVENT_MASK_BUTTON_PRESS |
                          XCB_EVENT_MASK_BUTTON_RELEASE,
                      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
                      XCB_NONE, XCB_BUTTON_INDEX_1, XCB_MOD_MASK_ANY);

      uint32_t titlebar_client_events[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};

      xcb_change_window_attributes(conn.get(), e->window, XCB_CW_EVENT_MASK,
                                   titlebar_client_events);

      xcb_gcontext_t titlebar_gc = xcb_generate_id(conn.get());
      uint32_t titlebar_gc_vals[] = {COLOR_INACTIVE};
      xcb_create_gc(conn.get(), titlebar_gc, titlebar, XCB_GC_FOREGROUND,
                    titlebar_gc_vals);

      xcb_map_window(conn.get(), titlebar);
      xcb_map_window(conn.get(), frame);
      xcb_map_window(conn.get(), e->window);

      std::string title = get_window_title(conn.get(), e->window);
      if (title.empty())
        title = "Untitled";
      clients[e->window] = {frame, titlebar, e->window, titlebar_gc, x,
                            y,     width,    height,    title};
      client_order.push_back(e->window);
      break;
    }

    case XCB_BUTTON_PRESS: {
      auto *e = reinterpret_cast<xcb_button_press_event_t *>(event);

      std::cout << "Button press event received for window: " << e->event
                << "\n"
                << std::endl;

      for (auto &[window, client] : clients) {

        if ((e->event == client.frame) || (e->event == client.window) ||
            (e->child == client.window) || (e->event == client.titlebar)) {

          if (e->detail == 1) {
            auto geom_cookie = xcb_get_geometry(conn.get(), client.frame);

            auto *geom =
                xcb_get_geometry_reply(conn.get(), geom_cookie, nullptr);

            if (!geom)
              break;

            int rx = e->root_x - geom->x;
            int ry = e->root_y - geom->y;

            resize.edges = RESIZE_NONE;
            if (rx < RESIZE_BORDER)
              resize.edges |= RESIZE_LEFT;
            if (rx > geom->width - RESIZE_BORDER)
              resize.edges |= RESIZE_RIGHT;
            if (ry < RESIZE_BORDER)
              resize.edges |= RESIZE_TOP;
            if (ry > geom->height - RESIZE_BORDER)
              resize.edges |= RESIZE_BOTTOM;

            if (resize.edges != RESIZE_NONE) {
              resize.active = true;
              resize.frame = client.frame;
              resize.start_root_x = e->root_x;
              resize.start_root_y = e->root_y;
              resize.start_x = geom->x;
              resize.start_y = geom->y;
              resize.start_w = geom->width;
              resize.start_h = geom->height;
            } else if (e->event == client.titlebar) {
              drag.active = true;
              drag.frame = client.frame;
              drag.start_root_x = e->root_x;
              drag.start_root_y = e->root_y;
              drag.start_x = geom->x;
              drag.start_y = geom->y;
            }

            free(geom);
          }
          focused_window = client.window;

          if (resize.active) {
            set_cursor(conn.get(),client.frame,cursor_for_edges(cursors,resize.edges));
          } else if(drag.active) {
            set_cursor(conn.get(),client.frame,cursors.move);
          }

          xcb_set_input_focus(conn.get(), XCB_INPUT_FOCUS_POINTER_ROOT,
                              client.window, XCB_CURRENT_TIME);

          uint32_t values[] = {XCB_STACK_MODE_ABOVE};
          xcb_configure_window(conn.get(), client.frame,
                               XCB_CONFIG_WINDOW_STACK_MODE, values);

          break;
        }
      }
      for (auto &[window, client] : clients) {
        draw_titlebar(
            conn.get(), client.titlebar, client.titlebar_gc,
            (client.window == focused_window ? COLOR_ACTIVE : COLOR_INACTIVE),
            client.title, client.width);
      }
      break;
    }

    case XCB_DESTROY_NOTIFY: {
      auto *e = reinterpret_cast<xcb_destroy_notify_event_t *>(event);
      std::cout << "Destroy notify received for window: " << e->window << "\n"
                << std::endl;

      auto it = clients.find(e->window);

      if (it != clients.end()) {
        std::cout << "Destroy notify it: " << it->second.frame << "\n"
                  << std::endl;

        if (drag.frame == it->second.frame) {
          drag.active = false;
          drag.frame = XCB_NONE;
        }

        if (resize.frame == it->second.frame) {
          resize.active = false;
          resize.frame = XCB_NONE;
        }
        xcb_destroy_window(conn.get(), it->second.frame);
        xcb_free_gc(conn.get(), it->second.titlebar_gc);
        clients.erase(it);
        if (focused_window == e->window) {
          focused_window = XCB_NONE;
        }
        auto it_order =
            std::find(client_order.begin(), client_order.end(), e->window);
        if (it_order != client_order.end()) {
          client_order.erase(it_order);
        }
      }
      break;
    }

    case XCB_MOTION_NOTIFY: {
      auto *e = reinterpret_cast<xcb_motion_notify_event_t *>(event);

      std::cout << "Motion notify received for window: " << e->event << "\n"
                << std::endl;

      if (!resize.active && !drag.active) {
        std::cout << "No active resize or drag\n" << std::endl;
        for (auto &[window, client] : clients) {
          if (client.frame == e->event || client.titlebar == e->event) {
            auto geom_cookie = xcb_get_geometry(conn.get(), client.frame);
            auto *geom =
                xcb_get_geometry_reply(conn.get(), geom_cookie, nullptr);
            if (!geom)
              break;

            int rx = e->root_x - geom->x;
            int ry = e->root_y - geom->y;

            int edges = RESIZE_NONE;
            if (rx < RESIZE_BORDER)
              edges |= RESIZE_LEFT;
            if (rx > geom->width - RESIZE_BORDER)
              edges |= RESIZE_RIGHT;
            if (ry < RESIZE_BORDER)
              edges |= RESIZE_TOP;
            if (ry > geom->height - RESIZE_BORDER)
              edges |= RESIZE_BOTTOM;

            if (edges != RESIZE_NONE) {
              set_cursor(conn.get(), client.frame,
                         cursor_for_edges(cursors, edges));
            } else if (e->event == client.titlebar) {
              set_cursor(conn.get(), client.frame, cursors.move);
            } else {
              set_cursor(conn.get(), client.frame, cursors.normal);
            }

            free(geom);
            break;
          }
        }
      }

        if (resize.active) {
          int dx = e->root_x - resize.start_root_x;
          int dy = e->root_y - resize.start_root_y;

          int x = resize.start_x;
          int y = resize.start_y;
          int w = resize.start_w;
          int h = resize.start_h;

          if (resize.edges & RESIZE_RIGHT)
            w += dx;
          if (resize.edges & RESIZE_LEFT) {
            x += dx;
            w -= dx;
          }
          if (resize.edges & RESIZE_TOP) {
            y += dy;
            h -= dy;
          }
          if (resize.edges & RESIZE_BOTTOM)
            h += dy;

          if (w < MIN_WIDTH)
            w = MIN_WIDTH;
          if (h < MIN_HEIGHT)
            h = MIN_HEIGHT;

          uint32_t vals[] = {static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                             static_cast<uint32_t>(w),
                             static_cast<uint32_t>(h) + TITLE_HEIGHT};
          xcb_configure_window(conn.get(), resize.frame,
                               XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                   XCB_CONFIG_WINDOW_WIDTH |
                                   XCB_CONFIG_WINDOW_HEIGHT,
                               vals);

          uint32_t client_vals[] = {static_cast<uint32_t>(w),
                                    static_cast<uint32_t>(h)};

          for (auto &[window, client] : clients) {
            if (client.frame == resize.frame) {
              xcb_configure_window(conn.get(), client.window,
                                   XCB_CONFIG_WINDOW_WIDTH |
                                       XCB_CONFIG_WINDOW_HEIGHT,
                                   client_vals);

              client.x = x;
              client.y = y;
              client.width = w;
              client.height = h;

              xcb_configure_window(
                  conn.get(), client.titlebar,
                  XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, &w);
            }
          }
        }

        if (drag.active) {
          int dx = e->root_x - drag.start_root_x;
          int dy = e->root_y - drag.start_root_y;

          int new_x = drag.start_x + dx;
          int new_y = drag.start_y + dy;

          uint32_t values[] = {static_cast<uint32_t>(new_x),
                               static_cast<uint32_t>(new_y)};

          xcb_configure_window(conn.get(), drag.frame,
                               XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                               values);
        }

        break;
      }

    case XCB_BUTTON_RELEASE: {
      std::cout << "Button release event received for window: __ " << "\n"
                << std::endl;

      drag.active = false;
      drag.frame = XCB_NONE;
      resize.active = false;
      resize.frame = XCB_NONE;
      if(focused_window != XCB_NONE){
        auto& c = clients[focused_window];
        set_cursor(conn.get(),c.frame,cursors.normal);
      }
      break;
    }

    case XCB_EXPOSE: {
      auto *e = reinterpret_cast<xcb_expose_event_t *>(event);

      std::cout << "Expose event received for window: " << e->window << "\n"
                << std::endl;

      if (e->count != 0)
        break;

      for (auto &[window, client] : clients) {
        if (e->window == client.titlebar) {
          uint32_t color =
              (focused_window == client.window ? COLOR_ACTIVE : COLOR_INACTIVE);
          draw_titlebar(conn.get(), client.titlebar, client.titlebar_gc, color,
                        client.title, client.width);
          break;
        }
      }
      break;
    }
    case XCB_PROPERTY_NOTIFY: {
      auto *e = reinterpret_cast<xcb_property_notify_event_t *>(event);

      std::cout << "Property notify event triggered: " << e->window << "\n"
                << std::endl;

      auto it = clients.find(e->window);

      if (it == clients.end())
        break;

      xcb_intern_atom_cookie_t name_cookie =
          xcb_intern_atom(conn.get(), 1, 12, "_NET_WM_NAME");
      xcb_intern_atom_reply_t *name_reply =
          xcb_intern_atom_reply(conn.get(), name_cookie, nullptr);

      bool is_title_change = false;
      if (name_reply) {
        is_title_change = (e->atom == name_reply->atom);
        free(name_reply);
      }

      if (!is_title_change) {
        name_cookie = xcb_intern_atom(conn.get(), 1, 7, "WM_NAME");
        name_reply = xcb_intern_atom_reply(conn.get(), name_cookie, nullptr);
        if (name_reply) {
          is_title_change = (e->atom == name_reply->atom);
          free(name_reply);
        }
      }

      if (is_title_change) {
        Client &client = it->second;
        client.title = get_window_title(conn.get(), client.window);

        if (client.title.empty())
          client.title = "Untitled";

        std::cout << "Title changed: " << client.title << "\n" << std::endl;

        uint32_t color =
            (focused_window == client.window ? COLOR_ACTIVE : COLOR_INACTIVE);
        draw_titlebar(conn.get(), client.titlebar, client.titlebar_gc, color,
                      client.title, client.width);
      }

      break;
    }
    case XCB_KEY_PRESS: {
      auto *e = reinterpret_cast<xcb_key_press_event_t *>(event);

      std::cout << "Key press event received for window: " << e->event << "\n"
                << std::endl;

      xcb_keysym_t sym = xcb_key_symbols_get_keysym(keysyms, e->detail, 0);

      std::cout << "Key press: keycode=" << (int)e->detail
                << " state=" << e->state << " keysym=" << sym << "\n"
                << std::endl;

      uint16_t clean_state =
          e->state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2 | XCB_MOD_MASK_3);

      if ((clean_state & XCB_MOD_MASK_1) && sym == XK_F4) {
        std::cout << "inside  alt + f4 key press \n" << std::endl;
        if (focused_window != XCB_NONE) {
          std::cout << "focused window: " << focused_window << "\n"
                    << std::endl;
          Client c = clients[focused_window];

          xcb_destroy_window(conn.get(), c.frame);
        }
      }
      if ((clean_state & XCB_MOD_MASK_1) && sym == XK_Tab) {
        std::cout << "inside alt + tab  key press \n" << std::endl;
        if (!client_order.empty()) {
          auto it = std::find(client_order.begin(), client_order.end(),
                              focused_window);

          if (it == client_order.end() || ++it == client_order.end()) {
            it = client_order.begin();
          }
          focused_window = *it;
          Client &c = clients[focused_window];

          xcb_set_input_focus(conn.get(), XCB_INPUT_FOCUS_POINTER_ROOT,
                              c.window, XCB_CURRENT_TIME);

          uint32_t raise[] = {XCB_STACK_MODE_ABOVE};
          xcb_configure_window(conn.get(), c.frame,
                               XCB_CONFIG_WINDOW_STACK_MODE, raise);

          for (auto &[window, client] : clients) {
            draw_titlebar(conn.get(), client.titlebar, client.titlebar_gc,
                          (client.window == focused_window ? COLOR_ACTIVE
                                                           : COLOR_INACTIVE),
                          client.title, client.width);
          }
        }
      }

      break;
    }
    case XCB_CLIENT_MESSAGE: {
      auto *msg = reinterpret_cast<xcb_client_message_event_t *>(event);
      std::cout << "Client message received for window: " << msg->window << "\n"
                << std::endl;
      break;
    }
    default:
      break;
    }
    xcb_flush(conn.get());

    free(event);
  }

  return 0;
}