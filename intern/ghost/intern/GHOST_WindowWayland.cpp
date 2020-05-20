/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_WindowWayland.h"
#include "GHOST_SystemWayland.h"
#include "GHOST_WindowManager.h"

#include "GHOST_Event.h"

#include "GHOST_ContextEGL.h"
#include "GHOST_ContextNone.h"

#include <wayland-egl.h>

#include <libdecor.h>

struct window_t {
  GHOST_WindowWayland *w;
  wl_surface *surface;
  struct libdecor_frame *frame;
  wl_egl_window *egl_window;
  bool is_maximised;
  bool is_fullscreen;
  bool is_active;
  bool is_dialog;
  int32_t width, height;
};

/* -------------------------------------------------------------------- */
/** \name Wayland Interface Callbacks
 *
 * These callbacks are registered for Wayland interfaces and called when
 * an event is received from the compositor.
 * \{ */

static void frame_configure(struct libdecor_frame *frame,
                            struct libdecor_configuration *configuration,
                            void *data)
{
  window_t *win = static_cast<window_t *>(data);

  int width, height;
  enum libdecor_window_state window_state;
  struct libdecor_state *state;

  if (!libdecor_configuration_get_content_size(configuration, frame, &width, &height)) {
    width = win->width;
    height = win->height;
  }

  win->width = width;
  win->height = height;

  wl_egl_window_resize(win->egl_window, win->width, win->height, 0, 0);
  win->w->notify_size();

  if (!libdecor_configuration_get_window_state(configuration, &window_state))
    window_state = LIBDECOR_WINDOW_STATE_NONE;

  win->is_maximised = window_state & LIBDECOR_WINDOW_STATE_MAXIMIZED;
  win->is_fullscreen = window_state & LIBDECOR_WINDOW_STATE_FULLSCREEN;
  win->is_active = window_state & LIBDECOR_WINDOW_STATE_ACTIVE;

  win->is_active ? win->w->activate() : win->w->deactivate();

  state = libdecor_state_new(width, height);
  libdecor_frame_commit(frame, state, configuration);
  libdecor_state_free(state);
}

static void frame_close(struct libdecor_frame * /*frame*/, void *data)
{
  static_cast<window_t *>(data)->w->close();
}

static void frame_commit(struct libdecor_frame */*frame*/, void *data)
{
  /* we have to swap twice to keep any pop-up menues alive */
  static_cast<window_t *>(data)->w->swapBuffers();
  static_cast<window_t *>(data)->w->swapBuffers();
}

static struct libdecor_frame_interface libdecor_frame_iface = {
    frame_configure,
    frame_close,
    frame_commit,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Ghost Implementation
 *
 * Wayland specific implementation of the GHOST_Window interface.
 * \{ */

GHOST_TSuccess GHOST_WindowWayland::hasCursorShape(GHOST_TStandardCursor cursorShape)
{
  return m_system->hasCursorShape(cursorShape);
}

GHOST_WindowWayland::GHOST_WindowWayland(GHOST_SystemWayland *system,
                                         const char *title,
                                         GHOST_TInt32 /*left*/,
                                         GHOST_TInt32 /*top*/,
                                         GHOST_TUns32 width,
                                         GHOST_TUns32 height,
                                         GHOST_TWindowState state,
                                         const GHOST_IWindow *parentWindow,
                                         GHOST_TDrawingContextType type,
                                         const bool is_dialog,
                                         const bool stereoVisual,
                                         const bool exclusive)
    : GHOST_Window(width, height, state, stereoVisual, exclusive),
      m_system(system),
      w(new window_t)
{
  w->w = this;

  w->width = int32_t(width);
  w->height = int32_t(height);

  w->is_dialog = is_dialog;

  /* Window surfaces. */
  w->surface = wl_compositor_create_surface(m_system->compositor());
  w->egl_window = wl_egl_window_create(w->surface, int(width), int(height));

  wl_surface_set_user_data(w->surface, this);

  /* create window decorations */
  w->frame = libdecor_decorate(m_system->decoration(), w->surface, &libdecor_frame_iface, w);
  libdecor_frame_map(w->frame);

  if (parentWindow) {
    libdecor_frame_set_parent(w->frame,
                              dynamic_cast<const GHOST_WindowWayland *>(parentWindow)->w->frame);
  }

  /* Call registered callbacks. */
  wl_display_roundtrip(m_system->display());
  wl_display_roundtrip(m_system->display());
  wl_display_roundtrip(m_system->display());

#ifdef GHOST_OPENGL_ALPHA
  setOpaque();
#endif

  setTitle(title);

  /* EGL context. */
  if (setDrawingContextType(type) == GHOST_kFailure) {
    GHOST_PRINT("Failed to create EGL context" << std::endl);
  }
}

GHOST_TSuccess GHOST_WindowWayland::close()
{
  return m_system->pushEvent(
      new GHOST_Event(m_system->getMilliSeconds(), GHOST_kEventWindowClose, this));
}

GHOST_TSuccess GHOST_WindowWayland::activate()
{
  if (m_system->getWindowManager()->setActiveWindow(this) == GHOST_kFailure) {
    return GHOST_kFailure;
  }
  return m_system->pushEvent(
      new GHOST_Event(m_system->getMilliSeconds(), GHOST_kEventWindowActivate, this));
}

GHOST_TSuccess GHOST_WindowWayland::deactivate()
{
  m_system->getWindowManager()->setWindowInactive(this);
  return m_system->pushEvent(
      new GHOST_Event(m_system->getMilliSeconds(), GHOST_kEventWindowDeactivate, this));
}

GHOST_TSuccess GHOST_WindowWayland::notify_size()
{
#ifdef GHOST_OPENGL_ALPHA
  setOpaque();
#endif

  return m_system->pushEvent(
      new GHOST_Event(m_system->getMilliSeconds(), GHOST_kEventWindowSize, this));
}

wl_surface *GHOST_WindowWayland::surface() const
{
  return w->surface;
}

GHOST_TSuccess GHOST_WindowWayland::setWindowCursorGrab(GHOST_TGrabCursorMode mode)
{
  return m_system->setCursorGrab(mode, w->surface);
}

GHOST_TSuccess GHOST_WindowWayland::setWindowCursorShape(GHOST_TStandardCursor shape)
{
  const GHOST_TSuccess ok = m_system->setCursorShape(shape);
  m_cursorShape = (ok == GHOST_kSuccess) ? shape : GHOST_kStandardCursorDefault;
  return ok;
}

GHOST_TSuccess GHOST_WindowWayland::setWindowCustomCursorShape(GHOST_TUns8 *bitmap,
                                                               GHOST_TUns8 *mask,
                                                               int sizex,
                                                               int sizey,
                                                               int hotX,
                                                               int hotY,
                                                               bool canInvertColor)
{
  return m_system->setCustomCursorShape(bitmap, mask, sizex, sizey, hotX, hotY, canInvertColor);
}

void GHOST_WindowWayland::setTitle(const char *title)
{
  libdecor_frame_set_app_id(w->frame, title);
  libdecor_frame_set_title(w->frame, title);
  this->title = title;
}

std::string GHOST_WindowWayland::getTitle() const
{
  return this->title.empty() ? "untitled" : this->title;
}

void GHOST_WindowWayland::getWindowBounds(GHOST_Rect &bounds) const
{
  getClientBounds(bounds);
}

void GHOST_WindowWayland::getClientBounds(GHOST_Rect &bounds) const
{
  bounds.set(0, 0, w->width, w->height);
}

GHOST_TSuccess GHOST_WindowWayland::setClientWidth(GHOST_TUns32 width)
{
  return setClientSize(width, GHOST_TUns32(w->height));
}

GHOST_TSuccess GHOST_WindowWayland::setClientHeight(GHOST_TUns32 height)
{
  return setClientSize(GHOST_TUns32(w->width), height);
}

GHOST_TSuccess GHOST_WindowWayland::setClientSize(GHOST_TUns32 width, GHOST_TUns32 height)
{
  wl_egl_window_resize(w->egl_window, int(width), int(height), 0, 0);
  return GHOST_kSuccess;
}

void GHOST_WindowWayland::screenToClient(GHOST_TInt32 inX,
                                         GHOST_TInt32 inY,
                                         GHOST_TInt32 &outX,
                                         GHOST_TInt32 &outY) const
{
  outX = inX;
  outY = inY;
}

void GHOST_WindowWayland::clientToScreen(GHOST_TInt32 inX,
                                         GHOST_TInt32 inY,
                                         GHOST_TInt32 &outX,
                                         GHOST_TInt32 &outY) const
{
  outX = inX;
  outY = inY;
}

GHOST_WindowWayland::~GHOST_WindowWayland()
{
  releaseNativeHandles();

  libdecor_frame_unref(w->frame);

  wl_egl_window_destroy(w->egl_window);
  wl_surface_destroy(w->surface);

  delete w;
}

GHOST_TSuccess GHOST_WindowWayland::setWindowCursorVisibility(bool visible)
{
  return m_system->setCursorVisibility(visible);
}

GHOST_TSuccess GHOST_WindowWayland::setState(GHOST_TWindowState state)
{
  switch (state) {
    case GHOST_kWindowStateNormal:
      /* Unset states. */
      switch (getState()) {
        case GHOST_kWindowStateMaximized:
          libdecor_frame_unset_maximized(w->frame);
          break;
        case GHOST_kWindowStateFullScreen:
          libdecor_frame_unset_fullscreen(w->frame);
          break;
        default:
          break;
      }
      break;
    case GHOST_kWindowStateMaximized:
      libdecor_frame_set_maximized(w->frame);
      break;
    case GHOST_kWindowStateMinimized:
      libdecor_frame_set_minimized(w->frame);
      break;
    case GHOST_kWindowStateFullScreen:
      libdecor_frame_set_fullscreen(w->frame, nullptr);
      break;
    case GHOST_kWindowStateEmbedded:
      return GHOST_kFailure;
  }
  return GHOST_kSuccess;
}

GHOST_TWindowState GHOST_WindowWayland::getState() const
{
  if (w->is_fullscreen) {
    return GHOST_kWindowStateFullScreen;
  }
  else if (w->is_maximised) {
    return GHOST_kWindowStateMaximized;
  }
  else {
    return GHOST_kWindowStateNormal;
  }
}

GHOST_TSuccess GHOST_WindowWayland::invalidate()
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWayland::setOrder(GHOST_TWindowOrder /*order*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWayland::beginFullScreen() const
{
  libdecor_frame_set_fullscreen(w->frame, nullptr);
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWayland::endFullScreen() const
{
  libdecor_frame_unset_fullscreen(w->frame);
  return GHOST_kSuccess;
}

#ifdef GHOST_OPENGL_ALPHA
void GHOST_WindowWayland::setOpaque() const
{
  struct wl_region *region;

  /* make window opaque */
  region = wl_compositor_create_region(m_system->compositor());
  wl_region_add(region, 0, 0, w->width, w->height);
  wl_surface_set_opaque_region(w->surface, region);
  wl_region_destroy(region);
}
#endif

bool GHOST_WindowWayland::isDialog() const
{
  return w->is_dialog;
}

/**
 * \param type: The type of rendering context create.
 * \return Indication of success.
 */
GHOST_Context *GHOST_WindowWayland::newDrawingContext(GHOST_TDrawingContextType type)
{
  GHOST_Context *context;
  switch (type) {
    case GHOST_kDrawingContextTypeNone:
      context = new GHOST_ContextNone(m_wantStereoVisual);
      break;
    case GHOST_kDrawingContextTypeOpenGL:
      context = new GHOST_ContextEGL(m_wantStereoVisual,
                                     EGLNativeWindowType(w->egl_window),
                                     EGLNativeDisplayType(m_system->display()),
                                     EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                     3,
                                     3,
                                     GHOST_OPENGL_EGL_CONTEXT_FLAGS,
                                     GHOST_OPENGL_EGL_RESET_NOTIFICATION_STRATEGY,
                                     EGL_OPENGL_API);
      break;
  }

  return (context->initializeDrawingContext() == GHOST_kSuccess) ? context : nullptr;
}

/** \} */
