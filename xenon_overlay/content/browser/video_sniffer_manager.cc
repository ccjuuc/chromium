// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/content/browser/video_sniffer_manager.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/gurl.h"

namespace xenon {

SniffedMedia::SniffedMedia(std::string m, std::string u)
    : mime_type(std::move(m)), url(std::move(u)) {}
SniffedMedia::SniffedMedia(const SniffedMedia& other) = default;
SniffedMedia::~SniffedMedia() = default;

MediaGroup::MediaGroup() = default;
MediaGroup::MediaGroup(const MediaGroup& other) = default;
MediaGroup::~MediaGroup() = default;

// static
VideoSnifferManager* VideoSnifferManager::GetInstance() {
  static base::NoDestructor<VideoSnifferManager> instance;
  return instance.get();
}

VideoSnifferManager::VideoSnifferManager() = default;
VideoSnifferManager::~VideoSnifferManager() = default;

void VideoSnifferManager::AddMedia(const std::string& mime_type,
                                   const std::string& url) {
  GURL gurl(url);
  if (!gurl.is_valid()) return;

  // Get base URL by removing the filename part
  std::string spec = gurl.spec();
  size_t last_slash = spec.find_last_of('/');
  std::string base_url = (last_slash != std::string::npos) 
                         ? spec.substr(0, last_slash + 1) 
                         : spec;

  auto& group = groups_[base_url];
  
  if (mime_type.find("mpegurl") != std::string::npos || 
      mime_type.find("mp4") != std::string::npos) {
    // Priority: .m3u8 or .mp4 as the main entry
    if (group.main_url.empty() || 
        (group.main_url.find(".ts") != std::string::npos)) {
      group.main_url = url;
      group.mime_type = mime_type;
    }
  } else if (mime_type == "video/mp2t" || spec.find(".ts") != std::string::npos) {
    // It's a segment
    for (const auto& s : group.segments) {
      if (s == url) return;
    }
    group.segments.push_back(url);
    if (group.main_url.empty()) {
       group.main_url = url; // Fallback if no m3u8 seen yet
       group.mime_type = mime_type;
    }
  } else {
    // Other media (standalone)
    if (group.main_url.empty()) {
      group.main_url = url;
      group.mime_type = mime_type;
    }
  }
}

std::vector<MediaGroup> VideoSnifferManager::GetGroups() const {
  std::vector<MediaGroup> result;
  for (auto const& [key, val] : groups_) {
    result.push_back(val);
  }
  return result;
}

namespace {

// Helper to write data to a file on a background thread (open-append-close).
// WriteAtCurrentPos may write less than requested; loop until complete.
bool WriteDataToFile(const base::FilePath& path, std::string data) {
  base::FilePath dir = path.DirName();
  if (!base::DirectoryExists(dir)) {
    if (!base::CreateDirectory(dir)) {
      LOG(ERROR) << "[Xenon] Failed to create directory: " << dir.value();
      return false;
    }
  }

  // Windows: FLAG_APPEND must not be combined with FLAG_WRITE (see file_win.cc).
  base::File file(path, base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_APPEND);
  if (!file.IsValid()) {
    LOG(ERROR) << "[Xenon] Failed to open file for append: " << path.value()
               << " Error: " << base::File::ErrorToString(file.error_details());
    return false;
  }

  size_t total = 0;
  while (total < data.size()) {
    std::optional<size_t> written =
        file.WriteAtCurrentPos(base::as_byte_span(data).subspan(total));
    if (!written.has_value() || *written == 0) {
      LOG(ERROR) << "[Xenon] Write failed at offset " << total << " for "
                 << path.value();
      return false;
    }
    total += *written;
  }
  return true;
}

bool IsMasterPlaylist(const std::string& content) {
  return content.find("#EXT-X-STREAM-INF") != std::string::npos ||
         content.find("#EXT-X-I-FRAME-STREAM-INF") != std::string::npos;
}

// Extract URI value from e.g. #EXT-X-MAP:URI="init.mp4", EXT-X-STREAM-INF URI="..."
bool ExtractHlsAttributeUri(const std::string& line, std::string* uri_out) {
  constexpr char kUri[] = "URI=";
  size_t pos = line.find(kUri);
  if (pos == std::string::npos)
    return false;
  pos += strlen(kUri);
  if (pos >= line.size())
    return false;
  char quote = line[pos];
  if (quote != '"' && quote != '\'')
    return false;
  ++pos;
  size_t end = line.find(quote, pos);
  if (end == std::string::npos || end == pos)
    return false;
  *uri_out = line.substr(pos, end - pos);
  return true;
}

// Pick highest BANDWIDTH variant URL (fallback: first valid URI line after INF).
bool ParseMasterPickBestVariant(const std::string& content,
                                const GURL& base_url,
                                std::string* out_variant_url) {
  std::vector<std::string> lines = base::SplitString(
      content, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  struct Variant {
    int bandwidth = 0;
    std::string url;
  };
  std::vector<Variant> variants;

  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (line.find("#EXT-X-STREAM-INF") == std::string::npos)
      continue;

    int bandwidth = 0;
    constexpr std::string_view kBandwidth = "BANDWIDTH=";
    size_t bw_pos = line.find(kBandwidth);
    if (bw_pos != std::string::npos) {
      base::StringToInt(
          std::string_view(line).substr(bw_pos + kBandwidth.size()),
          &bandwidth);
    }

    std::string variant_relative;
    if (!ExtractHlsAttributeUri(line, &variant_relative)) {
      size_t j = i + 1;
      while (j < lines.size() &&
             (!lines[j].empty() && lines[j][0] == '#')) {
        ++j;
      }
      if (j >= lines.size()) {
        continue;
      }
      variant_relative = lines[j];
    }

    GURL variant_gurl = base_url.Resolve(variant_relative);
    if (!variant_gurl.is_valid())
      continue;

    variants.push_back({bandwidth, variant_gurl.spec()});
  }

  if (variants.empty())
    return false;

  std::sort(variants.begin(), variants.end(),
            [](const Variant& a, const Variant& b) {
              return a.bandwidth > b.bandwidth;
            });
  *out_variant_url = variants.front().url;
  return true;
}

// Media playlist only: ordered segment URLs, with #EXT-X-MAP first if present.
void ParseMediaPlaylist(const std::string& content,
                        const GURL& base_url,
                        std::vector<std::string>* out_segments) {
  out_segments->clear();
  std::vector<std::string> lines = base::SplitString(
      content, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);

  std::string map_uri;
  for (const std::string& line : lines) {
    if (line.find("#EXT-X-MAP:") != std::string::npos) {
      std::string rel;
      if (ExtractHlsAttributeUri(line, &rel)) {
        GURL map_gurl = base_url.Resolve(rel);
        if (map_gurl.is_valid())
          map_uri = map_gurl.spec();
      }
      continue;
    }
  }

  if (!map_uri.empty())
    out_segments->push_back(map_uri);

  for (const std::string& line : lines) {
    if (line.empty() || line[0] == '#')
      continue;
    GURL seg = base_url.Resolve(line);
    if (seg.is_valid())
      out_segments->push_back(seg.spec());
  }
}

std::string ExtensionForFirstSegmentUrl(const std::string& url) {
  GURL g(url);
  if (!g.is_valid()) {
    return "ts";
  }
  std::string path = g.GetPath();
  if (base::EndsWith(path, ".m4s", base::CompareCase::INSENSITIVE_ASCII) ||
      base::EndsWith(path, ".mp4", base::CompareCase::INSENSITIVE_ASCII)) {
    return "mp4";
  }
  return "ts";
}

class HLSSynthesizer {
 public:
  HLSSynthesizer(const std::string& main_url, 
                const std::vector<std::string>& segments,
                content::BrowserContext* context)
      : main_url_(main_url), segments_(segments), context_(context) {}

  ~HLSSynthesizer() = default;

  void Start() {
    LOG(INFO) << "[Xenon] Starting HLS synthesis task for: " << main_url_;

    if (main_url_.find(".m3u8") != std::string::npos) {
      FetchM3U8();
      return;
    }

    StartDownloadingSegments();
  }

 private:
  void EnsureOutputPath() {
    if (!output_path_.empty())
      return;
    base::FilePath download_dir;
    base::PathService::Get(base::DIR_HOME, &download_dir);
    download_dir = download_dir.AppendASCII("Downloads");
    std::string ext =
        segments_.empty() ? "ts" : ExtensionForFirstSegmentUrl(segments_[0]);
    std::string filename = base::StringPrintf(
        "xenon_video_%llu.%s", base::Time::Now().ToInternalValue(), ext.c_str());
    output_path_ = download_dir.AppendASCII(filename);
  }

  void FetchM3U8() {
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = GURL(main_url_);
    request->credentials_mode = network::mojom::CredentialsMode::kInclude;

    loader_ = network::SimpleURLLoader::Create(std::move(request), MISSING_TRAFFIC_ANNOTATION);
    loader_->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
        context_->GetDefaultStoragePartition()->GetURLLoaderFactoryForBrowserProcess().get(),
        base::BindOnce(&HLSSynthesizer::OnM3U8Downloaded, base::Unretained(this)));
  }

  void OnM3U8Downloaded(std::optional<std::string> body) {
    if (!body) {
      LOG(ERROR) << "[Xenon] Failed to fetch M3U8: " << main_url_;
      delete this;
      return;
    }

    GURL playlist_base(main_url_);

    if (IsMasterPlaylist(*body)) {
      if (++master_follow_count_ > 8) {
        LOG(ERROR) << "[Xenon] Too many nested HLS master playlists";
        delete this;
        return;
      }
      std::string variant_url;
      if (!ParseMasterPickBestVariant(*body, playlist_base, &variant_url)) {
        LOG(ERROR) << "[Xenon] Master playlist has no usable variants: "
                   << main_url_;
        delete this;
        return;
      }
      LOG(INFO) << "[Xenon] Following HLS variant playlist: " << variant_url;
      main_url_ = variant_url;
      FetchM3U8();
      return;
    }

    if (body->find("#EXT-X-KEY") != std::string::npos) {
      LOG(WARNING) << "[Xenon] Playlist references #EXT-X-KEY (encryption); "
                        "concatenated file may not play in normal players.";
    }

    ParseMediaPlaylist(*body, playlist_base, &segments_);
    if (segments_.empty()) {
      LOG(ERROR) << "[Xenon] Media playlist has no segments: " << main_url_;
      delete this;
      return;
    }

    LOG(INFO) << "[Xenon] Parsed media playlist, segment URLs: "
              << segments_.size();
    StartDownloadingSegments();
  }

  void StartDownloadingSegments() {
    if (segments_.empty()) {
      LOG(ERROR) << "[Xenon] No segments found for " << main_url_;
      delete this;
      return;
    }
    EnsureOutputPath();
    LOG(INFO) << "[Xenon] Starting HLS synthesis to: " << output_path_.value();
    DownloadNext();
  }

  void DownloadNext() {
    if (current_index_ >= segments_.size()) {
      LOG(INFO) << "[Xenon] HLS synthesis complete: " << output_path_.value();
      delete this;
      return;
    }

    auto request = std::make_unique<network::ResourceRequest>();
    request->url = GURL(segments_[current_index_]);
    request->credentials_mode = network::mojom::CredentialsMode::kInclude;

    loader_ = network::SimpleURLLoader::Create(std::move(request), MISSING_TRAFFIC_ANNOTATION);
    loader_->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
        context_->GetDefaultStoragePartition()->GetURLLoaderFactoryForBrowserProcess().get(),
        base::BindOnce(&HLSSynthesizer::OnSegmentDownloaded, base::Unretained(this)));
  }

  void OnSegmentDownloaded(std::optional<std::string> body) {
    if (body) {
      LOG(INFO) << base::StringPrintf("[Xenon] Downloaded segment %zu/%zu (%zu bytes)", 
                                    current_index_ + 1, segments_.size(), body->size());
      // Move blocking file I/O to ThreadPool
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&WriteDataToFile, output_path_, std::move(*body)),
          base::BindOnce(&HLSSynthesizer::OnSegmentAppended, base::Unretained(this)));
    } else {
      LOG(ERROR) << "[Xenon] Failed to download segment " << (current_index_ + 1) 
                 << ": " << segments_[current_index_];
      OnSegmentAppended(false);
    }
  }

  void OnSegmentAppended(bool success) {
    if (!success) {
      LOG(WARNING) << "[Xenon] Segment " << (current_index_ + 1) << " append failed.";
    }
    current_index_++;
    DownloadNext();
  }

  std::string main_url_;
  std::vector<std::string> segments_;
  raw_ptr<content::BrowserContext> context_;
  base::FilePath output_path_;
  size_t current_index_ = 0;
  int master_follow_count_ = 0;
  std::unique_ptr<network::SimpleURLLoader> loader_;
};

} // namespace

void VideoSnifferManager::SynthesizeGroup(const std::string& main_url, 
                                          content::BrowserContext* context) {
  MediaGroup* group = GetGroupForUrl(main_url);
  std::vector<std::string> segments;
  if (group) {
    segments = group->segments;
  }

  // When main_url is .m3u8, HLSSynthesizer fetches and parses the playlist;
  // sniffed segments are optional. When there is no sniffed group (e.g.
  // omnibox-opened playlist), an empty segment list is fine.
  auto* synthesizer = new HLSSynthesizer(main_url, segments, context);
  synthesizer->Start();
}

MediaGroup* VideoSnifferManager::GetGroupForUrl(const std::string& url) {
  for (auto& [key, group] : groups_) {
    if (group.main_url == url) return &group;
  }
  return nullptr;
}

void VideoSnifferManager::ClearMedia() {
  groups_.clear();
}

}  // namespace xenon
