// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Portions copyright (c) 2019 GitHub, Inc. under the MIT license.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/asar/xenon_asar_url_loader_factory.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/byte_size.h"
#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/memory/self_deleting.h"
#include "base/numerics/safe_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/file_url_loader.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/file_data_source.h"
#include "net/base/filename_util.h"
#include "net/base/mime_sniffer.h"
#include "net/base/mime_util.h"
#include "net/http/http_byte_range.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/loading_params.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "xenon_overlay/common/asar/archive.h"

namespace xenon {

namespace {

net::Error ConvertMojoResultToNetError(MojoResult result) {
  switch (result) {
    case MOJO_RESULT_OK:
      return net::OK;
    case MOJO_RESULT_NOT_FOUND:
      return net::ERR_FILE_NOT_FOUND;
    case MOJO_RESULT_PERMISSION_DENIED:
      return net::ERR_ACCESS_DENIED;
    case MOJO_RESULT_RESOURCE_EXHAUSTED:
      return net::ERR_INSUFFICIENT_RESOURCES;
    case MOJO_RESULT_ABORTED:
      return net::ERR_ABORTED;
    default:
      return net::ERR_FAILED;
  }
}

uint32_t ResponsePipeSize(uint64_t body_size) {
  return base::saturated_cast<uint32_t>(std::clamp<uint64_t>(
      body_size, net::kMaxBytesToSniff,
      network::GetDataPipeDefaultAllocationSize(
          network::DataPipeAllocationSize::kLargerSizeIfPossible)));
}

class AsarURLLoader : public network::mojom::URLLoader {
 public:
  static void CreateAndStart(
      const network::ResourceRequest& request,
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client) {
    auto* asar_url_loader = new AsarURLLoader;
    asar_url_loader->Start(request, std::move(loader), std::move(client));
    ANALYZER_SKIP_THIS_PATH();
  }

  AsarURLLoader(const AsarURLLoader&) = delete;
  AsarURLLoader& operator=(const AsarURLLoader&) = delete;

  void FollowRedirect(
      network::HttpRequestHeadersUpdateParams headers_update_params,
      const std::optional<GURL>& new_url) override {}
  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {}

 private:
  AsarURLLoader() = default;
  ~AsarURLLoader() override = default;

  void Start(const network::ResourceRequest& request,
             mojo::PendingReceiver<network::mojom::URLLoader> loader,
             mojo::PendingRemote<network::mojom::URLLoaderClient> client) {
    base::FilePath path;
    if (!net::FileURLToFilePath(request.url, &path)) {
      mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
          ->OnComplete(network::URLLoaderCompletionStatus(net::ERR_FAILED));
      delete this;
      return;
    }

    base::FilePath archive_path;
    base::FilePath relative_path;
    if (!asar::GetAsarArchivePath(path, &archive_path, &relative_path)) {
      content::CreateFileURLLoaderBypassingSecurityChecks(
          request, std::move(loader), std::move(client), /*observer=*/nullptr,
          /*allow_directory_listing=*/false);
      delete this;
      return;
    }

    client_.Bind(std::move(client));
    receiver_.Bind(std::move(loader));
    receiver_.set_disconnect_handler(base::BindOnce(
        &AsarURLLoader::OnConnectionError, base::Unretained(this)));

    std::shared_ptr<asar::Archive> archive =
        asar::GetOrCreateAsarArchive(archive_path);
    asar::Archive::FileInfo info;
    if (!archive || !archive->GetFileInfo(relative_path, &info)) {
      OnClientComplete(net::ERR_FILE_NOT_FOUND);
      return;
    }

    base::File file;
    if (info.unpacked) {
      base::FilePath unpacked_path;
      if (!archive->GetUnpackedPath(relative_path, &unpacked_path)) {
        OnClientComplete(net::ERR_FILE_NOT_FOUND);
        return;
      }
      file = base::File(unpacked_path,
                        base::File::FLAG_OPEN | base::File::FLAG_READ |
                            base::File::FLAG_WIN_SHARE_DELETE);
      info.offset = 0;
    } else {
      file = archive->DuplicateFile();
    }
    if (!file.IsValid()) {
      OnClientComplete(net::ERR_FILE_NOT_FOUND);
      return;
    }

    net::HttpByteRange byte_range;
    if (std::optional<std::string> range_header =
            request.headers.GetHeader(net::HttpRequestHeaders::kRange);
        range_header) {
      std::vector<net::HttpByteRange> ranges;
      if (!net::HttpUtil::ParseRangeHeader(*range_header, &ranges) ||
          ranges.size() != 1 || !ranges[0].ComputeBounds(info.size)) {
        OnClientComplete(net::ERR_REQUEST_RANGE_NOT_SATISFIABLE);
        return;
      }
      byte_range = ranges[0];
    }

    uint64_t first_byte_to_send = 0;
    uint64_t total_bytes_to_send = info.size;
    if (byte_range.IsValid()) {
      first_byte_to_send = byte_range.first_byte_position();
      total_bytes_to_send =
          byte_range.last_byte_position() - first_byte_to_send + 1;
    }

    mojo::ScopedDataPipeProducerHandle producer_handle;
    mojo::ScopedDataPipeConsumerHandle consumer_handle;
    if (mojo::CreateDataPipe(ResponsePipeSize(total_bytes_to_send),
                             producer_handle,
                             consumer_handle) != MOJO_RESULT_OK) {
      OnClientComplete(net::ERR_FAILED);
      return;
    }

    auto file_data_source =
        std::make_unique<mojo::FileDataSource>(std::move(file));
    std::vector<char> initial_read_buffer(
        std::min<uint64_t>(net::kMaxBytesToSniff, info.size));
    auto read_result = file_data_source->Read(
        info.offset, base::span<char>(initial_read_buffer));
    if (read_result.result != MOJO_RESULT_OK) {
      OnClientComplete(ConvertMojoResultToNetError(read_result.result));
      return;
    }

    total_bytes_written_ = total_bytes_to_send;
    auto head = network::mojom::URLResponseHead::New();
    head->request_start = base::TimeTicks::Now();
    head->response_start = base::TimeTicks::Now();
    head->content_length = base::saturated_cast<int64_t>(total_bytes_to_send);
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        "HTTP/1.1 200 OK");

    if (first_byte_to_send < read_result.bytes_read) {
      const size_t write_size = std::min<uint64_t>(
          read_result.bytes_read - first_byte_to_send, total_bytes_to_send);
      const base::span<const uint8_t> bytes =
          base::as_byte_span(initial_read_buffer)
              .subspan(base::checked_cast<size_t>(first_byte_to_send),
                       write_size);
      size_t bytes_written = 0;
      const MojoResult result = producer_handle->WriteData(
          bytes, MOJO_WRITE_DATA_FLAG_NONE, bytes_written);
      if (result != MOJO_RESULT_OK || bytes_written != write_size) {
        OnFileWritten(result);
        return;
      }
      first_byte_to_send = read_result.bytes_read;
      total_bytes_to_send -= write_size;
    }

    if (!net::GetMimeTypeFromFile(path, &head->mime_type)) {
      std::string sniffed_type;
      net::SniffMimeType(
          std::string_view(initial_read_buffer.data(), read_result.bytes_read),
          request.url, /*type_hint=*/std::string(),
          net::ForceSniffFileUrlsForHtml::kDisabled, &sniffed_type);
      head->mime_type = std::move(sniffed_type);
      head->did_mime_sniff = true;
    }
    if (!head->mime_type.empty()) {
      head->headers->AddHeader(net::HttpRequestHeaders::kContentType,
                               head->mime_type);
    }
    client_->OnReceiveResponse(std::move(head), std::move(consumer_handle),
                               std::nullopt);

    if (total_bytes_to_send == 0) {
      OnFileWritten(MOJO_RESULT_OK);
      return;
    }

    file_data_source->SetRange(info.offset + first_byte_to_send,
                               info.offset + first_byte_to_send +
                                   total_bytes_to_send);
    data_producer_ =
        std::make_unique<mojo::DataPipeProducer>(std::move(producer_handle));
    data_producer_->Write(
        std::move(file_data_source),
        base::BindOnce(&AsarURLLoader::OnFileWritten, base::Unretained(this)));
  }

  void OnConnectionError() {
    data_producer_.reset();
    receiver_.reset();
    client_.reset();
    MaybeDeleteSelf();
  }

  void OnClientComplete(net::Error error) {
    client_->OnComplete(network::URLLoaderCompletionStatus(error));
    client_.reset();
    MaybeDeleteSelf();
  }

  void OnFileWritten(MojoResult result) {
    data_producer_.reset();
    if (result == MOJO_RESULT_OK) {
      network::URLLoaderCompletionStatus status(net::OK);
      status.encoded_data_length = base::ByteSize(total_bytes_written_);
      status.encoded_body_length = base::ByteSize(total_bytes_written_);
      status.decoded_body_length = base::ByteSize(total_bytes_written_);
      client_->OnComplete(status);
    } else {
      client_->OnComplete(network::URLLoaderCompletionStatus(net::ERR_FAILED));
    }
    client_.reset();
    MaybeDeleteSelf();
  }

  void MaybeDeleteSelf() {
    if (!receiver_.is_bound() && !client_.is_bound()) {
      delete this;
    }
  }

  std::unique_ptr<mojo::DataPipeProducer> data_producer_;
  mojo::Receiver<network::mojom::URLLoader> receiver_{this};
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  uint64_t total_bytes_written_ = 0;
};

void CreateAsarURLLoader(
    const network::ResourceRequest& request,
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client) {
  base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})
      ->PostTask(FROM_HERE,
                 base::BindOnce(&AsarURLLoader::CreateAndStart, request,
                                std::move(loader), std::move(client)));
}

class AsarURLLoaderFactory : public network::SelfDeletingURLLoaderFactory {
 public:
  AsarURLLoaderFactory(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      base::SelfDeletingPassKey key)
      : network::SelfDeletingURLLoaderFactory(std::move(receiver), key) {}

 private:
  ~AsarURLLoaderFactory() override = default;

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    CreateAsarURLLoader(request, std::move(loader), std::move(client));
  }
};

void BindFactoryOnIOThread(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver) {
  base::MakeSelfDeleting<AsarURLLoaderFactory>(std::move(receiver));
}

}  // namespace

mojo::PendingRemote<network::mojom::URLLoaderFactory>
CreateAsarURLLoaderFactory() {
  mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&BindFactoryOnIOThread,
                     remote.InitWithNewPipeAndPassReceiver()));
  return remote;
}

}  // namespace xenon
