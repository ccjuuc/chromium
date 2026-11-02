#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TEST_JOB_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TEST_JOB_H_

#include <cstdint>
#include <memory>
#include <string>

namespace cross_transfer {

// Native test-page input. Paths are UTF-8. Direct connections always use mutual
// TLS; relay connections always use the authenticated HTTPS relay protocol.
struct TestJobOptions {
  std::string role{"send"};                 // "send" or "receive"
  std::string transport{"direct"};          // "direct" or "relay"
  std::string source_path;
  std::string output_dir;
  std::string host;                          // Also the receiver's bind address.
  uint16_t port{0};                          // Receive/direct may request port 0.
  std::string ca_file;                       // Empty uses the default trust store.
  std::string certificate_file;
  std::string private_key_file;
  std::string peer_name;
  std::string room_id;
  std::string connection_id;                 // Shared per relay attempt, 32 hex.
  std::string bearer_token;                  // Private, never included in status.
  std::string task_id;                       // Persistent file identity, 32 hex.
  bool allow_overwrite{false};
  int connect_timeout_ms{30000};
  int io_timeout_ms{30000};
  int stage_timeout_ms{10 * 60 * 1000};
};

// Only scalar display data is exposed to the WebUI. No TLS or relay credentials
// are returned. Byte counts describe durably acknowledged file data.
struct TestJobSnapshot {
  std::string state{"idle"};
  std::string status;
  std::string error;
  std::string task_id;
  std::string saved_path;
  uint64_t bytes{0};
  uint64_t total{0};
  double percent{0};
  uint16_t listening_port{0};
  bool active{false};
};

class TestTransferJob {
 public:
  explicit TestTransferJob(TestJobOptions options);
  ~TestTransferJob();
  TestTransferJob(const TestTransferJob&) = delete;
  TestTransferJob& operator=(const TestTransferJob&) = delete;

  // Call once on a dedicated worker. The caller must retain the job until Run
  // returns; Cancel does not join that worker or destroy its transport.
  bool Run();
  void Cancel();                            // Safe from any thread, including UI.
  TestJobSnapshot snapshot() const;          // Safe from any thread; no file I/O.

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cross_transfer

#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TEST_JOB_H_
