# WebUI 资源自动打包 GRD 流程

本文记录 Xenon Overlay WebUI 资源从源码到 `.pak` 的自动打包流程，当前以 `xenon_overlay/resources/webui/xenon_node/BUILD.gn` 为参考。

## 目标

WebUI 页面通常包含几类资源：

- 静态文件：`*.html`、`*.css`、图片、字体等。
- TypeScript 文件：业务入口、模块代码。
- Mojo WebUI TS 文件：由 `.mojom` 生成，例如 `xenon_node.mojom-webui.ts`。
- Vite 或其它前端构建产物：常见于 `assets/` 目录。

最终目标是生成：

- `resources.grd`
- `grit/<prefix>_resources.h`
- `grit/<prefix>_resources_map.cc`
- `grit/<prefix>_resources_map.h`
- `<prefix>_resources.pak`

浏览器侧再通过 `WebUIDataSource::AddResourcePath()` 或资源 map 把这些资源注册给 `chrome://...` 页面。

## 推荐结构

推荐使用 `build_webui()` 自动处理 TS、Mojo TS、静态文件，再把额外 assets 作为独立 `grdp` 合入最终 `resources.grd`。

当前 `xenon_node` 使用的结构是：

```gn
import("//tools/grit/grit_rule.gni")
import("//ui/webui/resources/tools/build_webui.gni")
import("//ui/webui/resources/tools/generate_grd.gni")

action("generate_xenon_node_assets_manifest") {
  script = "//xenon_overlay/tools/generate_assets_grdp.py"
  outputs = [ "$target_gen_dir/xenon_node_assets_manifest.json" ]
  depfile = "$target_gen_dir/xenon_node_assets_manifest.json.d"
}

generate_grd("build_xenon_node_assets_grdp") {
  out_grd = "$target_gen_dir/xenon_node_assets.grdp"
  manifest_files = [ "$target_gen_dir/xenon_node_assets_manifest.json" ]
}

build_webui("build") {
  grd_prefix = "xenon_node_webui"
  generate_grdp = true
}

generate_grd("build_grd") {
  out_grd = "$target_gen_dir/resources.grd"
  grdp_files = [
    "$target_gen_dir/resources.grdp",
    "$target_gen_dir/xenon_node_assets.grdp",
  ]
}

grit("resources") {
  source = "$target_gen_dir/resources.grd"
  resource_ids = ""
}
```

## Target 职责

### `generate_xenon_node_assets_manifest`

扫描 `assets/` 目录，生成 JSON manifest：

```json
{
  "base_dir": "../../xenon_overlay/resources/webui/xenon_node",
  "files": [
    "assets/foo.svg"
  ]
}
```

这个 manifest 是 `generate_grd()` 可直接消费的格式。

脚本还会生成 depfile：

```text
gen/.../xenon_node_assets_manifest.json: xenon_overlay/tools/generate_assets_grdp.py ...
```

这样 assets 文件发生变化时，Ninja 能重新生成 manifest 和后续 GRD。

### `build_xenon_node_assets_grdp`

把 assets manifest 转成独立 `xenon_node_assets.grdp`。

好处是 assets 扫描逻辑和 WebUI 主构建逻辑分离：

- `build_webui()` 负责 HTML/CSS/TS/Mojo。
- `build_xenon_node_assets_grdp` 负责目录型静态资源。
- 最终 `build_grd` 统一合并。

### `build_webui("build")`

这是 Chromium WebUI 推荐模板，负责自动处理：

- `static_files` 里的 HTML/CSS。
- `ts_files` 的 preprocess 和 TypeScript 编译。
- `mojo_files` 的 copy 和 TypeScript 编译输入接入。
- `ts_deps` 的 path mappings。
- `generate_grdp = true` 时，只输出 `resources.grdp`，不直接生成最终 pak。

因此不需要自己写 `copy_files`、`copy_ts`、`copy_mojo`。

以前手写 `ts_library()` 时需要 copy，是因为 `ts_library.root_dir` 指向 `$target_gen_dir/preprocessed`，源码 TS 和 generated Mojo TS 必须先放进这个目录。换成 `build_webui()` 后，这一步由模板内部的 `preprocess_ts_files` 和 `copy_mojo` 自动完成。

### `build_grd`

把多个 `grdp` 合成最终 `resources.grd`：

```gn
grdp_files = [
  "$target_gen_dir/resources.grdp",
  "$target_gen_dir/xenon_node_assets.grdp",
]
```

其中：

- `resources.grdp` 来自 `build_webui("build")`。
- `xenon_node_assets.grdp` 来自 assets manifest。

### `grit("resources")`

生成最终资源头文件、资源 map 和 pak。

Xenon Overlay 当前保留本地 `grit("resources")`，而不是完全使用 `build_webui()` 默认的 `grit("resources")`，原因是 overlay 资源没有走全局 `default_resource_ids`，需要：

```gn
resource_ids = ""
```

否则会出现类似错误：

```text
Please update gen/tools/gritsettings/default_resource_ids and add a first id
```

## 新 WebUI 接入清单

新增一个 WebUI 资源目录时，建议按下面顺序接入：

1. 引入模板：

```gn
import("//tools/grit/grit_rule.gni")
import("//ui/webui/resources/tools/build_webui.gni")
import("//ui/webui/resources/tools/generate_grd.gni")
```

2. 用 `build_webui()` 描述主资源：

```gn
build_webui("build") {
  grd_prefix = "my_webui"
  generate_grdp = true

  static_files = [
    "index.html",
    "index.css",
  ]

  ts_files = [ "index.ts" ]

  mojo_files = [
    "$root_gen_dir/path/to/my_page.mojom-webui.ts",
  ]
  mojo_files_deps = [ "//path/to:my_page_mojo_bindings_ts__generator" ]

  ts_deps = [ "//ui/webui/resources/mojo:build_ts" ]
  webui_context_type = "trusted"
}
```

3. 如果有 `assets/`，加 assets manifest 和 grdp。

4. 用 `generate_grd("build_grd")` 合并 `resources.grdp` 和 assets grdp。

5. 用本地 `grit("resources")` 输出资源，并按需要保留 `resource_ids = ""`。

## 验证命令

单独验证资源构建：

```powershell
autoninja -C .\out\Debug_64\ xenon_overlay/resources/webui/xenon_node:resources
```

查看生成的主 GRD 是否合入两个 part：

```powershell
Select-String -Path .\out\Debug_64\gen\xenon_overlay\resources\webui\xenon_node\resources.grd -Pattern "part file"
```

期望类似：

```xml
<part file="resources.grdp" />
<part file="xenon_node_assets.grdp" />
```

查看 assets manifest：

```powershell
Get-Content .\out\Debug_64\gen\xenon_overlay\resources\webui\xenon_node\xenon_node_assets_manifest.json
```

## 常见坑

### 不要手写 `copy_files`

如果已经使用 `build_webui()`，普通 TS 和 Mojo TS 都会自动进入 preprocessed 目录。

只有在手写 `ts_library()`，并且 `root_dir` 指向 generated/preprocessed 目录时，才需要手写 copy。

### `build_webui()` 默认会生成 pak

默认 `build_webui()` 会自己创建 `grit("resources")`。如果当前模块不想接入全局 resource ids，应该设置：

```gn
generate_grdp = true
```

然后自己用 `generate_grd()` 和 `grit()` 包最终资源。

### assets 目录为空是允许的

当前 `xenon_node` 目录没有 `assets/` 时，manifest 会生成空列表：

```json
{
  "base_dir": "../../xenon_overlay/resources/webui/xenon_node",
  "files": []
}
```

这样流程仍然完整，后续新增 assets 文件时不需要改 GN 结构。

### `.map` 文件默认不打包

`generate_assets_grdp.py` 会跳过 `*.map`，避免把 source map 打进 pak。

### 资源路径以 WebUI 目录为 base

assets 文件 `assets/foo.svg` 最终 resource path 也是：

```text
assets/foo.svg
```

页面里可以按对应 WebUI URL 访问。
