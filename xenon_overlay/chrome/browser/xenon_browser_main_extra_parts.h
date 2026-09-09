#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_BROWSER_MAIN_EXTRA_PARTS_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_BROWSER_MAIN_EXTRA_PARTS_H_

#include <memory>

#include "chrome/browser/chrome_browser_main_extra_parts.h"

namespace xenon {
class XenonReminderBrowserObserver;
}

// Extra parts for Xenon Overlays's browser main loop initialization.
class XenonBrowserMainExtraParts : public ChromeBrowserMainExtraParts {
 public:
  XenonBrowserMainExtraParts();
  XenonBrowserMainExtraParts(const XenonBrowserMainExtraParts&) = delete;
  XenonBrowserMainExtraParts& operator=(const XenonBrowserMainExtraParts&) = delete;
  ~XenonBrowserMainExtraParts() override;

  // ChromeBrowserMainExtraParts:
  void PostEarlyInitialization() override;
  void PreProfileInit() override;
  void PostProfileInit(Profile* profile, bool is_initial_profile) override;
  void PostMainMessageLoopRun() override;

 private:
  bool use_embedded_ipc_test_main_ = false;
  std::unique_ptr<xenon::XenonReminderBrowserObserver> reminder_browser_observer_;
};

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_BROWSER_MAIN_EXTRA_PARTS_H_
