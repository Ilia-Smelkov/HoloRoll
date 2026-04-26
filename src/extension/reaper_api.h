#pragma once

#include <cstdint>
#include "reaper_plugin.h"

using GetPlayPositionFn = double (*)();
using GetPlayStateFn = int (*)();
using GetCursorPositionExFn = double (*)(ReaProject* proj);
using MainOnCommandFn = void (*)(int command, int flag);
using ShowConsoleMsgFn = void (*)(const char* msg);
using AddProjectMarker2Fn = int (*)(ReaProject* proj, bool isrgn, double pos, double rgnend, const char* name, int wantidx, int color);
using DeleteProjectMarkerFn = bool (*)(ReaProject* proj, int markrgnindexnumber, bool isrgn);
using EnumProjectMarkers3Fn = int (*)(ReaProject* proj, int idx, bool* isrgnOut, double* posOut, double* rgnendOut, const char** nameOut, int* markrgnindexnumberOut, int* colorOut);
using SetExtStateFn = void (*)(const char* section, const char* key, const char* value, bool persist);
using GetExtStateFn = const char* (*)(const char* section, const char* key);
using HasExtStateFn = bool (*)(const char* section, const char* key);
using DockWindowAddExFn = void (*)(HWND hwnd, const char* name, const char* identstr, bool allowShow);
using DockWindowRemoveFn = void (*)(HWND hwnd);
using DockWindowActivateFn = void (*)(HWND hwnd);

struct ReaperApi {
  GetPlayPositionFn getPlayPosition = nullptr;
  GetPlayStateFn getPlayState = nullptr;
  GetCursorPositionExFn getCursorPositionEx = nullptr;
  MainOnCommandFn mainOnCommand = nullptr;
  ShowConsoleMsgFn showConsoleMsg = nullptr;
  AddProjectMarker2Fn addProjectMarker2 = nullptr;
  DeleteProjectMarkerFn deleteProjectMarker = nullptr;
  EnumProjectMarkers3Fn enumProjectMarkers3 = nullptr;
  SetExtStateFn setExtState = nullptr;
  GetExtStateFn getExtState = nullptr;
  HasExtStateFn hasExtState = nullptr;
  DockWindowAddExFn dockWindowAddEx = nullptr;
  DockWindowRemoveFn dockWindowRemove = nullptr;
  DockWindowActivateFn dockWindowActivate = nullptr;
};

bool ResolveReaperApi(reaper_plugin_info_t* rec, ReaperApi& api);
