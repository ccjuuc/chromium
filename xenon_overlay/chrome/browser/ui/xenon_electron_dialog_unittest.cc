// Copyright 2026 The Xenon Authors. All rights reserved.

#include "xenon_overlay/chrome/browser/ui/xenon_electron_dialog.h"

#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/shell_dialogs/fake_select_file_dialog.h"
#include "ui/shell_dialogs/select_file_dialog_factory.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/message_box_view.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/test/button_test_api.h"
#include "ui/views/test/views_test_base.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/window/dialog_client_view.h"
#include "ui/views/window/dialog_delegate.h"

namespace xenon {
namespace {

using DialogFuture = base::test::TestFuture<base::Value, std::string>;
constexpr char kOwner[] = "dialog-test#1";

template <typename T>
void FindViews(views::View* view, std::vector<T*>* matches) {
  if (auto* match = views::AsViewClass<T>(view)) {
    matches->push_back(match);
  }
  for (const auto& child : view->children()) {
    FindViews(child.get(), matches);
  }
}

base::DictValue MessageOptions() {
  return base::DictValue()
      .Set("title", "Dialog contract")
      .Set("message", "Unsaved document")
      .Set(
          "buttons",
          base::ListValue().Append("&Save").Append("&Discard").Append("Cancel"))
      .Set("defaultId", 0)
      .Set("cancelId", 2)
      .Set("normalizeAccessKeys", true)
      .Set("checkboxLabel", "Remember")
      .Set("checkboxChecked", true);
}

base::DictValue SaveOptions() {
  return base::DictValue()
      .Set("title", "Save document")
      .Set("defaultPath", "draft.drawio")
      .Set(
          "filters",
          base::ListValue()
              .Append(
                  base::DictValue()
                      .Set("name", "Diagram")
                      .Set("extensions",
                           base::ListValue().Append("drawio").Append("xml")))
              .Append(base::DictValue()
                          .Set("name", "Image")
                          .Set("extensions", base::ListValue().Append("png"))));
}

// Records the arguments absent from Chromium's generic fake and deliberately
// permits late replies, so listener detachment can be checked without native
// UI.
class RecordingFileDialog final : public ui::SelectFileDialog {
 public:
  explicit RecordingFileDialog(Listener* listener)
      : SelectFileDialog(listener, nullptr) {}

  void SelectFileImpl(Type selected_type,
                      const std::u16string& selected_title,
                      const base::FilePath& selected_path,
                      const FileTypeInfo* selected_types,
                      int file_type_index,
                      const base::FilePath::StringType& default_extension,
                      gfx::NativeWindow selected_owner,
                      const GURL* caller) override {
    type = selected_type;
    title = selected_title;
    default_path = selected_path;
    owner = selected_owner;
    if (selected_types) {
      file_types = *selected_types;
    }
  }
  bool IsRunning(gfx::NativeWindow) const override { return !!listener_; }
  void ListenerDestroyed() override { listener_ = nullptr; }
  bool HasMultipleFileTypeChoicesImpl() override { return true; }
  bool Reply(const std::vector<base::FilePath>& paths) {
    if (!listener_) {
      return false;
    }
    listener_->MultiFilesSelected(
        ui::FilePathListToSelectedFileInfoList(paths));
    return true;
  }

  Type type = SELECT_NONE;
  std::u16string title;
  base::FilePath default_path;
  gfx::NativeWindow owner = gfx::NativeWindow();
  FileTypeInfo file_types;

 private:
  ~RecordingFileDialog() override = default;
};

class RecordingFileDialogFactory final : public ui::SelectFileDialogFactory {
 public:
  ui::SelectFileDialog* Create(ui::SelectFileDialog::Listener* listener,
                               std::unique_ptr<ui::SelectFilePolicy>) override {
    last_dialog = base::MakeRefCounted<RecordingFileDialog>(listener);
    return last_dialog.get();
  }
  scoped_refptr<RecordingFileDialog> last_dialog;
};

class ElectronDialogTest : public views::ViewsTestBase {
 public:
  void SetUp() override {
    views::ViewsTestBase::SetUp();
    owner_ = CreateTestWidget(views::Widget::InitParams::CLIENT_OWNS_WIDGET);
    owner_->Show();
    factory_ = ui::FakeSelectFileDialog::RegisterFactory();
    factory_->SetOpenCallback(base::DoNothing());
  }

  void TearDown() override {
    CancelElectronDialogs(kOwner);
    CancelElectronDialogs("dialog-test#2");
    RunPendingMessages();
    owner_.reset();
    factory_ = nullptr;
    ui::SelectFileDialog::SetFactory(nullptr);
    views::ViewsTestBase::TearDown();
  }

 protected:
  views::Widget* MessageWidget() {
    for (const auto& widget :
         views::Widget::GetAllOwnedWidgets(owner_->GetNativeView())) {
      if (widget != owner_.get() && widget->widget_delegate() &&
          widget->widget_delegate()->GetWindowTitle() == u"Dialog contract") {
        return widget.get();
      }
    }
    return nullptr;
  }

  std::unique_ptr<views::Widget> owner_;
  raw_ptr<ui::FakeSelectFileDialog::Factory> factory_ = nullptr;
};

TEST_F(ElectronDialogTest, ArbitraryButtonReturnsOriginalIndexAndCheckbox) {
  DialogFuture future;
  ShowElectronMessageBox(kOwner, owner_.get(), MessageOptions(),
                         future.GetCallback());
  auto* widget = MessageWidget();
  ASSERT_TRUE(widget);
  std::vector<views::MdTextButton*> buttons;
  FindViews(widget->GetContentsView(), &buttons);
  ASSERT_EQ(buttons.size(), 3u);
  EXPECT_EQ(buttons[0]->GetText(), u"Save");
  EXPECT_TRUE(buttons[0]->GetIsDefault());
  EXPECT_EQ(buttons[1]->GetText(), u"Discard");
  views::test::ButtonTestApi(buttons[1]).NotifyDefaultMouseClick();
  ASSERT_TRUE(future.Wait());
  EXPECT_TRUE(future.Get<1>().empty());
  EXPECT_EQ(future.Get<0>().GetDict().FindInt("response"), 1);
  EXPECT_EQ(future.Get<0>().GetDict().FindBool("checkboxChecked"), true);
  EXPECT_FALSE(MessageWidget());
}

TEST_F(ElectronDialogTest, EscapeReturnsCancelRatherThanDefaultButton) {
  DialogFuture future;
  ShowElectronMessageBox(kOwner, owner_.get(), MessageOptions(),
                         future.GetCallback());
  auto* widget = MessageWidget();
  ASSERT_TRUE(widget);
  auto* delegate = widget->widget_delegate()->AsDialogDelegate();
  ASSERT_TRUE(delegate);
  EXPECT_TRUE(delegate->GetDialogClientView()->AcceleratorPressed(
      ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE)));
  ASSERT_TRUE(future.Wait());
  EXPECT_TRUE(future.Get<1>().empty());
  EXPECT_EQ(future.Get<0>().GetDict().FindInt("response"), 2);
}

TEST_F(ElectronDialogTest, EnterSelectsConfiguredDefaultButton) {
  auto options = MessageOptions();
  options.Set("defaultId", 1);
  DialogFuture future;
  ShowElectronMessageBox(kOwner, owner_.get(), options, future.GetCallback());
  auto* widget = MessageWidget();
  ASSERT_TRUE(widget);
  EXPECT_TRUE(widget->GetFocusManager()->ProcessAccelerator(
      ui::Accelerator(ui::VKEY_RETURN, ui::EF_NONE)));
  ASSERT_TRUE(future.Wait());
  EXPECT_EQ(future.Get<0>().GetDict().FindInt("response"), 1);
}

TEST_F(ElectronDialogTest, NormalizeAccessKeysPreservesUnicodeAndEscapes) {
  auto options = MessageOptions();
  options.Set(
      "buttons",
      base::ListValue().Append("保存(&S)").Append("A&&B").Append("Tail&"));
  DialogFuture future;
  ShowElectronMessageBox(kOwner, owner_.get(), options, future.GetCallback());
  auto* widget = MessageWidget();
  ASSERT_TRUE(widget);
  std::vector<views::MdTextButton*> buttons;
  FindViews(widget->GetContentsView(), &buttons);
  ASSERT_EQ(buttons.size(), 3u);
  EXPECT_EQ(buttons[0]->GetText(), u"保存(S)");
  EXPECT_EQ(buttons[1]->GetText(), u"A&B");
  EXPECT_EQ(buttons[2]->GetText(), u"Tail");
  views::test::ButtonTestApi(buttons[2]).NotifyDefaultMouseClick();
  ASSERT_TRUE(future.Wait());
  EXPECT_EQ(future.Get<0>().GetDict().FindInt("response"), 2);
}

TEST_F(ElectronDialogTest, ExplicitNegativeCancelAndNativeDefault) {
  auto options = MessageOptions();
  options.Set("defaultId", -1);
  options.Set("cancelId", -1);
  DialogFuture future;
  ShowElectronMessageBox(kOwner, owner_.get(), options, future.GetCallback());
  auto* widget = MessageWidget();
  ASSERT_TRUE(widget);
  widget->CloseWithReason(views::Widget::ClosedReason::kCloseButtonClicked);
  ASSERT_TRUE(future.Wait());
  EXPECT_EQ(future.Get<0>().GetDict().FindInt("response"), -1);
}

TEST_F(ElectronDialogTest, DestroyingOwnerAbortsMessageOnce) {
  int replies = 0;
  std::string error;
  ShowElectronMessageBox(
      kOwner, owner_.get(), MessageOptions(),
      base::BindLambdaForTesting([&](base::Value result, std::string failure) {
        ++replies;
        error = std::move(failure);
      }));
  owner_.reset();
  CancelElectronDialogs(kOwner);
  RunPendingMessages();
  EXPECT_EQ(replies, 1);
  EXPECT_TRUE(error.starts_with("ERR_ABORTED:"));
}

TEST_F(ElectronDialogTest, SavePreservesGroupedFiltersAndChosenPath) {
  DialogFuture future;
  ShowElectronSaveDialog(kOwner, owner_.get(), SaveOptions(),
                         future.GetCallback());
  scoped_refptr<ui::FakeSelectFileDialog> dialog = factory_->GetLastDialog();
  ASSERT_TRUE(dialog);
  EXPECT_EQ(dialog->title(), u"Save document");
  ASSERT_EQ(dialog->file_types().extensions.size(), 2u);
  EXPECT_EQ(dialog->file_types().extensions[0].size(), 2u);
  EXPECT_EQ(dialog->file_types().extension_description_overrides[0],
            u"Diagram");
  const base::FilePath path(FILE_PATH_LITERAL("selected.drawio"));
  ASSERT_TRUE(dialog->CallFileSelected(path, "drawio"));
  ASSERT_TRUE(future.Wait());
  EXPECT_TRUE(future.Get<1>().empty());
  EXPECT_EQ(future.Get<0>().GetDict().FindBool("canceled"), false);
  EXPECT_EQ(*future.Get<0>().GetDict().FindString("filePath"),
            path.AsUTF8Unsafe());
}

TEST_F(ElectronDialogTest, NativePickerMayCancelSynchronously) {
  factory_->SetOpenCallback(base::BindLambdaForTesting(
      [&] { factory_->GetLastDialog()->CallFileSelectionCanceled(); }));
  DialogFuture future;
  ShowElectronSaveDialog(kOwner, owner_.get(), SaveOptions(),
                         future.GetCallback());
  EXPECT_FALSE(future.IsReady());
  ASSERT_TRUE(future.Wait());
  EXPECT_TRUE(future.Get<1>().empty());
  EXPECT_EQ(future.Get<0>().GetDict().FindBool("canceled"), true);
  EXPECT_EQ(*future.Get<0>().GetDict().FindString("filePath"), "");
}

TEST_F(ElectronDialogTest, SaveOwnerAndGenerationCancellationReplyOnce) {
  int replies = 0;
  std::string error;
  ShowElectronSaveDialog(
      kOwner, owner_.get(), SaveOptions(),
      base::BindLambdaForTesting([&](base::Value result, std::string failure) {
        ++replies;
        error = std::move(failure);
      }));
  CancelElectronDialogs("dialog-test#2");
  RunPendingMessages();
  EXPECT_EQ(replies, 0);
  owner_.reset();
  CancelElectronDialogs(kOwner);
  RunPendingMessages();
  EXPECT_EQ(replies, 1);
  EXPECT_TRUE(error.starts_with("ERR_ABORTED:"));
}

TEST_F(ElectronDialogTest, UnsupportedSaveOptionIsAnErrorNotCancellation) {
  auto options = SaveOptions();
  options.Set("properties", base::ListValue().Append("showHiddenFiles"));
  DialogFuture future;
  ShowElectronSaveDialog(kOwner, owner_.get(), options, future.GetCallback());
  EXPECT_FALSE(future.IsReady());
  ASSERT_TRUE(future.Wait());
  EXPECT_TRUE(future.Get<0>().is_none());
  EXPECT_TRUE(future.Get<1>().starts_with("ERR_NOT_SUPPORTED:"));
  EXPECT_FALSE(factory_->GetLastDialog());
}

TEST_F(ElectronDialogTest,
       OpenPreservesOwnerDefaultPathFiltersAndMultiplePaths) {
  auto recording = std::make_unique<RecordingFileDialogFactory>();
  auto* factory = recording.get();
  factory_ = nullptr;
  ui::SelectFileDialog::SetFactory(std::move(recording));
  auto options = SaveOptions();
  options.Set("properties",
              base::ListValue().Append("openFile").Append("multiSelections"));
  DialogFuture future;
  ShowElectronOpenDialog(kOwner, owner_.get(), options, future.GetCallback());
  auto dialog = factory->last_dialog;
  ASSERT_TRUE(dialog);
  EXPECT_FALSE(future.IsReady());
  EXPECT_EQ(dialog->type, ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE);
  EXPECT_EQ(dialog->default_path,
            base::FilePath(FILE_PATH_LITERAL("draft.drawio")));
  EXPECT_EQ(dialog->owner, owner_->GetNativeWindow());
  EXPECT_EQ(dialog->title, u"Save document");
  ASSERT_EQ(dialog->file_types.extensions.size(), 2u);
  EXPECT_EQ(dialog->file_types.extension_description_overrides[0], u"Diagram");
  EXPECT_EQ(dialog->file_types.extensions[0].size(), 2u);
  EXPECT_TRUE(dialog->Reply({base::FilePath(FILE_PATH_LITERAL("first.drawio")),
                             base::FilePath(FILE_PATH_LITERAL("second.xml"))}));
  EXPECT_FALSE(future.IsReady());
  ASSERT_TRUE(future.Wait());
  EXPECT_TRUE(future.Get<1>().empty());
  EXPECT_EQ(future.Get<0>().GetDict().FindBool("canceled"), false);
  EXPECT_EQ(*future.Get<0>().GetDict().FindList("filePaths"),
            base::ListValue().Append("first.drawio").Append("second.xml"));
  EXPECT_FALSE(dialog->Reply({base::FilePath(FILE_PATH_LITERAL("late.xml"))}));
}

TEST_F(ElectronDialogTest, OpenModesIncludeDefaultFileAndDirectoryCreation) {
  auto recording = std::make_unique<RecordingFileDialogFactory>();
  auto* factory = recording.get();
  factory_ = nullptr;
  ui::SelectFileDialog::SetFactory(std::move(recording));
  for (int mode = 0; mode < 3; ++mode) {
    base::DictValue options;
    if (mode) {
      base::ListValue properties;
      properties.Append("openDirectory");
      if (mode == 2) {
        properties.Append("createDirectory");
      }
      options.Set("properties", std::move(properties));
    }
    DialogFuture future;
    ShowElectronOpenDialog(kOwner, nullptr, options, future.GetCallback());
    const auto type = mode == 0   ? ui::SelectFileDialog::SELECT_OPEN_FILE
                      : mode == 1 ? ui::SelectFileDialog::SELECT_EXISTING_FOLDER
                                  : ui::SelectFileDialog::SELECT_FOLDER;
    EXPECT_EQ(factory->last_dialog->type, type);
    EXPECT_EQ(factory->last_dialog->owner, gfx::NativeWindow());
    EXPECT_TRUE(factory->last_dialog->Reply(
        {base::FilePath(FILE_PATH_LITERAL("selected"))}));
    ASSERT_TRUE(future.Wait());
    EXPECT_TRUE(future.Get<1>().empty());
    EXPECT_EQ(*future.Get<0>().GetDict().FindList("filePaths"),
              base::ListValue().Append("selected"));
  }
}

TEST_F(ElectronDialogTest, OpenCancellationAndOwnerAbortAreDistinct) {
  factory_->SetOpenCallback(base::BindLambdaForTesting(
      [&] { factory_->GetLastDialog()->CallFileSelectionCanceled(); }));
  DialogFuture canceled;
  ShowElectronOpenDialog(kOwner, owner_.get(), {}, canceled.GetCallback());
  EXPECT_FALSE(canceled.IsReady());
  ASSERT_TRUE(canceled.Wait());
  EXPECT_TRUE(canceled.Get<1>().empty());
  EXPECT_EQ(canceled.Get<0>().GetDict().FindBool("canceled"), true);
  EXPECT_TRUE(canceled.Get<0>().GetDict().FindList("filePaths")->empty());

  auto recording = std::make_unique<RecordingFileDialogFactory>();
  auto* factory = recording.get();
  factory_ = nullptr;
  ui::SelectFileDialog::SetFactory(std::move(recording));
  DialogFuture aborted;
  ShowElectronOpenDialog(kOwner, owner_.get(), {}, aborted.GetCallback());
  auto dialog = factory->last_dialog;
  CancelElectronDialogs("dialog-test#2");
  RunPendingMessages();
  EXPECT_FALSE(aborted.IsReady());
  owner_.reset();
  CancelElectronDialogs(kOwner);
  ASSERT_TRUE(aborted.Wait());
  EXPECT_TRUE(aborted.Get<0>().is_none());
  EXPECT_TRUE(aborted.Get<1>().starts_with("ERR_ABORTED:"));
  EXPECT_FALSE(dialog->Reply({base::FilePath(FILE_PATH_LITERAL("late"))}));
}

TEST_F(ElectronDialogTest,
       OpenUnsupportedPropertiesAndInvalidResultsAreErrors) {
  for (const char* property : {"showHiddenFiles", "multiSelections"}) {
    base::ListValue properties;
    properties.Append("openDirectory");
    properties.Append(property);
    DialogFuture future;
    ShowElectronOpenDialog(
        kOwner, nullptr,
        base::DictValue().Set("properties", properties.Clone()),
        future.GetCallback());
    ASSERT_TRUE(future.Wait());
    EXPECT_TRUE(future.Get<0>().is_none());
    EXPECT_TRUE(future.Get<1>().starts_with("ERR_NOT_SUPPORTED:"));
    EXPECT_FALSE(factory_->GetLastDialog());
  }
  auto recording = std::make_unique<RecordingFileDialogFactory>();
  auto* factory = recording.get();
  factory_ = nullptr;
  ui::SelectFileDialog::SetFactory(std::move(recording));
  DialogFuture future;
  ShowElectronOpenDialog(kOwner, nullptr, {}, future.GetCallback());
  EXPECT_TRUE(factory->last_dialog->Reply({}));
  ASSERT_TRUE(future.Wait());
  EXPECT_TRUE(future.Get<0>().is_none());
  EXPECT_TRUE(future.Get<1>().starts_with("ERR_FAILED:"));
}

}  // namespace
}  // namespace xenon
