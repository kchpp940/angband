#ifndef _WINGDI_STUB_H_
#define _WINGDI_STUB_H_

#include "windows.h"

#ifdef __cplusplus
extern "C" {
#endif

#define R2_BLACK 1
#define R2_NOTMERGEPEN 2
#define R2_MASKNOTPEN 3
#define R2_NOTCOPYPEN 4
#define R2_MASKPENNOT 5
#define R2_NOT 6
#define R2_XORPEN 7
#define R2_NOTMASKPEN 8
#define R2_MASKPEN 9
#define R2_NOTXORPEN 10
#define R2_NOP 11
#define R2_MERGENOTPEN 12
#define R2_COPYPEN 13
#define R2_MERGEPENNOT 14
#define R2_MERGEPEN 15
#define R2_WHITE 16

#define BLACK_BRUSH 4
#define DKGRAY_BRUSH 3
#define GRAY_BRUSH 2
#define LTGRAY_BRUSH 1
#define HOLLOW_BRUSH 5
#define NULL_BRUSH 5
#define WHITE_BRUSH 0

#define BLACK_PEN 7
#define WHITE_PEN 6
#define NULL_PEN 8

#define SYSTEM_FONT 13
#define DEFAULT_PEN 7
#define DEFAULT_BRUSH 4

typedef struct tagBITMAP {
    LONG bmType;
    LONG bmWidth;
    LONG bmHeight;
    LONG bmWidthBytes;
    WORD bmPlanes;
    WORD bmBitsPixel;
    LPVOID bmBits;
} BITMAP, *PBITMAP, *LPBITMAP;

typedef struct tagTEXTMETRICA {
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    BYTE tmFirstChar;
    BYTE tmLastChar;
    BYTE tmDefaultChar;
    BYTE tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
} TEXTMETRICA, *PTEXTMETRICA, *LPTEXTMETRICA;
typedef TEXTMETRICA TEXTMETRIC;

int WINAPI SetMapMode(HDC, int);
int WINAPI GetMapMode(HDC);
int WINAPI SetROP2(HDC, int);
int WINAPI GetROP2(HDC);
BOOL WINAPI PtVisible(HDC, int, int);
BOOL WINAPI RectVisible(HDC, const RECT *);
BOOL WINAPI GetTextExtentPoint32A(HDC, LPCSTR, int, LPSIZE);
BOOL WINAPI GetTextExtentPoint32W(HDC, const wchar_t *, int, LPSIZE);
BOOL WINAPI GetTextExtentPointA(HDC, LPCSTR, int, LPSIZE);

#ifdef __cplusplus
}
#endif

#endif
