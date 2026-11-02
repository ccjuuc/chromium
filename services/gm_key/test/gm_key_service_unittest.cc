// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <memory>
#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/test/test_suite.h"
#include "base/threading/thread.h"
#include "crypto/hash.h"
#include "mojo/core/embedder/configuration.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "services/gm_key/public/mojom/gm_key_service.mojom.h"
#include "services/gm_key/test/key_service_test_client.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace gm_key::test {
namespace {
using Error = gm_key::mojom::KeyError;
using ResultPtr = gm_key::mojom::KeyResultPtr;
using IdentityPtr = gm_key::mojom::KeyIdentityPtr;

class Observer : public gm_key::mojom::KeyObserver {
 public:
  mojo::PendingRemote<gm_key::mojom::KeyObserver> Bind() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  void OnIdentitiesChanged() override {
    ++changes;
    if (next_change) {
      std::move(next_change).Run();
    }
  }
  void OnServiceFailure(Error error) override { last_error = error; }
  void Flush() { receiver_.FlushForTesting(); }
  int changes = 0;
  Error last_error = Error::kOk;
  base::OnceClosure next_change;

 private:
  mojo::Receiver<gm_key::mojom::KeyObserver> receiver_{this};
};

class GmKeyServiceTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &directory_));
    base::Thread::Options options(base::MessagePumpType::IO, 0);
    ASSERT_TRUE(ipc_thread_.StartWithOptions(std::move(options)));
    ipc_ = std::make_unique<mojo::core::ScopedIPCSupport>(
        ipc_thread_.task_runner(),
        mojo::core::ScopedIPCSupport::ShutdownPolicy::CLEAN);
    process_ = std::make_unique<ServiceProcess>();
    ASSERT_TRUE(process_->Launch(
        directory_.AppendASCII("gm_key_service_test_helper.exe")));
  }
  void TearDown() override {
    process_.reset();
    ipc_.reset();
    ipc_thread_.Stop();
  }
  gm_key::mojom::DriverConfigPtr Config() {
    auto config = gm_key::mojom::DriverConfig::New();
    config->library_path = directory_.AppendASCII("gm_sm2_simulator.dll");
    config->sha256.resize(32);
    base::File file(config->library_path,
                    base::File::FLAG_OPEN | base::File::FLAG_READ);
    EXPECT_TRUE(
        crypto::hash::HashFile(crypto::hash::kSha256, &file, config->sha256));
    return config;
  }
  void Configure() {
    base::test::TestFuture<ResultPtr> future;
    process_->service()->Configure(Config(), observer_.Bind(),
                                   future.GetCallback());
    ASSERT_EQ(future.Get()->error, Error::kOk);
  }
  std::vector<IdentityPtr> Enumerate() {
    base::test::TestFuture<ResultPtr, std::vector<IdentityPtr>> future;
    process_->service()->Enumerate(future.GetCallback());
    EXPECT_EQ(future.Get<0>()->error, Error::kOk);
    return std::move(std::get<1>(future.Take()));
  }
  IdentityPtr Identity() {
    auto identities = Enumerate();
    EXPECT_EQ(identities.size(), 1u);
    return identities.empty() ? nullptr : std::move(identities.front());
  }
  std::vector<uint8_t> Control(uint32_t operation, uint32_t value = 0) {
    base::test::TestFuture<ResultPtr, std::vector<uint8_t>> future;
    process_->control()->Control(
        operation, value,
        future.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
    EXPECT_EQ(future.Get<0>()->error, Error::kOk);
    return std::move(std::get<1>(future.Take()));
  }
  mojo::Remote<gm_key::mojom::KeySigner> Authorize(
      const IdentityPtr& identity) {
    const uint64_t request_id = NextAuthorization();
    mojo::Remote<gm_key::mojom::KeySigner> signer;
    base::test::TestFuture<ResultPtr, IdentityPtr> future;
    auto pin = gm_key::mojom::KeyPin::New();
    pin->value = {'1', '2', '3', '4', '5', '6'};
    process_->service()->Authorize(
        request_id, identity->identity_id, identity->generation,
        SecurePin(std::move(pin->value)), signer.BindNewPipeAndPassReceiver(),
        future.GetCallback());
    EXPECT_EQ(future.Get<0>()->error, Error::kOk);
    EXPECT_TRUE(future.Get<1>());
    process_->service()->CommitAuthorization(request_id);
    process_->service().FlushForTesting();
    return signer;
  }
  uint64_t NextAuthorization() { return ++last_authorization_; }
  uint64_t Prepare(const IdentityPtr& identity,
                   std::vector<uint8_t> pin,
                   mojo::Remote<gm_key::mojom::KeySigner>* signer,
                   base::test::TestFuture<ResultPtr, IdentityPtr>* result) {
    const uint64_t request = NextAuthorization();
    process_->service()->Authorize(
        request, identity->identity_id, identity->generation,
        SecurePin(std::move(pin)), signer->BindNewPipeAndPassReceiver(),
        result->GetCallback());
    // The test-control pipe is independent of the service pipe. Ensure the
    // authorization is registered before waiting for its SDK call to start.
    process_->service().FlushForTesting();
    return request;
  }
  void ExpectPinRequired(const IdentityPtr& identity) {
    mojo::Remote<gm_key::mojom::KeySigner> signer;
    base::test::TestFuture<ResultPtr, IdentityPtr> result;
    process_->service()->GrantAuthorized(
        identity->identity_id, identity->generation,
        signer.BindNewPipeAndPassReceiver(), result.GetCallback());
    EXPECT_EQ(result.Get<0>()->error, Error::kPinRequired);
    EXPECT_FALSE(result.Get<1>());
  }
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::MainThreadType::IO};
  base::test::ScopedRunLoopTimeout timeout_{FROM_HERE, base::Seconds(15)};
  base::Thread ipc_thread_{"GmKeyTestParentIpc"};
  std::unique_ptr<mojo::core::ScopedIPCSupport> ipc_;
  std::unique_ptr<ServiceProcess> process_;
  base::FilePath directory_;
  Observer observer_;
  uint64_t last_authorization_ = 0;
};

TEST_F(GmKeyServiceTest, RejectsHashMismatchAndRepeatedConfiguration) {
  auto config = Config();
  config->sha256[0] ^= 1;
  base::test::TestFuture<ResultPtr> first;
  process_->service()->Configure(std::move(config), observer_.Bind(),
                                 first.GetCallback());
  ASSERT_EQ(first.Get()->error, Error::kUntrustedDriver);
  base::test::TestFuture<ResultPtr> second;
  Observer second_observer;
  process_->service()->Configure(Config(), second_observer.Bind(),
                                 second.GetCallback());
  EXPECT_EQ(second.Get()->error, Error::kAlreadyConfigured);
  base::test::TestFuture<ResultPtr, std::vector<IdentityPtr>> enumerate;
  process_->service()->Enumerate(enumerate.GetCallback());
  EXPECT_EQ(enumerate.Get<0>()->error, Error::kNotConfigured);
}

TEST_F(GmKeyServiceTest, RealSm2SignatureVerifiedOutsideDeviceProcess) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  EXPECT_NE(identity->signing_certificate, identity->encryption_certificate);
  EXPECT_FALSE(Control(0).empty());
  auto signer = Authorize(identity);
  std::vector<uint8_t> digest(32);
  digest[0] = 0x43;
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> signature;
  signer->Sign(1, digest,
               signature.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  ASSERT_EQ(signature.Get<0>()->error, Error::kOk);
  ASSERT_FALSE(signature.Get<1>().empty());
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  auto cert_file = temp.GetPath().AppendASCII("certificate.der");
  auto digest_file = temp.GetPath().AppendASCII("digest.bin");
  auto signature_file = temp.GetPath().AppendASCII("signature.der");
  ASSERT_TRUE(base::WriteFile(cert_file, identity->signing_certificate));
  ASSERT_TRUE(base::WriteFile(digest_file, digest));
  ASSERT_TRUE(base::WriteFile(signature_file, signature.Get<1>()));
  base::CommandLine command(directory_.AppendASCII("gm_sm2_verify.exe"));
  command.AppendArgPath(cert_file);
  command.AppendArgPath(digest_file);
  command.AppendArgPath(signature_file);
  base::LaunchOptions options;
  options.start_hidden = true;
  auto verifier = base::LaunchProcess(command, options);
  int code = -1;
  ASSERT_TRUE(verifier.WaitForExitWithTimeout(base::Seconds(5), &code));
  EXPECT_EQ(code, 0);
  digest[0] ^= 1;
  ASSERT_TRUE(base::WriteFile(digest_file, digest));
  verifier = base::LaunchProcess(command, options);
  ASSERT_TRUE(verifier.WaitForExitWithTimeout(base::Seconds(5), &code));
  EXPECT_EQ(code, 1);
}

TEST_F(GmKeyServiceTest, PinRetriesAreNotAutomaticAndLockIsReported) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  for (uint32_t remaining : {2u, 1u, 0u}) {
    mojo::Remote<gm_key::mojom::KeySigner> signer;
    auto pin = gm_key::mojom::KeyPin::New();
    pin->value = {'b', 'a', 'd'};
    base::test::TestFuture<ResultPtr, IdentityPtr> future;
    process_->service()->Authorize(
        NextAuthorization(), identity->identity_id, identity->generation,
        SecurePin(std::move(pin->value)), signer.BindNewPipeAndPassReceiver(),
        future.GetCallback());
    EXPECT_EQ(future.Get<0>()->error,
              remaining ? Error::kPinIncorrect : Error::kPinLocked);
    EXPECT_EQ(future.Get<0>()->pin_retries, remaining);
    EXPECT_FALSE(future.Get<1>());
  }
}

TEST_F(GmKeyServiceTest, PreparedPinCannotSignOrBeReusedBeforeCommit) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  mojo::Remote<gm_key::mojom::KeySigner> signer;
  base::test::TestFuture<ResultPtr, IdentityPtr> prepared;
  const uint64_t request =
      Prepare(identity, {'1', '2', '3', '4', '5', '6'}, &signer, &prepared);
  ASSERT_EQ(prepared.Get<0>()->error, Error::kOk);
  ExpectPinRequired(identity);
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> signature;
  signer->Sign(1, std::vector<uint8_t>(32),
               signature.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  process_->service().FlushForTesting();
  EXPECT_FALSE(signature.IsReady());
  process_->service()->CommitAuthorization(request);
  EXPECT_EQ(signature.Get<0>()->error, Error::kOk);
  EXPECT_FALSE(signature.Get<1>().empty());
}

TEST_F(GmKeyServiceTest, CancelDuringPinRejectsItsLateAuthorization) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  Control(5, 500);
  mojo::Remote<gm_key::mojom::KeySigner> signer;
  base::test::TestFuture<ResultPtr, IdentityPtr> prepared;
  const uint64_t request =
      Prepare(identity, {'1', '2', '3', '4', '5', '6'}, &signer, &prepared);
  ASSERT_EQ(Control(10, request), std::vector<uint8_t>({1}));
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  process_->service()->CancelAuthorization(request);
  EXPECT_EQ(prepared.Get<0>()->error, Error::kCancelled);
  EXPECT_TRUE(disconnected.Wait());
  Control(5, 0);  // Wait for the actual delayed SKF_VerifyPIN to return.
  process_->service()->CommitAuthorization(request);  // Cannot revive it.
  ExpectPinRequired(identity);
}

TEST_F(GmKeyServiceTest, CancelAfterPinReplyRejectsUncommittedAuthorization) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  mojo::Remote<gm_key::mojom::KeySigner> signer;
  base::test::TestFuture<ResultPtr, IdentityPtr> prepared;
  const uint64_t request =
      Prepare(identity, {'1', '2', '3', '4', '5', '6'}, &signer, &prepared);
  ASSERT_EQ(prepared.Get<0>()->error, Error::kOk);
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  process_->service()->CancelAuthorization(request);
  process_->service()->CancelAuthorization(request);
  process_->service()->CommitAuthorization(request);
  EXPECT_TRUE(disconnected.Wait());
  ExpectPinRequired(identity);
  mojo::Remote<gm_key::mojom::KeySigner> repeated;
  base::test::TestFuture<ResultPtr, IdentityPtr> reused;
  process_->service()->Authorize(
      request, identity->identity_id, identity->generation,
      SecurePin({'1', '2', '3', '4', '5', '6'}),
      repeated.BindNewPipeAndPassReceiver(), reused.GetCallback());
  EXPECT_EQ(reused.Get<0>()->error, Error::kInvalidArgument);
}

TEST_F(GmKeyServiceTest, EarlyCancellationCannotBeAuthorizedLater) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto original = Authorize(identity);
  const uint64_t request = NextAuthorization();
  process_->service()->CancelAuthorization(request);
  mojo::Remote<gm_key::mojom::KeySigner> cancelled;
  base::test::TestFuture<ResultPtr, IdentityPtr> rejected;
  process_->service()->Authorize(
      request, identity->identity_id, identity->generation,
      SecurePin({'b', 'a', 'd'}), cancelled.BindNewPipeAndPassReceiver(),
      rejected.GetCallback());
  EXPECT_EQ(rejected.Get<0>()->error, Error::kInvalidArgument);
  EXPECT_FALSE(rejected.Get<1>());
  process_->service()->CommitAuthorization(request);
  // The cancelled bad PIN must not enter the SDK or disturb an earlier grant.
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> signature;
  original->Sign(
      1, std::vector<uint8_t>(32),
      signature.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_EQ(signature.Get<0>()->error, Error::kOk);
}

TEST_F(GmKeyServiceTest,
       AbandonedPreparedAuthorizationExpiresWithoutKillingService) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  mojo::Remote<gm_key::mojom::KeySigner> signer;
  base::test::TestFuture<ResultPtr, IdentityPtr> prepared;
  const uint64_t request =
      Prepare(identity, {'1', '2', '3', '4', '5', '6'}, &signer, &prepared);
  ASSERT_EQ(prepared.Get<0>()->error, Error::kOk);
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  // The test helper uses a three-second deadline; production uses 30 seconds.
  EXPECT_TRUE(disconnected.Wait());
  process_->service()->CommitAuthorization(request);
  ExpectPinRequired(identity);
  auto fresh = Authorize(identity);
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> signature;
  fresh->Sign(1, std::vector<uint8_t>(32),
              signature.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_EQ(signature.Get<0>()->error, Error::kOk);
}

TEST_F(GmKeyServiceTest,
       ControlDisconnectRevokesCommittedPreparedAndRunningGrants) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto committed = Authorize(identity);
  mojo::Remote<gm_key::mojom::KeySigner> prepared_signer;
  base::test::TestFuture<ResultPtr, IdentityPtr> prepared;
  Prepare(identity, {'1', '2', '3', '4', '5', '6'}, &prepared_signer,
          &prepared);
  ASSERT_EQ(prepared.Get<0>()->error, Error::kOk);
  Control(5, 10000);
  mojo::Remote<gm_key::mojom::KeySigner> running_signer;
  base::test::TestFuture<ResultPtr, IdentityPtr> running;
  const uint64_t request = Prepare(identity, {'1', '2', '3', '4', '5', '6'},
                                   &running_signer, &running);
  ASSERT_EQ(Control(10, request), std::vector<uint8_t>({1}));
  base::test::TestFuture<void> committed_closed, prepared_closed,
      running_closed;
  committed.set_disconnect_handler(committed_closed.GetCallback());
  prepared_signer.set_disconnect_handler(prepared_closed.GetCallback());
  running_signer.set_disconnect_handler(running_closed.GetCallback());
  process_->service().reset();
  EXPECT_TRUE(committed_closed.Wait());
  EXPECT_TRUE(prepared_closed.Wait());
  EXPECT_TRUE(running_closed.Wait());
  int code = 0;
  ASSERT_TRUE(
      process_->process().WaitForExitWithTimeout(base::Seconds(5), &code));
  EXPECT_EQ(code, 72);
}

TEST_F(GmKeyServiceTest, CancellingAnotherPinPreservesCommittedAuthorization) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto original = Authorize(identity);
  Control(5, 500);
  mojo::Remote<gm_key::mojom::KeySigner> cancelled;
  base::test::TestFuture<ResultPtr, IdentityPtr> prepared;
  const uint64_t request =
      Prepare(identity, {'1', '2', '3', '4', '5', '6'}, &cancelled, &prepared);
  ASSERT_EQ(Control(10, request), std::vector<uint8_t>({1}));
  process_->service()->CancelAuthorization(request);
  EXPECT_EQ(prepared.Get<0>()->error, Error::kCancelled);
  Control(5, 0);
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> signature;
  original->Sign(
      1, std::vector<uint8_t>(32),
      signature.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_EQ(signature.Get<0>()->error, Error::kOk);
  mojo::Remote<gm_key::mojom::KeySigner> reused;
  base::test::TestFuture<ResultPtr, IdentityPtr> granted;
  process_->service()->GrantAuthorized(
      identity->identity_id, identity->generation,
      reused.BindNewPipeAndPassReceiver(), granted.GetCallback());
  EXPECT_EQ(granted.Get<0>()->error, Error::kOk);
}

TEST_F(GmKeyServiceTest, CancelledBadPinCannotCancelALaterCorrectPin) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  Control(5, 300);
  mojo::Remote<gm_key::mojom::KeySigner> cancelled;
  base::test::TestFuture<ResultPtr, IdentityPtr> first;
  const uint64_t first_request =
      Prepare(identity, {'b', 'a', 'd'}, &cancelled, &first);
  ASSERT_EQ(Control(10, first_request), std::vector<uint8_t>({1}));
  process_->service()->CancelAuthorization(first_request);
  EXPECT_EQ(first.Get<0>()->error, Error::kCancelled);
  mojo::Remote<gm_key::mojom::KeySigner> signer;
  base::test::TestFuture<ResultPtr, IdentityPtr> second;
  const uint64_t second_request =
      Prepare(identity, {'1', '2', '3', '4', '5', '6'}, &signer, &second);
  ASSERT_EQ(second.Get<0>()->error, Error::kOk);
  process_->service()->CommitAuthorization(second_request);
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> signature;
  signer->Sign(1, std::vector<uint8_t>(32),
               signature.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_EQ(signature.Get<0>()->error, Error::kOk);
}

TEST_F(GmKeyServiceTest, SameNameReinsertionRevokesWithoutAnAbsentPoll) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto signer = Authorize(identity);
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  Control(6);  // USB event, but every SKF_EnumDev still reports the same name.
  EXPECT_TRUE(disconnected.Wait());
  auto reinserted = Identity();
  ASSERT_TRUE(reinserted);
  EXPECT_NE(reinserted->identity_id, identity->identity_id);
  EXPECT_GT(reinserted->generation, identity->generation);
  ExpectPinRequired(reinserted);
}

TEST_F(GmKeyServiceTest, DeviceEventRevokesWhilePinSdkCallIsBlocked) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto original = Authorize(identity);
  base::test::TestFuture<void> disconnected;
  original.set_disconnect_handler(disconnected.GetCallback());
  Control(5, 1000);
  mojo::Remote<gm_key::mojom::KeySigner> pending;
  base::test::TestFuture<ResultPtr, IdentityPtr> prepared;
  const uint64_t request =
      Prepare(identity, {'1', '2', '3', '4', '5', '6'}, &pending, &prepared);
  ASSERT_EQ(Control(10, request), std::vector<uint8_t>({1}));
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> event;
  process_->control()->Control(
      6, 0, event.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_TRUE(disconnected.Wait());
  EXPECT_EQ(prepared.Get<0>()->error, Error::kIdentityUnavailable);
  ExpectPinRequired(identity);
  ASSERT_EQ(event.Get<0>()->error, Error::kOk);
  auto next = Identity();
  ASSERT_TRUE(next);
  process_->service()->CommitAuthorization(request);
  ExpectPinRequired(next);
}

TEST_F(GmKeyServiceTest, CertificateRenewalRevokesAndRefreshesIdentity) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto signer = Authorize(identity);
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  Control(7);
  Control(9);  // Run the same bounded metadata scan used by the periodic poll.
  EXPECT_TRUE(disconnected.Wait());
  auto renewed = Identity();
  ASSERT_TRUE(renewed);
  EXPECT_NE(renewed->signing_certificate, identity->signing_certificate);
  EXPECT_NE(renewed->encryption_certificate, identity->encryption_certificate);
  EXPECT_GT(renewed->generation, identity->generation);
  ExpectPinRequired(renewed);
}

TEST_F(GmKeyServiceTest, ContainerChangeRevokesButStableRefreshKeepsPin) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto signer = Authorize(identity);
  Control(9);
  auto unchanged = Identity();
  ASSERT_TRUE(unchanged);
  EXPECT_EQ(unchanged->identity_id, identity->identity_id);
  EXPECT_EQ(unchanged->generation, identity->generation);
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> signature;
  signer->Sign(1, std::vector<uint8_t>(32),
               signature.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_EQ(signature.Get<0>()->error, Error::kOk);
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  Control(8, 1);
  Control(9);
  EXPECT_TRUE(disconnected.Wait());
  auto next = Identity();
  ASSERT_TRUE(next);
  EXPECT_NE(next->identity_id, identity->identity_id);
  ExpectPinRequired(next);
}

TEST_F(GmKeyServiceTest, CancelDropsLateResultAndRequestIdCannotBeReused) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto signer = Authorize(identity);
  Control(2, 500);
  std::vector<uint8_t> digest(32);
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> cancelled;
  signer->Sign(1, digest,
               cancelled.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  signer->Cancel(1);
  EXPECT_EQ(cancelled.Get<0>()->error, Error::kCancelled);
  EXPECT_TRUE(cancelled.Get<1>().empty());
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> repeated;
  signer->Sign(1, digest,
               repeated.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_EQ(repeated.Get<0>()->error, Error::kInvalidArgument);
  Control(2,
          0);  // Waits for the cancelled SDK operation to finish on its thread.
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> next;
  signer->Sign(2, digest,
               next.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_EQ(next.Get<0>()->error, Error::kOk);
}

TEST_F(GmKeyServiceTest, LockRevokesSignerAndOldGeneration) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto signer = Authorize(identity);
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  base::test::TestFuture<ResultPtr> locked;
  process_->service()->Lock(identity->identity_id, locked.GetCallback());
  EXPECT_EQ(locked.Get()->error, Error::kOk);
  EXPECT_TRUE(disconnected.Wait());
  auto next = Identity();
  ASSERT_TRUE(next);
  EXPECT_GT(next->generation, identity->generation);
  mojo::Remote<gm_key::mojom::KeySigner> stale;
  auto pin = gm_key::mojom::KeyPin::New();
  pin->value = {'1', '2', '3', '4', '5', '6'};
  base::test::TestFuture<ResultPtr, IdentityPtr> rejected;
  process_->service()->Authorize(
      NextAuthorization(), identity->identity_id, identity->generation,
      SecurePin(std::move(pin->value)), stale.BindNewPipeAndPassReceiver(),
      rejected.GetCallback());
  EXPECT_EQ(rejected.Get<0>()->error, Error::kIdentityUnavailable);
}

TEST_F(GmKeyServiceTest, RemovalRevokesAndReinsertionCreatesNewIdentity) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto signer = Authorize(identity);
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  Control(1, 0);
  EXPECT_TRUE(disconnected.Wait());
  EXPECT_TRUE(Enumerate().empty());
  base::test::TestFuture<void> changed;
  observer_.next_change = changed.GetCallback();
  Control(1, 1);
  ASSERT_TRUE(changed.Wait());
  auto next = Identity();
  ASSERT_TRUE(next);
  EXPECT_NE(next->identity_id, identity->identity_id);
  EXPECT_GT(next->generation, identity->generation);
}

TEST_F(GmKeyServiceTest, LockCancelsAuthorizationAlreadyInFlight) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  Control(5, 300);
  mojo::Remote<gm_key::mojom::KeySigner> signer;
  base::test::TestFuture<ResultPtr, IdentityPtr> authorized;
  auto pin = gm_key::mojom::KeyPin::New();
  pin->value = {'1', '2', '3', '4', '5', '6'};
  process_->service()->Authorize(
      NextAuthorization(), identity->identity_id, identity->generation,
      SecurePin(std::move(pin->value)), signer.BindNewPipeAndPassReceiver(),
      authorized.GetCallback());
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  base::test::TestFuture<ResultPtr> locked;
  process_->service()->Lock(identity->identity_id, locked.GetCallback());
  EXPECT_EQ(authorized.Get<0>()->error, Error::kCancelled);
  EXPECT_FALSE(authorized.Get<1>());
  EXPECT_EQ(locked.Get()->error, Error::kOk);
  EXPECT_TRUE(disconnected.Wait());
}

TEST_F(GmKeyServiceTest, CapsLiveSignerCapabilitiesAndRecoversAfterRevocation) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  std::vector<mojo::Remote<gm_key::mojom::KeySigner>> signers;
  for (int i = 0; i < 64; ++i) {
    signers.push_back(Authorize(identity));
  }
  mojo::Remote<gm_key::mojom::KeySigner> overflow;
  base::test::TestFuture<ResultPtr, IdentityPtr> rejected;
  auto pin = gm_key::mojom::KeyPin::New();
  pin->value = {'1', '2', '3', '4', '5', '6'};
  process_->service()->Authorize(
      NextAuthorization(), identity->identity_id, identity->generation,
      SecurePin(std::move(pin->value)), overflow.BindNewPipeAndPassReceiver(),
      rejected.GetCallback());
  EXPECT_EQ(rejected.Get<0>()->error, Error::kQueueFull);
  EXPECT_FALSE(rejected.Get<1>());
  // Lock is a synchronous barrier across all capability revocations.
  base::test::TestFuture<ResultPtr> locked;
  process_->service()->Lock(identity->identity_id, locked.GetCallback());
  ASSERT_EQ(locked.Get()->error, Error::kOk);
  auto next = Identity();
  ASSERT_TRUE(next);
  auto recovered = Authorize(next);
  std::vector<uint8_t> digest(32);
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> signature;
  recovered->Sign(
      1, digest,
      signature.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_EQ(signature.Get<0>()->error, Error::kOk);
}

TEST_F(GmKeyServiceTest, GrantRequiresAuthorizationAndReusesItWithoutPin) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  mojo::Remote<gm_key::mojom::KeySigner> unauthenticated;
  base::test::TestFuture<ResultPtr, IdentityPtr> rejected;
  process_->service()->GrantAuthorized(
      identity->identity_id, identity->generation,
      unauthenticated.BindNewPipeAndPassReceiver(), rejected.GetCallback());
  EXPECT_EQ(rejected.Get<0>()->error, Error::kPinRequired);
  EXPECT_FALSE(rejected.Get<1>());

  auto original = Authorize(identity);
  // A second PIN call would hang beyond the test service watchdog. Creating a
  // second capability must neither retain/replay a PIN nor invoke the SDK.
  Control(5, 10000);
  mojo::Remote<gm_key::mojom::KeySigner> additional;
  base::test::TestFuture<ResultPtr, IdentityPtr> granted;
  process_->service()->GrantAuthorized(
      identity->identity_id, identity->generation,
      additional.BindNewPipeAndPassReceiver(), granted.GetCallback());
  ASSERT_EQ(granted.Get<0>()->error, Error::kOk);
  ASSERT_TRUE(granted.Get<1>());
  std::vector<uint8_t> digest(32);
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> signature;
  additional->Sign(
      1, digest,
      signature.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_EQ(signature.Get<0>()->error, Error::kOk);

  base::test::TestFuture<void> disconnected;
  additional.set_disconnect_handler(disconnected.GetCallback());
  base::test::TestFuture<ResultPtr> locked;
  process_->service()->Lock(identity->identity_id, locked.GetCallback());
  EXPECT_EQ(locked.Get()->error, Error::kOk);
  EXPECT_TRUE(disconnected.Wait());
  auto next = Identity();
  ASSERT_TRUE(next);
  mojo::Remote<gm_key::mojom::KeySigner> after_lock;
  base::test::TestFuture<ResultPtr, IdentityPtr> needs_pin;
  process_->service()->GrantAuthorized(next->identity_id, next->generation,
                                       after_lock.BindNewPipeAndPassReceiver(),
                                       needs_pin.GetCallback());
  EXPECT_EQ(needs_pin.Get<0>()->error, Error::kPinRequired);
  EXPECT_FALSE(needs_pin.Get<1>());
}

TEST_F(GmKeyServiceTest, SigningFailureClearsRememberedAuthorization) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto signer = Authorize(identity);
  Control(4);  // Simulate loss of the device's authenticated state.
  std::vector<uint8_t> digest(32);
  base::test::TestFuture<ResultPtr, std::vector<uint8_t>> signature;
  signer->Sign(1, digest,
               signature.GetCallback<ResultPtr, const std::vector<uint8_t>&>());
  EXPECT_EQ(signature.Get<0>()->error, Error::kPinRequired);
  EXPECT_TRUE(signature.Get<1>().empty());
  auto next = Identity();
  ASSERT_TRUE(next);
  EXPECT_GT(next->generation, identity->generation);
  mojo::Remote<gm_key::mojom::KeySigner> after_failure;
  base::test::TestFuture<ResultPtr, IdentityPtr> rejected;
  process_->service()->GrantAuthorized(
      next->identity_id, next->generation,
      after_failure.BindNewPipeAndPassReceiver(), rejected.GetCallback());
  EXPECT_EQ(rejected.Get<0>()->error, Error::kPinRequired);
  EXPECT_FALSE(rejected.Get<1>());
}

TEST_F(GmKeyServiceTest, FailedPinRevokesPriorAuthorization) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto signer = Authorize(identity);
  const int previous_changes = observer_.changes;
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  auto bad_pin = gm_key::mojom::KeyPin::New();
  bad_pin->value = {'b', 'a', 'd'};
  mojo::Remote<gm_key::mojom::KeySigner> failed;
  base::test::TestFuture<ResultPtr, IdentityPtr> denied;
  process_->service()->Authorize(
      NextAuthorization(), identity->identity_id, identity->generation,
      SecurePin(std::move(bad_pin->value)), failed.BindNewPipeAndPassReceiver(),
      denied.GetCallback());
  EXPECT_EQ(denied.Get<0>()->error, Error::kPinIncorrect);
  EXPECT_TRUE(disconnected.Wait());
  observer_.Flush();
  EXPECT_EQ(observer_.changes, previous_changes);
  mojo::Remote<gm_key::mojom::KeySigner> cannot_reuse;
  base::test::TestFuture<ResultPtr, IdentityPtr> rejected;
  process_->service()->GrantAuthorized(
      identity->identity_id, identity->generation,
      cannot_reuse.BindNewPipeAndPassReceiver(), rejected.GetCallback());
  EXPECT_EQ(rejected.Get<0>()->error, Error::kPinRequired);
}

TEST_F(GmKeyServiceTest, HungDriverTerminatesOnlyItsDedicatedProcess) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto signer = Authorize(identity);
  Control(2, 10000);
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  std::vector<uint8_t> digest(32);
  signer->Sign(1, digest,
               base::BindOnce([](ResultPtr, const std::vector<uint8_t>&) {}));
  EXPECT_TRUE(disconnected.Wait());
  int code = 0;
  ASSERT_TRUE(
      process_->process().WaitForExitWithTimeout(base::Seconds(5), &code));
  EXPECT_EQ(code, 71);
}

TEST_F(GmKeyServiceTest, CrashedDriverClosesItsSignerCapability) {
  Configure();
  auto identity = Identity();
  ASSERT_TRUE(identity);
  auto signer = Authorize(identity);
  Control(3, 1);
  base::test::TestFuture<void> disconnected;
  signer.set_disconnect_handler(disconnected.GetCallback());
  std::vector<uint8_t> digest(32);
  signer->Sign(1, digest,
               base::BindOnce([](ResultPtr, const std::vector<uint8_t>&) {}));
  EXPECT_TRUE(disconnected.Wait());
  int code = 0;
  ASSERT_TRUE(
      process_->process().WaitForExitWithTimeout(base::Seconds(5), &code));
  EXPECT_EQ(code, 73);
}

}  // namespace
}  // namespace gm_key::test

int main(int argc, char** argv) {
  base::TestSuite suite(argc, argv);
  mojo::core::Configuration configuration;
  configuration.is_broker_process = true;
  mojo::core::Init(configuration);
  return suite.Run();
}
