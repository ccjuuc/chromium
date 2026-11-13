// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_utility_process_pipe.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/string_number_conversions.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/unguessable_token.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/strings/utf_string_conversions.h"
#include "base/win/scoped_handle.h"
#elif BUILDFLAG(IS_POSIX)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>

#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/posix/eintr_wrapper.h"
#endif

namespace xenon::ipc {

UtilityProcessPipe::UtilityProcessPipe(base::File reader)
    : reader_(std::move(reader)) {}
UtilityProcessPipe::~UtilityProcessPipe() = default;

scoped_refptr<UtilityProcessPipe> UtilityProcessPipe::Create(
    base::File* writer,
    std::string* error) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
#if BUILDFLAG(IS_WIN)
  const std::wstring name =
      L"\\\\.\\pipe\\xenon-utility-" +
      base::UTF8ToWide(base::UnguessableToken::Create().ToString());
  base::win::ScopedHandle reader(
      CreateNamedPipeW(name.c_str(),
                       PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
                           FILE_FLAG_FIRST_PIPE_INSTANCE,
                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                           PIPE_REJECT_REMOTE_CLIENTS,
                       1, 65536, 65536, 0, nullptr));
  if (!reader.is_valid()) {
    *error = "Cannot create utility output pipe: " +
             base::NumberToString(GetLastError());
    return {};
  }
  base::win::ScopedHandle write_handle(
      CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!write_handle.is_valid()) {
    *error = "Cannot open utility output pipe: " +
             base::NumberToString(GetLastError());
    return {};
  }
  *writer = base::File(write_handle.release());
  return base::WrapRefCounted(
      new UtilityProcessPipe(base::File(reader.release())));
#elif BUILDFLAG(IS_POSIX)
  base::ScopedFD reader;
  base::ScopedFD write_handle;
  if (!base::CreatePipe(&reader, &write_handle, true)) {
    *error = "Cannot create utility output pipe";
    return {};
  }
  const int flags = fcntl(reader.get(), F_GETFL);
  const int write_flags = fcntl(write_handle.get(), F_GETFL);
  if (flags < 0 || write_flags < 0 ||
      fcntl(reader.get(), F_SETFL, flags | O_NONBLOCK) < 0 ||
      fcntl(write_handle.get(), F_SETFL, write_flags & ~O_NONBLOCK) < 0) {
    *error = "Cannot configure utility output pipe";
    return {};
  }
  *writer = base::File(write_handle.release());
  return base::WrapRefCounted(
      new UtilityProcessPipe(base::File(reader.release())));
#else
  *error = "ERR_NOT_SUPPORTED: Utility output pipes are unavailable";
  return {};
#endif
}

void UtilityProcessPipe::Cancel() {
  cancelled_.store(true, std::memory_order_release);
}

void UtilityProcessPipe::SetReadPendingCallbackForTesting(
    base::OnceClosure callback) {
  CHECK(!reading_.load());
  read_pending_for_testing_ = std::move(callback);
}

UtilityProcessPipe::ReadResult UtilityProcessPipe::Read(
    uint32_t maximum_bytes) {
  ReadResult result;
  if (maximum_bytes == 0 || maximum_bytes > 65536) {
    result.error =
        "ERR_OUT_OF_RANGE: Utility pipe reads require 1..65536 bytes";
    return result;
  }
  if (reading_.exchange(true)) {
    result.error = "ERR_RESOURCE_BUSY: Utility pipe already has a pending read";
    return result;
  }
  base::ScopedClosureRunner reset(base::BindOnce(
      [](std::atomic<bool>* reading) { reading->store(false); }, &reading_));
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  if (cancelled_.load(std::memory_order_acquire)) {
    result.eof = true;
    return result;
  }
  result.data.resize(maximum_bytes);
#if BUILDFLAG(IS_WIN)
  base::win::ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.is_valid()) {
    result.data.clear();
    result.error = "Cannot create utility pipe read event";
    return result;
  }
  OVERLAPPED operation{};
  operation.hEvent = event.get();
  DWORD read = 0;
  BOOL success = ReadFile(reader_.GetPlatformFile(), result.data.data(),
                          maximum_bytes, &read, &operation);
  DWORD status = success ? ERROR_SUCCESS : GetLastError();
  if (!success && status == ERROR_IO_PENDING) {
    if (read_pending_for_testing_) {
      std::move(read_pending_for_testing_).Run();
    }
    while (WaitForSingleObject(event.get(), 100) == WAIT_TIMEOUT) {
      if (cancelled_.load(std::memory_order_acquire)) {
        CancelIoEx(reader_.GetPlatformFile(), &operation);
        break;
      }
    }
    // Cancellation must finish before the stack OVERLAPPED/buffer are freed.
    success =
        GetOverlappedResult(reader_.GetPlatformFile(), &operation, &read, TRUE);
    status = success ? ERROR_SUCCESS : GetLastError();
  }
  if (success) {
    result.data.resize(read);
    result.eof = read == 0;
  } else {
    result.data.clear();
    result.eof = status == ERROR_BROKEN_PIPE ||
                 status == ERROR_PIPE_NOT_CONNECTED ||
                 (status == ERROR_OPERATION_ABORTED && cancelled_.load());
    if (!result.eof) {
      result.error =
          "Utility pipe read failed: " + base::NumberToString(status);
    }
  }
#elif BUILDFLAG(IS_POSIX)
  while (!cancelled_.load(std::memory_order_acquire)) {
    pollfd descriptor{reader_.GetPlatformFile(), POLLIN, 0};
    if (read_pending_for_testing_ &&
        HANDLE_EINTR(poll(&descriptor, 1, 0)) == 0) {
      std::move(read_pending_for_testing_).Run();
    }
    const int ready = HANDLE_EINTR(poll(&descriptor, 1, 100));
    if (ready == 0) {
      continue;
    }
    if (ready < 0) {
      result.error = "Utility pipe poll failed";
      break;
    }
    const auto read = reader_.ReadAtCurrentPos(result.data);
    if (!read.has_value()) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      result.error = "Utility pipe read failed";
      break;
    }
    result.data.resize(*read);
    result.eof = *read == 0;
    return result;
  }
  result.data.clear();
  result.eof = cancelled_.load();
#else
  result.data.clear();
  result.error = "ERR_NOT_SUPPORTED: Utility pipe reads are unavailable";
#endif
  return result;
}

}  // namespace xenon::ipc
