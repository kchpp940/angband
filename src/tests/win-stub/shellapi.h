#ifndef _SHELLAPI_STUB_H_
#define _SHELLAPI_STUB_H_

#include "windows.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef HANDLE HDROP;

#define DragQueryFileA __stub_DragQueryFileA
#define DragQueryFileW __stub_DragQueryFileW
#define DragQueryFile  __stub_DragQueryFile
#define DragAcceptFiles __stub_DragAcceptFiles
#define DragFinish __stub_DragFinish
#define DragQueryPoint __stub_DragQueryPoint
#define ExtractIconA __stub_ExtractIconA
#define ExtractIconW __stub_ExtractIconW
#define ExtractIcon __stub_ExtractIcon
#define ExtractAssociatedIconA __stub_ExtractAssociatedIconA
#define ExtractAssociatedIconW __stub_ExtractAssociatedIconW
#define ExtractAssociatedIcon __stub_ExtractAssociatedIcon
#define ExtractIconExA __stub_ExtractIconExA
#define ExtractIconExW __stub_ExtractIconExW
#define ExtractIconEx __stub_ExtractIconEx
#define ShlLoadFromRegPath __stub_ShlLoadFromRegPath

UINT WINAPI DragQueryFileA(HDROP, UINT, LPSTR, UINT);
void WINAPI DragAcceptFiles(HWND, BOOL);
void WINAPI DragFinish(HDROP);
BOOL WINAPI DragQueryPoint(HDROP, LPPOINT);
HICON WINAPI ExtractIconA(HINSTANCE, LPCSTR, UINT);

#ifdef __cplusplus
}
#endif

#endif
