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

// ---- v0.12.0-alpha.4 Track FX APIs (motion envelope hosting) -------------
//
// TrackFX_AddByName: add an FX to a track by display name. instantiate
// flag controls behaviour:
//    0  = query-only, returns existing FX index or -1 if not present
//    1  = always add a new instance, returns its index
//   <0  = add only if not already present, returns existing index if found
//
// Returns FX index (>=0) on success, -1 on failure. For JSFX, the name
// uses the file path inside Effects/, e.g. "HoloRoll/holoroll_motion".
using TrackFX_AddByNameFn = int (*)(MediaTrack* track, const char* fxname, bool recFX, int instantiate);

// Number of FX on the track's regular FX chain (excludes record/input FX).
using TrackFX_GetCountFn = int (*)(MediaTrack* track);

// Get the display name of an FX. bufOut is filled with up to bufOut_sz bytes.
// Returns true on success.
using TrackFX_GetFXNameFn = bool (*)(MediaTrack* track, int fx, char* bufOut, int bufOut_sz);

// Generic track property getter/setter. We use it for track names
// (parmname="P_NAME"). REAPER docs say the parameter buffer must be writable
// even when reading.
using GetSetMediaTrackInfo_StringFn = bool (*)(MediaTrack* tr, const char* parmname, char* stringNeedBig, bool setNewValue);

// ---- v0.12.0-alpha.5 envelope APIs (motion-curve writing) ----------------
//
// GetFXEnvelope: get-or-create the envelope for a specific FX parameter.
// `create=true` materialises the envelope on the track if it doesn't
// already exist. Returns nullptr if the (track, fx, param) tuple is
// invalid or REAPER refuses for any reason.
class TrackEnvelope;
using GetFXEnvelopeFn = TrackEnvelope* (*)(MediaTrack* track, int fxindex, int parameterindex, bool create);

// Insert one envelope point at `time` with `value`. shape: 0=linear,
// 1=square, 2=slow start/end, 3=fast start, 4=fast end, 5=bezier.
// noSortInOptional: pass a pointer to bool=true while batch-inserting,
// then call Envelope_SortPoints once at the end. Pass nullptr for
// per-point sorting (slow on bulk insert).
using InsertEnvelopePointFn = bool (*)(TrackEnvelope* envelope, double time, double value,
                                       int shape, double tension, bool selected,
                                       bool* noSortInOptional);

// Sort envelope points by time. Required after a batch insert that used
// noSortIn=true. Returns false if the envelope pointer is invalid.
using Envelope_SortPointsFn = bool (*)(TrackEnvelope* envelope);

// Delete all envelope points within [time_start, time_end]. Used to
// clean a stale section before re-writing motion data. Endpoints are
// inclusive. Returns false if the envelope pointer is invalid.
using DeleteEnvelopePointRangeFn = bool (*)(TrackEnvelope* envelope, double time_start, double time_end);

// Number of FX parameters on a given FX. Used to validate paramIdx is in
// range before calling GetFXEnvelope.
using TrackFX_GetNumParamsFn = int (*)(MediaTrack* track, int fx);

// ---- v0.12.0-alpha.7 envelope state-chunk APIs (visibility flag) -------
//
// GetFXEnvelope creates the envelope but leaves its VIS flag at 0 by
// default — the envelope holds data but doesn't render in the track
// view until the user manually opens the FX window. To make placed
// motion envelopes appear immediately we read the envelope's state
// chunk, flip its VIS line to "1 1 1.0" (visible / lane visible /
// height factor 1.0), and write it back.
using GetEnvelopeStateChunkFn = bool (*)(TrackEnvelope* env, char* strNeedBig,
                                         int strNeedBig_sz, bool isundoOptional);
using SetEnvelopeStateChunkFn = bool (*)(TrackEnvelope* env, const char* str,
                                         bool isundoOptional);

// ---- v0.12.0-alpha.11 socket-bridge APIs --------------------------------
//
// Backing functions for the WAAPI-style TCP socket bridge that lets
// external apps query and modify the project from outside REAPER.
// See src/extension/socket_server.cpp for the verb handlers.
using GetSelectedMediaItemFn   = MediaItem* (*)(ReaProject* proj, int selitem);
using CountSelectedMediaItemsFn = int (*)(ReaProject* proj);
using GetTakeNameFn            = const char* (*)(MediaItem_Take* take);
using TakeIsMIDIFn             = bool (*)(MediaItem_Take* take);
using Undo_BeginBlockFn        = void (*)();
using Undo_EndBlockFn          = void (*)(const char* descchange, int extraflags);
using PreventUIRefreshFn       = void (*)(int prevent_count);
using AddRemoveReaScriptFn     = int (*)(bool isAdd, int sectionID,
                                         const char* scriptfn, bool commit);

// ---- v0.14.0-alpha.11 canonical chunk API (split Get/Set) -----------------
//
// Modern REAPER SDK exposes item-chunk access via two separate
// functions, not the deprecated unified `GetSetItemStateChunk`:
//
//   bool GetItemStateChunk(MediaItem* item, char* strNeedBig,
//                          int strNeedBig_sz, bool isundoOptional);
//   bool SetItemStateChunk(MediaItem* item, const char* str,
//                          bool isundoOptional);
//
// alpha.9 bound the obsolete name `GetSetItemStateChunk` and GetFunc
// returned nullptr because that symbol doesn't exist in current
// REAPER builds — manifested in alpha.9/.10 as the audit/diagnostic
// reporting "all paths failed" for ReadItemChunk.
//
// We also fall back to `GetSetMediaItemInfo_String("P_CHUNK")` for
// very old REAPER builds, even though P_CHUNK isn't officially
// supported for that function.
using GetItemStateChunkFn = bool (*)(MediaItem* item, char* strNeedBig,
                                     int strNeedBig_sz, bool isundo);
using SetItemStateChunkFn = bool (*)(MediaItem* item, const char* str,
                                     bool isundo);

// ---- v0.15.0-alpha.1 take Info_Value (D_PLAYRATE for slowdown) ----------
//
// Slowdown/speedup is implemented via REAPER's per-take D_PLAYRATE
// (1.0 = original speed, 0.5 = half speed = 2x slowdown). Setting it
// on a take affects how the source is consumed inside the item;
// HoloRoll combines this with reverse + frame-time mapping in
// ResolvePlayheadFromItems.
using SetMediaItemTakeInfo_ValueFn = bool (*)(MediaItem_Take* take,
                                              const char* parmname,
                                              double newvalue);
using GetMediaItemTakeInfo_ValueFn = double (*)(MediaItem_Take* take,
                                                const char* parmname);

// ---- v0.14.0-alpha.6 silent-source attachment (SECTION-reverse mechanism) -
//
// Reverse playback in REAPER goes through a <SOURCE SECTION ... MODE 2>
// chunk wrapper around the active take's audio source. Without an
// underlying PCM_source (i.e. on empty takes) the wrapper is a no-op:
// REAPER has nothing to play backwards. alpha.6 attaches a tiny silent
// WAV (5 minutes, 8 kHz mono 16-bit) as the source for every HoloRoll-
// created item, so SECTION/MODE 2 wrapping is mechanically valid even
// though the audio itself is silence. The user hears nothing either
// way; what matters is that REAPER's GUI reflects the reverse state
// (Item Properties → Section/Reverse checkbox checked) and the chunk
// substring scan in IsItemReversedViaChunk can detect it.
using PCM_Source_CreateFromFileFn = PCM_source* (*)(const char* filename);
using SetMediaItemTake_SourceFn = bool (*)(MediaItem_Take* take, PCM_source* source);

// ---- v0.12.0-alpha.15 socket-bridge APIs (register / shortcut / cursor) -
//
// register_action returns a string named command id (e.g. "_RS1234abcd")
// — that's what ReverseNamedCommandLookup is for. script_shortcut /
// assign_shortcut work against the main keyboard section, which we
// obtain via SectionFromUniqueID(0). KbdSectionInfo comes from
// reaper_plugin.h (already included above) — it's a typedef for a
// struct, so a `class` forward-declaration here would clash with the
// SDK's definition (LNK2371 in MSVC). We rely on the include and just
// reference the type directly.
using ReverseNamedCommandLookupFn = const char* (*)(int command_id);
using NamedCommandLookupFn        = int (*)(const char* command_name);
using GetCursorPositionFn         = double (*)();
using SectionFromUniqueIDFn       = KbdSectionInfo* (*)(int uniqueID);
using CountActionShortcutsFn      = int (*)(KbdSectionInfo* section, int cmdID);
using GetActionShortcutDescFn     = bool (*)(KbdSectionInfo* section, int cmdID,
                                             int shortcutidx,
                                             char* descOut, int descOut_sz);

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

  // v0.12.0-alpha.4: track FX hosting for motion envelopes.
  TrackFX_AddByNameFn trackFX_AddByName = nullptr;
  TrackFX_GetCountFn trackFX_GetCount = nullptr;
  TrackFX_GetFXNameFn trackFX_GetFXName = nullptr;
  GetSetMediaTrackInfo_StringFn getSetMediaTrackInfo_String = nullptr;

  // v0.12.0-alpha.5: envelope writing.
  GetFXEnvelopeFn getFXEnvelope = nullptr;
  InsertEnvelopePointFn insertEnvelopePoint = nullptr;
  Envelope_SortPointsFn envelope_SortPoints = nullptr;
  DeleteEnvelopePointRangeFn deleteEnvelopePointRange = nullptr;
  TrackFX_GetNumParamsFn trackFX_GetNumParams = nullptr;

  // v0.12.0-alpha.7: envelope visibility flag via state chunk.
  GetEnvelopeStateChunkFn getEnvelopeStateChunk = nullptr;
  SetEnvelopeStateChunkFn setEnvelopeStateChunk = nullptr;

  // v0.12.0-alpha.11: socket bridge (selection, undo, scripts).
  GetSelectedMediaItemFn   getSelectedMediaItem = nullptr;
  CountSelectedMediaItemsFn countSelectedMediaItems = nullptr;
  GetTakeNameFn            getTakeName = nullptr;
  TakeIsMIDIFn             takeIsMIDI = nullptr;
  Undo_BeginBlockFn        undo_BeginBlock = nullptr;
  Undo_EndBlockFn          undo_EndBlock = nullptr;
  PreventUIRefreshFn       preventUIRefresh = nullptr;
  AddRemoveReaScriptFn     addRemoveReaScript = nullptr;

  // v0.14.0-alpha.6: silent-source attachment for SECTION/MODE 2 reverse.
  PCM_Source_CreateFromFileFn pcm_Source_CreateFromFile = nullptr;
  SetMediaItemTake_SourceFn   setMediaItemTake_Source = nullptr;

  // v0.15.0-alpha.1: per-take playrate for slowdown feature.
  SetMediaItemTakeInfo_ValueFn setMediaItemTakeInfo_Value = nullptr;
  GetMediaItemTakeInfo_ValueFn getMediaItemTakeInfo_Value = nullptr;

  // v0.14.0-alpha.11: canonical item-chunk API — split into two
  // functions to match modern REAPER SDK. alpha.9's
  // `getSetItemStateChunk` symbol does not exist in current builds.
  GetItemStateChunkFn getItemStateChunk = nullptr;
  SetItemStateChunkFn setItemStateChunk = nullptr;

  // v0.12.0-alpha.15: socket bridge — register_action, script_shortcut,
  // assign_shortcut, get_cursor verbs.
  ReverseNamedCommandLookupFn reverseNamedCommandLookup = nullptr;
  NamedCommandLookupFn        namedCommandLookup = nullptr;
  GetCursorPositionFn         getCursorPosition = nullptr;
  SectionFromUniqueIDFn       sectionFromUniqueID = nullptr;
  CountActionShortcutsFn      countActionShortcuts = nullptr;
  GetActionShortcutDescFn     getActionShortcutDesc = nullptr;
};

bool ResolveReaperApi(reaper_plugin_info_t* rec, ReaperApi& api);
