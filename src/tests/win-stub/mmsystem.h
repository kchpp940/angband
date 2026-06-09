#ifndef _MMSYSTEM_STUB_H_
#define _MMSYSTEM_STUB_H_

#include "windows.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MMNODRV
typedef UINT MMRESULT;
#endif

#define JOYERR_NOERROR 0
#define MMSYSERR_NOERROR 0
#define MMSYSERR_ERROR 1
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_NOTENABLED 3
#define MMSYSERR_ALLOCATED 4
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NODRIVER 6
#define MMSYSERR_NOMEM 7
#define MMSYSERR_NOTSUPPORTED 8
#define MMSYSERR_BADERRNUM 9
#define MMSYSERR_INVALFLAG 10
#define MMSYSERR_INVALPARAM 11
#define MMSYSERR_HANDLEBUSY 12
#define MMSYSERR_INVALIDALIAS 13
#define MMSYSERR_BADDB 14
#define MMSYSERR_KEYNOTFOUND 15
#define MMSYSERR_READERROR 16
#define MMSYSERR_WRITEERROR 17
#define MMSYSERR_DELETEERROR 18
#define MMSYSERR_VALNOTFOUND 19
#define MMSYSERR_NODRIVERCB 20
#define MMSYSERR_MOREDATA 21

#ifndef MMNOMCI
#define MCI_OPEN 0x0803
#define MCI_CLOSE 0x0804
#define MCI_ESCAPE 0x0805
#define MCI_PLAY 0x0806
#define MCI_SEEK 0x0807
#define MCI_STOP 0x0808
#define MCI_PAUSE 0x0809
#define MCI_INFO 0x080A
#define MCI_GETDEVCAPS 0x080B
#define MCI_SPIN 0x080C
#define MCI_SET 0x080D
#define MCI_STEP 0x080E
#define MCI_RECORD 0x080F
#define MCI_SYSINFO 0x0810
#define MCI_BREAK 0x0811
#define MCI_SOUND 0x0812
#define MCI_SAVE 0x0813
#define MCI_STATUS 0x0814
#define MCI_CUE 0x0830
#define MCI_REALIZE 0x0840
#define MCI_WINDOW 0x0841
#define MCI_PUT 0x0842
#define MCI_WHERE 0x0843
#define MCI_FREEZE 0x0844
#define MCI_UNFREEZE 0x0845
#define MCI_LOAD 0x0850
#define MCI_CUT 0x0851
#define MCI_COPY 0x0852
#define MCI_PASTE 0x0853
#define MCI_UPDATE 0x0854
#define MCI_RESUME 0x0855
#define MCI_DELETE 0x0856
#endif

#ifndef MMNOTIMER
typedef void (CALLBACK TIMECALLBACK)(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
typedef TIMECALLBACK *LPTIMECALLBACK;

#define TIME_ONESHOT 0x0000
#define TIME_PERIODIC 0x0001
#endif

#ifndef MMNOWAVE
#define WAVE_MAPPER ((UINT)-1)

typedef struct tWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *NPWAVEFORMATEX, *LPWAVEFORMATEX;

typedef struct wavehdr_tag {
    LPSTR lpData;
    DWORD dwBufferLength;
    DWORD dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD dwFlags;
    DWORD dwLoops;
    struct wavehdr_tag *lpNext;
    DWORD_PTR reserved;
} WAVEHDR, *PWAVEHDR, *NPWAVEHDR, *LPWAVEHDR;

#define WAVE_FORMAT_PCM 1

#endif

#ifndef MMNOMIDIOUT
typedef HMIDIOUT HHWAVE;
#endif

#ifdef __cplusplus
}
#endif

#endif
