#include "cross_transfer/transfer_session.h"

#include <openssl/rand.h>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <future>

namespace cross_transfer {
namespace {
using Command = FrameHeader::Command;

// A session owns channel I/O for the worker call. An error closes the channel
// so the peer cannot wait indefinitely for an ACK that will never arrive.
class ChannelScope {
 public:
  ChannelScope(ITransportChannel* channel, const std::atomic<bool>* cancel,
               int timeout) : channel_(channel) {
    channel_->SetCancellationFlag(cancel);
    channel_->SetIoTimeout(timeout);
  }
  ~ChannelScope() {
    if (!completed) channel_->Close();
    channel_->SetCancellationFlag(nullptr);
  }
  bool completed{false};
 private:
  BorrowedPtr<ITransportChannel> channel_;
};

bool Send(ITransportChannel* channel, Command command, const TaskId& id,
          std::span<const uint8_t> payload = {}, uint64_t index = 0) {
  FrameHeader h;
  h.cmd = command;
  h.task_id = id;
  h.chunk_index = index;
  h.payload_len = static_cast<uint32_t>(payload.size());
  h.checksum = ComputeCRC32C(payload);
  return ValidateFrameHeader(h) && channel->SendFrame(h, payload);
}

bool Receive(ITransportChannel* channel, FrameHeader& h, std::vector<uint8_t>& data,
             const TaskId* id, int timeout_ms, int restore_io_ms) {
  channel->SetIoTimeout(timeout_ms);
  const bool ok = channel->ReadNextFrame(h, data);
  channel->SetIoTimeout(restore_io_ms);
  return ok && ValidateFrameHeader(h) && h.payload_len == data.size() &&
         h.checksum == ComputeCRC32C(data) && IsValidTaskId(h.task_id) &&
         (!id || h.task_id == *id);
}

// Disk work owns its state on one background job. Network I/O stays on the
// session worker. Joining precedes every return, so no job outlives its store,
// callbacks or cancellation token. Disk work never sends network traffic.
template <typename Work>
bool StorageJob(const std::atomic<bool>& cancelled, const TransferOptions& options,
                Work work) {
  std::atomic<bool> stop{false};
  auto future = std::async(std::launch::async, [&] { return work(&stop); });
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(options.stage_timeout_ms);
  const auto interval = std::chrono::milliseconds(25);
  bool healthy = true;
  while (future.wait_for(interval) != std::future_status::ready) {
    if (cancelled.load() || std::chrono::steady_clock::now() >= deadline) {
      stop = true;
      healthy = false;
      break;
    }
  }
  const bool result = future.get();
  return healthy && result && !cancelled.load() && std::chrono::steady_clock::now() < deadline;
}

void SendError(ITransportChannel* channel, const TaskId& id) {
  if (!IsValidTaskId(id) || !channel->IsConnected()) return;
  const std::string text = "Receiver rejected or could not commit transfer";
  Send(channel, Command::kError, id,
       std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

uint64_t ChunkBytes(const FileMetadataPayload& m, uint64_t index) {
  return std::min<uint64_t>(m.chunk_size, m.file_size - index * m.chunk_size);
}

bool Ready(ITransportChannel* channel, const TransferOptions& o) {
  return channel && channel->IsConnected() && o.io_timeout_ms > 0 && o.stage_timeout_ms > 0 &&
         (channel->IsSecure() || o.allow_insecure_transport);
}
}  // namespace

SenderSession::SenderSession(TransferOptions options)
    : options_(std::move(options)), task_id_(options_.task_id) {}
SenderSession::~SenderSession() = default;
std::atomic<bool>& SenderSession::CancellationFlag() {
  return options_.external_cancellation ? *options_.external_cancellation : cancelled_;
}
void SenderSession::Cancel() { CancellationFlag().store(true); }

bool SenderSession::PrepareFile(const std::string& path, StatusCallback status) {
  if (!options_.external_cancellation) cancelled_ = false;
  prepared_metadata_.reset();
  prepared_path_.clear();
  error_.clear();
  state_ = TransferState::kPreparing;
  auto fail = [&](const std::string& message) {
    error_ = CancellationFlag().load() ? "File preparation cancelled" : message;
    state_ = CancellationFlag().load() ? TransferState::kCancelled : TransferState::kFailed;
    if (status) status(error_);
    return false;
  };
  if (options_.stage_timeout_ms <= 0) return fail("A finite preparation deadline is required");
  ScopedSystemSleepBlocker sleep_guard("CrossTransfer file preparation");
  if (options_.chunk_size == 0 || options_.chunk_size > kMaxChunkSize ||
      options_.max_in_flight_bytes < options_.chunk_size ||
      options_.max_in_flight_bytes > 16 * 1024 * 1024)
    return fail("Invalid chunk size or in-flight byte limit");
  StreamingFileReader reader;
  if (!reader.Open(path)) return fail("Cannot open source file");
  FileMetadataPayload metadata;
  const auto filename = std::filesystem::path(std::u8string(path.begin(), path.end()))
                            .filename().u8string();
  metadata.file_name.assign(filename.begin(), filename.end());
  metadata.file_size = reader.file_size();
  metadata.chunk_size = options_.chunk_size;
  metadata.total_chunks = metadata.file_size / metadata.chunk_size +
                          (metadata.file_size % metadata.chunk_size != 0);
  if (metadata.file_size > kMaxFileSize || metadata.total_chunks > kMaxTotalChunks)
    return fail("Source file exceeds transfer limits");
  if (!IsValidTaskId(task_id_)) {
    if (RAND_bytes(task_id_.data(), task_id_.size()) != 1 || !IsValidTaskId(task_id_))
      return fail("Cannot generate task identity");
  }
  state_ = TransferState::kPreparing;
  if (!StorageJob(CancellationFlag(), options_, [&](const auto* stop) {
        metadata.file_sha256 = StreamingFileReader::ComputeFileSHA256(path, stop);
        return !metadata.file_sha256.empty();
      })) return fail("Source hashing failed, was cancelled or exceeded its deadline");
  if (!ValidateFileMetadata(metadata, &error_)) return fail(error_);
  const std::string identity = metadata.file_name + ":" + metadata.file_sha256 + ":" +
                               std::to_string(metadata.file_size) + ":" +
                               std::to_string(metadata.chunk_size);
  if (!file_identity_.empty() && file_identity_ != identity)
    return fail("Create a new task to send a different file");
  file_identity_ = identity;
  prepared_metadata_ = std::move(metadata);
  prepared_path_ = path;
  state_ = TransferState::kPrepared;
  return true;
}

bool SenderSession::SendFile(ITransportChannel* channel, const std::string& path,
                             ProgressCallback progress, StatusCallback status) {
  if (!options_.external_cancellation) cancelled_ = false;
  error_.clear();
  auto fail = [&](const std::string& message) {
    error_ = CancellationFlag().load() ? "Transfer cancelled" : message;
    state_ = CancellationFlag().load() ? TransferState::kCancelled : TransferState::kFailed;
    if (status) status(error_);
    return false;
  };
  if (!Ready(channel, options_)) return fail("An authenticated channel and finite timeout are required");
  ChannelScope scope(channel, &CancellationFlag(), options_.io_timeout_ms);
  ScopedSystemSleepBlocker sleep_guard("CrossTransfer sender");
  if ((!prepared_metadata_ || prepared_path_ != path) && !PrepareFile(path, status)) return false;
  const auto metadata = *prepared_metadata_;
  StreamingFileReader reader;
  if (!reader.Open(path) || reader.file_size() != metadata.file_size)
    return fail("Prepared source file changed or cannot be reopened");
  state_ = TransferState::kHandshaking;
  HandshakePayload request;
  request.device_name = options_.device_name;
  request.device_os = options_.device_os;
  request.accepted = true;
  auto data = request.Serialize();
  if (!Send(channel, Command::kHandshakeReq, task_id_, data)) return fail("Handshake send failed");
  FrameHeader header;
  if (!Receive(channel, header, data, &task_id_, options_.io_timeout_ms, options_.io_timeout_ms) || header.cmd != Command::kHandshakeResp)
    return fail("Invalid handshake response");
  auto response = HandshakePayload::Deserialize(data);
  if (!response || !response->accepted) return fail("Peer rejected handshake");
  state_ = TransferState::kNegotiating;
  data = metadata.Serialize();
  if (!Send(channel, Command::kFileMetadata, task_id_, data) ||
      !Receive(channel, header, data, &task_id_, options_.stage_timeout_ms, options_.io_timeout_ms) || header.cmd != Command::kChunkAck ||
      header.chunk_index != 0)
    return fail("Peer rejected file metadata");
  if (!Send(channel, Command::kSyncBitmapReq, task_id_) ||
      !Receive(channel, header, data, &task_id_, options_.io_timeout_ms, options_.io_timeout_ms) || header.cmd != Command::kSyncBitmapResp)
    return fail("Resume negotiation failed");
  auto bitmap = ChunkBitmap::Deserialize(data);
  if (!bitmap || bitmap->total_chunks() != metadata.total_chunks)
    return fail("Invalid resume bitmap");
  state_ = TransferState::kTransferring;
  uint64_t completed_bytes = 0;
  for (uint64_t i = 0; i < metadata.total_chunks; ++i)
    if (bitmap->IsCompleted(i)) completed_bytes += ChunkBytes(metadata, i);
  auto report = [&] {
    if (progress) progress(completed_bytes, metadata.file_size,
        metadata.file_size ? static_cast<float>(100.0 * completed_bytes / metadata.file_size) : 100.0f);
  };
  report();
  // Bounded window; ACKs confirm durable bytes and receiver checkpoints.
  std::deque<uint64_t> pending;
  const size_t window = std::clamp<size_t>(
      options_.max_in_flight_bytes / metadata.chunk_size, 1, 32);
  uint64_t next = 0;
  while (next < metadata.total_chunks || !pending.empty()) {
    if (CancellationFlag().load()) return fail("Cancelled");
    while (next < metadata.total_chunks && pending.size() < window) {
      const uint64_t index = next++;
      if (bitmap->IsCompleted(index)) continue;
      if (!reader.ReadChunk(index, metadata.chunk_size, data) ||
          data.size() != ChunkBytes(metadata, index)) return fail("Source file changed or read failed");
      if (!Send(channel, Command::kChunkData, task_id_, data, index)) return fail("Chunk send failed");
      pending.push_back(index);
    }
    if (pending.empty()) continue;
    if (!Receive(channel, header, data, &task_id_, options_.io_timeout_ms, options_.io_timeout_ms) || header.cmd != Command::kChunkAck ||
        header.chunk_index != pending.front()) return fail("Invalid or missing durable chunk ACK");
    completed_bytes += ChunkBytes(metadata, pending.front());
    pending.pop_front();
    report();
  }
  state_ = TransferState::kVerifying;
  if (!Send(channel, Command::kTransferFinish, task_id_) ||
      !Receive(channel, header, data, &task_id_, options_.stage_timeout_ms, options_.io_timeout_ms) || header.cmd != Command::kTransferFinish)
    return fail("Receiver did not commit a verified file");
  scope.completed = true;
  state_ = TransferState::kCompleted;
  if (status) status("Transfer completed and verified");
  return true;
}

ReceiverSession::ReceiverSession(TransferOptions options) : options_(std::move(options)) {}
ReceiverSession::~ReceiverSession() = default;
std::atomic<bool>& ReceiverSession::CancellationFlag() {
  return options_.external_cancellation ? *options_.external_cancellation : cancelled_;
}
void ReceiverSession::Cancel() { CancellationFlag().store(true); }

bool ReceiverSession::ReceiveFile(ITransportChannel* channel, const std::string& output_dir,
                                  std::string* out_saved_path,
                                  ProgressCallback progress, StatusCallback status) {
  if (!options_.external_cancellation) cancelled_ = false;
  error_.clear();
  bitmap_.Reset(0);
  if (out_saved_path) out_saved_path->clear();
  TaskId task_id{};
  auto fail = [&](const std::string& message) {
    error_ = CancellationFlag().load() ? "Transfer cancelled" : message;
    state_ = CancellationFlag().load() ? TransferState::kCancelled : TransferState::kFailed;
    if (channel && !CancellationFlag().load()) SendError(channel, task_id);
    if (status) status(error_);
    return false;
  };
  if (!Ready(channel, options_)) return fail("An authenticated channel and finite timeout are required");
  ChannelScope scope(channel, &CancellationFlag(), options_.io_timeout_ms);
  if (!options_.authorize_receive) return fail("Receiver admission policy is required");
  ScopedSystemSleepBlocker sleep_guard("CrossTransfer receiver");
  state_ = TransferState::kHandshaking;
  FrameHeader header;
  std::vector<uint8_t> data;
  if (!Receive(channel, header, data, nullptr, options_.stage_timeout_ms, options_.io_timeout_ms) || header.cmd != Command::kHandshakeReq)
    return fail("Invalid handshake request");
  task_id = header.task_id;
  auto peer = HandshakePayload::Deserialize(data);
  if (!peer) return fail("Invalid handshake payload");
  if (IsValidTaskId(options_.task_id) && options_.task_id != task_id)
    return fail("Unexpected task identity");
  HandshakePayload response;
  response.device_name = options_.device_name;
  response.device_os = options_.device_os;
  response.accepted = true;
  data = response.Serialize();
  if (!Send(channel, Command::kHandshakeResp, task_id, data)) return fail("Handshake response failed");
  state_ = TransferState::kNegotiating;
  if (!Receive(channel, header, data, &task_id, options_.io_timeout_ms, options_.io_timeout_ms) || header.cmd != Command::kFileMetadata)
    return fail("Expected file metadata");
  auto metadata = FileMetadataPayload::Deserialize(data);
  if (!metadata || !options_.authorize_receive(*peer, *metadata)) return fail("File was not authorized");
  TransferStore store;
  if (!StorageJob(CancellationFlag(), options_, [&](const auto* stop) {
        return store.Open(output_dir, *metadata, task_id, stop);
      })) return fail(store.error().empty() ? "Preparing destination exceeded its deadline" : store.error());
  auto snapshot = ChunkBitmap::Deserialize(store.bitmap().Serialize());
  if (!snapshot) return fail("Invalid stored checkpoint");
  bitmap_ = std::move(*snapshot);
  if (!Send(channel, Command::kChunkAck, task_id) ||
      !Receive(channel, header, data, &task_id, options_.io_timeout_ms, options_.io_timeout_ms) || header.cmd != Command::kSyncBitmapReq)
    return fail("Resume negotiation failed");
  data = store.bitmap().Serialize();
  if (!Send(channel, Command::kSyncBitmapResp, task_id, data)) return fail("Resume response failed");
  state_ = TransferState::kTransferring;
  uint64_t completed_bytes = 0;
  for (uint64_t i = 0; i < metadata->total_chunks; ++i)
    if (bitmap_.IsCompleted(i)) completed_bytes += ChunkBytes(*metadata, i);
  while (!bitmap_.IsAllCompleted()) {
    if (CancellationFlag().load()) return fail("Cancelled");
    if (!Receive(channel, header, data, &task_id, options_.io_timeout_ms, options_.io_timeout_ms)) return fail("Connection interrupted; progress saved");
    if (header.cmd == Command::kCancel) return fail("Sender cancelled transfer");
    if (header.cmd != Command::kChunkData || header.chunk_index >= metadata->total_chunks ||
        data.size() != ChunkBytes(*metadata, header.chunk_index)) return fail("Invalid file chunk");
    const bool duplicate = bitmap_.IsCompleted(header.chunk_index);
    if (!store.WriteChunk(header.chunk_index, data)) return fail(store.error());
    bitmap_.MarkCompleted(header.chunk_index);
    if (!duplicate) completed_bytes += data.size();
    if (!Send(channel, Command::kChunkAck, task_id, {}, header.chunk_index)) return fail("Chunk ACK failed");
    if (progress) progress(completed_bytes, metadata->file_size,
        metadata->file_size ? static_cast<float>(100.0 * completed_bytes / metadata->file_size) : 100.0f);
  }
  if (!Receive(channel, header, data, &task_id, options_.io_timeout_ms, options_.io_timeout_ms) || header.cmd != Command::kTransferFinish)
    return fail("Expected transfer finish");
  if (CancellationFlag().load()) return fail("Cancelled");
  state_ = TransferState::kVerifying;
  if (!StorageJob(CancellationFlag(), options_, [&](const auto* stop) {
        return store.VerifyAndCommit(out_saved_path, stop, options_.allow_overwrite);
      })) {
    auto saved = ChunkBitmap::Deserialize(store.bitmap().Serialize());
    if (saved) bitmap_ = std::move(*saved);
    return fail(store.error().empty() ? "Verification exceeded its deadline" : store.error());
  }
  if (!Send(channel, Command::kTransferFinish, task_id)) return fail("Commit ACK lost; reconnect to confirm");
  scope.completed = true;
  state_ = TransferState::kCompleted;
  if (progress) progress(metadata->file_size, metadata->file_size, 100.0f);
  if (status) status("File verified and committed");
  return true;
}
}  // namespace cross_transfer
