// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_FILTERS_OPENH264_VIDEO_DECODER_H_
#define MEDIA_FILTERS_OPENH264_VIDEO_DECODER_H_

#include <set>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "media/base/media_export.h"
#include "media/base/video_decoder.h"
#include "media/base/video_decoder_config.h"
#include "media/filters/h264_to_annex_b_bitstream_converter.h"
#include "media/formats/mp4/box_definitions.h"

class ISVCDecoder;

namespace media {

class DecoderBuffer;
class MediaLog;
class VideoFrame;

// OpenH264 (Cisco) software H.264 decoder, registered in
// `DefaultDecoderFactory` ahead of `FFmpegVideoDecoder` when
// `enable_openh264_video_decoder=true`. Supports clear (non-encrypted)
// H.264 streams. Initialize() returns kUnsupportedConfig for non-H.264 or
// encrypted configs so the decoder selector can fall through to other
// decoders.
class MEDIA_EXPORT OpenH264VideoDecoder : public VideoDecoder {
 public:
  explicit OpenH264VideoDecoder(MediaLog* media_log);

  OpenH264VideoDecoder(const OpenH264VideoDecoder&) = delete;
  OpenH264VideoDecoder& operator=(const OpenH264VideoDecoder&) = delete;

  ~OpenH264VideoDecoder() override;

  // VideoDecoder implementation.
  VideoDecoderType GetDecoderType() const override;
  void Initialize(const VideoDecoderConfig& config,
                  bool low_delay,
                  CdmContext* cdm_context,
                  InitCB init_cb,
                  const OutputCB& output_cb,
                  const WaitingCB& waiting_cb) override;
  void Decode(scoped_refptr<DecoderBuffer> buffer, DecodeCB decode_cb) override;
  void Reset(base::OnceClosure closure) override;
  int GetMaxDecodeRequests() const override;

 private:
  enum class State { kUninitialized, kNormal, kError };

  // Synchronously decodes one buffer. Outputs zero or more frames via
  // `output_cb_`. Returns false on a fatal decoder error.
  bool DecodeBuffer(const DecoderBuffer& buffer);

  // Drains any frames buffered inside OpenH264 (called on EOS).
  bool FlushDecoder();

  // Builds a VideoFrame from the OpenH264 output picture and pushes it to
  // `output_cb_` after assigning the next pending input PTS.
  bool EmitFrameFromDecoder(unsigned char* const planes[3],
                            int strides[2],
                            int width,
                            int height);

  // Tears down the underlying OpenH264 ISVCDecoder.
  void ReleaseDecoder();

  SEQUENCE_CHECKER(sequence_checker_);

  const raw_ptr<MediaLog, DanglingUntriaged> media_log_;

  State state_ = State::kUninitialized;
  OutputCB output_cb_;
  VideoDecoderConfig config_;

  // Owned OpenH264 instance; created via WelsCreateDecoder.
  raw_ptr<ISVCDecoder, DanglingUntriaged> decoder_ = nullptr;

  // AVCC -> Annex-B conversion (MP4 streams arrive length-prefixed).
  H264ToAnnexBBitstreamConverter h264_converter_;
  mp4::AVCDecoderConfigurationRecord avc_config_;
  bool inject_param_sets_next_ = true;
  std::vector<uint8_t> annex_scratch_;

  // OpenH264 returns frames in display order but provides no timestamp
  // plumbing. Track every input PTS in a sorted multiset and assign the
  // smallest pending one to each output frame.
  std::multiset<base::TimeDelta> pending_pts_;
  base::TimeDelta last_output_ts_ = base::TimeDelta();

  int decoded_frame_count_ = 0;
};

}  // namespace media

#endif  // MEDIA_FILTERS_OPENH264_VIDEO_DECODER_H_
