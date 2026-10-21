#include "chrome/updater/app/app_patch_worker.h"

#include <optional>
#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/multiprocess_test.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/task_environment.h"
#include "base/test/test_timeouts.h"
#include "chrome/updater/app/app.h"
#include "chrome/updater/ipc/ipc_support.h"
#include "components/services/patch/public/mojom/file_patcher.mojom.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "third_party/puffin/src/include/puffin/puffpatch.h"

namespace updater {

class AppPatchWorkerTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temporary_.CreateUniqueTempDir());
    test_data_ = base::PathService::CheckedGet(base::DIR_SRC_TEST_DATA_ROOT)
                     .AppendASCII("components/test/data/update_client")
                     .AppendASCII("puffin_patch_test");
    mojo::PlatformChannel channel;
    base::LaunchOptions options;
    base::CommandLine command_line =
        base::GetMultiProcessTestChildBaseCommandLine();
    channel.PrepareToPassRemoteEndpoint(&options, &command_line);
    worker_ = base::SpawnMultiProcessTestChild("PatchWorkerChild", command_line,
                                               options);
    channel.RemoteProcessLaunchAttempted();
    ASSERT_TRUE(worker_.IsValid());
    remote_.Bind(mojo::PendingRemote<patch::mojom::FilePatcher>(
        mojo::OutgoingInvitation::SendIsolated(channel.TakeLocalEndpoint(), {},
                                               worker_.Handle()),
        patch::mojom::FilePatcher::Version_));
  }

  void TearDown() override {
    remote_.reset();
    if (worker_.IsValid()) {
      int exit_code = -1;
      EXPECT_TRUE(worker_.WaitForExitWithTimeout(TestTimeouts::action_timeout(),
                                                 &exit_code));
      EXPECT_EQ(exit_code, 0);
    }
  }

  int ApplyPatch(const base::FilePath& patch_path) {
    base::File input(test_data_.AppendASCII("puffin_app_v1.crx3"),
                     base::File::FLAG_OPEN | base::File::FLAG_READ);
    base::File patch_file(patch_path,
                          base::File::FLAG_OPEN | base::File::FLAG_READ);
    base::File output(temporary_.GetPath().AppendASCII("output.crx3"),
                      base::File::FLAG_CREATE | base::File::FLAG_WRITE);
    EXPECT_TRUE(input.IsValid());
    EXPECT_TRUE(patch_file.IsValid());
    EXPECT_TRUE(output.IsValid());
    base::RunLoop loop;
    base::test::ScopedRunLoopTimeout timeout(FROM_HERE,
                                             TestTimeouts::action_timeout());
    std::optional<int> result;
    remote_.set_disconnect_handler(loop.QuitClosure());
    remote_->PatchFilePuffPatch(std::move(input), std::move(patch_file),
                                std::move(output),
                                base::BindLambdaForTesting([&](int status) {
                                  result = status;
                                  loop.Quit();
                                }));
    loop.Run();
    EXPECT_TRUE(result.has_value());
    return result.value_or(0);
  }

  base::test::TaskEnvironment environment_;
  ScopedIPCSupportWrapper ipc_support_;
  base::ScopedTempDir temporary_;
  base::FilePath test_data_;
  base::Process worker_;
  mojo::Remote<patch::mojom::FilePatcher> remote_;
};

TEST_F(AppPatchWorkerTest, AppliesPuffPatchWithoutBlockingMainThread) {
  EXPECT_EQ(ApplyPatch(test_data_.AppendASCII("puffin_app_v1_to_v2.puff")),
            puffin::P_OK);
  EXPECT_TRUE(
      base::ContentsEqual(temporary_.GetPath().AppendASCII("output.crx3"),
                          test_data_.AppendASCII("puffin_app_v2.crx3")));
}

TEST_F(AppPatchWorkerTest, CorruptPatchReturnsErrorWithoutCrashing) {
  const base::FilePath corrupt_patch =
      temporary_.GetPath().AppendASCII("corrupt.puff");
  ASSERT_TRUE(base::WriteFile(corrupt_patch, "invalid patch"));
  EXPECT_EQ(ApplyPatch(corrupt_patch), puffin::P_BAD_PUFFIN_MAGIC);
}

MULTIPROCESS_TEST_MAIN(PatchWorkerChild) {
  base::test::TaskEnvironment environment;
  ScopedIPCSupportWrapper ipc_support;
  return MakeAppPatchWorker()->Run();
}

}
