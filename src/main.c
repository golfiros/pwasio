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

WINE_DEFAULT_DEBUG_CHANNEL(pwasio_dll);

static HINSTANCE g_hinst;

static HRESULT WINAPI QueryInterface(LPCLASSFACTORY, REFIID, LPVOID *ptr) {
  WINE_TRACE("\n");
  return ptr ? E_NOINTERFACE : E_POINTER;
}
static ULONG WINAPI AddRef(LPCLASSFACTORY _data) {
  WINE_TRACE("\n");
  struct factory *factory = (struct factory *)_data;
  return InterlockedIncrement(&factory->ref);
}
static ULONG WINAPI Release(LPCLASSFACTORY _data) {
  WINE_TRACE("\n");
  struct factory *factory = (struct factory *)_data;
  if (InterlockedDecrement(&factory->ref))
    return factory->ref;
  HeapFree(GetProcessHeap(), 0, factory);
  return 0;
}
static HRESULT WINAPI LockServer(LPCLASSFACTORY, BOOL) {
  WINE_TRACE("\n");
  return S_OK;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppvObj) {
  WINE_TRACE("\n");
  if (ppvObj == nullptr || !IsEqualIID(riid, &IID_IClassFactory))
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

HRESULT WINAPI DllCanUnloadNow(void) {
  WINE_TRACE("\n");
  return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  WINE_TRACE("%d\n", reason);
  if (reason == DLL_PROCESS_ATTACH)
    g_hinst = hinst;
  return TRUE;
}

#define REG_STR(str) REG_SZ, (const BYTE *)(str), sizeof(str)

HRESULT WINAPI DllRegisterServer(void) {
  WINE_TRACE("\n");
  LONG err = ERROR_SUCCESS;
  HKEY class = nullptr, clsid = nullptr, ips32 = nullptr, driver = nullptr;
#define CHK(call)                                                              \
  do {                                                                         \
    err = (call);                                                              \
    if (err != ERROR_SUCCESS) {                                                \
      WINE_ERR("registry call " #call "failed\n");                             \
      goto cleanup;                                                            \
    }                                                                          \
  } while (false)

  CHK(RegCreateKeyExA(HKEY_CLASSES_ROOT, "CLSID", 0, nullptr, 0, KEY_WRITE,
                      nullptr, &class, nullptr));

  WCHAR wstr[39];
  StringFromGUID2(&class_id, wstr, 39);
  CHK(RegCreateKeyExW(class, wstr, 0, nullptr, 0, KEY_WRITE, nullptr, &clsid,
                      nullptr));

  CHK(RegSetValueExA(clsid, nullptr, 0, REG_STR("pwasio Object")));

  CHK(RegCreateKeyExA(clsid, "InProcServer32", 0, nullptr, 0, KEY_WRITE,
                      nullptr, &ips32, nullptr));

  CHK(RegSetValueExA(ips32, nullptr, 0, REG_STR(LIB_NAME)));
  CHK(RegSetValueExA(ips32, "ThreadingModel", 0, REG_STR("Apartment")));

  CHK(RegCreateKeyExA(HKEY_LOCAL_MACHINE, DRIVER_REG, 0, nullptr, 0, KEY_WRITE,
                      nullptr, &driver, 0));

  char str[39];
  WideCharToMultiByte(CP_ACP, 0, wstr, -1, str, sizeof str, nullptr, nullptr);
  CHK(RegSetValueExA(driver, "CLSID", 0, REG_STR(str)));
  CHK(RegSetValueExA(driver, "Description", 0, REG_STR("pwasio Driver")));
#undef CHK

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

HRESULT WINAPI DllUnregisterServer(void) {
  WINE_TRACE("\n");
  LONG err = ERROR_SUCCESS;
  HKEY key = nullptr;
#define CHK(call)                                                              \
  do {                                                                         \
    err = (call);                                                              \
    if (err != ERROR_SUCCESS && err != ERROR_FILE_NOT_FOUND) {                 \
      WINE_ERR("registry call " #call "failed\n");                             \
      goto cleanup;                                                            \
    }                                                                          \
  } while (false)

  CHK(RegOpenKeyExA(HKEY_CLASSES_ROOT, "CLSID", 0, KEY_READ | KEY_WRITE, &key));

  WCHAR wstr[39];
  StringFromGUID2(&class_id, wstr, 39);
  CHK(RegDeleteTreeW(key, wstr));

  CHK(RegDeleteTreeA(HKEY_LOCAL_MACHINE, DRIVER_REG));
  CHK(RegDeleteTreeA(HKEY_CURRENT_USER, DRIVER_REG));
#undef CHK

cleanup:
  if (key)
    RegCloseKey(key);
  return err != ERROR_SUCCESS && err != ERROR_FILE_NOT_FOUND
             ? HRESULT_FROM_WIN32(err)
             : S_OK;
}
