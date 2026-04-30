// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/openh264_video_decoder.h"

#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_status.h"
#include "media/base/media_log.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_codecs.h"
#include "media/base/video_color_space.h"
#include "media/base/video_frame.h"
#include "third_party/openh264/src/codec/api/wels/codec_api.h"
#include "third_party/openh264/src/codec/api/wels/codec_app_def.h"
#include "third_party/openh264/src/codec/api/wels/codec_def.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace media {

namespace {

// Copies a single plane from a source buffer (with possibly larger stride)
// into a destination buffer (with VideoFrame's tightly packed stride).
void CopyPlane(base::span<const uint8_t> src,
               int src_stride,
               base::span<uint8_t> dst,
               int dst_stride,
               int row_bytes,
               int rows) {
  if (src.empty() || dst.empty() || rows <= 0 || row_bytes <= 0 ||
      src_stride < row_bytes || dst_stride < row_bytes) {
    return;
  }
  const size_t row_bytes_sz = static_cast<size_t>(row_bytes);
  const size_t src_stride_sz = static_cast<size_t>(src_stride);
  const size_t dst_stride_sz = static_cast<size_t>(dst_stride);
  CHECK_GE(src.size(), src_stride_sz * static_cast<size_t>(rows - 1) +
                           row_bytes_sz);
  CHECK_GE(dst.size(), dst_stride_sz * static_cast<size_t>(rows - 1) +
                           row_bytes_sz);
  for (int r = 0; r < rows; ++r) {
    dst.subspan(static_cast<size_t>(r) * dst_stride_sz, row_bytes_sz)
        .copy_from(
            src.subspan(static_cast<size_t>(r) * src_stride_sz, row_bytes_sz));
  }
}

}  // namespace

OpenH264VideoDecoder::OpenH264VideoDecoder(MediaLog* media_log)
    : media_log_(media_log) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

OpenH264VideoDecoder::~OpenH264VideoDecoder() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ReleaseDecoder();
}

VideoDecoderType OpenH264VideoDecoder::GetDecoderType() const {
  return VideoDecoderType::kOpenH264;
}

int OpenH264VideoDecoder::GetMaxDecodeRequests() const {
  return 1;
}

void OpenH264VideoDecoder::ReleaseDecoder() {
  if (decoder_) {
    ISVCDecoder* d = decoder_;
    decoder_ = nullptr;
    d->Uninitialize();
    WelsDestroyDecoder(d);
  }
  inject_param_sets_next_ = true;
  pending_pts_.clear();
  last_output_ts_ = base::TimeDelta();
}

void OpenH264VideoDecoder::Initialize(const VideoDecoderConfig& config,
                                      bool /*low_delay*/,
                                      CdmContext* /*cdm_context*/,
                                      InitCB init_cb,
                                      const OutputCB& output_cb,
                                      const WaitingCB& /*waiting_cb*/) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto bound_init_cb = base::BindPostTaskToCurrentDefault(std::move(init_cb));

  ReleaseDecoder();
  state_ = State::kUninitialized;
  output_cb_ = output_cb;
  config_ = config;

  LOG(ERROR) << "OpenH264VideoDecoder::Initialize codec="
             << GetCodecName(config.codec())
             << " profile=" << static_cast<int>(config.profile())
             << " encrypted=" << config.is_encrypted()
             << " valid=" << config.IsValidConfig()
             << " coded_size=" << config.coded_size().ToString()
             << " extra_data_size=" << config.extra_data().size();

  if (config.codec() != VideoCodec::kH264 || config.is_encrypted() ||
      !config.IsValidConfig()) {
    LOG(ERROR) << "OpenH264VideoDecoder: unsupported config -> falling back";
    MEDIA_LOG(INFO, media_log_)
        << "OpenH264VideoDecoder: unsupported config (codec="
        << GetCodecName(config.codec())
        << ", encrypted=" << config.is_encrypted() << ")";
    std::move(bound_init_cb).Run(DecoderStatus::Codes::kUnsupportedConfig);
    return;
  }

  // Parse AVCDecoderConfigurationRecord (extra_data). Some streams (e.g.
  // already-Annex-B WebRTC inputs) may have empty extra_data; in that case
  // we skip extradata-based SPS/PPS injection and rely on inline params.
  const bool has_extradata = !config.extra_data().empty();
  if (has_extradata &&
      !h264_converter_.ParseConfiguration(config.extra_data(), &avc_config_)) {
    LOG(ERROR) << "OpenH264VideoDecoder: ParseConfiguration FAILED -> falling back";
    MEDIA_LOG(WARNING, media_log_)
        << "OpenH264VideoDecoder: failed to parse AVC extradata";
    std::move(bound_init_cb).Run(DecoderStatus::Codes::kUnsupportedConfig);
    return;
  }
  LOG(ERROR) << "OpenH264VideoDecoder: extradata ok (has=" << has_extradata
             << "), creating ISVCDecoder";

  ISVCDecoder* dec = nullptr;
  if (WelsCreateDecoder(&dec) != 0 || !dec) {
    LOG(ERROR) << "OpenH264VideoDecoder: WelsCreateDecoder FAILED";
    std::move(bound_init_cb).Run(DecoderStatus::Codes::kFailed);
    return;
  }

  SDecodingParam param = {};
  param.uiTargetDqLayer = static_cast<unsigned char>(-1);
  param.eEcActiveIdc = ERROR_CON_FRAME_COPY_CROSS_IDR;
  param.bParseOnly = false;
  param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;

  if (dec->Initialize(&param) != 0) {
    LOG(ERROR) << "OpenH264VideoDecoder: ISVCDecoder::Initialize failed";
    WelsDestroyDecoder(dec);
    std::move(bound_init_cb).Run(DecoderStatus::Codes::kFailed);
    return;
  }

  decoder_ = dec;
  inject_param_sets_next_ = has_extradata;
  state_ = State::kNormal;

  LOG(ERROR) << "OpenH264VideoDecoder: ACTIVATED for clear H.264 (coded_size="
             << config.coded_size().ToString() << ")";
  MEDIA_LOG(INFO, media_log_)
      << "OpenH264VideoDecoder: activated for clear H.264 (coded_size="
      << config.coded_size().ToString() << ")";

  std::move(bound_init_cb).Run(DecoderStatus::Codes::kOk);
}

void OpenH264VideoDecoder::Decode(scoped_refptr<DecoderBuffer> buffer,
                                  DecodeCB decode_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(buffer);

  auto bound_decode_cb =
      base::BindPostTaskToCurrentDefault(std::move(decode_cb));

  if (state_ == State::kError) {
    std::move(bound_decode_cb).Run(DecoderStatus::Codes::kFailed);
    return;
  }
  if (state_ != State::kNormal || !decoder_) {
    std::move(bound_decode_cb).Run(DecoderStatus::Codes::kNotInitialized);
    return;
  }

  if (buffer->end_of_stream()) {
    if (!FlushDecoder()) {
      state_ = State::kError;
      std::move(bound_decode_cb).Run(DecoderStatus::Codes::kFailed);
      return;
    }
    std::move(bound_decode_cb).Run(DecoderStatus::Codes::kOk);
    return;
  }

  if (!DecodeBuffer(*buffer)) {
    state_ = State::kError;
    std::move(bound_decode_cb).Run(DecoderStatus::Codes::kFailed);
    return;
  }

  std::move(bound_decode_cb).Run(DecoderStatus::Codes::kOk);
}

bool OpenH264VideoDecoder::DecodeBuffer(const DecoderBuffer& buffer) {
  DCHECK(decoder_);
  DCHECK(!buffer.end_of_stream());

  // Convert AVCC (length-prefixed) to Annex-B. Inject SPS/PPS only on the
  // first packet so subsequent IDRs do not duplicate them.
  base::span<const uint8_t> in = buffer;
  const mp4::AVCDecoderConfigurationRecord* cfg =
      inject_param_sets_next_ ? &avc_config_ : nullptr;

  uint32_t needed = h264_converter_.CalculateNeededOutputBufferSize(in, cfg);
  static int s_decode_call = 0;
  if (s_decode_call < 3) {
    LOG(ERROR) << "OpenH264VideoDecoder::DecodeBuffer #" << s_decode_call
               << " in_size=" << in.size() << " inject_sps_pps=" << !!cfg
               << " needed_annexb=" << needed;
    ++s_decode_call;
  }
  if (needed == 0) {
    LOG(WARNING) << "OpenH264VideoDecoder: empty Annex-B payload";
    return true;
  }
  if (annex_scratch_.size() < needed) {
    annex_scratch_.resize(needed);
  }
  uint32_t out_size = static_cast<uint32_t>(annex_scratch_.size());
  if (!h264_converter_.ConvertNalUnitStreamToByteStream(
          in, cfg, base::span(annex_scratch_).first(out_size), &out_size)) {
    LOG(ERROR) << "OpenH264VideoDecoder: AVCC -> Annex-B conversion failed";
    return false;
  }
  inject_param_sets_next_ = false;

  pending_pts_.insert(buffer.timestamp());

  unsigned char* yuv_planes[3] = {nullptr, nullptr, nullptr};
  SBufferInfo dst_info = {};

  DECODING_STATE rc = decoder_->DecodeFrameNoDelay(
      annex_scratch_.data(), static_cast<int>(out_size), yuv_planes, &dst_info);

  // OpenH264 may return non-zero status flags but still output a usable frame
  // (e.g. concealed frames). Treat status as fatal only when no frame is
  // produced for an extended run; here we just log and rely on iBufferStatus.
  if (rc != dsErrorFree) {
    DLOG(WARNING) << "OpenH264VideoDecoder: DecodeFrameNoDelay status=0x"
                  << std::hex << rc;
  }

  if (dst_info.iBufferStatus != 1) {
    return true;  // Frame not yet ready; consumed input is fine.
  }

  const SSysMEMBuffer& sys = dst_info.UsrData.sSystemBuffer;
  if (sys.iFormat != videoFormatI420) {
    LOG(ERROR) << "OpenH264VideoDecoder: unexpected output format "
               << sys.iFormat;
    return false;
  }

  int strides[2] = {sys.iStride[0], sys.iStride[1]};
  return EmitFrameFromDecoder(yuv_planes, strides, sys.iWidth, sys.iHeight);
}

bool OpenH264VideoDecoder::FlushDecoder() {
  DCHECK(decoder_);

  // Drain any frames buffered inside OpenH264. Repeated calls with NULL
  // input force the decoder to emit reordered/buffered frames one at a
  // time until iBufferStatus stays 0.
  for (int i = 0; i < 32; ++i) {
    unsigned char* yuv_planes[3] = {nullptr, nullptr, nullptr};
    SBufferInfo dst_info = {};

    DECODING_STATE rc = decoder_->DecodeFrameNoDelay(
        nullptr, 0, yuv_planes, &dst_info);
    if (rc != dsErrorFree) {
      DLOG(WARNING) << "OpenH264VideoDecoder: flush status=0x" << std::hex
                    << rc;
    }
    if (dst_info.iBufferStatus != 1) {
      break;
    }
    const SSysMEMBuffer& sys = dst_info.UsrData.sSystemBuffer;
    if (sys.iFormat != videoFormatI420) {
      LOG(ERROR) << "OpenH264VideoDecoder: flush unexpected format "
                 << sys.iFormat;
      return false;
    }
    int strides[2] = {sys.iStride[0], sys.iStride[1]};
    if (!EmitFrameFromDecoder(yuv_planes, strides, sys.iWidth, sys.iHeight)) {
      return false;
    }
  }
  return true;
}

bool OpenH264VideoDecoder::EmitFrameFromDecoder(unsigned char* const planes[3],
                                                int strides[2],
                                                int width,
                                                int height) {
  // SAFETY: OpenH264 fills planes[0..2] / strides for an I420 picture of
  // |width| x |height|; null checks below reject invalid outputs.
  base::span<unsigned char* const, 3> plane_span =
      UNSAFE_BUFFERS(base::span<unsigned char* const, 3>(planes, 3u));
  base::span<int, 2> stride_span = UNSAFE_BUFFERS(base::span<int, 2>(strides, 2u));
  if (width <= 0 || height <= 0 || !plane_span[0] || !plane_span[1] ||
      !plane_span[2]) {
    LOG(ERROR) << "OpenH264VideoDecoder: invalid output picture";
    return false;
  }

  // Pick the timestamp for this frame. OpenH264 outputs in display order, so
  // assigning the smallest pending input PTS reproduces the correct PTS for
  // streams without B-frames and is a safe approximation otherwise.
  base::TimeDelta ts;
  if (!pending_pts_.empty()) {
    auto it = pending_pts_.begin();
    ts = *it;
    pending_pts_.erase(it);
  } else {
    ts = last_output_ts_ + base::Microseconds(33333);
  }
  if (ts <= last_output_ts_ && decoded_frame_count_ > 0) {
    ts = last_output_ts_ + base::Microseconds(1);
  }
  last_output_ts_ = ts;

  const gfx::Size coded_size(width, height);
  gfx::Rect visible_rect = config_.visible_rect();
  if (visible_rect.IsEmpty() ||
      visible_rect.right() > width ||
      visible_rect.bottom() > height) {
    visible_rect = gfx::Rect(coded_size);
  }
  gfx::Size natural_size = config_.natural_size();
  if (natural_size.IsEmpty()) {
    natural_size = visible_rect.size();
  }

  scoped_refptr<VideoFrame> frame = VideoFrame::CreateFrame(
      PIXEL_FORMAT_I420, coded_size, visible_rect, natural_size, ts);
  if (!frame) {
    LOG(ERROR) << "OpenH264VideoDecoder: VideoFrame::CreateFrame failed";
    return false;
  }

  const int chroma_w = (width + 1) / 2;
  const int chroma_h = (height + 1) / 2;
  const size_t y_src_size =
      static_cast<size_t>(stride_span[0]) * static_cast<size_t>(height);
  const size_t c_src_size =
      static_cast<size_t>(stride_span[1]) * static_cast<size_t>(chroma_h);
  const size_t y_dst_size =
      static_cast<size_t>(frame->stride(VideoFrame::Plane::kY)) *
      static_cast<size_t>(height);
  const size_t u_dst_size =
      static_cast<size_t>(frame->stride(VideoFrame::Plane::kU)) *
      static_cast<size_t>(chroma_h);
  const size_t v_dst_size =
      static_cast<size_t>(frame->stride(VideoFrame::Plane::kV)) *
      static_cast<size_t>(chroma_h);

  // SAFETY: plane pointers/strides come from OpenH264 I420 output for this
  // frame size; destination spans match VideoFrame allocation.
  CopyPlane(UNSAFE_BUFFERS(base::span(plane_span[0], y_src_size)),
            stride_span[0],
            UNSAFE_BUFFERS(base::span(
                frame->GetWritableVisibleData(VideoFrame::Plane::kY),
                y_dst_size)),
            frame->stride(VideoFrame::Plane::kY), width, height);
  CopyPlane(UNSAFE_BUFFERS(base::span(plane_span[1], c_src_size)),
            stride_span[1],
            UNSAFE_BUFFERS(base::span(
                frame->GetWritableVisibleData(VideoFrame::Plane::kU),
                u_dst_size)),
            frame->stride(VideoFrame::Plane::kU), chroma_w, chroma_h);
  CopyPlane(UNSAFE_BUFFERS(base::span(plane_span[2], c_src_size)),
            stride_span[1],
            UNSAFE_BUFFERS(base::span(
                frame->GetWritableVisibleData(VideoFrame::Plane::kV),
                v_dst_size)),
            frame->stride(VideoFrame::Plane::kV), chroma_w, chroma_h);

  frame->set_color_space(config_.color_space_info().ToGfxColorSpace());

  ++decoded_frame_count_;
  if (decoded_frame_count_ <= 3 || decoded_frame_count_ % 60 == 0) {
    LOG(ERROR) << "OpenH264VideoDecoder: emitted frame #"
               << decoded_frame_count_ << " ts=" << ts.InMilliseconds()
               << "ms size=" << width << "x" << height;
  }
  output_cb_.Run(std::move(frame));
  return true;
}

void OpenH264VideoDecoder::Reset(base::OnceClosure closure) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Easiest correct path: tear down and recreate the decoder so the next
  // Decode() call starts cleanly from the upcoming IDR.
  if (decoder_) {
    ReleaseDecoder();

    ISVCDecoder* dec = nullptr;
    if (WelsCreateDecoder(&dec) == 0 && dec) {
      SDecodingParam param = {};
      param.uiTargetDqLayer = static_cast<unsigned char>(-1);
      param.eEcActiveIdc = ERROR_CON_FRAME_COPY_CROSS_IDR;
      param.bParseOnly = false;
      param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
      if (dec->Initialize(&param) == 0) {
        decoder_ = dec;
        inject_param_sets_next_ = !config_.extra_data().empty();
        state_ = State::kNormal;
      } else {
        WelsDestroyDecoder(dec);
        state_ = State::kError;
      }
    } else {
      state_ = State::kError;
    }
  }

  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                           std::move(closure));
}

}  // namespace media
