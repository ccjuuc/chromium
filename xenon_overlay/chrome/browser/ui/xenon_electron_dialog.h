// Copyright 2026 The Xenon Authors. All rights reserved.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_DIALOG_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_DIALOG_H_

#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace views {
class Widget;
}

namespace xenon {

using ElectronDialogCallback =
    base::OnceCallback<void(base::Value result, std::string error)>;

// UI-thread only. The caller must validate that |owner| belongs to the live
// container generation represented by |owner_token|. Replies are always posted,
// including validation failures, and represent actual user choices only.
void ShowElectronMessageBox(const std::string& owner_token,
                            views::Widget* owner,
                            const base::DictValue& options,
                            ElectronDialogCallback callback);
void ShowElectronSaveDialog(const std::string& owner_token,
                            views::Widget* owner,
                            const base::DictValue& options,
                            ElectronDialogCallback callback);
void ShowElectronOpenDialog(const std::string& owner_token,
                            views::Widget* owner,
                            const base::DictValue& options,
                            ElectronDialogCallback callback);

// Completes outstanding requests with ERR_ABORTED and closes Views dialogs.
// SelectFileDialog has no portable close API: its listener is detached so a
// late native picker reply cannot reach a terminated container generation.
void CancelElectronDialogs(const std::string& owner_token);

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_ELECTRON_DIALOG_H_
