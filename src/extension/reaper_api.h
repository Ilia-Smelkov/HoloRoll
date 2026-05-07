#pragma once

#include <cstdint>
#include "reaper_plugin.h"

// Forward declarations of REAPER opaque types we pass through the API. These
// are declared (without definitions) in reaper_plugin_functions.h, but that
// header pulls in a lot more than we need; declaring them here keeps our
// dependency surface minimal.
class MediaTrack;
class MediaItem;
class MediaItem_Take;

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

// Read the toggle-state of an action. Returns 1 if the toggle is ON,
// 0 if OFF, -1 if the action doesn't have a toggle state. Used in
// v0.12.0-alpha.1 to check whether REAPER's console window is currently
// open before issuing the toggle action 40078 (so we don't accidentally
// close it).
using GetToggleCommandStateFn = int (*)(int command_id);

// ---- Track / item / take APIs (used for spike: create empty item on a track) -----------
//
// MediaTrack and MediaItem / MediaItem_Take are opaque pointer types defined
// inside reaper_plugin.h — we only ever pass them around without dereferencing.
using GetSelectedTrackFn = MediaTrack* (*)(ReaProject* proj, int seltrackidx);
using GetTrackFn = MediaTrack* (*)(ReaProject* proj, int trackidx);
using CountTracksFn = int (*)(ReaProject* proj);
using AddMediaItemToTrackFn = MediaItem* (*)(MediaTrack* tr);
using AddTakeToMediaItemFn = MediaItem_Take* (*)(MediaItem* item);
using SetMediaItemInfo_ValueFn = bool (*)(MediaItem* item, const char* parmname, double newvalue);
using GetSetMediaItemTakeInfo_StringFn = bool (*)(MediaItem_Take* take, const char* parmname, char* stringNeedBig, bool setNewValue);
using UpdateArrangeFn = void (*)();

// ---- Item enumeration / read APIs (v0.6.0 items workflow) -------------------
using CountTrackMediaItemsFn = int (*)(MediaTrack* tr);
using GetTrackMediaItemFn = MediaItem* (*)(MediaTrack* tr, int itemidx);
using GetMediaItemInfo_ValueFn = double (*)(MediaItem* item, const char* parmname);
using GetMediaItemTakeFn = MediaItem_Take* (*)(MediaItem* item, int takeidx);
using GetActiveTakeFn = MediaItem_Take* (*)(MediaItem* item);
using GetSetMediaItemInfo_StringFn = bool (*)(MediaItem* item, const char* parmname, char* stringNeedBig, bool setNewValue);

// Insert a new track at the given index. wantDefaults=true makes REAPER
// apply default-track settings (sane volume, no FX). v0.9.1 uses this to
// create a fresh track at index 0 when placing newly-imported animations
// so the user keeps a clean separation between HoloRoll and other tracks.
using InsertTrackAtIndexFn = void (*)(int idx, bool wantDefaults);

// ---- Project APIs (v0.7.0 project-relative animations folder) -------------
//
// EnumProjects(idx, path, sz): returns the ReaProject at idx (-1 = current);
// fills `path` with the .rpp file path for that project, or empty if the
// project hasn't been saved yet (Untitled project).
using EnumProjectsFn = ReaProject* (*)(int idx, char* projfn, int projfn_sz);

// SetProjExtState/GetProjExtState: per-project key-value strings, persisted
// inside the .rpp on save. We use this to remember a folder override for
// projects whose user pointed Choose folder... at a non-default path.
using SetProjExtStateFn = int (*)(ReaProject* proj, const char* extname, const char* key, const char* value);
using GetProjExtStateFn = int (*)(ReaProject* proj, const char* extname, const char* key, char* valOutNeedBig, int valOutNeedBig_sz);

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
  GetToggleCommandStateFn getToggleCommandState = nullptr;

  // Track / item / take (spike for v0.6.0 items model).
  GetSelectedTrackFn getSelectedTrack = nullptr;
  GetTrackFn getTrack = nullptr;
  CountTracksFn countTracks = nullptr;
  AddMediaItemToTrackFn addMediaItemToTrack = nullptr;
  AddTakeToMediaItemFn addTakeToMediaItem = nullptr;
  SetMediaItemInfo_ValueFn setMediaItemInfo_Value = nullptr;
  GetSetMediaItemTakeInfo_StringFn getSetMediaItemTakeInfo_String = nullptr;
  UpdateArrangeFn updateArrange = nullptr;

  // Item enumeration / read (v0.6.0 ResolvePlayhead via items).
  CountTrackMediaItemsFn countTrackMediaItems = nullptr;
  GetTrackMediaItemFn getTrackMediaItem = nullptr;
  GetMediaItemInfo_ValueFn getMediaItemInfo_Value = nullptr;
  GetMediaItemTakeFn getMediaItemTake = nullptr;
  GetActiveTakeFn getActiveTake = nullptr;
  GetSetMediaItemInfo_StringFn getSetMediaItemInfo_String = nullptr;

  // Track manipulation (v0.9.1 "create new track on top for placements").
  InsertTrackAtIndexFn insertTrackAtIndex = nullptr;

  // Project introspection (v0.7.0 project-relative folders).
  EnumProjectsFn enumProjects = nullptr;
  SetProjExtStateFn setProjExtState = nullptr;
  GetProjExtStateFn getProjExtState = nullptr;
};

bool ResolveReaperApi(reaper_plugin_info_t* rec, ReaperApi& api);
