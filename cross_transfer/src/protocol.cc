#include "cross_transfer/protocol.h"

#include <algorithm>
#include <bit>
#include <cctype>

namespace cross_transfer {

namespace {

inline uint64_t HostToNet64(uint64_t val) {
  if constexpr (std::endian::native == std::endian::big) return val;
  return ((val & 0xFF00000000000000ULL) >> 56) |
         ((val & 0x00FF000000000000ULL) >> 40) |
         ((val & 0x0000FF0000000000ULL) >> 24) |
         ((val & 0x000000FF00000000ULL) >> 8)  |
         ((val & 0x00000000FF000000ULL) << 8)  |
         ((val & 0x0000000000FF0000ULL) << 24) |
         ((val & 0x000000000000FF00ULL) << 40) |
         ((val & 0x00000000000000FFULL) << 56);
}

inline uint64_t NetToHost64(uint64_t val) {
  return HostToNet64(val);
}

inline uint32_t HostToNet32(uint32_t val) {
  if constexpr (std::endian::native == std::endian::big) return val;
  return ((val & 0xFF000000) >> 24) |
         ((val & 0x00FF0000) >> 8)  |
         ((val & 0x0000FF00) << 8)  |
         ((val & 0x000000FF) << 24);
}

inline uint32_t NetToHost32(uint32_t val) {
  return HostToNet32(val);
}

inline uint16_t HostToNet16(uint16_t val) {
  if constexpr (std::endian::native == std::endian::big) return val;
  return static_cast<uint16_t>(((val & 0xFF00) >> 8) | ((val & 0x00FF) << 8));
}

inline uint16_t NetToHost16(uint16_t val) {
  return HostToNet16(val);
}

// CRC32-C (Castagnoli) Table
constexpr uint32_t kCRC32CPoly = 0x82F63B78;

std::array<uint32_t, 256> GenerateCRC32CTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int j = 0; j < 8; ++j) {
      if (crc & 1) {
        crc = (crc >> 1) ^ kCRC32CPoly;
      } else {
        crc >>= 1;
      }
    }
    table[i] = crc;
  }
  return table;
}

const auto kCRC32CTable = GenerateCRC32CTable();

// Safe bounds-checking binary buffer reader
class ByteReader {
 public:
  explicit ByteReader(std::span<const uint8_t> buffer) : buffer_(buffer) {}

  bool Read16(uint16_t* out) {
    uint16_t net;
    if (!ReadInteger(&net)) return false;
    *out = NetToHost16(net);
    return true;
  }

  bool Read32(uint32_t* out) {
    uint32_t net;
    if (!ReadInteger(&net)) return false;
    *out = NetToHost32(net);
    return true;
  }

  bool Read64(uint64_t* out) {
    uint64_t net;
    if (!ReadInteger(&net)) return false;
    *out = NetToHost64(net);
    return true;
  }

  bool Read8(uint8_t* out) {
    if (Remaining() < 1) return false;
    *out = buffer_[offset_++];
    return true;
  }

  bool ReadBytes(std::span<uint8_t> dst) {
    if (dst.size() > Remaining()) return false;
    const auto source = buffer_.subspan(offset_, dst.size());
    std::copy(source.begin(), source.end(), dst.begin());
    offset_ += dst.size();
    return true;
  }

  bool ReadString(std::string* out) {
    uint16_t len = 0;
    if (!Read16(&len)) return false;
    if (len > Remaining()) return false;
    const auto bytes = buffer_.subspan(offset_, len);
    out->assign(bytes.begin(), bytes.end());
    offset_ += len;
    return true;
  }

  size_t Remaining() const { return buffer_.size() - offset_; }

 private:
  template <typename T>
  bool ReadInteger(T* out) {
    std::array<uint8_t, sizeof(T)> bytes{};
    if (!ReadBytes(bytes)) return false;
    *out = std::bit_cast<T>(bytes);
    return true;
  }

  std::span<const uint8_t> buffer_;
  size_t offset_{0};
};

} // namespace

uint32_t ComputeCRC32C(std::span<const uint8_t> data) {
  uint32_t crc = 0xFFFFFFFF;
  for (uint8_t byte : data) {
    crc = (crc >> 8) ^ kCRC32CTable[(crc ^ byte) & 0xFF];
  }
  return crc ^ 0xFFFFFFFF;
}

bool IsValidTaskId(const TaskId& id) {
  return std::any_of(id.begin(), id.end(), [](uint8_t b) { return b != 0; });
}

bool ValidateFrameHeader(const FrameHeader& h) {
  if (h.magic != FrameHeader::kMagic || h.version != FrameHeader::kVersion ||
      h.flags != 0 || h.payload_len > kMaxFramePayload ||
      (h.payload_len == 0 && h.checksum != 0)) return false;
  using C = FrameHeader::Command;
  switch (h.cmd) {
    case C::kHandshakeReq:
    case C::kHandshakeResp:
      return h.chunk_index == 0 && h.payload_len >= 9 && h.payload_len <= 16384;
    case C::kFileMetadata:
      return h.chunk_index == 0 && h.payload_len >= 24 && h.payload_len <= 1024;
    case C::kChunkData:
      return h.chunk_index < kMaxTotalChunks && h.payload_len > 0;
    case C::kChunkAck:
      return h.chunk_index < kMaxTotalChunks && h.payload_len == 0;
    case C::kSyncBitmapReq:
    case C::kTransferFinish:
    case C::kCancel:
      return h.chunk_index == 0 && h.payload_len == 0;
    case C::kSyncBitmapResp:
      return h.chunk_index == 0 && h.payload_len >= 16 &&
             h.payload_len <= kMaxBitmapPayload;
    case C::kError:
      return h.chunk_index == 0 && h.payload_len > 0 && h.payload_len <= 1024;
  }
  return false;
}

bool ValidateFileMetadata(const FileMetadataPayload& m, std::string* error) {
  auto fail = [&](const char* reason) { if (error) *error = reason; return false; };
  if (m.file_name.empty() || m.file_name.size() > 255 || m.file_name == "." ||
      m.file_name == ".." || m.file_name.back() == '.' || m.file_name.back() == ' ')
    return fail("Invalid file name");
  for (unsigned char c : m.file_name) {
    if (c < 32 || c == 127 || std::string_view("/\\:<>\"|?*").find(c) != std::string_view::npos)
      return fail("File name must be a portable single component");
  }
  std::string stem = m.file_name.substr(0, m.file_name.find('.'));
  std::transform(stem.begin(), stem.end(), stem.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
      (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
       stem[3] >= '1' && stem[3] <= '9')) return fail("Reserved file name");
  if (m.chunk_size == 0 || m.chunk_size > kMaxChunkSize || m.file_size > kMaxFileSize)
    return fail("File or chunk size exceeds limits");
  const uint64_t expected = m.file_size / m.chunk_size + (m.file_size % m.chunk_size != 0);
  if (expected != m.total_chunks || expected > kMaxTotalChunks)
    return fail("Inconsistent chunk count");
  if (m.file_sha256.size() != 64 || !std::all_of(m.file_sha256.begin(), m.file_sha256.end(),
      [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
    return fail("Expected lowercase SHA-256 digest");
  if (error) error->clear();
  return true;
}

std::array<uint8_t, 38> FrameHeader::Serialize() const {
  std::array<uint8_t, 38> out{};
  size_t offset = 0;

  auto writeInteger = [&]<typename T>(T value) {
    const auto bytes = std::bit_cast<std::array<uint8_t, sizeof(T)>>(value);
    const auto destination = std::span(out).subspan(offset, bytes.size());
    std::copy(bytes.begin(), bytes.end(), destination.begin());
    offset += bytes.size();
  };
  auto write16 = [&](uint16_t val) {
    writeInteger(HostToNet16(val));
  };
  auto write8 = [&](uint8_t val) {
    out[offset++] = val;
  };
  auto write32 = [&](uint32_t val) {
    writeInteger(HostToNet32(val));
  };
  auto write64 = [&](uint64_t val) {
    writeInteger(HostToNet64(val));
  };

  write16(magic);
  write8(version);
  write8(static_cast<uint8_t>(cmd));
  write16(flags);

  const auto task_bytes = std::span(out).subspan(offset, task_id.size());
  std::copy(task_id.begin(), task_id.end(), task_bytes.begin());
  offset += task_id.size();

  write64(chunk_index);
  write32(payload_len);
  write32(checksum);

  return out;
}

std::optional<FrameHeader> FrameHeader::Deserialize(std::span<const uint8_t, 38> buffer) {
  ByteReader reader(buffer);
  FrameHeader header;

  if (!reader.Read16(&header.magic) || header.magic != kMagic) return std::nullopt;
  if (!reader.Read8(&header.version) || header.version != kVersion) return std::nullopt;

  uint8_t cmd_byte = 0;
  if (!reader.Read8(&cmd_byte)) return std::nullopt;
  header.cmd = static_cast<Command>(cmd_byte);

  if (!reader.Read16(&header.flags)) return std::nullopt;
  if (!reader.ReadBytes(header.task_id)) return std::nullopt;
  if (!reader.Read64(&header.chunk_index)) return std::nullopt;
  if (!reader.Read32(&header.payload_len)) return std::nullopt;
  if (!reader.Read32(&header.checksum)) return std::nullopt;
  if (!ValidateFrameHeader(header)) return std::nullopt;
  return header;
}

std::vector<uint8_t> FileMetadataPayload::Serialize() const {
  if (!ValidateFileMetadata(*this)) return {};
  std::vector<uint8_t> buffer;
  auto append16 = [&](uint16_t v) {
    const auto bytes = std::bit_cast<std::array<uint8_t, 2>>(HostToNet16(v));
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
  };
  auto append32 = [&](uint32_t v) {
    const auto bytes = std::bit_cast<std::array<uint8_t, 4>>(HostToNet32(v));
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
  };
  auto append64 = [&](uint64_t v) {
    const auto bytes = std::bit_cast<std::array<uint8_t, 8>>(HostToNet64(v));
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
  };
  auto appendStr = [&](const std::string& str) {
    append16(static_cast<uint16_t>(str.size()));
    buffer.insert(buffer.end(), str.begin(), str.end());
  };

  appendStr(file_name);
  append64(file_size);
  append32(chunk_size);
  append64(total_chunks);
  appendStr(file_sha256);

  return buffer;
}

std::optional<FileMetadataPayload> FileMetadataPayload::Deserialize(std::span<const uint8_t> buffer) {
  ByteReader reader(buffer);
  FileMetadataPayload meta;

  if (!reader.ReadString(&meta.file_name)) return std::nullopt;
  if (!reader.Read64(&meta.file_size)) return std::nullopt;
  if (!reader.Read32(&meta.chunk_size)) return std::nullopt;
  if (!reader.Read64(&meta.total_chunks)) return std::nullopt;
  if (!reader.ReadString(&meta.file_sha256)) return std::nullopt;
  if (reader.Remaining() != 0 || !ValidateFileMetadata(meta)) return std::nullopt;
  return meta;
}

std::vector<uint8_t> HandshakePayload::Serialize() const {
  if (device_name.size() > 256 || device_os.size() > 32 ||
      public_key_hex.size() > 512 || message.size() > 1024) return {};
  std::vector<uint8_t> buffer;
  auto append16 = [&](uint16_t v) {
    const auto bytes = std::bit_cast<std::array<uint8_t, 2>>(HostToNet16(v));
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
  };
  auto appendStr = [&](const std::string& str) {
    append16(static_cast<uint16_t>(str.size()));
    buffer.insert(buffer.end(), str.begin(), str.end());
  };

  appendStr(device_name);
  appendStr(device_os);
  appendStr(public_key_hex);
  buffer.push_back(accepted ? 1 : 0);
  appendStr(message);

  return buffer;
}

std::optional<HandshakePayload> HandshakePayload::Deserialize(std::span<const uint8_t> buffer) {
  ByteReader reader(buffer);
  HandshakePayload payload;

  if (!reader.ReadString(&payload.device_name)) return std::nullopt;
  if (!reader.ReadString(&payload.device_os)) return std::nullopt;
  if (!reader.ReadString(&payload.public_key_hex)) return std::nullopt;

  uint8_t accepted_byte = 0;
  if (!reader.Read8(&accepted_byte)) return std::nullopt;
  if (accepted_byte > 1) return std::nullopt;
  payload.accepted = (accepted_byte != 0);

  if (!reader.ReadString(&payload.message)) return std::nullopt;
  if (reader.Remaining() != 0 || payload.Serialize().empty()) return std::nullopt;
  return payload;
}

} // namespace cross_transfer
