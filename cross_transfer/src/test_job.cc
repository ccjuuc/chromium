#include "cross_transfer/test_job.h"

#include "cross_transfer/https_relay_channel.h"
#include "cross_transfer/streaming_file_io.h"
#include "cross_transfer/tls_transport_channel.h"
#include "cross_transfer/transfer_session.h"

#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>

namespace cross_transfer {
namespace {

std::filesystem::path Path(const std::string& value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

std::string FormatTaskId(const TaskId& id) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(32);
  for (uint8_t byte : id) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}

bool ParseTaskId(const std::string& value, TaskId* id) {
  if (value.size() != 32) return false;
  auto hex = [](unsigned char byte) -> int {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < id->size(); ++i) {
    int high = hex(value[i * 2]);
    int low = hex(value[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    (*id)[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return IsValidTaskId(*id);
}

std::string ConnectionError(TransportError error) {
  switch (error) {
    case TransportError::kCancelled: return "Connection cancelled";
    case TransportError::kTimeout: return "Connection timed out";
    case TransportError::kSecurity: return "TLS authentication or relay authorization failed";
    case TransportError::kProtocol: return "Invalid connection options or protocol";
    case TransportError::kResourceLimit: return "Connection resource limit reached";
    default: return "Could not establish an authenticated connection";
  }
}

// Session calls cannot replace the job's lifetime-long cancellation token. In
// particular, a fresh session resetting its own flag must not revive a job that
// the UI already cancelled. All channel methods still run on the owner worker.
class JobChannel final : public ITransportChannel {
 public:
  JobChannel(std::unique_ptr<ITransportChannel> channel,
             const std::atomic<bool>* cancellation,
             std::function<void(const FrameHeader&, std::span<const uint8_t>)> observe)
      : channel_(std::move(channel)), cancellation_(cancellation),
        observe_(std::move(observe)) {
    channel_->SetCancellationFlag(cancellation_);
  }
  ~JobChannel() override {
    channel_->Close();
    channel_->SetCancellationFlag(nullptr);
  }
  bool SendFrame(const FrameHeader& header, std::span<const uint8_t> bytes) override {
    if (cancellation_->load()) return false;
    observe_(header, bytes);
    return channel_->SendFrame(header, bytes);
  }
  bool ReadNextFrame(FrameHeader& header, std::vector<uint8_t>& bytes) override {
    if (cancellation_->load() || !channel_->ReadNextFrame(header, bytes)) return false;
    observe_(header, bytes);
    return true;
  }
  void Close() override { channel_->Close(); }
  bool IsConnected() const override { return !cancellation_->load() && channel_->IsConnected(); }
  TransportType GetType() const override { return channel_->GetType(); }
  bool IsSecure() const override { return channel_->IsSecure(); }
  void SetCancellationFlag(const std::atomic<bool>*) override {
    channel_->SetCancellationFlag(cancellation_);
  }
  void SetIoTimeout(int timeout) override { channel_->SetIoTimeout(timeout); }
  TransportError LastError() const override {
    return cancellation_->load() ? TransportError::kCancelled : channel_->LastError();
  }

 private:
  std::unique_ptr<ITransportChannel> channel_;
  BorrowedPtr<const std::atomic<bool>> cancellation_;
  std::function<void(const FrameHeader&, std::span<const uint8_t>)> observe_;
};

}  // namespace

struct TestTransferJob::Impl {
  explicit Impl(TestJobOptions input) : options(std::move(input)) {}
  TestJobOptions options;
  std::atomic<bool> cancelled{false};
  mutable std::mutex mutex;
  TestJobSnapshot view;
  bool started{false};

  void Status(const std::string& state, const std::string& status) {
    std::lock_guard lock(mutex);
    if (!cancelled.load()) {
      if (!state.empty()) view.state = state;
      view.status = status;
    }
  }
  void Progress(uint64_t bytes, uint64_t total, float percent) {
    std::lock_guard lock(mutex);
    view.bytes = bytes;
    view.total = total;
    view.percent = std::isfinite(percent) ? std::clamp<double>(percent, 0, 100) : 0;
  }
  bool Finish(bool success, const std::string& error = {}) {
    std::lock_guard lock(mutex);
    view.active = false;
    if (success) {
      view.state = "completed";
      view.status = "File transferred and verified";
      view.error.clear();
      view.percent = 100;
    } else if (cancelled.load()) {
      view.state = "cancelled";
      view.status = "Transfer cancelled; durable progress is kept for resuming";
      view.error.clear();
    } else {
      view.state = "failed";
      view.status = "Transfer failed";
      view.error = error;
    }
    return success;
  }
  void Observe(const FrameHeader& header, std::span<const uint8_t> bytes) {
    if (!ValidateFrameHeader(header) || header.payload_len != bytes.size() ||
        header.checksum != ComputeCRC32C(bytes) || !IsValidTaskId(header.task_id)) return;
    std::lock_guard lock(mutex);
    if (header.cmd == FrameHeader::Command::kHandshakeReq && view.task_id.empty())
      view.task_id = FormatTaskId(header.task_id);
    if (cancelled.load()) return;
    switch (header.cmd) {
      case FrameHeader::Command::kHandshakeReq:
      case FrameHeader::Command::kHandshakeResp:
        view.state = "handshaking"; break;
      case FrameHeader::Command::kFileMetadata:
      case FrameHeader::Command::kSyncBitmapReq:
      case FrameHeader::Command::kSyncBitmapResp:
        view.state = "negotiating"; break;
      case FrameHeader::Command::kChunkData:
        view.state = "transferring"; break;
      case FrameHeader::Command::kTransferFinish:
        view.state = "verifying"; break;
      default: break;
    }
  }
  bool Authorize(const FileMetadataPayload& metadata) {
    if (cancelled.load() || !ValidateFileMetadata(metadata)) return false;
    if ((!options.expected_file_name.empty() &&
         metadata.file_name != options.expected_file_name) ||
        (options.expected_file_size &&
         metadata.file_size != *options.expected_file_size)) return false;
    std::error_code error;
    auto target = Path(options.output_dir) / Path(metadata.file_name);
    const auto status = std::filesystem::symlink_status(target, error);
    const bool missing = status.type() == std::filesystem::file_type::not_found;
    if (error && error != std::errc::no_such_file_or_directory) return false;
    if (!missing && !std::filesystem::is_regular_file(status)) return false;
    if (!missing && !options.allow_overwrite && !IsCommittedRecovery(metadata)) {
      Status({}, "Destination exists; enable overwrite explicitly to replace it");
      return false;
    }
    Progress(0, metadata.file_size, metadata.file_size ? 0 : 100);
    Status("negotiating", "Receiving an authorized file");
    return true;
  }
  bool IsCommittedRecovery(const FileMetadataPayload& metadata) {
    TaskId id{};
    std::string identity;
    {
      std::lock_guard lock(mutex);
      identity = view.task_id;
    }
    if (!ParseTaskId(identity, &id)) return false;
    // Only an already committed transaction may bypass overwrite consent.
    // Checking an identical destination's hash alone would authorize a new job.
    const auto staging = Path(options.output_dir) / ".cross-transfer";
    std::error_code error;
    const auto partial = std::filesystem::symlink_status(staging / (identity + ".part"), error);
    if (partial.type() != std::filesystem::file_type::not_found ||
        (error && error != std::errc::no_such_file_or_directory)) return false;
    error.clear();
    const auto state = std::filesystem::symlink_status(staging / (identity + ".state"), error);
    if (error || !std::filesystem::is_regular_file(state)) return false;
    // Open validates checkpoint digest, task identity, exact metadata, complete
    // bitmap, target size and full target SHA-256. No commit/write is requested.
    TransferStore committed;
    return committed.Open(options.output_dir, metadata, id, &cancelled) &&
           committed.bitmap().IsAllCompleted();
  }
};

TestTransferJob::TestTransferJob(TestJobOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
TestTransferJob::~TestTransferJob() = default;

void TestTransferJob::Cancel() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->started && !impl_->view.active) return;
  impl_->cancelled = true;
  impl_->view.state = impl_->view.active ? "cancelling" : "cancelled";
  impl_->view.status = "Cancellation requested";
}

TestJobSnapshot TestTransferJob::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->view;
}

bool TestTransferJob::Run() {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->started) return false;
    impl_->started = true;
    impl_->view.active = true;
    impl_->view.state = "starting";
  }
  auto& o = impl_->options;
  auto fail = [&](const std::string& reason) { return impl_->Finish(false, reason); };
  if (impl_->cancelled.load()) return fail({});
  if ((o.role != "send" && o.role != "receive") ||
      (o.transport != "direct" && o.transport != "relay"))
    return fail("Choose send/receive and direct/relay");
  if (o.connect_timeout_ms <= 0 || o.io_timeout_ms <= 0 || o.stage_timeout_ms <= 0)
    return fail("Finite positive connection and transfer deadlines are required");
  const bool sending = o.role == "send";
  if ((sending && (o.source_path.empty() || !IsValidUtf8Path(o.source_path))) ||
      (!sending && (o.output_dir.empty() || !IsValidUtf8Path(o.output_dir))))
    return fail("A valid source file or destination directory is required");
  if ((sending || o.transport == "relay") && (o.host.empty() || o.port == 0))
    return fail("A host and nonzero port are required");
  if (o.transport == "direct" && (o.certificate_file.empty() ||
      o.private_key_file.empty() || o.peer_name.empty()))
    return fail("Direct transfer requires a certificate, private key and expected peer name");

  TransferOptions transfer;
  transfer.io_timeout_ms = o.io_timeout_ms;
  transfer.stage_timeout_ms = o.stage_timeout_ms;
  transfer.external_cancellation = &impl_->cancelled;
  transfer.allow_overwrite = o.allow_overwrite;
  if (!o.task_id.empty() && !ParseTaskId(o.task_id, &transfer.task_id))
    return fail("Task ID must be a nonzero 32-character hexadecimal value");
  if (sending && !IsValidTaskId(transfer.task_id) &&
      (RAND_bytes(transfer.task_id.data(), transfer.task_id.size()) != 1 ||
       !IsValidTaskId(transfer.task_id))) return fail("Could not create a transfer identity");
  if (IsValidTaskId(transfer.task_id)) {
    std::lock_guard lock(impl_->mutex);
    impl_->view.task_id = FormatTaskId(transfer.task_id);
  }
  transfer.authorize_receive = [&](const auto&, const auto& metadata) {
    return impl_->Authorize(metadata);
  };
  auto status = [&](const std::string& value) { impl_->Status({}, value); };
  auto progress = [&](uint64_t bytes, uint64_t total, float percent) {
    impl_->Progress(bytes, total, percent);
  };

  std::unique_ptr<SenderSession> sender;
  if (sending) {
    impl_->Status("preparing", "Preparing the source file and SHA-256");
    sender = std::make_unique<SenderSession>(transfer);
    if (!sender->PrepareFile(o.source_path, status)) return fail(sender->error());
    if (impl_->cancelled.load()) return fail({});
    std::error_code error;
    auto size = std::filesystem::file_size(Path(o.source_path), error);
    if (error) return fail("Could not inspect the prepared source file");
    impl_->Progress(0, size, size ? 0 : 100);
  }

  TransportError connection_error{TransportError::kNone};
  std::unique_ptr<ITransportChannel> connection;
  if (o.transport == "relay") {
    impl_->Status("connecting", "Connecting to the authenticated HTTPS relay");
    HttpsRelayOptions relay;
    relay.host = o.host;
    relay.port = o.port;
    relay.room_id = o.room_id;
    relay.connection_id = o.connection_id;
    relay.role = sending ? RelayRole::kSender : RelayRole::kReceiver;
    relay.bearer_token = o.bearer_token;
    relay.ca_file = o.ca_file;
    relay.ca_pem = o.ca_pem;
    relay.timeout_ms = o.connect_timeout_ms;
    connection = HttpsRelayChannel::Connect(std::move(relay), &impl_->cancelled,
                                            &connection_error);
  } else {
    TlsConfig tls{o.ca_file, o.certificate_file, o.private_key_file, o.peer_name};
    tls.ca_pem = o.ca_pem;
    if (sending) {
      impl_->Status("connecting", "Connecting and authenticating the direct peer");
      connection = TlsTransportChannel::Connect(o.host, o.port, tls,
          o.connect_timeout_ms, &impl_->cancelled, &connection_error);
    } else {
      LanTcpServer server;
      server.SetCancellationFlag(&impl_->cancelled);
      if (!server.Start(o.port, o.host)) return fail("Could not listen on the requested address and port");
      {
        std::lock_guard lock(impl_->mutex);
        impl_->view.listening_port = server.listening_port();
      }
      impl_->Status("listening", "Listening for a mutually authenticated sender");
      auto tcp = server.Accept(o.connect_timeout_ms);
      if (!tcp) return fail(ConnectionError(server.LastError()));
      impl_->Status("connecting", "Authenticating the incoming direct peer");
      connection = TlsTransportChannel::Accept(std::move(tcp), tls,
          o.connect_timeout_ms, &impl_->cancelled, &connection_error);
    }
  }
  if (!connection) return fail(ConnectionError(connection_error));
  JobChannel channel(std::move(connection), &impl_->cancelled,
      [&](const auto& header, auto bytes) { impl_->Observe(header, bytes); });
  if (impl_->cancelled.load()) return fail({});
  impl_->Status("handshaking", "Negotiating file transfer and durable resume progress");
  if (sending) {
    const bool success = sender->SendFile(&channel, o.source_path, progress, status);
    return impl_->Finish(success, sender->error());
  }
  ReceiverSession receiver(transfer);
  std::string saved;
  const bool success = receiver.ReceiveFile(&channel, o.output_dir, &saved, progress, status);
  if (success) {
    std::lock_guard lock(impl_->mutex);
    impl_->view.saved_path = saved;
  }
  return impl_->Finish(success, receiver.error());
}

}  // namespace cross_transfer
