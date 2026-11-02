#include "cross_transfer/transfer_store.h"
#include "cross_transfer/streaming_file_io.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

using namespace cross_transfer;
namespace {
struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory() {
    std::random_device rng;
    for (int i = 0; i < 100; ++i) {
      path = std::filesystem::temp_directory_path() /
          ("cross-transfer-store-" + std::to_string(rng()) + "-" + std::to_string(i));
      if (std::filesystem::create_directory(path)) return;
    }
    assert(false);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};
std::span<const uint8_t> Bytes(const std::string& value) {
  return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
FileMetadataPayload Metadata(const std::string& data, const std::string& name = "result.bin") {
  FileMetadataPayload metadata;
  metadata.file_name = name;
  metadata.file_size = data.size();
  metadata.chunk_size = 3;
  metadata.total_chunks = data.size() / 3 + (data.size() % 3 != 0);
  metadata.file_sha256 = ComputeSHA256(Bytes(data));
  return metadata;
}
std::array<uint8_t, 16> Id() { std::array<uint8_t, 16> id{}; id[0] = 1; return id; }
std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Put(const std::filesystem::path& path, const std::string& data) {
  std::ofstream output(path, std::ios::binary); output << data;
}
void TestResumeAndCommitAcknowledgement() {
  TemporaryDirectory dir;
  auto metadata = Metadata("abcdefg");
  {
    TransferStore store;
    assert(store.Open(dir.path.string(), metadata, Id()));
    assert(store.WriteChunk(0, Bytes("abc")));
    TransferStore competing;
    assert(!competing.Open(dir.path.string(), metadata, Id()));
    assert(!store.WriteChunk(1, Bytes("d")));
    assert(!store.WriteChunk(3, Bytes("abc")));
    assert(!store.VerifyAndCommit());
  }
  {
    TransferStore store;
    assert(store.Open(dir.path.string(), metadata, Id()));
    assert(store.bitmap().completed_chunks() == 1);
    assert(store.bitmap().IsCompleted(0));
    assert(store.WriteChunk(2, Bytes("g")));
    assert(store.WriteChunk(1, Bytes("def")));
    std::string saved;
    assert(store.VerifyAndCommit(&saved));
    assert(Read(saved) == "abcdefg");
  }
  // Reconnect after commit succeeded but its acknowledgement was lost.
  {
    std::atomic<bool> cancelled{true};
    TransferStore cancelled_open;
    assert(!cancelled_open.Open(dir.path.string(), metadata, Id(), &cancelled));
    TransferStore store;
    assert(store.Open(dir.path.string(), metadata, Id()));
    assert(store.bitmap().IsAllCompleted());
    assert(store.VerifyAndCommit());
    assert(Read(dir.path / "result.bin") == "abcdefg");
  }
}
void TestIdentityAndIntegrityFailure() {
  TemporaryDirectory dir;
  auto metadata = Metadata("abc");
  Put(dir.path / metadata.file_name, "original");
  {
    TransferStore store;
    assert(store.Open(dir.path.string(), metadata, Id()));
    assert(store.WriteChunk(0, Bytes("xyz")));
    assert(!store.VerifyAndCommit());
    assert(store.bitmap().completed_chunks() == 0);
    assert(Read(dir.path / metadata.file_name) == "original");
  }
  {
    TransferStore store;
    auto different = Metadata("xyz");
    assert(!store.Open(dir.path.string(), different, Id()));
    assert(Read(dir.path / metadata.file_name) == "original");
  }
  {
    TransferStore retry;
    assert(retry.Open(dir.path.string(), metadata, Id()));
    assert(retry.bitmap().completed_chunks() == 0);
    assert(retry.WriteChunk(0, Bytes("abc")));
    assert(retry.VerifyAndCommit(nullptr, nullptr, true));
    assert(Read(dir.path / metadata.file_name) == "abc");
  }
  auto state = dir.path / ".cross-transfer" / "01000000000000000000000000000000.state";
  Put(state, "corrupted checkpoint");
  TransferStore store;
  assert(!store.Open(dir.path.string(), metadata, Id()));
  assert(Read(dir.path / metadata.file_name) == "abc");
}
void TestInitialCheckpointFailureCanRetry() {
  TemporaryDirectory dir;
  auto metadata = Metadata("abc");
  Put(dir.path / metadata.file_name, "original");
  auto staging = dir.path / ".cross-transfer";
  assert(std::filesystem::create_directory(staging));
  auto partial = staging / "01000000000000000000000000000000.part";
  auto checkpoint = staging / "01000000000000000000000000000000.state";
  auto obstacle = staging / "01000000000000000000000000000000.state.next";
  // Force the first checkpoint to fail after its partial file was created.
  assert(std::filesystem::create_directory(obstacle));
  TransferStore failed;
  assert(!failed.Open(dir.path.string(), metadata, Id()));
  assert(!failed.error().empty());
  assert(!std::filesystem::exists(partial));
  assert(!std::filesystem::exists(checkpoint));
  assert(Read(dir.path / metadata.file_name) == "original");
  assert(std::filesystem::is_directory(obstacle));
  assert(std::filesystem::remove(obstacle));

  // A failed Open also releases its lock, even while that store is still alive.
  TransferStore retry;
  assert(retry.Open(dir.path.string(), metadata, Id()));
  assert(retry.bitmap().completed_chunks() == 0);
  assert(retry.WriteChunk(0, Bytes("abc")));
  assert(retry.VerifyAndCommit(nullptr, nullptr, true));
  assert(Read(dir.path / metadata.file_name) == "abc");
}
void TestUntrustedPartialIsPreserved() {
  TemporaryDirectory dir;
  auto staging = dir.path / ".cross-transfer";
  assert(std::filesystem::create_directory(staging));
  auto partial = staging / "01000000000000000000000000000000.part";
  Put(partial, "untrusted partial");
  TransferStore store;
  assert(!store.Open(dir.path.string(), Metadata("abc"), Id()));
  assert(Read(partial) == "untrusted partial");
}
void TestLateDestinationRequiresOverwriteConsent() {
  TemporaryDirectory dir;
  auto metadata = Metadata("abc");
  {
    TransferStore store;
    assert(store.Open(dir.path.string(), metadata, Id()));
    assert(store.WriteChunk(0, Bytes("abc")));
    // Another application creates the destination after admission/preparation.
    Put(dir.path / metadata.file_name, "created-during-transfer");
    assert(!store.VerifyAndCommit());
    assert(Read(dir.path / metadata.file_name) == "created-during-transfer");
  }
  // Refusing the collision keeps all acknowledged bytes available to resume.
  TransferStore retry;
  assert(retry.Open(dir.path.string(), metadata, Id()));
  assert(retry.bitmap().IsAllCompleted());
  assert(retry.VerifyAndCommit(nullptr, nullptr, true));
  assert(Read(dir.path / metadata.file_name) == "abc");
}
void TestEmptyAndUnsafePaths() {
  TemporaryDirectory dir;
  {
    TransferStore store;
    assert(store.Open(dir.path.string(), Metadata(""), Id()));
    assert(store.bitmap().IsAllCompleted());
    assert(store.VerifyAndCommit());
    assert(std::filesystem::file_size(dir.path / "result.bin") == 0);
  }
  for (auto name : {"../outside", "/absolute", "back\\slash", "bad:name", ".cross-transfer"}) {
    TransferStore store;
    assert(!store.Open(dir.path.string(), Metadata("abc", name), Id()));
  }
#if !defined(_WIN32)
  auto outside = dir.path / "outside.bin";
  Put(outside, "original");
  auto destination = dir.path / "link.bin";
  std::filesystem::create_symlink(outside, destination);
  TransferStore store;
  assert(!store.Open(dir.path.string(), Metadata("abc", "link.bin"), Id()));
  assert(Read(outside) == "original");
#endif
}
void TestWriterAtomicFailureAndExplicitResume() {
  TemporaryDirectory dir;
  std::string target = (dir.path / "target.bin").string();
  Put(target, "original");
  StreamingFileWriter writer;
  assert(writer.Create(target + ".part", 6));
  assert(writer.WriteChunk(0, 3, Bytes("abc")));
  writer.Close();
  assert(!writer.Create(target + ".part", 6));
  assert(writer.Resume(target + ".part", 6));
  assert(writer.WriteChunk(1, 3, Bytes("def")));
  assert(writer.ComputeSHA256() == ComputeSHA256(Bytes("abcdef")));
  writer.Close();
  std::filesystem::remove(target + ".part");
  assert(!writer.Finalize(target));
  assert(Read(target) == "original");
}
void TestSha256AndCancellation() {
  assert(IsValidUtf8Path("文件.bin"));
  assert(!IsValidUtf8Path(std::string("bad\0path", 8)));
  assert(!IsValidUtf8Path("\xc0\xaf"));
  assert(!IsValidUtf8Path("\xed\xa0\x80"));
  assert(!IsValidUtf8Path("\xf4\x90\x80\x80"));
  assert(ComputeSHA256(Bytes("abc")) ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert(ComputeSHA256({}) ==
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  TemporaryDirectory dir;
  auto target = dir.path / "source";
  Put(target, "abc");
  std::atomic<bool> cancelled{true};
  assert(StreamingFileReader::ComputeFileSHA256(target.string(), &cancelled).empty());
  TransferStore store;
  assert(store.Open(dir.path.string(), Metadata("abc"), Id()));
  assert(store.WriteChunk(0, Bytes("abc")));
  assert(!store.VerifyAndCommit(nullptr, &cancelled));
  assert(!std::filesystem::exists(dir.path / "result.bin"));
}
}  // namespace
int main() {
  TestResumeAndCommitAcknowledgement();
  TestIdentityAndIntegrityFailure();
  TestInitialCheckpointFailureCanRetry();
  TestUntrustedPartialIsPreserved();
  TestLateDestinationRequiresOverwriteConsent();
  TestEmptyAndUnsafePaths();
  TestWriterAtomicFailureAndExplicitResume();
  TestSha256AndCancellation();
  std::cout << "All TransferStore regression tests passed\n";
}
