/*
Copyright (C) 2003 John K. Hohm
Copyright (C) 2006 Robert Reif
Copyright (C) 2025 Gabriel Golfetti

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "pwasio.h"
#include <unknwn.h>

#ifdef DEBUG
#include <wine/debug.h>
WINE_DEFAULT_DEBUG_CHANNEL(asio);
#else
#define TRACE(...)
#define WARN(...)
#define ERR(...)
#endif

static HINSTANCE g_hinst;

static HRESULT WINAPI QueryInterface(LPCLASSFACTORY, REFIID, LPVOID *ptr) {
  return ptr ? E_NOINTERFACE : E_POINTER;
}
static ULONG WINAPI AddRef(LPCLASSFACTORY _data) {
  struct factory *factory = (struct factory *)_data;
  return InterlockedIncrement(&factory->ref);
}
static ULONG WINAPI Release(LPCLASSFACTORY _data) {
  struct factory *factory = (struct factory *)_data;
  if (InterlockedDecrement(&factory->ref))
    return factory->ref;
  HeapFree(GetProcessHeap(), 0, factory);
  return 0;
}
static HRESULT WINAPI LockServer(LPCLASSFACTORY, BOOL) { return S_OK; }

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppvObj) {
  TRACE("\n");
  if (ppvObj == NULL || !IsEqualIID(riid, &IID_IClassFactory))
    return E_INVALIDARG;
  if (!IsEqualGUID(rclsid, &class_id))
    return CLASS_E_CLASSNOTAVAILABLE;

  struct factory *factory;
  if (!(*ppvObj = factory =
            HeapAlloc(GetProcessHeap(), 0, sizeof(struct factory))))
    return E_OUTOFMEMORY;

  static struct IClassFactoryVtbl vtbl = {
      .QueryInterface = QueryInterface,
      .AddRef = AddRef,
      .Release = Release,

      .CreateInstance = CreateInstance,
      .LockServer = LockServer,
  };

  *factory = (typeof(*factory)){
      .vtbl = &vtbl,
      .ref = 1,
      .hinst = g_hinst,
  };

  return S_OK;
}

HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH)
    g_hinst = hinst;
  return TRUE;
}

#define REG_STR(str) REG_SZ, (const BYTE *)(str), sizeof(str)

#define CHK(call)                                                              \
  do {                                                                         \
    err = (call);                                                              \
    if (err != ERROR_SUCCESS)                                                  \
      goto cleanup;                                                            \
  } while (false)
HRESULT WINAPI DllRegisterServer(void) {
  LONG err = ERROR_SUCCESS;
  HKEY class = NULL, clsid = NULL, ips32 = NULL, driver = NULL;

  CHK(RegCreateKeyExA(HKEY_CLASSES_ROOT, "CLSID", 0, NULL, 0, KEY_WRITE, NULL,
                      &class, NULL));

  WCHAR wstr[39];
  StringFromGUID2(&class_id, wstr, 39);
  CHK(RegCreateKeyExW(class, wstr, 0, NULL, 0, KEY_WRITE, NULL, &clsid, NULL));

  CHK(RegSetValueExA(clsid, NULL, 0, REG_STR("pwasio Object")));

  CHK(RegCreateKeyExA(clsid, "InProcServer32", 0, NULL, 0, KEY_WRITE, NULL,
                      &ips32, NULL));

  CHK(RegSetValueExA(ips32, NULL, 0, REG_STR(LIB_NAME)));
  CHK(RegSetValueExA(ips32, "ThreadingModel", 0, REG_STR("Apartment")));

  CHK(RegCreateKeyExA(HKEY_LOCAL_MACHINE, DRIVER_REG, 0, NULL, 0, KEY_WRITE,
                      NULL, &driver, 0));

  char str[39];
  WideCharToMultiByte(CP_ACP, 0, wstr, -1, str, sizeof str, NULL, NULL);
  CHK(RegSetValueExA(driver, "CLSID", 0, REG_STR(str)));
  CHK(RegSetValueExA(driver, "Description", 0, REG_STR("pwasio Driver")));

cleanup:
  if (driver)
    RegCloseKey(driver);
  if (ips32)
    RegCloseKey(ips32);
  if (clsid)
    RegCloseKey(clsid);
  if (class)
    RegCloseKey(class);

  return err == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(err);
}

#undef CHK
#define CHK(call)                                                              \
  do {                                                                         \
    err = (call);                                                              \
    if (err != ERROR_SUCCESS && err != ERROR_FILE_NOT_FOUND)                   \
      goto cleanup;                                                            \
  } while (false)
HRESULT WINAPI DllUnregisterServer(void) {
  LONG err = ERROR_SUCCESS;
  HKEY key = NULL;

  CHK(RegOpenKeyExA(HKEY_CLASSES_ROOT, "CLSID", 0, KEY_READ | KEY_WRITE, &key));

  WCHAR wstr[39];
  StringFromGUID2(&class_id, wstr, 39);
  CHK(RegDeleteTreeW(key, wstr));

  CHK(RegDeleteTreeA(HKEY_LOCAL_MACHINE, DRIVER_REG));
  CHK(RegDeleteTreeA(HKEY_CURRENT_USER, DRIVER_REG));

cleanup:
  if (key)
    RegCloseKey(key);
  return err != ERROR_SUCCESS && err != ERROR_FILE_NOT_FOUND
             ? HRESULT_FROM_WIN32(err)
             : S_OK;
}
