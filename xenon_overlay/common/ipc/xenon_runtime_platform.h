// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_IPC_XENON_RUNTIME_PLATFORM_H_
#define XENON_OVERLAY_COMMON_IPC_XENON_RUNTIME_PLATFORM_H_

#include "build/build_config.h"

namespace xenon::ipc {

// Node's platform and architecture describe the running binary, which may
// differ from the host machine when running under emulation. Unknown targets
// stay empty so callers can report unsupported capability instead of guessing.
constexpr const char* PlatformName() {
#if BUILDFLAG(IS_WIN)
  return "win32";
#elif BUILDFLAG(IS_MAC)
  return "darwin";
#elif BUILDFLAG(IS_ANDROID)
  return "android";
#elif BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  return "linux";
#elif BUILDFLAG(IS_FREEBSD)
  return "freebsd";
#elif BUILDFLAG(IS_OPENBSD)
  return "openbsd";
#elif BUILDFLAG(IS_NETBSD)
  return "netbsd";
#elif BUILDFLAG(IS_SOLARIS)
  return "sunos";
#elif BUILDFLAG(IS_AIX)
  return "aix";
#elif BUILDFLAG(IS_FUCHSIA)
  return "fuchsia";
#elif BUILDFLAG(IS_QNX)
  return "qnx";
#else
  return "";
#endif
}

constexpr const char* ArchitectureName() {
#if defined(ARCH_CPU_X86_64)
  return "x64";
#elif defined(ARCH_CPU_ARM64)
  return "arm64";
#elif defined(ARCH_CPU_X86)
  return "ia32";
#elif defined(ARCH_CPU_ARMEL)
  return "arm";
#elif defined(ARCH_CPU_MIPS64EL)
  return "mips64el";
#elif defined(ARCH_CPU_MIPSEL)
  return "mipsel";
#elif defined(ARCH_CPU_MIPS64)
  return "mips64";
#elif defined(ARCH_CPU_MIPS)
  return "mips";
#elif defined(ARCH_CPU_PPC64)
  return "ppc64";
#elif defined(ARCH_CPU_S390X)
  return "s390x";
#elif defined(ARCH_CPU_S390)
  return "s390";
#elif defined(ARCH_CPU_RISCV64)
  return "riscv64";
#elif defined(ARCH_CPU_LOONGARCH64)
  return "loong64";
#else
  return "";
#endif
}

constexpr const char* EndiannessName() {
#if defined(ARCH_CPU_LITTLE_ENDIAN)
  return "LE";
#elif defined(ARCH_CPU_BIG_ENDIAN)
  return "BE";
#else
  return "";
#endif
}

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_COMMON_IPC_XENON_RUNTIME_PLATFORM_H_
