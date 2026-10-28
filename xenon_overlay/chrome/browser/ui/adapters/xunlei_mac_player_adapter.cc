// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/adapters/xunlei_mac_player_adapter.h"

#include "base/no_destructor.h"
#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_adapter.h"

namespace xenon {
namespace {

base::ListValue ObjectFailure() {
  return base::ListValue().Append(
      base::DictValue().Set("success", 0).Set("data", base::DictValue()));
}
base::ListValue EmptyStringFailure() {
  return base::ListValue().Append("");
}
base::ListValue LegacyFailure() {
  return base::ListValue().Append(false).Append("");
}
base::ListValue ZeroFailure() {
  return base::ListValue().Append(0);
}
base::ListValue FalseFailure() {
  return base::ListValue().Append(false);
}
base::ListValue EmptyFilesFailure() {
  return base::ListValue().Append(
      base::DictValue().Set("files", base::ListValue()));
}

// Exact SDK callback contracts. Completion is the final argument; the two
// NasGetFileLastPostion arities reflect the SDK's reused progress-report key.
constexpr MacPlayerCallbackContract kContracts[] = {
    {"setGlobalHttpRequest", "", 6, EmptyStringFailure},
    {"addStatCallback", "", 2, nullptr},
    {"registerPanApi", "GetFileInfo", 3, ObjectFailure},
    {"registerPanApi", "GetScrapeFileInfo", 3, ObjectFailure},
    {"registerPanApi", "CheckUrlsAccessable", 2, ObjectFailure},
    {"registerPanApi", "SearchNasFileInSameDirectory", 3, ObjectFailure},
    {"registerPanApi", "GetNasSubtitleFilesInSameDirectory", 4,
     EmptyFilesFailure},
    {"registerPanApi", "NasGetFileLastPostion", 3, ZeroFailure},
    {"registerPanApi", "NasGetFileLastPostion", 4, FalseFailure},
    {"registerPanApi", "GetScrapeDramasList", 3, ObjectFailure},
    {"registerPanApi", "ScrapeGetFileLastPostion", 3, ObjectFailure},
    {"registerPanApi", "ScrapeReportPlayProgress", 6, ObjectFailure},
    {"registerPanApi", "ScrapeAsyncGetServerPlayList", 1, ObjectFailure},
    {"registerPanApi", "ScrapeAsyncDeleteServerPlayRecord", 4, ObjectFailure},
    {"registerPanApi", "AsyncGetVideoSpeedToken", 2, ObjectFailure},
    {"registerPanApi", "PcLiteGetPlayList", 2, ObjectFailure},
    {"registerPanApi", "PcLiteGetPos", 2, ObjectFailure},
    {"registerPanApi", "SearchFileInSameDirectory", 3, ObjectFailure},
    {"registerPanApi", "GetSubtitleFilesInSameDirectory", 4, ObjectFailure},
    {"registerPanApi", "GetLink", 4, ObjectFailure},
    {"registerPanApi", "ReportParam", 1, nullptr},
    {"registerPanApi", "GetParam", 3, ObjectFailure},
    {"registerPanApi", "GetConfig", 1, ObjectFailure},
    {"registerUniversalApi", "GetUrlPlayIdcInfo", 4, ObjectFailure},
    {"registerUniversalApi", "GetMagnetLinkOncePlayInfo", 4, ObjectFailure},
    {"registerUniversalApi", "GetLinkSubFileInfo", 5, ObjectFailure},
    {"registerUniversalApi", "GetSpeedStrategy", 8, LegacyFailure},
    {"registerUniversalApi", "GetDefaultSpeedStrategy", 2, LegacyFailure},
    {"registerMediaStorageOnlineApi", "FindMediaId", 2, ObjectFailure},
    {"registerMediaStorageOnlineApi", "AddMedia", 2, ObjectFailure},
    {"registerMediaStorageOnlineApi", "UpdatePlayInfo", 2, ObjectFailure},
    {"registerMediaStorageOnlineApi", "GetPlayInfo", 2, ObjectFailure},
    {"registerMediaStorageOnlineApi", "GetPlayInfoByUrl", 2, LegacyFailure},
};

constexpr MacPlayerSubscriptionContract kSubscriptions[] = {
    {"attachMediaChangeEvent", "detachMediaChangeEvent"},
    {"attachMediaClosedEvent", "detachMediaCloseEvent"},
    {"attachInitFinishEvent", "detachInitFinishEvent"},
    {"attachQuitPlayEvent", "detachQuitPlayEvent"},
    {"attachWndMessageEvent", "detachWndMessageEvent"},
    {"attachPlayerCrashEvent", "detachPlayerCrashEvent"},
    {"attachVideoScreenShotFinishEvent", "detachVideoScreenShotFinishEvent"},
    {"attachDropFilesEvent", "detachDropFilesEvent"},
    {"attachAudioTrackPreparedEvent", "detachAudioTrackPreparedEvent"},
    {"attachAudioTrackSelectChangeEvent", "detachAudioTrackSelectChangeEvent"},
    {"attachRatioPreparedEvent", "detachRatioPreparedEvent"},
    {"attachProgressChangedEvent", "detachProgressChangedEvent"},
    {"attachProgressSetToLatestEvent", "detachProgressSetToLatestEvent"},
    {"attachPlayStateChangeEvent", "detachPlayStateChangeEvent"},
    {"attachPlayBufferEvent", "detachPlayBufferEvent"},
    {"attachDolbyVisionPreparedEvent", "detachDolbyVisionPreparedEvent"},
    {"attachHDRPreparedEvent", "detachHDRPreparedEvent"},
    {"attachFirstRenderEvent", "detachFirstRenderEvent"},
    {"attachTransmitPlayEvent", "detachTransmitPlayEvent"},
    {"attachUniversalSpeedRuleChangeEvent",
     "detachUniversalSpeedRuleChangeEvent"},
    {"attachPlayListPreparedEvent", "detachPlayListPreparedEvent"},
    {"attachPlayListSelectChangeEvent", "detachPlayListSelectChangeEvent"},
    {"attachPlayListItemDeleteEvent", "detachPlayListItemDeleteEvent"},
    {"attachPlayListItemAddEvent", "detachPlayListItemAddEvent"},
    {"attachPreparedEvent", "detachPreparedEvent"},
    {"attachItemUpdate", "detachItemUpdate"},
    {"attachItemDeleteEvent", "detachItemDeleteEvent"},
    {"attachSelectChangeEvent", "detachSelectChangeEvent"},
    {"attachManualAdd", "detachManualAdd"},
    {"AttachDeviceListChangeEvent", "DetachDeviceListChangeEvent"},
    {"AttachDeliverStartedChangeEvent", "DetachDeliverStartedChangeEvent"},
    {"AttachDeliverStopChangeEvent", "DetachDeliverStopChangeEvent"},
    {"AttachDlnaTypeChangeEvent", "DetachDlnaTypeChangeEvent"},
    {"AttachDlnaSupportTypeChangeEvent", "DetachDlnaSupportTypeChangeEvent"},
    {"AttachDeliverFailedEvent", "DetachDeliverFailedEvent"},
    {"AttachDeliverDeviceChangeEvent", "DetachDeliverDeviceChangeEvent"},
};
constexpr std::string_view kIdentityKeys[] = {
    "transportAppName", "transportAppKey", "transportConfigPath",
    "panPlayCache",     "dbDir",           "dbName"};
constexpr std::string_view kExports[] = {"initAddon",
                                         "unInitAddon",
                                         "setGlobalHttpRequest",
                                         "addStatCallback",
                                         "registerPanApi",
                                         "updateUserInfo",
                                         "registerUniversalApi",
                                         "registerMediaStorageOnlineApi",
                                         "setPlayableExt",
                                         "NativeAplayerStack"};
// View geometry belongs to the Browser's Cocoa attachment, not a remote JS
// subscription that could outlive that NSView.
constexpr std::string_view kExcludedInstanceMethods[] = {
    "attachPlayViewSizeChangeEvent", "attachPlayViewVisibleChangeEvent"};

}  // namespace

const MacPlayerAdapter& XunleiMacPlayerAdapterV1() {
  static const base::NoDestructor<MacPlayerAdapter> adapter(MacPlayerAdapter{
      .id = "xunlei.mac-player",
      .version = 1,
      .module_relative_path = "app/playercontrol.node",
      .initialize_export = "initAddon",
      .shutdown_export = "unInitAddon",
      .root_constructor = "NativeAplayerStack",
      .bind_view_export = "setWnd",
      .close_media_method = "closeMedia",
      .close_view_method = "closePlayWindow",
      .initialization_identity_keys = kIdentityKeys,
      .allowed_exports = kExports,
      .excluded_instance_methods = kExcludedInstanceMethods,
      .subscriptions = kSubscriptions,
      .callbacks = kContracts,
  });
  return *adapter;
}

}  // namespace xenon
