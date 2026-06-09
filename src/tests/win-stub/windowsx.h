#ifndef _WINDOWSX_STUB_H_
#define _WINDOWSX_STUB_H_

#include "windows.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GET_WM_COMMAND_ID(wp, lp)           (LOWORD(wp))
#define GET_WM_COMMAND_CMD(wp, lp)          (HIWORD(wp))
#define GET_WM_COMMAND_HWND(wp, lp)         ((HWND)(lp))

#define GET_WM_HSCROLL_CODE(wp, lp)         (LOWORD(wp))
#define GET_WM_HSCROLL_POS(wp, lp)          (HIWORD(wp))
#define GET_WM_HSCROLL_HWND(wp, lp)         ((HWND)(lp))

#define GET_WM_VSCROLL_CODE(wp, lp)         (LOWORD(wp))
#define GET_WM_VSCROLL_POS(wp, lp)          (HIWORD(wp))
#define GET_WM_VSCROLL_HWND(wp, lp)         ((HWND)(lp))

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp)                    ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp)                    ((int)(short)HIWORD(lp))
#endif

#define FORWARD_WM_COMMAND(hwnd, id, hwndCtl, code, fn) \
    (void)(fn)((hwnd), WM_COMMAND, MAKEWPARAM((UINT)(id), (UINT)(code)), (LPARAM)(HWND)(hwndCtl))

#define HANDLE_WM_COMMAND(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), (UINT)(LOWORD(wParam)), (HWND)(lParam), (UINT)(HIWORD(wParam))), 0L)

#ifdef __cplusplus
}
#endif

#endif
