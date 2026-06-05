#include "extension/reaper_bridge.h"

bool ResolveReaperApi(reaper_plugin_info_t* rec, ReaperApi& api) {
  if (!rec || !rec->GetFunc) {
    return false;
  }

  api.getPlayPosition = reinterpret_cast<GetPlayPositionFn>(rec->GetFunc("GetPlayPosition"));
  api.getPlayState = reinterpret_cast<GetPlayStateFn>(rec->GetFunc("GetPlayState"));
  api.getCursorPositionEx = reinterpret_cast<GetCursorPositionExFn>(rec->GetFunc("GetCursorPositionEx"));
  api.mainOnCommand = reinterpret_cast<MainOnCommandFn>(rec->GetFunc("Main_OnCommand"));
  api.showConsoleMsg = reinterpret_cast<ShowConsoleMsgFn>(rec->GetFunc("ShowConsoleMsg"));
  api.addProjectMarker2 = reinterpret_cast<AddProjectMarker2Fn>(rec->GetFunc("AddProjectMarker2"));
  api.deleteProjectMarker = reinterpret_cast<DeleteProjectMarkerFn>(rec->GetFunc("DeleteProjectMarker"));
  api.enumProjectMarkers3 = reinterpret_cast<EnumProjectMarkers3Fn>(rec->GetFunc("EnumProjectMarkers3"));
  api.setExtState = reinterpret_cast<SetExtStateFn>(rec->GetFunc("SetExtState"));
  api.getExtState = reinterpret_cast<GetExtStateFn>(rec->GetFunc("GetExtState"));
  api.hasExtState = reinterpret_cast<HasExtStateFn>(rec->GetFunc("HasExtState"));
  api.dockWindowAddEx = reinterpret_cast<DockWindowAddExFn>(rec->GetFunc("DockWindowAddEx"));
  api.dockWindowRemove = reinterpret_cast<DockWindowRemoveFn>(rec->GetFunc("DockWindowRemove"));
  api.dockWindowActivate = reinterpret_cast<DockWindowActivateFn>(rec->GetFunc("DockWindowActivate"));

  // v0.12.0-alpha.1: GetToggleCommandState lets us check console-window
  // visibility before issuing the toggle action 40078.
  api.getToggleCommandState = reinterpret_cast<GetToggleCommandStateFn>(rec->GetFunc("GetToggleCommandState"));

  // Track / item / take APIs (v0.6.0 items spike). All optional — if any is
  // missing, the spike button will report a useful error.
  api.getSelectedTrack = reinterpret_cast<GetSelectedTrackFn>(rec->GetFunc("GetSelectedTrack"));
  api.getTrack = reinterpret_cast<GetTrackFn>(rec->GetFunc("GetTrack"));
  api.countTracks = reinterpret_cast<CountTracksFn>(rec->GetFunc("CountTracks"));
  api.addMediaItemToTrack = reinterpret_cast<AddMediaItemToTrackFn>(rec->GetFunc("AddMediaItemToTrack"));
  api.addTakeToMediaItem = reinterpret_cast<AddTakeToMediaItemFn>(rec->GetFunc("AddTakeToMediaItem"));
  api.setMediaItemInfo_Value = reinterpret_cast<SetMediaItemInfo_ValueFn>(rec->GetFunc("SetMediaItemInfo_Value"));
  api.getSetMediaItemTakeInfo_String = reinterpret_cast<GetSetMediaItemTakeInfo_StringFn>(rec->GetFunc("GetSetMediaItemTakeInfo_String"));
  api.updateArrange = reinterpret_cast<UpdateArrangeFn>(rec->GetFunc("UpdateArrange"));

  // Item enumeration / read (v0.6.0 items resolution path).
  api.countTrackMediaItems = reinterpret_cast<CountTrackMediaItemsFn>(rec->GetFunc("CountTrackMediaItems"));
  api.getTrackMediaItem = reinterpret_cast<GetTrackMediaItemFn>(rec->GetFunc("GetTrackMediaItem"));
  api.getMediaItemInfo_Value = reinterpret_cast<GetMediaItemInfo_ValueFn>(rec->GetFunc("GetMediaItemInfo_Value"));
  api.getMediaItemTake = reinterpret_cast<GetMediaItemTakeFn>(rec->GetFunc("GetMediaItemTake"));
  api.getActiveTake = reinterpret_cast<GetActiveTakeFn>(rec->GetFunc("GetActiveTake"));
  api.getSetMediaItemInfo_String = reinterpret_cast<GetSetMediaItemInfo_StringFn>(rec->GetFunc("GetSetMediaItemInfo_String"));

  // Track manipulation (v0.9.1).
  api.insertTrackAtIndex = reinterpret_cast<InsertTrackAtIndexFn>(rec->GetFunc("InsertTrackAtIndex"));

  // Project introspection (v0.7.0 project-relative folders).
  api.enumProjects = reinterpret_cast<EnumProjectsFn>(rec->GetFunc("EnumProjects"));
  api.setProjExtState = reinterpret_cast<SetProjExtStateFn>(rec->GetFunc("SetProjExtState"));
  api.getProjExtState = reinterpret_cast<GetProjExtStateFn>(rec->GetFunc("GetProjExtState"));

  // v0.12.0-alpha.4: track FX APIs for motion envelope hosting.
  api.trackFX_AddByName = reinterpret_cast<TrackFX_AddByNameFn>(rec->GetFunc("TrackFX_AddByName"));
  api.trackFX_GetCount = reinterpret_cast<TrackFX_GetCountFn>(rec->GetFunc("TrackFX_GetCount"));
  api.trackFX_GetFXName = reinterpret_cast<TrackFX_GetFXNameFn>(rec->GetFunc("TrackFX_GetFXName"));
  api.getSetMediaTrackInfo_String = reinterpret_cast<GetSetMediaTrackInfo_StringFn>(rec->GetFunc("GetSetMediaTrackInfo_String"));

  // v0.12.0-alpha.5: envelope writing APIs.
  api.getFXEnvelope = reinterpret_cast<GetFXEnvelopeFn>(rec->GetFunc("GetFXEnvelope"));
  api.insertEnvelopePoint = reinterpret_cast<InsertEnvelopePointFn>(rec->GetFunc("InsertEnvelopePoint"));
  api.envelope_SortPoints = reinterpret_cast<Envelope_SortPointsFn>(rec->GetFunc("Envelope_SortPoints"));
  api.deleteEnvelopePointRange = reinterpret_cast<DeleteEnvelopePointRangeFn>(rec->GetFunc("DeleteEnvelopePointRange"));
  api.trackFX_GetNumParams = reinterpret_cast<TrackFX_GetNumParamsFn>(rec->GetFunc("TrackFX_GetNumParams"));

  // v0.12.0-alpha.7: envelope state-chunk APIs (used to flip VIS to 1
  // so freshly written envelopes appear without manual FX-window open).
  api.getEnvelopeStateChunk = reinterpret_cast<GetEnvelopeStateChunkFn>(rec->GetFunc("GetEnvelopeStateChunk"));
  api.setEnvelopeStateChunk = reinterpret_cast<SetEnvelopeStateChunkFn>(rec->GetFunc("SetEnvelopeStateChunk"));

  // v0.12.0-alpha.11: socket bridge — selection, undo, script execution.
  api.getSelectedMediaItem    = reinterpret_cast<GetSelectedMediaItemFn>(rec->GetFunc("GetSelectedMediaItem"));
  api.countSelectedMediaItems = reinterpret_cast<CountSelectedMediaItemsFn>(rec->GetFunc("CountSelectedMediaItems"));
  api.getTakeName             = reinterpret_cast<GetTakeNameFn>(rec->GetFunc("GetTakeName"));
  api.takeIsMIDI              = reinterpret_cast<TakeIsMIDIFn>(rec->GetFunc("TakeIsMIDI"));
  api.undo_BeginBlock         = reinterpret_cast<Undo_BeginBlockFn>(rec->GetFunc("Undo_BeginBlock"));
  api.undo_EndBlock           = reinterpret_cast<Undo_EndBlockFn>(rec->GetFunc("Undo_EndBlock"));
  api.preventUIRefresh        = reinterpret_cast<PreventUIRefreshFn>(rec->GetFunc("PreventUIRefresh"));
  api.addRemoveReaScript      = reinterpret_cast<AddRemoveReaScriptFn>(rec->GetFunc("AddRemoveReaScript"));

  // v0.14.0-alpha.6: silent-source attachment so SECTION/MODE 2 has a
  // real underlying PCM_source to wrap. Required for the section/reverse
  // mechanism to be mechanically valid; HoloRoll items previously had
  // empty takes, which made the SECTION wrap a no-op.
  api.pcm_Source_CreateFromFile = reinterpret_cast<PCM_Source_CreateFromFileFn>(rec->GetFunc("PCM_Source_CreateFromFile"));
  api.setMediaItemTake_Source   = reinterpret_cast<SetMediaItemTake_SourceFn>(rec->GetFunc("SetMediaItemTake_Source"));

  // v0.12.0-alpha.15: socket bridge — action registration / shortcut
  // introspection / edit cursor.
  api.reverseNamedCommandLookup = reinterpret_cast<ReverseNamedCommandLookupFn>(rec->GetFunc("ReverseNamedCommandLookup"));
  api.namedCommandLookup        = reinterpret_cast<NamedCommandLookupFn>(rec->GetFunc("NamedCommandLookup"));
  api.getCursorPosition         = reinterpret_cast<GetCursorPositionFn>(rec->GetFunc("GetCursorPosition"));
  api.sectionFromUniqueID       = reinterpret_cast<SectionFromUniqueIDFn>(rec->GetFunc("SectionFromUniqueID"));
  api.countActionShortcuts      = reinterpret_cast<CountActionShortcutsFn>(rec->GetFunc("CountActionShortcuts"));
  api.getActionShortcutDesc     = reinterpret_cast<GetActionShortcutDescFn>(rec->GetFunc("GetActionShortcutDesc"));

  return api.getPlayPosition && api.mainOnCommand;
}

bool ReaperBridge::Initialize(reaper_plugin_info_t* rec) {
  if (!ResolveReaperApi(rec, api_)) {
    return false;
  }

  OnTimerTick();
  return true;
}

void ReaperBridge::Shutdown(reaper_plugin_info_t* rec) {
  (void)rec;
  timelineTimeSeconds_.store(0.0);
}

void ReaperBridge::OnTimerTick() {
  if (!api_.getPlayPosition) {
    return;
  }

  double timelineTime = api_.getPlayPosition();
  const bool hasTransportAndCursor = api_.getPlayState && api_.getCursorPositionEx;
  if (hasTransportAndCursor) {
    const int playState = api_.getPlayState();
    const bool isPlayingOrRecording = (playState & 1) != 0 || (playState & 4) != 0;
    if (!isPlayingOrRecording) {
      timelineTime = api_.getCursorPositionEx(nullptr);
    }
  }

  timelineTimeSeconds_.store(timelineTime);
}
