// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_desktop_capturer.h"

#include <atomic>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "build/build_config.h"
#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE) && !BUILDFLAG(IS_ANDROID) && \
    !BUILDFLAG(IS_CHROMEOS)

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/timer/timer.h"
#include "chrome/browser/media/webrtc/desktop_capturer_wrapper.h"
#include "chrome/browser/media/webrtc/desktop_media_list_observer.h"
#include "chrome/browser/media/webrtc/native_desktop_media_list.h"
#include "chrome/browser/media/webrtc/window_icon_util.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/desktop_capture.h"
#include "ui/gfx/image/image.h"

#if BUILDFLAG(IS_MAC)
#include "ui/base/cocoa/permissions_utils.h"
#endif

namespace xenon::ipc {
namespace {

using Reply = base::OnceCallback<void(base::Value, std::string)>;

// DesktopMediaList normally represents native enumeration failures as an empty
// list. Preserve the capturer's real result for the Promise API instead.
class CheckedThumbnailCapturer final : public ThumbnailCapturer {
 public:
  CheckedThumbnailCapturer(std::unique_ptr<webrtc::DesktopCapturer> capturer,
                           std::shared_ptr<std::atomic<bool>> failed)
      : wrapped_(std::move(capturer)), failed_(std::move(failed)) {}
  void Start(Consumer* consumer) override { wrapped_.Start(consumer); }
  FrameDeliveryMethod GetFrameDeliveryMethod() const override {
    return wrapped_.GetFrameDeliveryMethod();
  }
  void SetMaxFrameRate(uint32_t rate) override {
    wrapped_.SetMaxFrameRate(rate);
  }
  webrtc::DelegatedSourceListController* GetDelegatedSourceListController()
      override {
    return wrapped_.GetDelegatedSourceListController();
  }
  bool GetSourceList(SourceList* sources) override {
    const bool success = wrapped_.GetSourceList(sources);
    failed_->store(!success, std::memory_order_relaxed);
    return success;
  }
  bool SelectSource(SourceId id) override { return wrapped_.SelectSource(id); }
  void CaptureFrame() override { wrapped_.CaptureFrame(); }

 private:
  DesktopCapturerWrapper wrapped_;
  std::shared_ptr<std::atomic<bool>> failed_;
};

std::string EncodeImage(const gfx::ImageSkia& image) {
  if (image.isNull()) {
    return {};
  }
  const auto bytes = gfx::Image(image).As1xPNGBytes();
  return bytes ? base::Base64Encode(base::span(*bytes)) : std::string();
}

class DesktopRequest {
 public:
  explicit DesktopRequest(base::OnceClosure done) : done_(std::move(done)) {}

  bool Start(const base::DictValue& options, Reply reply) {
    reply_ = std::move(reply);
    auto* types = options.FindList("types");
    const auto width = options.FindInt("width");
    const auto height = options.FindInt("height");
    if (!types || !width || !height || *width < 0 || *height < 0 ||
        *width > 4096 || *height > 4096) {
      Finish({}, "ERR_INVALID_ARG_VALUE: invalid desktop capture options");
      return false;
    }
    icons_ = options.FindBool("fetchWindowIcons").value_or(false);
    gfx::Size size(*width, *height);
    bool screen = false, window = false;
    for (const auto& type : *types) {
      if (type.is_string() && type.GetString() == "screen") {
        screen = true;
      } else if (type.is_string() && type.GetString() == "window") {
        window = true;
      } else {
        Finish({}, "ERR_INVALID_ARG_VALUE: unknown desktop source type");
        return false;
      }
    }
    if (!screen && !window) {
      Finish(base::Value(base::ListValue()), "");
      return false;
    }
#if BUILDFLAG(IS_MAC)
    if (!ui::TryPromptUserForScreenCapture()) {
      Finish({}, "ERR_ACCESS_DENIED: screen capture permission was denied");
      return false;
    }
#endif
    // PipeWire can provide one native picker for both source types.
    if (screen && window) {
      auto capturer = webrtc::DesktopCapturer::CreateGenericCapturer(
          content::desktop_capture::CreateDesktopCaptureOptions());
      if (capturer && capturer->GetDelegatedSourceListController()) {
        Add(DesktopMediaList::Type::kWindow, std::move(capturer), size);
        screen = window = false;
      }
    }
    if (window &&
        !Add(DesktopMediaList::Type::kWindow,
             content::desktop_capture::CreateWindowCapturer(
                 content::desktop_capture::CreateDesktopCaptureOptions()),
             size)) {
      Finish({}, "ERR_NOT_SUPPORTED: window capture is unavailable");
      return false;
    }
    if (screen &&
        !Add(DesktopMediaList::Type::kScreen,
             content::desktop_capture::CreateScreenCapturer(
                 content::desktop_capture::CreateDesktopCaptureOptions(),
                 /*for_snapshot=*/true),
             size)) {
      Finish({}, "ERR_NOT_SUPPORTED: screen capture is unavailable");
      return false;
    }
    remaining_ = slots_.size();
    timeout_.Start(
        FROM_HERE, base::Seconds(60),
        base::BindOnce(&DesktopRequest::TimedOut, weak_factory_.GetWeakPtr()));
    for (size_t i = 0; i < slots_.size(); ++i) {
      auto& slot = slots_[i];
      if (slot->list->IsSourceListDelegated()) {
        slot->list->StartUpdating(slot.get());
        slot->list->ShowDelegatedList();
      } else {
        slot->list->UpdateWithThumbnails(
            base::BindPostTaskToCurrentDefault(base::BindOnce(
                &DesktopRequest::Ready, weak_factory_.GetWeakPtr(), i)));
      }
    }
    return true;
  }

  void Cancel() { Finish({}, "ERR_ABORTED: desktop capture owner stopped"); }

 private:
  struct Slot final : DesktopMediaListObserver {
    Slot(base::WeakPtr<DesktopRequest> request,
         size_t index,
         bool skip_thumbnails)
        : request(std::move(request)),
          index(index),
          thumbnail(skip_thumbnails) {}
    void OnSourceAdded(int) override { MaybeReady(); }
    void OnSourceRemoved(int) override {}
    void OnSourceMoved(int, int) override {}
    void OnSourceNameChanged(int) override {}
    void OnSourcePreviewChanged(size_t) override {}
    void OnSourceThumbnailChanged(int) override {
      thumbnail = true;
      MaybeReady();
    }
    void OnDelegatedSourceListSelection() override {
      selected = true;
      MaybeReady();
    }
    void OnDelegatedSourceListDismissed() override {
      base::BindPostTaskToCurrentDefault(
          base::BindOnce(&DesktopRequest::Cancel, request))
          .Run();
    }
    void MaybeReady() {
      if (!selected || !thumbnail || posted || !list->GetSourceCount()) {
        return;
      }
      posted = true;
      base::BindPostTaskToCurrentDefault(
          base::BindOnce(&DesktopRequest::Ready, request, index))
          .Run();
    }
    base::WeakPtr<DesktopRequest> request;
    size_t index;
    bool selected = false;
    bool thumbnail = false;
    bool posted = false;
    bool ready = false;
    std::shared_ptr<std::atomic<bool>> enumeration_failed =
        std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<NativeDesktopMediaList> list;
  };

  bool Add(DesktopMediaList::Type type,
           std::unique_ptr<webrtc::DesktopCapturer> capturer,
           gfx::Size size) {
    if (!capturer) {
      return false;
    }
    auto slot = std::make_unique<Slot>(weak_factory_.GetWeakPtr(),
                                       slots_.size(), size.IsEmpty());
    slot->list = std::make_unique<NativeDesktopMediaList>(
        type,
        std::make_unique<CheckedThumbnailCapturer>(std::move(capturer),
                                                   slot->enumeration_failed),
        type == DesktopMediaList::Type::kWindow &&
            !content::desktop_capture::ShouldEnumerateCurrentProcessWindows(),
        /*auto_show_delegated_source_list=*/false);
    slot->list->SetThumbnailSize(size);
    slots_.push_back(std::move(slot));
    return true;
  }

  void Ready(size_t index) {
    if (!reply_ || slots_[index]->ready) {
      return;
    }
    if (slots_[index]->enumeration_failed->load(std::memory_order_relaxed)) {
      Finish({},
             "ERR_OPERATION_FAILED: native desktop source enumeration failed");
      return;
    }
    if (slots_[index]->list->IsSourceListDelegated() &&
        !slots_[index]->list->GetSourceCount()) {
      slots_[index]->posted = false;
      return;
    }
    slots_[index]->ready = true;
    if (--remaining_) {
      return;
    }
    base::ListValue sources;
    for (const auto& slot : slots_) {
      const auto& list = *slot->list;
      for (int i = 0; i < list.GetSourceCount(); ++i) {
        const auto& source = list.GetSource(i);
        std::string display_id;
#if BUILDFLAG(IS_MAC)
        if (source.id.type == content::DesktopMediaID::TYPE_SCREEN &&
            source.id.id_type ==
                content::DesktopMediaID::IdType::kPlatformNative) {
          display_id = base::NumberToString(source.id.id);
        }
#endif
        auto value = base::DictValue()
                         .Set("id", source.id.ToString())
                         .Set("name", base::UTF16ToUTF8(source.name))
                         .Set("display_id", display_id)
                         .Set("thumbnail", EncodeImage(source.thumbnail));
        if (icons_ && source.id.type == content::DesktopMediaID::TYPE_WINDOW &&
            source.id.id_type ==
                content::DesktopMediaID::IdType::kPlatformNative) {
          value.Set("appIcon", EncodeImage(GetWindowIcon(source.id)));
        } else {
          value.Set("appIcon", base::Value());
        }
        sources.Append(std::move(value));
      }
    }
    Finish(base::Value(std::move(sources)), "");
  }

  void TimedOut() {
    Finish({}, "ERR_OPERATION_FAILED: desktop capture timed out");
  }
  void Finish(base::Value result, std::string error) {
    if (!reply_) {
      return;
    }
    timeout_.Stop();
    weak_factory_.InvalidateWeakPtrs();
    slots_.clear();
    auto reply = std::move(reply_);
    auto done = std::move(done_);
    std::move(reply).Run(std::move(result), std::move(error));
    // Deletion is posted: callers and native enumeration callbacks may still
    // be unwinding their stacks. Cancelled owners have already erased entries.
    std::move(done).Run();
  }

  Reply reply_;
  base::OnceClosure done_;
  std::vector<std::unique_ptr<Slot>> slots_;
  size_t remaining_ = 0;
  bool icons_ = false;
  base::OneShotTimer timeout_;
  base::WeakPtrFactory<DesktopRequest> weak_factory_{this};
};

class DesktopBroker {
 public:
  void Get(const std::string& owner, const base::Value& options, Reply reply) {
    if (!options.is_dict()) {
      std::move(reply).Run(
          {}, "ERR_INVALID_ARG_TYPE: capture options must be an object");
      return;
    }
    if (requests_.size() >= 32) {
      std::move(reply).Run({},
                           "ERR_RESOURCE_BUSY: desktop capture queue is full");
      return;
    }
    const uint64_t id = ++next_id_;
    auto request =
        std::make_unique<DesktopRequest>(base::BindPostTaskToCurrentDefault(
            base::BindOnce(&DesktopBroker::Erase, base::Unretained(this), id)));
    auto* raw = request.get();
    requests_.emplace(id, Entry{owner, std::move(request)});
    raw->Start(options.GetDict(), std::move(reply));
  }
  void Close(const std::string& owner) {
    std::vector<std::unique_ptr<DesktopRequest>> retiring;
    for (auto it = requests_.begin(); it != requests_.end();) {
      if (it->second.owner == owner) {
        retiring.push_back(std::move(it->second.request));
        it = requests_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& request : retiring) {
      request->Cancel();
    }
  }
  static DesktopBroker& GetInstance() {
    static base::NoDestructor<DesktopBroker> instance;
    return *instance;
  }

 private:
  struct Entry {
    std::string owner;
    std::unique_ptr<DesktopRequest> request;
  };
  void Erase(uint64_t id) { requests_.erase(id); }
  std::map<uint64_t, Entry> requests_;
  uint64_t next_id_ = 0;
};

}  // namespace

void GetElectronDesktopSources(const std::string& owner,
                               const base::Value& options,
                               Reply reply) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DesktopBroker::GetInstance().Get(owner, options, std::move(reply));
}

void CloseElectronDesktopSources(const std::string& owner) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DesktopBroker::GetInstance().Close(owner);
}

}  // namespace xenon::ipc

#else

namespace xenon::ipc {
void GetElectronDesktopSources(
    const std::string&,
    const base::Value&,
    base::OnceCallback<void(base::Value, std::string)> reply) {
  std::move(reply).Run(
      {}, "ERR_NOT_SUPPORTED: Electron desktop capture is unavailable");
}
void CloseElectronDesktopSources(const std::string&) {}
}  // namespace xenon::ipc

#endif
