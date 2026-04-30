# Chromium 视频播放全链路（含 OpenH264 解码器接入）

> 适用版本：Chromium 142（本仓库 `h:\chromium_142\src`）
> 阅读对象：需要理解 `<video>` 标签从加载到画面显示+声音输出的完整调用链
> 关注点：Blink → //media 管线 → 自定义 OpenH264 解码器 → 显示与音频输出

---

## 1. 总览

```
HTML <video>  ──────  Blink (renderer 进程，主线程)
   │
   ▼  HTMLMediaElement → WebMediaPlayerImpl
WebMediaPlayer  ──────  Blink platform/media（renderer 进程，独立 media 线程）
   │
   ▼  PipelineImpl → RendererImpl
媒体管线 (media pipeline)  ──────  //media（同 renderer 进程）
   │   ┌──── Demuxer：FFmpegDemuxer 拆出 H.264 ES
   │   ├──── VideoRendererImpl + VideoFrameStream + VideoDecoderSelector → OpenH264VideoDecoder
   │   └──── AudioRendererImpl + AudioDecoderStream + DecoderSelector → FFmpegAudioDecoder
   │
   ▼ 视频帧
VideoFrameCompositor ─► cc::VideoLayer ─► cc / Viz GPU 进程合成 ─► 屏幕
   │
   ▼ 音频
AudioRendererSink → AudioOutputDevice ─► Audio Service 进程 ─► 系统音频
```

跨进程划分：

| 进程 | 干的事 |
|---|---|
| **Renderer 进程** | Blink、`WebMediaPlayerImpl`、Demuxer、`OpenH264VideoDecoder`、`VideoRendererImpl`、`AudioRendererImpl`、`VideoFrameCompositor`、`cc` 主端 |
| **GPU/Viz 进程** | `cc` 服务端、`DisplayCompositor`、把 `VideoFrame`(I420) 上传成纹理 + YUV→RGB shader、最终送 swapchain |
| **Audio Service 进程**（utility，sandbox=`kAudio`） | `AudioOutputStream` 服务端 + 平台后端（Windows: WASAPI），把 renderer 推过来的 PCM 灌进系统音频 |
| **Browser 进程** | 创建子进程、管理权限/UI；不参与每帧解码与渲染 |

> 本文涉及的 H.264 软解走的是 renderer 进程内的 `OpenH264VideoDecoder`，**不经过 MojoVideoDecoder / GPU 硬解**。

---

## 2. Blink 层：HTML 标签 → WebMediaPlayer

### 2.1 `<video>` 元素

* `third_party/blink/renderer/core/html/media/html_video_element.{h,cc}`
  解析 `<video src="...">`、`videoWidth`/`videoHeight`、控件 UI。
* 继承自 `HTMLMediaElement`：
  ```157:157:third_party/blink/renderer/core/html/media/html_media_element.h
    WebMediaPlayer* GetWebMediaPlayer() const { return web_media_player_.get(); }
  ```

### 2.2 `HTMLMediaElement` 的关键时刻

当解析到 `src`、脚本调用 `video.src = ...` / `video.load()` / `video.play()` 时：

```
HTMLMediaElement::SelectMediaResource     // 选定 src
└── HTMLMediaElement::LoadResource        // 决定加载方式
    └── HTMLMediaElement::StartPlayerLoad // 调 WebMediaPlayer
        └── WebMediaPlayer::Load
```

`web_media_player_` 是抽象接口，运行时实现位于 Blink platform 层：

* `third_party/blink/renderer/platform/media/web_media_player_impl.{h,cc}` — 类 `WebMediaPlayerImpl`

### 2.3 `WebMediaPlayerImpl` 启动

构造时：

* 创建 **media 线程**（`media_task_runner_`）
* 创建 `media::PipelineImpl`
* 创建 `VideoFrameCompositor`（合成线程）

调用顺序：

```
HTMLMediaElement::LoadResource
└── WebMediaPlayerImpl::Load
    └── WebMediaPlayerImpl::DoLoad
        └── WebMediaPlayerImpl::StartPipeline
            └── PipelineImpl::Start
```

---

## 3. //media 管线层（renderer 进程的 media 线程）

### 3.1 `PipelineImpl` 启动

`media/base/pipeline_impl.cc`：核心 `PipelineImpl::Start` → 切到 media 线程 → `RendererWrapper::Start`：

1. 创建 **Demuxer**（HTTP/file 源 → `FFmpegDemuxer`，MSE 源 → `ChunkDemuxer`）
2. 创建 **Renderer**（默认 `RendererImpl`）
3. `Demuxer::Initialize` → 建立网络/IO + 解析容器
4. `Renderer::Initialize`，把 demuxer 给 renderer

### 3.2 `FFmpegDemuxer`：拆容器、产出 ES

`media/filters/ffmpeg_demuxer.cc`：

* 用 FFmpeg 的 `avformat_open_input` / `avformat_find_stream_info` 解析 MP4 容器
* 为每条流（视频/音频）建一个 `FFmpegDemuxerStream`
* 关键：每次 `Read()` 返回一个 **`DecoderBuffer`** —— 视频流即一个完整的 **H.264 NAL 单元（AVCC 格式）+ PTS**
* 把 SPS/PPS 信息塞进 `VideoDecoderConfig::extra_data()`（AVCC 的 `avcC` box 数据）

这就是后面交给 OpenH264 的输入。

### 3.3 `RendererImpl`：把 demuxer 接到两个 renderer

`media/renderers/renderer_impl.cc`：

```
RendererImpl::Initialize
├── audio_renderer_->Initialize(audio_stream, ...)   // AudioRendererImpl
└── video_renderer_->Initialize(video_stream, ...)   // VideoRendererImpl
```

`RendererImpl` 的另一个职责：**音视频同步** —— 持有一个 `TimeSource`（通常是 `AudioRendererImpl` 自带时钟），让视频对齐到音频时间。

### 3.4 `VideoRendererImpl` & `VideoFrameStream`

`media/renderers/video_renderer_impl.cc` 内部持有一个 `VideoFrameStream`（`media/filters/decoder_stream.h` 模板的 video 实例化）。

```
VideoRendererImpl::Initialize
└── VideoFrameStream::Initialize
    └── VideoDecoderSelector::BeginDecoderSelection   // 选解码器
```

---

## 4. 解码器选择：`OpenH264VideoDecoder` 是怎么被挑中的

### 4.1 `VideoDecoderSelector::BeginDecoderSelection`

入口签名（`media/filters/decoder_selector.h`）：

```93:96:media/filters/decoder_selector.h
  // |SelectDecoderCB| may be called with an error if no decoders are available.
  // |waiting_cb| is called whenever the selected decoder is waiting due to e.g.
  // CDM not available, key not available, etc.
  void BeginDecoderSelection(SelectDecoderCB select_decoder_cb,
```

候选列表来自 **`DefaultDecoderFactory::CreateVideoDecoders`**（`media/renderers/default_decoder_factory.cc`）。本仓库中已被改成下面的优先级：

```
OpenH264VideoDecoder  ◄── 当前实际生效的
  └─失败──► FFmpegVideoDecoder
```

`Selector` 会**逐个**调 `Initialize()`，第一个返回 `kOk` 的即被锁定。

### 4.2 `OpenH264VideoDecoder::Initialize`

`media/filters/openh264_video_decoder.cc`：

1. 校验 `config.codec() == VideoCodec::kH264`、`!is_encrypted`
2. `H264ToAnnexBBitstreamConverter::ParseConfiguration(config.extra_data(), &avc_config_)` —— 解析 AVCC 的 SPS/PPS
3. `WelsCreateDecoder(&decoder_)` —— 创建 OpenH264 的 `ISVCDecoder`
4. `decoder_->Initialize(&dec_param)` —— 配置参数
5. 调 `init_cb` 报告成功 → `Selector` 锁定该解码器

`chrome_debug.log` 中标志性日志：

```
DefaultDecoderFactory: adding OpenH264VideoDecoder candidate
OpenH264VideoDecoder::Initialize codec=h264 profile=... encrypted=0 valid=1 coded_size=...
OpenH264VideoDecoder: extradata ok (has=1), creating ISVCDecoder
OpenH264VideoDecoder: ACTIVATED for clear H.264 (coded_size=...)
```

---

## 5. 解码循环（每一帧的精确路径）

### 5.1 `VideoRendererImpl` 拉取下一个 packet

```
VideoRendererImpl::AttemptRead
└── VideoFrameStream::Read(read_cb)             // media/filters/decoder_stream.cc
    └── DemuxerStream::Read                     // FFmpegDemuxerStream::Read
        └── 回调里得到 DecoderBuffer (一个 H.264 AU, AVCC)
    └── DecoderStream<...>::OnBufferReady
        └── DecodeInternal(buffer)
            └── decoder_->Decode(buffer, decode_cb)
                                                 ▲
                                                 │ decoder_ == OpenH264VideoDecoder
```

### 5.2 `OpenH264VideoDecoder::Decode`

`media/filters/openh264_video_decoder.cc`：

1. **AVCC → Annex-B 转换**
   * `H264ToAnnexBBitstreamConverter::ConvertNalUnitStreamToByteStream(...)`
   * 第一次或关键帧时 `inject_sps_pps=true`，把 SPS/PPS 拼在前面
   * 对应日志：`inject_sps_pps=1 needed_annexb=1`

2. **送入 OpenH264**
   ```cpp
   decoder_->DecodeFrameNoDelay(annexb_data, annexb_size,
                                yuv_planes, &buffer_info);
   ```
   * `yuv_planes[3]` 由 OpenH264 内部分配并返回（**指向其内部缓冲，下一次 Decode 会被覆盖**）
   * `buffer_info.iBufferStatus == 1` 表示这次拿到了一帧

3. **拷贝并包装成 `media::VideoFrame`**
   * `VideoFrame::CreateFrame(PIXEL_FORMAT_I420, coded_size, visible_rect, natural_size, ts)` 自分配内存
   * 按行 `memcpy` Y / U / V 三个 plane（**这一步的 `memcpy` 是必须的**，因为 OpenH264 的内部缓冲会被复用）
   * 给它 PTS（从输入 buffer 的 timestamp 推出）

4. **吐给上层**
   ```cpp
   output_cb_.Run(std::move(frame));   // 回到 VideoFrameStream
   ```

5. 调用 `decode_cb(DecoderStatus::Codes::kOk)`，`VideoFrameStream` 即可发起下一次 `AttemptRead`。

> 标志性日志：`OpenH264VideoDecoder: emitted frame #N ts=Tms size=WxH`

### 5.3 `VideoFrameStream::OnFrameReady` → 队列

每个吐出来的 `VideoFrame` 进入 `VideoFrameStream` 内部的 ready frames 队列，最终回到：

```
VideoRendererImpl::FrameReady
└── algorithm_->EnqueueFrame(frame)   // VideoRendererAlgorithm: media/filters/video_renderer_algorithm.cc
```

`VideoRendererAlgorithm` 负责：**根据当前 vsync 时间，决定播放哪一帧、丢哪一帧**。它不是简单按 PTS 顺序播，而是按显示器刷新节拍 + 音频时钟做"最佳匹配"。

---

## 6. 把帧画到屏幕

### 6.1 `VideoFrameCompositor`：媒体线程 ⇄ 合成线程的桥

`third_party/blink/renderer/platform/media/video_frame_compositor.{h,cc}`：

* `VideoRendererImpl` 把自己作为 `VideoRendererSink::RenderCallback` 注册给 `VideoFrameCompositor`
* 合成线程每个 vsync 调用 `VideoFrameCompositor::UpdateCurrentFrame`
* `UpdateCurrentFrame` 回调 `VideoRendererImpl::Render(deadline_min, deadline_max, ...)` —— 这里 `VideoRendererAlgorithm` 选出"应该现在显示的那一帧"
* 选中的帧通过 `VideoFrameCompositor::PaintSingleFrame` / `OnNewFrame` 提交给 `cc::VideoLayer`

### 6.2 `cc` 合成 + Viz GPU 进程

* `cc::VideoLayer` → `VideoLayerImpl` 在合成树里持有当前 `VideoFrame`
* 每个 vsync 由 `cc` 把整张合成帧打包成 `CompositorFrame`，通过 mojo 提交给 **Viz**（GPU 进程内）
* Viz 的 `DisplayCompositor` 把 I420 上传成纹理（YUV→RGB 在 shader 里转），合到屏幕上的 swapchain

涉及的辅助类：`VideoFrameYUVConverter` 等负责把 I420 上传到 GL/D3D 纹理。

> 这一段属于 GPU 进程，与 OpenH264 本身无关。

---

## 7. 音频通路（与视频并行）

### 7.1 `AudioRendererImpl`

`media/renderers/audio_renderer_impl.cc`：

```
AudioRendererImpl::Initialize
└── AudioDecoderStream::Initialize
    └── AudioDecoderSelector → FFmpegAudioDecoder（默认）
```

播放循环：

```
AudioRendererImpl::Render(audio_bus, ...)         ◄── 由 AudioRendererSink 在音频线程上回调
└── AudioBufferConverter / 算法层混音/重采样
└── 最终把 PCM 写进 audio_bus
```

### 7.2 跨进程的音频输出

`AudioRendererSink` 在 renderer 端实现是 `media::AudioOutputDevice`，背后是一段 mojo IPC：

```
AudioOutputDevice (renderer 进程)
   │  mojo::Remote<media::mojom::AudioOutputStream>
   ▼
Audio Service 进程 (utility 类型 sandbox=kAudio)
   │
   ▼
平台后端（Windows: WASAPI / IAudioClient）→ 系统混音器 → 扬声器
```

### 7.3 音视频同步

* `AudioRendererImpl` 维护一个 `MediaTime`（基于已经播出的样本数 + 起点 PTS）
* `RendererImpl` 把 audio_renderer 的 `TimeSource` 设给 video_renderer
* 视频侧 `VideoRendererAlgorithm` 每个 vsync 拉到 "当前媒体时间"，挑最接近的视频帧
* 视频偏慢 → 丢帧；视频偏快 → 当前帧维持一两个 vsync

---

## 8. 调试与验证

### 8.1 关键日志一览（chrome_debug.log）

| 日志行 | 触发位置 |
|---|---|
| `DefaultDecoderFactory: adding OpenH264VideoDecoder candidate` | `media/renderers/default_decoder_factory.cc::CreateVideoDecoders` |
| `OpenH264VideoDecoder::Initialize codec=h264 ...` | `media/filters/openh264_video_decoder.cc::Initialize` |
| `OpenH264VideoDecoder: ACTIVATED for clear H.264 (...)` | 同上，`ISVCDecoder` 创建后 |
| `OpenH264VideoDecoder::DecodeBuffer #N in_size=... inject_sps_pps=...` | `openh264_video_decoder.cc::Decode` |
| `OpenH264VideoDecoder: emitted frame #N ts=...` | `openh264_video_decoder.cc::EmitFrameFromDecoder` |

### 8.2 内置工具

* **`chrome://media-internals/`**
  选当前播放 → "Player Properties" 里能看到：
  * `video_decoder: OpenH264VideoDecoder`
  * `audio_decoder: FFmpegAudioDecoder`
  * `renderer: RendererImpl`

* **`chrome://gpu/`**
  "Graphics Feature Status" 中 `Video Decode` 项目可确认是否启用了 GPU 硬解（本仓库走软解时此项不影响）。

* **`about:tracing`**
  开启 `media,gpu,viz` 类目，能看到一条贯穿轨：
  `VideoFrameCompositor::Render` → `cc::VideoLayerImpl::AppendQuads` → `Display::DrawAndSwap`

### 8.3 加日志验证某段链路

1. 看 demuxer 喂出来的 packet：在 `FFmpegDemuxerStream::SatisfyPendingRead` 加 `LOG(ERROR)`，能看到每个 packet 的大小/时间戳，即 OpenH264 的输入。
2. 看合成线程拿走帧：在 `VideoRendererImpl::Render` 加日志，确认 algorithm 选了哪个 PTS 的帧。
3. 看 OpenH264 状态：`openh264_video_decoder.cc` 已经在关键路径上有节流日志（前 3 帧 + 每 60 帧一次），需要更详细可临时改阈值。

---

## 9. 与本仓库自定义改动的关系

| 改动 | 文件 | 作用 |
|---|---|---|
| 新增 OpenH264 软解 | `media/filters/openh264_video_decoder.{h,cc}` | 替代 / 补充 FFmpeg 视频解码 |
| 注册 OpenH264 到候选列表 | `media/renderers/default_decoder_factory.cc` | 让 `DecoderSelector` 能选到它 |
| 新增 `kOpenH264` 枚举 | `media/base/decoder.{h,cc}` | `chrome://media-internals/` 中能显示解码器名 |

> 历史：曾尝试集成 `VlcVideoDecoder`（基于 libvlc 的软解器），因 sandbox/CIG/win32k lockdown 等多重限制无法在 renderer 中可靠加载 `libvlc.dll`，相关代码与沙箱白名单已全部回退。

---

## 10. 一键追溯调用栈（速查）

```
[renderer 进程 / 主线程]
  HTMLMediaElement::SelectMediaResource
  → HTMLMediaElement::LoadResource
  → HTMLMediaElement::StartPlayerLoad
  → WebMediaPlayerImpl::Load
  → WebMediaPlayerImpl::DoLoad
  → WebMediaPlayerImpl::StartPipeline

[renderer 进程 / media 线程]
  → PipelineImpl::Start
  → RendererWrapper::Start
  → FFmpegDemuxer::Initialize
  → RendererImpl::Initialize
       ├── AudioRendererImpl::Initialize → AudioDecoderStream → FFmpegAudioDecoder
       └── VideoRendererImpl::Initialize → VideoFrameStream → VideoDecoderSelector
                                            → OpenH264VideoDecoder::Initialize

[每帧解码循环]
  VideoRendererImpl::AttemptRead
  → VideoFrameStream::Read
  → FFmpegDemuxerStream::Read   (产出 H.264 AU, AVCC)
  → DecoderStream::DecodeInternal
  → OpenH264VideoDecoder::Decode
       ├── H264ToAnnexBBitstreamConverter::ConvertNalUnitStreamToByteStream
       ├── ISVCDecoder::DecodeFrameNoDelay              (核心解码)
       └── EmitFrameFromDecoder → VideoFrame::CreateFrame + memcpy YUV
  → output_cb_(frame)
  → VideoRendererImpl::FrameReady
  → VideoRendererAlgorithm::EnqueueFrame

[显示线程 / 合成线程 / GPU 进程]
  VideoFrameCompositor::UpdateCurrentFrame   (每个 vsync)
  → VideoRendererImpl::Render
  → VideoRendererAlgorithm 选帧
  → cc::VideoLayer / VideoLayerImpl
  → cc 提交 CompositorFrame
  → Viz / DisplayCompositor (GPU 进程)
  → swapchain → 屏幕

[音频通路]
  AudioRendererSink::Render (音频线程)
  → AudioRendererImpl::Render
  → AudioOutputDevice (mojo)
  → Audio Service 进程
  → WASAPI / IAudioClient → 系统音频
```

---

## 参考文件清单

| 角色 | 文件 |
|---|---|
| HTML 媒体元素 | `third_party/blink/renderer/core/html/media/html_video_element.{h,cc}` |
| 媒体元素基类 | `third_party/blink/renderer/core/html/media/html_media_element.{h,cc}` |
| WebMediaPlayer 实现 | `third_party/blink/renderer/platform/media/web_media_player_impl.{h,cc}` |
| 媒体管线 | `media/base/pipeline_impl.{h,cc}` |
| 默认 Renderer | `media/renderers/renderer_impl.{h,cc}` |
| 视频 Renderer | `media/renderers/video_renderer_impl.{h,cc}` |
| 音频 Renderer | `media/renderers/audio_renderer_impl.{h,cc}` |
| 帧调度算法 | `media/filters/video_renderer_algorithm.{h,cc}` |
| Decoder 流抽象 | `media/filters/decoder_stream.{h,cc}` |
| Decoder 选择器 | `media/filters/decoder_selector.{h,cc}` |
| Decoder 工厂 | `media/renderers/default_decoder_factory.{h,cc}` |
| H.264 比特流转换 | `media/filters/h264_to_annex_b_bitstream_converter.{h,cc}` |
| OpenH264 解码器 | `media/filters/openh264_video_decoder.{h,cc}` |
| Demuxer | `media/filters/ffmpeg_demuxer.{h,cc}` |
| 合成桥 | `third_party/blink/renderer/platform/media/video_frame_compositor.{h,cc}` |
| Sandbox 白名单 | `sandbox/policy/win/sandbox_win.cc`（`IsThirdPartyVideoDllAllowedForSandbox`） |
