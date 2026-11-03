#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TRANSFER_SESSION_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TRANSFER_SESSION_H_

#include "cross_transfer/protocol.h"
#include "cross_transfer/chunk_bitmap.h"
#include "cross_transfer/streaming_file_io.h"
#include "cross_transfer/transport_channel.h"
#include "cross_transfer/platform_adapter.h"
#include "cross_transfer/transfer_store.h"

#include <string>
#include <memory>
#include <functional>
#include <atomic>

namespace cross_transfer {

using ProgressCallback = std::function<void(uint64_t transferred_bytes, uint64_t total_bytes, float percentage)>;
using StatusCallback = std::function<void(const std::string& status_msg)>;

struct TransferOptions {
  uint32_t chunk_size{1024 * 1024}; // 1MB default chunk
  std::string device_name{"Desktop Node"};
  std::string device_os{"mac"};
  // Persist this in the caller's job record to resume after a process restart.
  TaskId task_id{};
  int io_timeout_ms{30000};
  // Separate finite budget for file preparation, destination setup and commit.
  int stage_timeout_ms{10 * 60 * 1000};
  uint32_t max_in_flight_bytes{4 * 1024 * 1024};
  // Production requires authenticated TLS or HTTPS; opt in only for tests.
  bool allow_insecure_transport{false};
  // Applied atomically at commit, in addition to the receiver admission policy.
  bool allow_overwrite{false};
  // Optional caller-owned cancellation token. It must outlive the session.
  // Session entry points never clear an external cancellation request.
  BorrowedPtr<std::atomic<bool>> external_cancellation{nullptr};
  std::function<bool(const HandshakePayload&, const FileMetadataPayload&)>
      authorize_receive;
};

enum class TransferState {
  kIdle, kPreparing, kPrepared, kHandshaking, kNegotiating, kTransferring,
  kVerifying, kCompleted, kCancelled, kFailed
};

class SenderSession {
 public:
  explicit SenderSession(TransferOptions options = {});
  ~SenderSession();

  // Prefer this before establishing a connection. Generates metadata/task ID
  // without network traffic. The source must remain unchanged until completion.
  bool PrepareFile(const std::string& file_path, StatusCallback on_status = nullptr);

  // Run on one dedicated worker sequence, never the UI/transport event loop.
  // Reconnecting with the same task ID negotiates durable receiver progress.
  bool SendFile(ITransportChannel* channel, const std::string& file_path,
                ProgressCallback on_progress = nullptr,
                StatusCallback on_status = nullptr);

  void Cancel();
  const TaskId& task_id() const { return task_id_; }
  TransferState state() const { return state_.load(); }
  const std::string& error() const { return error_; }

 private:
  std::atomic<bool>& CancellationFlag();
  TransferOptions options_;
  std::atomic<bool> cancelled_{false};
  std::atomic<TransferState> state_{TransferState::kIdle};
  TaskId task_id_{};
  std::string file_identity_;
  std::optional<FileMetadataPayload> prepared_metadata_;
  std::string prepared_path_;
  std::string error_;
};

class ReceiverSession {
 public:
  explicit ReceiverSession(TransferOptions options = {});
  ~ReceiverSession();

  // Receives an incoming file over the transport channel, saving to output_dir.
  // Returns true after verification and the required finish exchange. A
  // false result with committed() true still leaves a verified out_saved_path;
  // reconnecting with the same task can recover the lost confirmation.
  bool ReceiveFile(ITransportChannel* channel, const std::string& output_dir,
                   std::string* out_saved_path = nullptr,
                   ProgressCallback on_progress = nullptr,
                   StatusCallback on_status = nullptr);

  void Cancel();

  const ChunkBitmap& bitmap() const { return bitmap_; }
  // Durable local receipt is independent of delivery of the final confirmation.
  bool committed() const { return committed_.load(); }
  TransferState state() const { return state_.load(); }
  const std::string& error() const { return error_; }

 private:
  std::atomic<bool>& CancellationFlag();
  TransferOptions options_;
  ChunkBitmap bitmap_;
  std::atomic<bool> committed_{false};
  std::atomic<bool> cancelled_{false};
  std::atomic<TransferState> state_{TransferState::kIdle};
  std::string error_;
};

} // namespace cross_transfer

#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TRANSFER_SESSION_H_
