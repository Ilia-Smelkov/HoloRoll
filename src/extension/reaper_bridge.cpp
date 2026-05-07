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
