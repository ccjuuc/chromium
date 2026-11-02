#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_PROTOCOL_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_PROTOCOL_H_

#include <cstdint>
#include <array>
#include <vector>
#include <string>
#include <span>
#include <optional>

namespace cross_transfer {

// Shared wire/resource limits. Every transport validates before allocation.
inline constexpr uint32_t kMaxChunkSize = 4 * 1024 * 1024;
inline constexpr uint32_t kMaxFramePayload = kMaxChunkSize;
inline constexpr uint64_t kMaxTotalChunks = 8 * 1024 * 1024;
inline constexpr uint64_t kMaxFileSize = 8ULL * 1024 * 1024 * 1024 * 1024;
inline constexpr uint32_t kMaxBitmapPayload = 16 + (kMaxTotalChunks / 64) * 8;
using TaskId = std::array<uint8_t, 16>;

// This is an in-memory value, not a wire-layout struct. Keeping normal alignment
// avoids undefined accesses through integer member pointers on ARM/UBSan.
struct FrameHeader {
  static constexpr size_t kWireSize = 38;
  static constexpr uint16_t kMagic = 0x5854; // 'X', 'T'
  // v2 requires task identity, resume negotiation and explicit failure frames.
  static constexpr uint8_t kVersion = 2;

  enum class Command : uint8_t {
    kHandshakeReq   = 0x01,
    kHandshakeResp  = 0x02,
    kFileMetadata   = 0x03,
    kChunkData      = 0x04,
    kChunkAck       = 0x05,
    kSyncBitmapReq  = 0x06,
    kSyncBitmapResp = 0x07,
    kTransferFinish = 0x08,
    kCancel         = 0x09,
    kError          = 0x0A
  };

  uint16_t magic{kMagic};
  uint8_t version{kVersion};
  Command cmd{Command::kHandshakeReq};
  uint16_t flags{0};
  std::array<uint8_t, 16> task_id{0};
  uint64_t chunk_index{0};
  uint32_t payload_len{0};
  uint32_t checksum{0}; // CRC32-C of payload

  // Serialize header to network-byte-order (Big Endian)
  std::array<uint8_t, 38> Serialize() const;

  // Deserialize header from network-byte-order buffer
  static std::optional<FrameHeader> Deserialize(std::span<const uint8_t, 38> buffer);
};

// File metadata payload exchanged in kFileMetadata
struct FileMetadataPayload {
  std::string file_name;
  uint64_t file_size{0};
  uint32_t chunk_size{0};
  uint64_t total_chunks{0};
  std::string file_sha256;

  std::vector<uint8_t> Serialize() const;
  static std::optional<FileMetadataPayload> Deserialize(std::span<const uint8_t> buffer);
};

// Handshake payload exchanged in kHandshakeReq / kHandshakeResp
struct HandshakePayload {
  std::string device_name;
  std::string device_os;       // "mac", "win", "ios", "android"
  // Reserved peer metadata; this field does not establish authentication/E2EE.
  // Authentication is provided by the transport and receiver admission policy.
  std::string public_key_hex;
  bool accepted{false};
  std::string message;

  std::vector<uint8_t> Serialize() const;
  static std::optional<HandshakePayload> Deserialize(std::span<const uint8_t> buffer);
};

// Fast CRC32-C checksum calculation
uint32_t ComputeCRC32C(std::span<const uint8_t> data);

bool ValidateFrameHeader(const FrameHeader& header);
bool ValidateFileMetadata(const FileMetadataPayload& metadata,
                          std::string* error = nullptr);
bool IsValidTaskId(const TaskId& task_id);

} // namespace cross_transfer

#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_PROTOCOL_H_
