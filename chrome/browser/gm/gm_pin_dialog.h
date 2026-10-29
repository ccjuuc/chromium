// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GM_GM_PIN_DIALOG_H_
#define CHROME_BROWSER_GM_GM_PIN_DIALOG_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "services/gm_key/public/cpp/secure_pin.h"

namespace views {
class Widget;
}
namespace content {
class WebContents;
}
namespace gm_browser {
using PinCallback =
    base::OnceCallback<void(size_t, std::unique_ptr<gm_key::SecurePin>)>;

// Browser-owned UI modal to the initiating page. Closing/cancelling, or having
// no usable page dialog host, replies with a null PIN.
base::WeakPtr<views::Widget> ShowPinDialogForWebContents(
    content::WebContents* web_contents,
    const std::string& origin,
    const std::vector<std::u16string>& identities,
    const std::u16string& message,
    PinCallback callback);

// Informational page-modal notice. It offers no PIN entry or retry. The caller
// can keep the failed request pending until on_close runs, so its error-page
// navigation does not immediately dismiss the notice.
base::WeakPtr<views::Widget> ShowPinLockedDialogForWebContents(
    content::WebContents* web_contents,
    const std::string& origin,
    base::OnceClosure on_close);

// Standalone dialog for focused UI tests only. Production requests must use
// ShowPinDialogForWebContents so that the page owns the interaction.
base::WeakPtr<views::Widget> ShowPinDialog(
    const std::string& origin,
    const std::vector<std::u16string>& identities,
    const std::u16string& message,
    PinCallback callback);
}  // namespace gm_browser

#endif  // CHROME_BROWSER_GM_GM_PIN_DIALOG_H_
