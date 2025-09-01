#ifndef __WINEASIO_WINEASIO_H__
#define __WINEASIO_WINEASIO_H__

#include <windows.h>

static GUID const class_id = {
    0x9d9612bc,
    0xcadd,
    0x43a2,
    {0xaa, 0x6f, 0x59, 0xf6, 0xac, 0xa4, 0xfe, 0x74},
};

bool pwasio_init();
void *pwasio_create();

#endif // !__WINEASIO_WINEASIO_H__
