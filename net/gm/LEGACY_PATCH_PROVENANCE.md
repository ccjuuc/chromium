# 旧国密补丁来源与公开后续实现

核查日期：2026-10-27。此记录是本地溯源、公开仓库和发布说明审查，
不表示已构建或测试这些公开 BoringSSL 分支。

## 本地材料能证明什么

文件：`F:/aiswork/src/third_party/boringssl/src/0001-add-sm-tls.patch`。

- 补丁头提交：`7c43ad4964fa55d04359e974f2626c2120b0445f`。
- 作者署名：`tianxinchao <tian.xc@asiainfo-sec.com>`。
- 日期：2023-10-13 17:45:41 +0800。
- 标题：`add sm tls;`。
- SHA-256：`09eb2f4bf279f397e1a04346fdae4dad32450eb70304d2e677e9e2a792b7ad7a`。
- 补丁统计：80 文件、11730 行新增、49 行删除。

邮箱域名属于亚信安全，但署名本身不能证明是该公司的正式公开发行版。
本地补丁未跟踪，针对该文件的 Git 日志为空；旧 checkout 的 fetch 指向官方
BoringSSL，push 指向内部仓库。这些信息不能确定补丁最初从哪个公开仓库取得。
本次没有访问内部远端，也没有上传本地补丁搜索其来源。

补丁头的 `7c43ad49` 提交对象在本地不存在，但本地缓存保存了同一作者的后续国密提交：

- `c65074f6166c2e0f77c5dc3924690cb78eb8d5b5`，2023-10-27，
  `support sm tls;`，直接父提交是旧 BoringSSL 基线 `1ccef490`。
  实际提交涉及 76 文件、11312 行新增、49 行删除，不能与 80 文件的补丁视为逐字相同。
- `refs/remotes/origin/cr108` 的本地缓存头为
  `94c361398ef6f138312455d472fb4eeee913d5a2`，2024-04-06，
  `fix mac or linux systemx build error`。
- `refs/remotes/origin/cr108_linux` 的本地缓存头为
  `b8f299d6723eef78caa80029ba7faa05de9f93dd`，2024-03-08，
  `fix configuration compilation issues related to Linux systems`。

从 `c65074f6` 到缓存的 `cr108` 头，差异只有 8 个文件、26 行新增、36 行删除：
主要是构建配置、SM3 头符号、移除 wotrus 开关及工具调用适配。
没有发现这段历史包含算法性能优化、TLS 1.3 key schedule 改进或异步 SKF 改造。
两个缓存分支仍保留旧签名适配的 x64 长度指针类型问题。
这些证据确认存在本地可追溯的后续团队分支；缓存时间不能代表内部远端当前最新状态。

文件中的来源声明与注释表明混合引用了多个项目：

| 部分 | 补丁中保留的声明/注释 |
| --- | --- |
| SM2 实现 | OpenSSL 项目版权声明 |
| SKF、SM2 密钥协商等 | GmSSL 项目版权声明 |
| SM3、SM4 | OpenSSL、Ribose 等版权声明 |
| 国密协议相关定义 | 存在“copy from GMSSL”注释 |

这能确认代码的部分来源沿革，不能确定完整集成补丁的公开上游、所用原始版本，
或是否曾经过另一个 fork 中转。没有证据把它称为 BoringSSL 官方国密补丁，
也没有证据把下面的社区项目称为这份补丁的新版。

## 公开的较新 BoringSSL 国密实现

### MojoSSL

[mogoweb/mojo-ssl](https://github.com/mogoweb/mojo-ssl) 的
[main README](https://github.com/mogoweb/mojo-ssl/blob/main/README.md)
声明以 BoringSSL 为基础，国密实现移植自铜锁，支持 SM2/SM3/SM4、TLCP；
README 所列 SM4 为 CBC，不能扩大解释为 RFC 8998/GCM 已完整支持。

main 有具体的
[SM2 签名 OID 修正](https://github.com/mogoweb/mojo-ssl/commit/db9be08a19c1ffccfc0b9918ea85ec91446ba797)
和 [X509 验签修正](https://github.com/mogoweb/mojo-ssl/commit/2675bc57fe24d6a01ba893a71fbee64068bf8c16)。
其最后提交为 2025-09-13 的文档更新。

较新的 [for_cr146 分支](https://github.com/mogoweb/mojo-ssl/tree/for_cr146)
核验 HEAD 为 `87114ce5d6c6055b678eed575a20f6ad40f5e3ef`，日期 2026-04-25。
但该 revision 的
[TLCP 握手](https://github.com/mogoweb/mojo-ssl/blob/87114ce5d6c6055b678eed575a20f6ad40f5e3ef/ssl/tlcp_handshake.cc#L38)
仍标为 skeleton，主体直接返回 true；
[集成测试](https://github.com/mogoweb/mojo-ssl/blob/87114ce5d6c6055b678eed575a20f6ad40f5e3ef/ssl/test/tlcp_test.cc#L48)
仍 TODO 实际握手。

[SM4 实现](https://github.com/mogoweb/mojo-ssl/blob/87114ce5d6c6055b678eed575a20f6ad40f5e3ef/crypto/sm4/sm4.cc#L333)
全部轮次采用 `SM4_T_slow`，对应
[2026-04-15 提交](https://github.com/mogoweb/mojo-ssl/commit/ac19fde5bc6406465838cfaeaadd50a066fec89b)
以验证正确性为目的。未发现该分支的 SM4 x64 汇编/AVX/AES-NI 实现。
“safe slow”命名也不能证明恒定时间，代码仍存在数据相关的 S-box 索引。

这些是迁移参考；不能把 for_cr146 当作已完成、已验证 Windows x64 性能的国密浏览器底座。

### 近期 SM4 专项分支

[stevenzxs/BoringSSL 的 gm-v43.3.0 分支](https://github.com/stevenzxs/BoringSSL/tree/gm-v43.3.0)
标明基于 Electron v43.3.0。核验 HEAD 为
`875cbd4347d6494b79eaa3a4abf421cc47980aa3`，日期 2026-10-08。

其主要工作是 SM4/SM4-GCM；
[2026-10-08 的提交](https://github.com/stevenzxs/BoringSSL/commit/65edc89c62ba4eac4d3f5967e759bb6c8b752197)
修正了 GCM 对密文计算认证和认证成功后释放明文的行为。
它不是已完成 SM2、TLCP、USBKey 的整体方案，未发现 Windows x64 专项性能数据。
正确性修正也不能解释为性能提高。

补充核查：该 revision 的
[发布工作流](https://github.com/stevenzxs/BoringSSL/blob/875cbd4347d6494b79eaa3a4abf421cc47980aa3/.github/workflows/publish-gm-source.yml#L29)
实际配置了 Windows 2022 和 Linux 构建、ctest；本次未核验工作流运行结果。
[活动的 SM4-GCM 测试](https://github.com/stevenzxs/BoringSSL/blob/875cbd4347d6494b79eaa3a4abf421cc47980aa3/crypto/sm4/sm4_gcm_test.cc#L7)
包含往返、篡改检测和 RFC 8998 附录 A 向量。算法测试向量不代表完整的 TLS 握手实现。

### 其他有真实算法代码的 BoringSSL 分支

| 仓库/分支 | 核验版本 | 能力与边界 |
| --- | --- | --- |
| [HunterChan-SR/BoringSSL-GM / main](https://github.com/HunterChan-SR/BoringSSL-GM/tree/main) | `460f9562a3e61c4cba844fb81f5261e4a2ab7bf8`，2025-11-21，基线 `0.20240913.0` | 有 SM2 加解密/签名和 SM3，未发现 SM4、完整 RFC 8998 或 TLCP。README 定位实验研究。 |
| [zhizhi-dev/boringssl-with-sm / sm](https://github.com/zhizhi-dev/boringssl-with-sm/tree/sm) | `07825d6d07b806f176b57870c302ab677f9560ef`，2024-01-19 | 有 SM2/SM3/SM4 原语，部分 SM4-GCM/SM3 TLS 接入；缺 SM2 TLS 签名和曲线协商，未发现 TLCP。 |

Hunter 的
[bcm.c](https://github.com/HunterChan-SR/BoringSSL-GM/blob/460f9562a3e61c4cba844fb81f5261e4a2ab7bf8/crypto/fipsmodule/bcm.c#L107)
实际包含 SM2/SM3 实现，但 `sm2_test.cc` 中的测试声明均被注释，不能当作活动测试覆盖。
SM3 有活动用例。`tmp` 分支的 README 明写 SM2/SM4 未实现，不是更完整的替代版本。

zhizhi 的
[完整差异](https://github.com/zhizhi-dev/boringssl-with-sm/compare/c38dc29860a72540eb2c4fdb8a8bfb27ef94ddf3...07825d6d07b806f176b57870c302ab677f9560ef)
显示登记了 C6/C7，但 AEAD 映射只完成 SM4-GCM，SM4-CCM 仍走不支持路径。
`ssl_privkey.cc`、`ssl_key_share.cc`、`extensions.cc` 缺 SM2 握手接入。
没有核验到 Windows x64 或飞天 SKF 支持，不能把它当作完整国密 TLS 新版。

### 排除的误匹配

- [rigel-7/boringssl](https://github.com/rigel-7/boringssl)：描述提及计划实现国密，
  但公开 `master`/`dev` 都指向官方 BoringSSL 的
  [`b34976ca`](https://github.com/google/boringssl/commit/b34976cae99f8d1b864dbab31e20fc00d06acb09)
  （2024-07-10）。所查算法目录无 SM 实现，仍声明 `OPENSSL_NO_SM2/SM3/SM4`。
- [ayayay1/gm-browser](https://github.com/ayayay1/gm-browser)：README 明确采用
  dlopen 铜锁与 BoringSSL 隔离，属于双库集成，不是国密 BoringSSL 分支。
- [zhenyuwei/Guomi](https://github.com/zhenyuwei/Guomi)：公开内容主要是说明、
  证书脚本、OpenSSL 包、TLS 示例和静态库，未提供完整可审计的国密 BoringSSL 实现。
- 所查 openEuler BoringSSL SP4 打包来自官方 BoringSSL，只列构建脚本修正；
  Chromium 128 的打包补丁列表也未发现国密接入，不能作为已支持的证据。

这次检索确认有第三方 BoringSSL 国密分支。MojoSSL main 最接近可参考的 TLCP 接入；
其他项目提供算法或部分协议实现。尚未核验到同时满足 Chromium 153、完整 RFC 8998/TLCP、
Windows x64、飞天 USBKey 和浏览器证书验证要求的现成分支。

## 成熟算法库的新版

这些发行版不是本地集成补丁的后续版本，但可作为算法审查和互操作参考：

- [铜锁 8.5.0](https://github.com/Tongsuo-Project/Tongsuo/releases/tag/8.5.0)，
  发布于 2026-09-30。发布说明包括 SM4-GCM、其他密码算法和 TLS 性能优化、
  ClientHello 多 key_share、TLCP 双证书 keyUsage 检查及相关协议修正。
  官方“最多翻倍”是对其特定方案/配置相较 8.4.0 的描述，不是本浏览器或飞天 Key 的实测。
  当前隔离后端已经使用该版本。
- [GmSSL 3.2.0](https://github.com/guanzhi/GmSSL/releases/tag/v3.2.0)，
  发布于 2026-06-21。发布说明列出国密算法性能提升和协议套件扩展。
  源树有 `sm2_z256_amd64.S`、`sm3_avx2.c`、`sm4_aesni.c`、`sm4_avx2.c` 等实现；
  文件存在不代表已经适配并启用了本项目 Windows x64 构建。

截至核查时，官方 BoringSSL 的
[`e651545d` revision](https://github.com/google/boringssl/commit/e651545da6e26e1f4420723a3b69a78ed1b9c51d)
（2026-10-26 UTC）仍在
[opensslconf.h](https://github.com/google/boringssl/blob/e651545da6e26e1f4420723a3b69a78ed1b9c51d/include/openssl/opensslconf.h)
声明 `OPENSSL_NO_SM2/SM3/SM4`，不能通过更新官方 BoringSSL 自动获得这些国密能力。

## 对单库方案的影响

保留当前 Chromium 配套 BoringSSL 基线；按来源、许可证、恒定时间、测试及 Windows x64
适配结果选择具体算法实现。旧补丁和社区分支可参考接入方式，但不整包移植。
旧补丁已发现 `unsigned int*` 强转 `size_t*` 的 x64 写越界风险、全局协议状态和同步 SKF
调用，这些不应带入新方案。

尚未找到可直接证明“本地补丁有官方新版、且已完成 Chromium 153 与 Windows x64 优化”
的公开证据。更完整的迁移边界见 [SINGLE_LIBRARY_DESIGN.md](SINGLE_LIBRARY_DESIGN.md)。
