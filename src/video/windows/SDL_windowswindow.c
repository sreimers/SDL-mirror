/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2017 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_WINDOWS

#include "../../core/windows/SDL_windows.h"

#include "SDL_assert.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"

#include "SDL_windowsvideo.h"
#include "SDL_windowswindow.h"
#include "SDL_hints.h"

/* Dropfile support */
#include <shellapi.h>

/* This is included after SDL_windowsvideo.h, which includes windows.h */
#include "SDL_syswm.h"

/* Windows CE compatibility */
#ifndef SWP_NOCOPYBITS
#define SWP_NOCOPYBITS 0
#endif

/* #define HIGHDPI_DEBUG */

/* Fake window to help with DirectInput events. */
HWND SDL_HelperWindow = NULL;
static WCHAR *SDL_HelperWindowClassName = TEXT("SDLHelperWindowInputCatcher");
static WCHAR *SDL_HelperWindowName = TEXT("SDLHelperWindowInputMsgWindow");
static ATOM SDL_HelperWindowClass = 0;

// for borderless Windows, still want the following flags:
// - WS_CAPTION: this seems to enable the Windows minimize animation
// - WS_SYSMENU: enables system context menu on task bar
// - WS_MINIMIZEBOX: window will respond to Windows minimize commands sent to all windows, such as windows key + m, shaking title bar, etc.

#define STYLE_BASIC         (WS_CLIPSIBLINGS | WS_CLIPCHILDREN)
#define STYLE_FULLSCREEN    (WS_POPUP)
#define STYLE_BORDERLESS    (WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)
#define STYLE_NORMAL        (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)
#define STYLE_RESIZABLE     (WS_THICKFRAME | WS_MAXIMIZEBOX)
#define STYLE_MASK          (STYLE_FULLSCREEN | STYLE_BORDERLESS | STYLE_NORMAL | STYLE_RESIZABLE)

static DWORD
GetWindowStyle(SDL_Window * window)
{
    DWORD style = 0;

    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        style |= STYLE_FULLSCREEN;
    } else {
        if (window->flags & SDL_WINDOW_BORDERLESS) {
            style |= STYLE_BORDERLESS;
        } else {
            style |= STYLE_NORMAL;
        }

        /* You can have a borderless resizable window */
        if (window->flags & SDL_WINDOW_RESIZABLE) {
            style |= STYLE_RESIZABLE;
        }
    }
    return style;
}

static void
WIN_AdjustWindowRectWithStyle_SpecifiedRect(SDL_Window *window, DWORD style, BOOL menu, int *x, int *y, int *width, int *height)
{
    const SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    RECT rect;
    int x_pixels, y_pixels;
    int w_pixels, h_pixels;

    w_pixels = *width;
    h_pixels = *height;
    WIN_VirtualToPhysical_ClientPoint(window, &w_pixels, &h_pixels);

    rect.left = 0;
    rect.top = 0;
    rect.right = w_pixels;
    rect.bottom = h_pixels;

    // borderless windows will have WM_NCCALCSIZE return 0 for the non-client area. When this happens, it looks like windows will send a resize message
    // expanding the window client area to the previous window + chrome size, so shouldn't need to adjust the window size for the set styles.
    if (!(window->flags & SDL_WINDOW_BORDERLESS))
        if (data->videodata->highdpi_enabled && data->videodata->AdjustWindowRectExForDpi) {
            data->videodata->AdjustWindowRectExForDpi(&rect, style, menu, 0, data->scaling_xdpi);
        } else {
            AdjustWindowRectEx(&rect, style, menu, 0);
        }

    x_pixels = *x;
    y_pixels = *y;
    WIN_VirtualToPhysical_ScreenPoint(&x_pixels, &y_pixels, *width, *height);

    *x = x_pixels + rect.left;
    *y = y_pixels + rect.top;
    *width = (rect.right - rect.left);
    *height = (rect.bottom - rect.top);
}

static void
WIN_AdjustWindowRectWithStyle(SDL_Window *window, DWORD style, BOOL menu, int *x, int *y, int *width, int *height, SDL_bool use_current)
{
    *x = (use_current ? window->x : window->windowed.x);
    *y = (use_current ? window->y : window->windowed.y);
    *width = (use_current ? window->w : window->windowed.w);
    *height = (use_current ? window->h : window->windowed.h);

    WIN_AdjustWindowRectWithStyle_SpecifiedRect(window, style, menu, x, y, width, height);
}

/*
in: window client position / size in SDL screen coordinates. typically window->x/y/w/h or window->windowed.x/y/w/h
out: window rect (incl. decoration) in Windows virtual screen coordinates (pixels if highdpi).
*/
void
WIN_AdjustWindowRect_SpecifiedRect(SDL_Window *window, int *x, int *y, int *width, int *height)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    HWND hwnd = data->hwnd;
    DWORD style;
    BOOL menu;

    style = GetWindowLong(hwnd, GWL_STYLE);
    menu = (style & WS_CHILDWINDOW) ? FALSE : (GetMenu(hwnd) != NULL);
    WIN_AdjustWindowRectWithStyle_SpecifiedRect(window, style, menu, x, y, width, height);
}

void
WIN_AdjustWindowRect(SDL_Window *window, int *x, int *y, int *width, int *height, SDL_bool use_current)
{
    *x = (use_current ? window->x : window->windowed.x);
    *y = (use_current ? window->y : window->windowed.y);
    *width = (use_current ? window->w : window->windowed.w);
    *height = (use_current ? window->h : window->windowed.h);

    WIN_AdjustWindowRect_SpecifiedRect(window, x, y, width, height);
}

static void
WIN_SetWindowPositionInternal(_THIS, SDL_Window * window, UINT flags)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    HWND hwnd = data->hwnd;
    HWND top;
    int x, y;
    int w, h;

    /* Figure out what the window area will be */
    if (SDL_ShouldAllowTopmost() && ((window->flags & (SDL_WINDOW_FULLSCREEN|SDL_WINDOW_INPUT_FOCUS)) == (SDL_WINDOW_FULLSCREEN|SDL_WINDOW_INPUT_FOCUS) || (window->flags & SDL_WINDOW_ALWAYS_ON_TOP))) {
        top = HWND_TOPMOST;
    } else {
        top = HWND_NOTOPMOST;
    }

    WIN_AdjustWindowRect(window, &x, &y, &w, &h, SDL_TRUE);    

    data->expected_resize = SDL_TRUE;
    SetWindowPos(hwnd, top, x, y, w, h, flags);
    data->expected_resize = SDL_FALSE;
}

static void
WIN_GetDPIForHWND(const SDL_VideoData *videodata, HWND hwnd, int *xdpi, int *ydpi)
{
    *xdpi = 96;
    *ydpi = 96;

    /* highdpi not requested? */
    if (!videodata->highdpi_enabled)
        return;

    /* Window 10+ */
    if (videodata->GetDpiForWindow) {
        *xdpi = videodata->GetDpiForWindow(hwnd);
        *ydpi = *xdpi;
        return;
    }

    /* window 8.1+ */
    if (videodata->GetDpiForMonitor) {
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (monitor) {
            UINT xdpi_uint, ydpi_uint;
            if (S_OK == videodata->GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &xdpi_uint, &ydpi_uint)) {
                *xdpi = xdpi_uint;
                *ydpi = ydpi_uint;
            }
        }
        return;
    }

    /* windows 8.0 and below */
    {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            *xdpi = GetDeviceCaps(hdc, LOGPIXELSX);
            *ydpi = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(NULL, hdc);
        }
    }
}

static int
SetupWindowData(_THIS, SDL_Window * window, HWND hwnd, HWND parent, SDL_bool created)
{
    SDL_VideoData *videodata = (SDL_VideoData *) _this->driverdata;
    SDL_WindowData *data;

    /* Allocate the window data */
    data = (SDL_WindowData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }
    data->window = window;
    data->hwnd = hwnd;
    data->parent = parent;
    data->hdc = GetDC(hwnd);
    data->hinstance = (HINSTANCE) GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
    data->created = created;
    data->mouse_button_flags = 0;
    data->videodata = videodata;
    data->initializing = SDL_TRUE;
    WIN_GetDPIForHWND(videodata, hwnd, &data->scaling_xdpi, &data->scaling_ydpi);

    window->driverdata = data;

    /* Associate the data with the window */
    if (!SetProp(hwnd, TEXT("SDL_WindowData"), data)) {
        ReleaseDC(hwnd, data->hdc);
        SDL_free(data);
        return WIN_SetError("SetProp() failed");
    }

    /* Set up the window proc function */
#ifdef GWLP_WNDPROC
    data->wndproc = (WNDPROC) GetWindowLongPtr(hwnd, GWLP_WNDPROC);
    if (data->wndproc == WIN_WindowProc) {
        data->wndproc = NULL;
    } else {
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR) WIN_WindowProc);
    }
#else
    data->wndproc = (WNDPROC) GetWindowLong(hwnd, GWL_WNDPROC);
    if (data->wndproc == WIN_WindowProc) {
        data->wndproc = NULL;
    } else {
        SetWindowLong(hwnd, GWL_WNDPROC, (LONG_PTR) WIN_WindowProc);
    }
#endif

    /* Move window to the correct monitor and size it */
    WIN_SetWindowPositionInternal(_this, window, SWP_NOCOPYBITS | SWP_NOACTIVATE);

    /* Fill in the SDL window with the window data */
    {
        RECT rect;
        if (GetClientRect(hwnd, &rect)) {
            int w = rect.right;
            int h = rect.bottom;
            WIN_PhysicalToVirtual_ClientPoint(window, &w, &h);
            if ((window->w && window->w != w) || (window->h && window->h != h)) {
                /* We tried to create a window larger than the desktop and Windows didn't allow it.  Override! */
                int x, y;
                int w, h;

                /* Figure out what the window area will be */
                WIN_AdjustWindowRect(window, &x, &y, &w, &h, SDL_TRUE);
                SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_NOCOPYBITS | SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                window->w = w;
                window->h = h;
            }
        }
    }
    {
        POINT point;
        point.x = 0;
        point.y = 0;
        if (ClientToScreen(hwnd, &point)) {
            int x = point.x;
            int y = point.y;
            WIN_PhysicalToVirtual_ScreenPoint(&x, &y, window->w, window->h);
            window->x = x;
            window->y = y;
        }
    }
    {
        DWORD style = GetWindowLong(hwnd, GWL_STYLE);
        if (style & WS_VISIBLE) {
            window->flags |= SDL_WINDOW_SHOWN;
        } else {
            window->flags &= ~SDL_WINDOW_SHOWN;
        }
        if (style & WS_POPUP) {
            window->flags |= SDL_WINDOW_BORDERLESS;
        } else {
            window->flags &= ~SDL_WINDOW_BORDERLESS;
        }
        if (style & WS_THICKFRAME) {
            window->flags |= SDL_WINDOW_RESIZABLE;
        } else {
            window->flags &= ~SDL_WINDOW_RESIZABLE;
        }
#ifdef WS_MAXIMIZE
        if (style & WS_MAXIMIZE) {
            window->flags |= SDL_WINDOW_MAXIMIZED;
        } else
#endif
        {
            window->flags &= ~SDL_WINDOW_MAXIMIZED;
        }
#ifdef WS_MINIMIZE
        if (style & WS_MINIMIZE) {
            window->flags |= SDL_WINDOW_MINIMIZED;
        } else
#endif
        {
            window->flags &= ~SDL_WINDOW_MINIMIZED;
        }
    }
    if (GetFocus() == hwnd) {
        window->flags |= SDL_WINDOW_INPUT_FOCUS;
        SDL_SetKeyboardFocus(data->window);

        if (window->flags & SDL_WINDOW_INPUT_GRABBED) {
            RECT rect;
            GetClientRect(hwnd, &rect);
            ClientToScreen(hwnd, (LPPOINT) & rect);
            ClientToScreen(hwnd, (LPPOINT) & rect + 1);
            ClipCursor(&rect);
        }
    }

    /* Enable multi-touch */
    if (videodata->RegisterTouchWindow) {
        videodata->RegisterTouchWindow(hwnd, (TWF_FINETOUCH|TWF_WANTPALM));
    }

    /* Enable dropping files */
    DragAcceptFiles(hwnd, TRUE);

    data->initializing = SDL_FALSE;

    /* All done! */
    return 0;
}



int
WIN_CreateWindow(_THIS, SDL_Window * window)
{
    HWND hwnd, parent = NULL;
    DWORD style = STYLE_BASIC;

    if (window->flags & SDL_WINDOW_SKIP_TASKBAR) {
        parent = CreateWindow(SDL_Appname, TEXT(""), STYLE_BASIC, 0, 0, 32, 32, NULL, NULL, SDL_Instance, NULL);
    }

    style |= GetWindowStyle(window);

    /* For high-DPI support, it's easier / more robust to create the window
       with a width/height of 0, then in SetupWindowData we will check the
       DPI and adjust the position and size to match window->x,y,w,h */
    hwnd =
        CreateWindow(SDL_Appname, TEXT(""), style, CW_USEDEFAULT, 0, 0, 0, parent, NULL,
                     SDL_Instance, NULL);
    if (!hwnd) {
        return WIN_SetError("Couldn't create window");
    }

    WIN_PumpEvents(_this);

    if (SetupWindowData(_this, window, hwnd, parent, SDL_TRUE) < 0) {
        DestroyWindow(hwnd);
        if (parent) {
            DestroyWindow(parent);
        }
        return -1;
    }

    // Inform Windows of the frame change so we can respond to WM_NCCALCSIZE
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOSIZE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);

    if (!(window->flags & SDL_WINDOW_OPENGL)) {
        return 0;
    }

    /* The rest of this macro mess is for OpenGL or OpenGL ES windows */
#if SDL_VIDEO_OPENGL_ES2
    if (_this->gl_config.profile_mask == SDL_GL_CONTEXT_PROFILE_ES
#if SDL_VIDEO_OPENGL_WGL
        && (!_this->gl_data || WIN_GL_UseEGL(_this))
#endif /* SDL_VIDEO_OPENGL_WGL */
    ) {
#if SDL_VIDEO_OPENGL_EGL
        if (WIN_GLES_SetupWindow(_this, window) < 0) {
            WIN_DestroyWindow(_this, window);
            return -1;
        }
        return 0;
#else
        return SDL_SetError("Could not create GLES window surface (EGL support not configured)");
#endif /* SDL_VIDEO_OPENGL_EGL */ 
    }
#endif /* SDL_VIDEO_OPENGL_ES2 */

#if SDL_VIDEO_OPENGL_WGL
    if (WIN_GL_SetupWindow(_this, window) < 0) {
        WIN_DestroyWindow(_this, window);
        return -1;
    }
#else
    return SDL_SetError("Could not create GL window (WGL support not configured)");
#endif

    return 0;
}

int
WIN_CreateWindowFrom(_THIS, SDL_Window * window, const void *data)
{
    HWND hwnd = (HWND) data;
    LPTSTR title;
    int titleLen;

    /* Query the title from the existing window */
    titleLen = GetWindowTextLength(hwnd);
    title = SDL_stack_alloc(TCHAR, titleLen + 1);
    if (title) {
        titleLen = GetWindowText(hwnd, title, titleLen);
    } else {
        titleLen = 0;
    }
    if (titleLen > 0) {
        window->title = WIN_StringToUTF8(title);
    }
    if (title) {
        SDL_stack_free(title);
    }

    if (SetupWindowData(_this, window, hwnd, GetParent(hwnd), SDL_FALSE) < 0) {
        return -1;
    }

#if SDL_VIDEO_OPENGL_WGL
    {
        const char *hint = SDL_GetHint(SDL_HINT_VIDEO_WINDOW_SHARE_PIXEL_FORMAT);
        if (hint) {
            /* This hint is a pointer (in string form) of the address of
               the window to share a pixel format with
            */
            SDL_Window *otherWindow = NULL;
            SDL_sscanf(hint, "%p", (void**)&otherWindow);

            /* Do some error checking on the pointer */
            if (otherWindow != NULL && otherWindow->magic == &_this->window_magic)
            {
                /* If the otherWindow has SDL_WINDOW_OPENGL set, set it for the new window as well */
                if (otherWindow->flags & SDL_WINDOW_OPENGL)
                {
                    window->flags |= SDL_WINDOW_OPENGL;
                    if(!WIN_GL_SetPixelFormatFrom(_this, otherWindow, window)) {
                        return -1;
                    }
                }
            }
        }
    }
#endif
    return 0;
}

void
WIN_SetWindowTitle(_THIS, SDL_Window * window)
{
    HWND hwnd = ((SDL_WindowData *) window->driverdata)->hwnd;
    LPTSTR title = WIN_UTF8ToString(window->title);
    SetWindowText(hwnd, title);
    SDL_free(title);
}

void
WIN_SetWindowIcon(_THIS, SDL_Window * window, SDL_Surface * icon)
{
    HWND hwnd = ((SDL_WindowData *) window->driverdata)->hwnd;
    HICON hicon = NULL;
    BYTE *icon_bmp;
    int icon_len, y;
    SDL_RWops *dst;

    /* Create temporary bitmap buffer */
    icon_len = 40 + icon->h * icon->w * sizeof(Uint32);
    icon_bmp = SDL_stack_alloc(BYTE, icon_len);
    dst = SDL_RWFromMem(icon_bmp, icon_len);
    if (!dst) {
        SDL_stack_free(icon_bmp);
        return;
    }

    /* Write the BITMAPINFO header */
    SDL_WriteLE32(dst, 40);
    SDL_WriteLE32(dst, icon->w);
    SDL_WriteLE32(dst, icon->h * 2);
    SDL_WriteLE16(dst, 1);
    SDL_WriteLE16(dst, 32);
    SDL_WriteLE32(dst, BI_RGB);
    SDL_WriteLE32(dst, icon->h * icon->w * sizeof(Uint32));
    SDL_WriteLE32(dst, 0);
    SDL_WriteLE32(dst, 0);
    SDL_WriteLE32(dst, 0);
    SDL_WriteLE32(dst, 0);

    /* Write the pixels upside down into the bitmap buffer */
    SDL_assert(icon->format->format == SDL_PIXELFORMAT_ARGB8888);
    y = icon->h;
    while (y--) {
        Uint8 *src = (Uint8 *) icon->pixels + y * icon->pitch;
        SDL_RWwrite(dst, src, icon->w * sizeof(Uint32), 1);
    }

    hicon = CreateIconFromResource(icon_bmp, icon_len, TRUE, 0x00030000);

    SDL_RWclose(dst);
    SDL_stack_free(icon_bmp);

    /* Set the icon for the window */
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM) hicon);

    /* Set the icon in the task manager (should we do this?) */
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM) hicon);
}

void
WIN_SetWindowPosition(_THIS, SDL_Window * window)
{
    WIN_SetWindowPositionInternal(_this, window, SWP_NOCOPYBITS | SWP_NOSIZE | SWP_NOACTIVATE);
}

void
WIN_SetWindowSize(_THIS, SDL_Window * window)
{
    WIN_SetWindowPositionInternal(_this, window, SWP_NOCOPYBITS | SWP_NOMOVE | SWP_NOACTIVATE);
}

void
WIN_ShowWindow(_THIS, SDL_Window * window)
{
    HWND hwnd = ((SDL_WindowData *) window->driverdata)->hwnd;
    ShowWindow(hwnd, SW_SHOW);
}

void
WIN_HideWindow(_THIS, SDL_Window * window)
{
    HWND hwnd = ((SDL_WindowData *) window->driverdata)->hwnd;
    ShowWindow(hwnd, SW_HIDE);
}

void
WIN_RaiseWindow(_THIS, SDL_Window * window)
{
    HWND hwnd = ((SDL_WindowData *) window->driverdata)->hwnd;
    SetForegroundWindow(hwnd);
}

void
WIN_MaximizeWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    HWND hwnd = data->hwnd;
    data->expected_resize = SDL_TRUE;
    ShowWindow(hwnd, SW_MAXIMIZE);
    data->expected_resize = SDL_FALSE;
}

void
WIN_MinimizeWindow(_THIS, SDL_Window * window)
{
    HWND hwnd = ((SDL_WindowData *) window->driverdata)->hwnd;
    ShowWindow(hwnd, SW_MINIMIZE);
}

void
WIN_SetWindowBordered(_THIS, SDL_Window * window, SDL_bool bordered)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    HWND hwnd = data->hwnd;
    DWORD style;

    style = GetWindowLong(hwnd, GWL_STYLE);
    style &= ~STYLE_MASK;
    style |= GetWindowStyle(window);

    data->in_border_change = SDL_TRUE;
    SetWindowLong(hwnd, GWL_STYLE, style);
    WIN_SetWindowPositionInternal(_this, window, SWP_NOCOPYBITS | SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    data->in_border_change = SDL_FALSE;
}

void
WIN_SetWindowResizable(_THIS, SDL_Window * window, SDL_bool resizable)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    HWND hwnd = data->hwnd;
    DWORD style;

    style = GetWindowLong(hwnd, GWL_STYLE);
    style &= ~STYLE_MASK;
    style |= GetWindowStyle(window);

    SetWindowLong(hwnd, GWL_STYLE, style);
}

void
WIN_RestoreWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    HWND hwnd = data->hwnd;
    data->expected_resize = SDL_TRUE;
    ShowWindow(hwnd, SW_RESTORE);
    data->expected_resize = SDL_FALSE;
}

void
WIN_SetWindowFullscreen(_THIS, SDL_Window * window, SDL_VideoDisplay * display, SDL_bool fullscreen)
{
    SDL_DisplayData *displaydata = (SDL_DisplayData *) display->driverdata;
    SDL_WindowData *data = (SDL_WindowData *) window->driverdata;
    SDL_VideoData *videodata = data->videodata;
    HWND hwnd = data->hwnd;
    MONITORINFO minfo;
    DWORD style;
    HWND top;
    int x, y;
    int w, h;

    /* BUG: windows don't receive a WM_DPICHANGED message after a ChangeDisplaySettingsEx,
       so we must manually update the cached DPI (see WIN_SetDisplayMode). */
#ifdef HIGHDPI_DEBUG
    SDL_Log("WIN_SetWindowFullscreen: dpi: %d (stale) cached dpi: %d", WIN_DPIForHWND(videodata, hwnd), data->scaling_dpi);
#endif
    WIN_GetDPIForHWND(videodata, hwnd, &data->scaling_xdpi, &data->scaling_ydpi);

    /* clear the window size, to cause us to send a SDL_WINDOWEVENT_RESIZED event in WM_WINDOWPOSCHANGED */
    data->window->w = 0;
    data->window->h = 0;

    if (SDL_ShouldAllowTopmost() && ((window->flags & (SDL_WINDOW_FULLSCREEN|SDL_WINDOW_INPUT_FOCUS)) == (SDL_WINDOW_FULLSCREEN|SDL_WINDOW_INPUT_FOCUS) || window->flags & SDL_WINDOW_ALWAYS_ON_TOP)) {
        top = HWND_TOPMOST;
    } else {
        top = HWND_NOTOPMOST;
    }

    style = GetWindowLong(hwnd, GWL_STYLE);
    style &= ~STYLE_MASK;
    style |= GetWindowStyle(window);

    /* Prefer GetMonitorInfo over WIN_GetDisplayBounds because we want the
       monitor bounds in pixels rather than SDL coordinates (points). */
    SDL_zero(minfo);
    minfo.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfo(displaydata->MonitorHandle, &minfo)) {
        SDL_SetError("GetMonitorInfo failed");
        return;
    }

    if (fullscreen) {
		float xdpi, ydpi;

        x = minfo.rcMonitor.left;
        y = minfo.rcMonitor.top;
        w = minfo.rcMonitor.right - minfo.rcMonitor.left;
        h = minfo.rcMonitor.bottom - minfo.rcMonitor.top;

        /* Unset the maximized flag.  This fixes
           https://bugzilla.libsdl.org/show_bug.cgi?id=3215
        */
        if (style & WS_MAXIMIZE) {
            data->windowed_mode_was_maximized = SDL_TRUE;
            style &= ~WS_MAXIMIZE;
        }
    } else {
        BOOL menu;

        /* Restore window-maximization state, as applicable.
           Special care is taken to *not* do this if and when we're
           alt-tab'ing away (to some other window; as indicated by
           in_window_deactivation), otherwise
           https://bugzilla.libsdl.org/show_bug.cgi?id=3215 can reproduce!
        */
        if (data->windowed_mode_was_maximized && !data->in_window_deactivation) {
            style |= WS_MAXIMIZE;
            data->windowed_mode_was_maximized = SDL_FALSE;
        }

        menu = (style & WS_CHILDWINDOW) ? FALSE : (GetMenu(hwnd) != NULL);
        WIN_AdjustWindowRectWithStyle(window, style, menu, &x, &y, &w, &h, SDL_FALSE);
    }
    SetWindowLong(hwnd, GWL_STYLE, style);
    data->expected_resize = SDL_TRUE;
    SetWindowPos(hwnd, top, x, y, w, h, SWP_NOCOPYBITS | SWP_NOACTIVATE);
    data->expected_resize = SDL_FALSE;
}

int
WIN_SetWindowGammaRamp(_THIS, SDL_Window * window, const Uint16 * ramp)
{
    SDL_VideoDisplay *display = SDL_GetDisplayForWindow(window);
    SDL_DisplayData *data = (SDL_DisplayData *) display->driverdata;
    HDC hdc;
    BOOL succeeded = FALSE;

    hdc = CreateDC(data->DeviceName, NULL, NULL, NULL);
    if (hdc) {
        succeeded = SetDeviceGammaRamp(hdc, (LPVOID)ramp);
        if (!succeeded) {
            WIN_SetError("SetDeviceGammaRamp()");
        }
        DeleteDC(hdc);
    }
    return succeeded ? 0 : -1;
}

int
WIN_GetWindowGammaRamp(_THIS, SDL_Window * window, Uint16 * ramp)
{
    SDL_VideoDisplay *display = SDL_GetDisplayForWindow(window);
    SDL_DisplayData *data = (SDL_DisplayData *) display->driverdata;
    HDC hdc;
    BOOL succeeded = FALSE;

    hdc = CreateDC(data->DeviceName, NULL, NULL, NULL);
    if (hdc) {
        succeeded = GetDeviceGammaRamp(hdc, (LPVOID)ramp);
        if (!succeeded) {
            WIN_SetError("GetDeviceGammaRamp()");
        }
        DeleteDC(hdc);
    }
    return succeeded ? 0 : -1;
}

void
WIN_SetWindowGrab(_THIS, SDL_Window * window, SDL_bool grabbed)
{
    WIN_UpdateClipCursor(window);

    if (window->flags & SDL_WINDOW_FULLSCREEN) {
        UINT flags = SWP_NOCOPYBITS | SWP_NOMOVE | SWP_NOSIZE;

        if (!(window->flags & SDL_WINDOW_SHOWN)) {
            flags |= SWP_NOACTIVATE;
        }
        WIN_SetWindowPositionInternal(_this, window, flags);
    }
}

void
WIN_DestroyWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = (SDL_WindowData *) window->driverdata;

    if (data) {
        ReleaseDC(data->hwnd, data->hdc);
        RemoveProp(data->hwnd, TEXT("SDL_WindowData"));
        if (data->created) {
            DestroyWindow(data->hwnd);
            if (data->parent) {
                DestroyWindow(data->parent);
            }
        } else {
            /* Restore any original event handler... */
            if (data->wndproc != NULL) {
#ifdef GWLP_WNDPROC
                SetWindowLongPtr(data->hwnd, GWLP_WNDPROC,
                                 (LONG_PTR) data->wndproc);
#else
                SetWindowLong(data->hwnd, GWL_WNDPROC,
                              (LONG_PTR) data->wndproc);
#endif
            }
        }
        SDL_free(data);
    }
    window->driverdata = NULL;
}

SDL_bool
WIN_GetWindowWMInfo(_THIS, SDL_Window * window, SDL_SysWMinfo * info)
{
    const SDL_WindowData *data = (const SDL_WindowData *) window->driverdata;
    if (info->version.major <= SDL_MAJOR_VERSION) {
        int versionnum = SDL_VERSIONNUM(info->version.major, info->version.minor, info->version.patch);

        info->subsystem = SDL_SYSWM_WINDOWS;
        info->info.win.window = data->hwnd;

        if (versionnum >= SDL_VERSIONNUM(2, 0, 4)) {
            info->info.win.hdc = data->hdc;
        }

        if (versionnum >= SDL_VERSIONNUM(2, 0, 5)) {
            info->info.win.hinstance = data->hinstance;
        }

        return SDL_TRUE;
    } else {
        SDL_SetError("Application not compiled with SDL %d.%d",
                     SDL_MAJOR_VERSION, SDL_MINOR_VERSION);
        return SDL_FALSE;
    }
}


/*
 * Creates a HelperWindow used for DirectInput events.
 */
int
SDL_HelperWindowCreate(void)
{
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASS wce;

    /* Make sure window isn't created twice. */
    if (SDL_HelperWindow != NULL) {
        return 0;
    }

    /* Create the class. */
    SDL_zero(wce);
    wce.lpfnWndProc = DefWindowProc;
    wce.lpszClassName = (LPCWSTR) SDL_HelperWindowClassName;
    wce.hInstance = hInstance;

    /* Register the class. */
    SDL_HelperWindowClass = RegisterClass(&wce);
    if (SDL_HelperWindowClass == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return WIN_SetError("Unable to create Helper Window Class");
    }

    /* Create the window. */
    SDL_HelperWindow = CreateWindowEx(0, SDL_HelperWindowClassName,
                                      SDL_HelperWindowName,
                                      WS_OVERLAPPED, CW_USEDEFAULT,
                                      CW_USEDEFAULT, CW_USEDEFAULT,
                                      CW_USEDEFAULT, HWND_MESSAGE, NULL,
                                      hInstance, NULL);
    if (SDL_HelperWindow == NULL) {
        UnregisterClass(SDL_HelperWindowClassName, hInstance);
        return WIN_SetError("Unable to create Helper Window");
    }

    return 0;
}


/*
 * Destroys the HelperWindow previously created with SDL_HelperWindowCreate.
 */
void
SDL_HelperWindowDestroy(void)
{
    HINSTANCE hInstance = GetModuleHandle(NULL);

    /* Destroy the window. */
    if (SDL_HelperWindow != NULL) {
        if (DestroyWindow(SDL_HelperWindow) == 0) {
            WIN_SetError("Unable to destroy Helper Window");
            return;
        }
        SDL_HelperWindow = NULL;
    }

    /* Unregister the class. */
    if (SDL_HelperWindowClass != 0) {
        if ((UnregisterClass(SDL_HelperWindowClassName, hInstance)) == 0) {
            WIN_SetError("Unable to destroy Helper Window Class");
            return;
        }
        SDL_HelperWindowClass = 0;
    }
}

void WIN_OnWindowEnter(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = (SDL_WindowData *) window->driverdata;

    if (!data || !data->hwnd) {
        /* The window wasn't fully initialized */
        return;
    }

    if (window->flags & SDL_WINDOW_ALWAYS_ON_TOP) {
        WIN_SetWindowPositionInternal(_this, window, SWP_NOCOPYBITS | SWP_NOSIZE | SWP_NOACTIVATE);
    }

#ifdef WM_MOUSELEAVE
    {
        TRACKMOUSEEVENT trackMouseEvent;

        trackMouseEvent.cbSize = sizeof(TRACKMOUSEEVENT);
        trackMouseEvent.dwFlags = TME_LEAVE;
        trackMouseEvent.hwndTrack = data->hwnd;

        TrackMouseEvent(&trackMouseEvent);
    }
#endif /* WM_MOUSELEAVE */
}

void
WIN_UpdateClipCursor(SDL_Window *window)
{
    SDL_WindowData *data = (SDL_WindowData *) window->driverdata;
    SDL_Mouse *mouse = SDL_GetMouse();

    if (data->focus_click_pending) {
        return;
    }

    if ((mouse->relative_mode || (window->flags & SDL_WINDOW_INPUT_GRABBED)) &&
        (window->flags & SDL_WINDOW_INPUT_FOCUS)) {
        if (mouse->relative_mode && !mouse->relative_mode_warp) {
            LONG cx, cy;
            RECT rect;
            GetWindowRect(data->hwnd, &rect);

            cx = (rect.left + rect.right) / 2;
            cy = (rect.top + rect.bottom) / 2;

            /* Make an absurdly small clip rect */
            rect.left = cx - 1;
            rect.right = cx + 1;
            rect.top = cy - 1;
            rect.bottom = cy + 1;

            ClipCursor(&rect);
        } else {
            RECT rect;
            if (GetClientRect(data->hwnd, &rect) && !IsRectEmpty(&rect)) {
                ClientToScreen(data->hwnd, (LPPOINT) & rect);
                ClientToScreen(data->hwnd, (LPPOINT) & rect + 1);
                ClipCursor(&rect);
            }
        }
    } else {
        ClipCursor(NULL);
    }
}

int
WIN_SetWindowHitTest(SDL_Window *window, SDL_bool enabled)
{
    return 0;  /* just succeed, the real work is done elsewhere. */
}

int
WIN_SetWindowOpacity(_THIS, SDL_Window * window, float opacity)
{
    const SDL_WindowData *data = (SDL_WindowData *) window->driverdata;
    const HWND hwnd = data->hwnd;
    const LONG style = GetWindowLong(hwnd, GWL_EXSTYLE);

    SDL_assert(style != 0);

    if (opacity == 1.0f) {
        /* want it fully opaque, just mark it unlayered if necessary. */
        if (style & WS_EX_LAYERED) {
            if (SetWindowLong(hwnd, GWL_EXSTYLE, style & ~WS_EX_LAYERED) == 0) {
                return WIN_SetError("SetWindowLong()");
            }
        }
    } else {
        const BYTE alpha = (BYTE) ((int) (opacity * 255.0f));
        /* want it transparent, mark it layered if necessary. */
        if ((style & WS_EX_LAYERED) == 0) {
            if (SetWindowLong(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED) == 0) {
                return WIN_SetError("SetWindowLong()");
            }
        }

        if (SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA) == 0) {
            return WIN_SetError("SetLayeredWindowAttributes()");
        }
    }

    return 0;
}

void
WIN_GetDrawableSize(const SDL_Window *window, int *w, int *h)
{
    const SDL_WindowData *data = ((SDL_WindowData *)window->driverdata);
    HWND hwnd = data->hwnd;
    RECT rect;

    if (GetClientRect(hwnd, &rect)) {
        *w = rect.right;
        *h = rect.bottom;
    } else {
        *w = 0;
        *h = 0;
    }
}

void
WIN_PhysicalToVirtual_ClientPoint(const SDL_Window *window, int *x, int *y)
{
    const SDL_WindowData *data = ((SDL_WindowData *)window->driverdata);

    *x = MulDiv(*x, 96, data->scaling_xdpi);
    *y = MulDiv(*y, 96, data->scaling_ydpi);
}

void
WIN_VirtualToPhysical_ClientPoint(const SDL_Window *window, int *x, int *y)
{
    const SDL_WindowData *data = ((SDL_WindowData *)window->driverdata);

    *x = MulDiv(*x, data->scaling_xdpi, 96);
    *y = MulDiv(*y, data->scaling_ydpi, 96);
}

/* Given a point in screen coordinates (they're interpreted as Windows coordinates)
   tries to guess what DPI and which monitor Windows would assign a window located there.

   If the system has a uniform DPI value on all monitors (or DPI awareness is disabled),
   sets `uniformDPI` to SDL_TRUE and returns the value in `dpi`.

   Otherwise, `uniformDPI` is set to SDL_FALSE and the DPI and rects are returned.
*/
static void
WIN_DPIAtScreenPoint(int x, int y, int widthHint, int heightHint, UINT *dpi, RECT *monRectScaled, RECT *monRectUnscaled, SDL_bool *uniformDPI)
{
    HMONITOR monitor = NULL;
    HRESULT result;
    MONITORINFO moninfo = { 0 };
    const SDL_VideoData *videodata;
    const SDL_VideoDevice *videodevice;
    RECT clientRect;
    UINT unused;
    int w, h;

    /* non-DPI-aware return values */
    *uniformDPI = SDL_TRUE;
    *dpi = 96;

    videodevice = SDL_GetVideoDevice();
    if (!videodevice || !videodevice->driverdata)
        return;

    videodata = (SDL_VideoData *)videodevice->driverdata;
    if (!videodata->highdpi_enabled)
        return;

    /* Check for Windows < 8.1*/
    if (!videodata->GetDpiForMonitor) {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            *dpi = GetDeviceCaps(hdc, LOGPIXELSX);
            ReleaseDC(NULL, hdc);
        }
        return;
    }
    
    /* General case: Windows 8.1+ */

    clientRect.left = x;
    clientRect.top = y;
    clientRect.right = x + widthHint;
    clientRect.bottom = y + heightHint;

    monitor = MonitorFromRect(&clientRect, MONITOR_DEFAULTTONEAREST);

    result = videodata->GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, dpi, &unused);
    if (result != S_OK) {
        /* Shouldn't happen? */
        *dpi = 96;
        return;
    }

    moninfo.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfo(monitor, &moninfo)) {
        /* Shouldn't happen? */
        *dpi = 96;
        return;
    }

    *uniformDPI = SDL_FALSE;
    *monRectUnscaled = moninfo.rcMonitor;
    *monRectScaled = moninfo.rcMonitor;

    /* fix up the right/bottom of the scaled rect */
    w = moninfo.rcMonitor.right - moninfo.rcMonitor.left;
    h = moninfo.rcMonitor.bottom - moninfo.rcMonitor.top;
    w = MulDiv(w, 96, *dpi);
    h = MulDiv(h, 96, *dpi);

    monRectScaled->right = monRectScaled->left + w;
    monRectScaled->bottom = monRectScaled->top + h;
}

/* Convert an SDL to a Windows screen coordinate. */
void WIN_VirtualToPhysical_ScreenPoint(int *x, int *y, int widthHint, int heightHint)
{
    RECT monitorRectScaled, monitorRectUnscaled;
    UINT dpi;
    SDL_bool uniformDPI;

    WIN_DPIAtScreenPoint(*x, *y, widthHint, heightHint, &dpi, &monitorRectScaled, &monitorRectUnscaled, &uniformDPI);

    if (uniformDPI) {
        *x = MulDiv(*x, dpi, 96);
        *y = MulDiv(*y, dpi, 96);
        return;
    }

    *x = monitorRectScaled.left + MulDiv(*x - monitorRectScaled.left, dpi, 96);
    *y = monitorRectScaled.top + MulDiv(*y - monitorRectScaled.top, dpi, 96);

    /* ensure the result is not past the right/bottom of the monitor rect */
    if (*x >= monitorRectUnscaled.right) 
        *x = monitorRectUnscaled.right - 1;
    if (*y >= monitorRectUnscaled.bottom) 
        *y = monitorRectUnscaled.bottom - 1;
}

/* Converts a Windows screen coordinate to an SDL one. */
void WIN_PhysicalToVirtual_ScreenPoint(int *x, int *y, int widthHint, int heightHint)
{
    RECT monitorRectScaled, monitorRectUnscaled;
    UINT dpi;
    SDL_bool uniformDPI;

    WIN_DPIAtScreenPoint(*x, *y, widthHint, heightHint, &dpi, &monitorRectScaled, &monitorRectUnscaled, &uniformDPI);

    if (uniformDPI) {
        *x = MulDiv(*x, 96, dpi);
        *y = MulDiv(*y, 96, dpi);
        return;
    }

    *x = monitorRectUnscaled.left + MulDiv(*x - monitorRectUnscaled.left, 96, dpi);
    *y = monitorRectUnscaled.top + MulDiv(*y - monitorRectUnscaled.top, 96, dpi);
}

#endif /* SDL_VIDEO_DRIVER_WINDOWS */

/* vi: set ts=4 sw=4 expandtab: */
