// windows.h MUST come before shlobj.h / objbase.h.
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>

#include "extension/folder_picker.h"

#include <string>

namespace folder_picker {

namespace {
int CALLBACK BrowseCallback(HWND hwnd, UINT msg, LPARAM lParam, LPARAM data) {
  (void)lParam;
  if (msg == BFFM_INITIALIZED) {
    const char* initial = reinterpret_cast<const char*>(data);
    if (initial && *initial) {
      SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, reinterpret_cast<LPARAM>(initial));
    }
  }
  return 0;
}
}  // namespace

std::string BrowseForFolder(HWND owner, const std::string& title, const std::string& initialDir) {
  // SHBrowseForFolder uses COM under the hood.
  const HRESULT coInit = OleInitialize(nullptr);

  char displayName[MAX_PATH] = {};
  BROWSEINFOA bi{};
  bi.hwndOwner = owner;
  bi.pszDisplayName = displayName;
  bi.lpszTitle = title.c_str();
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
  bi.lpfn = BrowseCallback;
  bi.lParam = reinterpret_cast<LPARAM>(initialDir.c_str());

  std::string result;
  PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
  if (pidl) {
    char path[MAX_PATH] = {};
    if (SHGetPathFromIDListA(pidl, path)) {
      result = path;
    }
    CoTaskMemFree(pidl);
  }

  if (SUCCEEDED(coInit)) {
    OleUninitialize();
  }
  return result;
}

}  // namespace folder_picker
