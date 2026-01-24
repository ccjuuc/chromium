#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_BROWSER_MAIN_EXTRA_PARTS_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_BROWSER_MAIN_EXTRA_PARTS_H_

#include "chrome/browser/chrome_browser_main_extra_parts.h"

// Extra parts for Xenon Overlays's browser main loop initialization.
class XenonBrowserMainExtraParts : public ChromeBrowserMainExtraParts {
 public:
  XenonBrowserMainExtraParts();
  XenonBrowserMainExtraParts(const XenonBrowserMainExtraParts&) = delete;
  XenonBrowserMainExtraParts& operator=(const XenonBrowserMainExtraParts&) = delete;
  ~XenonBrowserMainExtraParts() override;

  // ChromeBrowserMainExtraParts:
  void PostProfileInit(Profile* profile, bool is_initial_profile) override;
};

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_BROWSER_MAIN_EXTRA_PARTS_H_
