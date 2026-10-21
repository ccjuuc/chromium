// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/updater/xenon_system_updater.h"

#include <optional>

#include "base/apple/bundle_locations.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/timer/timer.h"
#include "base/version.h"
#include "chrome/updater/service_proxy_factory.h"
#include "chrome/updater/update_service.h"
#include "chrome/updater/updater_branding.h"
#include "chrome/updater/updater_scope.h"

namespace xenon::updater {
namespace {
class SystemUpdater final : public XenonSystemUpdater {
 public:
  std::string GetFeedURL() const override { return UPDATE_CHECK_URL; }
  void Start(bool check_only, Observer observer) override {
    if (busy_) {
      return;
    }
    weak_factory_.InvalidateWeakPtrs();
    check_only_ = check_only;
    available_.reset();
    observer_ = std::move(observer);
    terminal_ = false;
    // A home-directory copy must never report a successful update of the
    // registered system application while continuing to run its old version.
    if (base::apple::OuterBundlePath() !=
        base::FilePath(FILE_PATH_LITERAL("/Applications/xl153.app"))) {
      Fail("请从 /Applications/xl153.app 启动系统安装的浏览器");
      return;
    }
    busy_ = true;
    ArmWatchdog(base::Minutes(2));
    service_ = ::updater::CreateUpdateServiceProxy(
        ::updater::UpdaterScope::kSystem, base::Seconds(10));
    service_->GetVersion(base::BindOnce(
        &SystemUpdater::OnVersion, weak_factory_.GetWeakPtr(), check_only));
  }

 private:
  using Service = ::updater::UpdateService;
  void ArmWatchdog(base::TimeDelta timeout) {
    watchdog_.Start(
        FROM_HERE, timeout,
        base::BindOnce(
            &SystemUpdater::Fail, weak_factory_.GetWeakPtr(),
            "等待系统更新服务超时，任务可能仍在后台运行，请稍后重新检查"));
  }
  void Fail(std::string error) {
    watchdog_.Stop();
    weak_factory_.InvalidateWeakPtrs();
    busy_ = false;
    terminal_ = true;
    Event event;
    event.phase = Event::Phase::kError;
    event.error = std::move(error);
    observer_.Run(event);
  }
  void OnVersion(bool check_only, const base::Version& version) {
    if (!version.IsValid()) {
      Fail("无法连接系统级 Xenon Updater，请重新安装包含更新服务的 PKG");
      return;
    }
    auto state = base::BindRepeating(&SystemUpdater::OnState,
                                     weak_factory_.GetWeakPtr());
    auto done = base::BindOnce(&SystemUpdater::OnDone,
                               weak_factory_.GetWeakPtr(), check_only);
    if (check_only) {
      // Compatibility with Electron autoDownload=false requires the existing
      // query-only RPC. Update() would also start installation.
      service_->CheckForUpdate(BROWSER_APPID, Service::Priority::kForeground,
                               Service::PolicySameVersionUpdate::kNotAllowed,
                               "", state, std::move(done));
    } else {
      service_->Update(BROWSER_APPID, "", Service::Priority::kForeground,
                       Service::PolicySameVersionUpdate::kNotAllowed, "", state,
                       std::move(done));
    }
  }
  void OnState(const Service::UpdateState& state) {
    using State = Service::UpdateState::State;
    ArmWatchdog(state.state == State::kInstalling ? base::Minutes(15)
                                                  : base::Minutes(2));
    Event event;
    event.version = state.next_version;
    event.downloaded = state.downloaded_bytes;
    event.total = state.total_bytes;
    switch (state.state) {
      case State::kUnknown:
      case State::kNotStarted:
      case State::kCheckingForUpdates:
        event.phase = Event::Phase::kChecking;
        break;
      case State::kUpdateAvailable:
        event.phase = Event::Phase::kAvailable;
        break;
      case State::kDownloading:
        event.phase = Event::Phase::kDownloading;
        break;
      case State::kDecompressing:
      case State::kPatching:
        event.phase = Event::Phase::kPreparing;
        break;
      case State::kInstalling:
        event.phase = Event::Phase::kAwaitingExit;
        break;
      case State::kUpdated:
        event.phase = Event::Phase::kInstalled;
        terminal_ = true;
        break;
      case State::kNoUpdate:
        event.phase = Event::Phase::kNoUpdate;
        terminal_ = true;
        break;
      case State::kUpdateError:
        Fail("Xenon Updater 更新失败（类别 " +
             base::NumberToString(static_cast<int>(state.error_category)) +
             "，代码 " + base::NumberToString(state.error_code) +
             "，附加代码 " + base::NumberToString(state.extra_code1) + ") " +
             state.installer_text);
        return;
    }
    if (check_only_ && event.phase == Event::Phase::kAvailable) {
      available_ = event;
      return;
    }
    observer_.Run(event);
  }
  void OnDone(bool check_only, Service::Result result) {
    watchdog_.Stop();
    busy_ = false;
    if (result == Service::Result::kSuccess && check_only && available_) {
      Event event = std::move(*available_);
      available_.reset();
      observer_.Run(event);
      return;
    }
    if (result != Service::Result::kSuccess && !terminal_) {
      Fail("Xenon Updater 请求失败，代码 " +
           base::NumberToString(static_cast<int>(result)));
    } else if (!check_only && !terminal_) {
      // RPC completion alone is not proof that an installation succeeded.
      Fail("Xenon Updater 未返回最终安装结果，请重新检查更新");
    }
  }
  base::OneShotTimer watchdog_;
  bool check_only_ = false;
  std::optional<Event> available_;
  bool busy_ = false;
  bool terminal_ = false;
  Observer observer_;
  scoped_refptr<Service> service_;
  base::WeakPtrFactory<SystemUpdater> weak_factory_{this};
};
}  // namespace
std::unique_ptr<XenonSystemUpdater> CreateXenonSystemUpdater() {
  return std::make_unique<SystemUpdater>();
}
}  // namespace xenon::updater
