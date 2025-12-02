#ifndef __PWASIO_PWASIO_H__
#define __PWASIO_PWASIO_H__

#include <unknwn.h>

static GUID const class_id = {
    0x9d9612bc,
    0xcadd,
    0x43a2,
    {0xaa, 0x6f, 0x59, 0xf6, 0xac, 0xa4, 0xfe, 0x74},
};

struct factory {
  struct IClassFactoryVtbl *vtbl;
  LONG ref;
  HINSTANCE hinst;
};

HRESULT WINAPI CreateInstance(LPCLASSFACTORY, LPUNKNOWN, REFIID, LPVOID *);

#ifdef DEBUG
#include <wine/debug.h>
#else
#define WINE_DEFAULT_DEBUG_CHANNEL(...)
#define WINE_TRACE(...)
#define WINE_WARN(...)
#define WINE_ERR(...)
#endif

#endif // !__PWASIO_PWASIO_H__
