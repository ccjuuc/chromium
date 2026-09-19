// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/asar/xenon_asar_url_loader_factory.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_writer.h"
#include "base/pickle.h"
#include "base/values.h"
#include "content/public/test/browser_task_environment.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "net/base/filename_util.h"
#include "net/base/net_errors.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/test/test_url_loader_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/asar/test_support.h"

namespace xenon {
namespace {

// An integral number of cipher blocks keeps the same expected response in the
// standard and encrypted cases without discarding the format's space padding.
constexpr char kBody[] = "local resource fixture contents\n";
static_assert((sizeof(kBody) - 1) % 16 == 0);
enum class FileKind {
  kPlain,
  kPacked,
  kUnpacked,
  kEncryptedPacked,
  kEncryptedHeaderPacked,
  kEncryptedHeaderUnpacked
};

class XenonAsarURLLoaderFactoryTest : public testing::TestWithParam<FileKind> {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    base::FilePath path = temp_dir_.GetPath().AppendASCII("fixture.txt");
    if (GetParam() == FileKind::kPlain) {
      ASSERT_TRUE(base::WriteFile(path, kBody));
    } else if (GetParam() == FileKind::kEncryptedPacked ||
               GetParam() == FileKind::kEncryptedHeaderPacked ||
               GetParam() == FileKind::kEncryptedHeaderUnpacked) {
      const auto archive_path = temp_dir_.GetPath().AppendASCII("fixture.asar");
      asar::TestArchiveBuilder builder;
      ASSERT_TRUE(builder.Write(
          archive_path,
          {{"fixture.txt", kBody, GetParam() == FileKind::kEncryptedPacked,
            GetParam() == FileKind::kEncryptedHeaderUnpacked}}));
      ASSERT_TRUE(asar::SetArchivePublicKeys(
          "url-loader-test",
          {{temp_dir_.GetPath(), builder.public_key_pem()}}));
      path = archive_path.AppendASCII("fixture.txt");
    } else {
      base::DictValue entry;
      entry.Set("size", static_cast<int>(sizeof(kBody) - 1));
      if (GetParam() == FileKind::kUnpacked) {
        entry.Set("unpacked", true);
      } else {
        entry.Set("offset", "0");
      }
      const auto json = base::WriteJson(base::DictValue().Set(
          "files", base::DictValue().Set("fixture.txt", std::move(entry))));
      ASSERT_TRUE(json);
      base::Pickle header;
      header.WriteString(*json);
      base::Pickle size;
      size.WriteUInt32(static_cast<uint32_t>(header.AsBytes().size()));
      std::string archive(size.AsStringView());
      archive.append(header.AsStringView());
      if (GetParam() == FileKind::kPacked) {
        archive.append(kBody);
      }
      const auto archive_path = temp_dir_.GetPath().AppendASCII("fixture.asar");
      ASSERT_TRUE(base::WriteFile(archive_path, archive));
      if (GetParam() == FileKind::kUnpacked) {
        const auto unpacked =
            archive_path.AddExtension(FILE_PATH_LITERAL("unpacked"));
        ASSERT_TRUE(base::CreateDirectory(unpacked));
        ASSERT_TRUE(
            base::WriteFile(unpacked.AppendASCII("fixture.txt"), kBody));
      }
      path = archive_path.AppendASCII("fixture.txt");
    }
    url_ = net::FilePathToFileURL(path);
    factory_.Bind(CreateAsarURLLoaderFactory());
  }

  void TearDown() override {
    loader_.reset();
    factory_.reset();
    task_environment_.RunUntilIdle();
    asar::RemoveArchivePublicKeys("url-loader-test");
  }

  void Load(network::mojom::RequestMode mode,
            std::optional<url::Origin> initiator,
            network::TestURLLoaderClient& client,
            std::string_view range = {}) {
    network::ResourceRequest request;
    request.url = url_;
    request.mode = mode;
    request.request_initiator = std::move(initiator);
    if (!range.empty()) {
      request.headers.SetHeader("Range", range);
    }
    factory_->CreateLoaderAndStart(
        loader_.BindNewPipeAndPassReceiver(), 1, 0, request, client.CreateRemote(),
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS));
    client.RunUntilComplete();
  }

  void ExpectBody(network::TestURLLoaderClient& client) {
    std::string body;
    ASSERT_TRUE(
        mojo::BlockingCopyToString(client.response_body_release(), &body));
    EXPECT_EQ(kBody, body);
  }

  content::BrowserTaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  GURL url_;
  mojo::Remote<network::mojom::URLLoaderFactory> factory_;
  mojo::Remote<network::mojom::URLLoader> loader_;
};

TEST_P(XenonAsarURLLoaderFactoryTest, CrossOriginDisplayIsOpaque) {
  network::TestURLLoaderClient client;
  Load(network::mojom::RequestMode::kNoCors,
       url::Origin::Create(GURL("chrome://mapped-fixture/")), client);
  ASSERT_EQ(net::OK, client.completion_status().error_code);
  ASSERT_TRUE(client.has_received_response());
  EXPECT_EQ(network::mojom::FetchResponseType::kOpaque,
            client.response_head()->response_type);
  ExpectBody(client);
}

TEST_P(XenonAsarURLLoaderFactoryTest, CrossOriginReadIsRejected) {
  network::TestURLLoaderClient client;
  Load(network::mojom::RequestMode::kCors,
       url::Origin::Create(GURL("chrome://mapped-fixture/")), client);
  EXPECT_FALSE(client.has_received_response());
  ASSERT_TRUE(client.completion_status().cors_error_status);
  EXPECT_EQ(network::mojom::CorsError::kCorsDisabledScheme,
            client.completion_status().cors_error_status->cors_error);
}

TEST_P(XenonAsarURLLoaderFactoryTest, SameOriginFileReadRemainsReadable) {
  network::TestURLLoaderClient client;
  Load(network::mojom::RequestMode::kCors, url::Origin::Create(url_), client);
  ASSERT_EQ(net::OK, client.completion_status().error_code);
  ASSERT_TRUE(client.has_received_response());
  EXPECT_EQ(network::mojom::FetchResponseType::kBasic,
            client.response_head()->response_type);
  ExpectBody(client);
}

TEST_P(XenonAsarURLLoaderFactoryTest, CorsWithoutInitiatorIsRejected) {
  network::TestURLLoaderClient client;
  Load(network::mojom::RequestMode::kCors, std::nullopt, client);
  EXPECT_FALSE(client.has_received_response());
  EXPECT_EQ(net::ERR_INVALID_ARGUMENT, client.completion_status().error_code);
}

TEST_P(XenonAsarURLLoaderFactoryTest, ByteRangeReturnsOnlyRequestedPlaintext) {
  network::TestURLLoaderClient client;
  Load(network::mojom::RequestMode::kNoCors, std::nullopt, client,
       "bytes=3-18");
  ASSERT_EQ(net::OK, client.completion_status().error_code);
  ASSERT_TRUE(client.has_received_response());
  EXPECT_EQ(16, client.response_head()->content_length);
  std::string body;
  ASSERT_TRUE(
      mojo::BlockingCopyToString(client.response_body_release(), &body));
  EXPECT_EQ(std::string_view(kBody).substr(3, 16), body);
}

TEST_P(XenonAsarURLLoaderFactoryTest, UnsatisfiableRangeIsRejected) {
  network::TestURLLoaderClient client;
  Load(network::mojom::RequestMode::kNoCors, std::nullopt, client,
       "bytes=4096-8192");
  EXPECT_EQ(net::ERR_REQUEST_RANGE_NOT_SATISFIABLE,
            client.completion_status().error_code);
  EXPECT_FALSE(client.has_received_response());
}

INSTANTIATE_TEST_SUITE_P(FileKinds,
                         XenonAsarURLLoaderFactoryTest,
                         testing::Values(FileKind::kPlain,
                                         FileKind::kPacked,
                                         FileKind::kUnpacked,
                                         FileKind::kEncryptedPacked,
                                         FileKind::kEncryptedHeaderPacked,
                                         FileKind::kEncryptedHeaderUnpacked));

class XenonEncryptedAsarURLLoaderTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    factory_.Bind(CreateAsarURLLoaderFactory());
  }

  void TearDown() override {
    loader_.reset();
    factory_.reset();
    task_environment_.RunUntilIdle();
    asar::RemoveArchivePublicKeys("encrypted-url-loader-test");
  }

  void CreateEntry(const std::string& body) {
    asar::TestArchiveBuilder builder;
    const auto archive_path = temp_dir_.GetPath().AppendASCII("stream.asar");
    // The missing extension exercises MIME detection on the same decoded
    // reader that later feeds the streaming response.
    ASSERT_TRUE(builder.Write(archive_path, {{"resource", body, true}}));
    ASSERT_TRUE(asar::SetArchivePublicKeys(
        "encrypted-url-loader-test",
        {{temp_dir_.GetPath(), builder.public_key_pem()}}));
    url_ = net::FilePathToFileURL(archive_path.AppendASCII("resource"));
  }

  void LoadBody(std::string_view range,
                network::TestURLLoaderClient& client,
                std::string* body) {
    network::ResourceRequest request;
    request.url = url_;
    request.mode = network::mojom::RequestMode::kNoCors;
    if (!range.empty()) {
      request.headers.SetHeader("Range", range);
    }
    factory_->CreateLoaderAndStart(
        loader_.BindNewPipeAndPassReceiver(), 1, 0, request,
        client.CreateRemote(),
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS));
    client.RunUntilResponseReceived();
    ASSERT_TRUE(
        mojo::BlockingCopyToString(client.response_body_release(), body));
    client.RunUntilComplete();
    ASSERT_EQ(net::OK, client.completion_status().error_code);
  }

  content::BrowserTaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  GURL url_;
  mojo::Remote<network::mojom::URLLoaderFactory> factory_;
  mojo::Remote<network::mojom::URLLoader> loader_;
};

TEST_F(XenonEncryptedAsarURLLoaderTest, SniffsAndStreamsDecodedContent) {
  std::string expected = "%PDF-1.7\n";
  expected.resize(2 * 1024 * 1024, 'x');
  ASSERT_NO_FATAL_FAILURE(CreateEntry(expected));
  network::TestURLLoaderClient client;
  std::string body;
  ASSERT_NO_FATAL_FAILURE(LoadBody({}, client, &body));
  EXPECT_EQ(expected, body);
  EXPECT_EQ(static_cast<int64_t>(expected.size()),
            client.response_head()->content_length);
  EXPECT_TRUE(client.response_head()->did_mime_sniff);
  EXPECT_EQ("application/pdf", client.response_head()->mime_type);
}

TEST_F(XenonEncryptedAsarURLLoaderTest,
       RangeBeyondSniffBufferUsesLogicalOffset) {
  std::string expected = "%PDF-1.7\n";
  for (size_t i = expected.size(); i < 128 * 1024; ++i) {
    expected.push_back(static_cast<char>(i % 251));
  }
  ASSERT_NO_FATAL_FAILURE(CreateEntry(expected));
  network::TestURLLoaderClient client;
  std::string body;
  ASSERT_NO_FATAL_FAILURE(LoadBody("bytes=8195-73730", client, &body));
  EXPECT_EQ(std::string_view(expected).substr(8195, 65536), body);
  EXPECT_EQ(65536, client.response_head()->content_length);
  EXPECT_TRUE(client.response_head()->did_mime_sniff);
  EXPECT_EQ("application/pdf", client.response_head()->mime_type);
}

}  // namespace
}  // namespace xenon
