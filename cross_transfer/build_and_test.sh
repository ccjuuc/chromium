#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${CROSS_TRANSFER_BUILD_DIR:-${SCRIPT_DIR}/build}"
mkdir -p "${BUILD_DIR}"
CXX="${CXX:-clang++}"
CXXFLAGS=(-std=c++20 -O2 -g -Wall -Wextra -UNDEBUG -pthread -I"${SCRIPT_DIR}/include")
LDFLAGS=(-pthread)
if [[ -n "${CROSS_TRANSFER_SANITIZE:-}" ]]; then
  CXXFLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  LDFLAGS+=(-fsanitize=address,undefined)
fi
if [[ -n "${OPENSSL_ROOT_DIR:-}" ]]; then
  CXXFLAGS+=(-I"${OPENSSL_ROOT_DIR}/include")
  LDFLAGS+=(-L"${OPENSSL_ROOT_DIR}/lib" -Wl,-rpath,"${OPENSSL_ROOT_DIR}/lib" -lssl -lcrypto)
elif [[ -f "${REPO_DIR}/out/debug_arm64/libthird_party_boringssl.dylib" ]]; then
  CXXFLAGS+=(-I"${REPO_DIR}/third_party/boringssl/src/include")
  LDFLAGS+=(-L"${REPO_DIR}/out/debug_arm64" -Wl,-rpath,"${REPO_DIR}/out/debug_arm64" -lthird_party_boringssl)
elif command -v pkg-config >/dev/null && pkg-config --exists openssl; then
  read -r -a TLS_CFLAGS <<< "$(pkg-config --cflags openssl)"
  read -r -a TLS_LDFLAGS <<< "$(pkg-config --libs openssl)"
  CXXFLAGS+=("${TLS_CFLAGS[@]}")
  LDFLAGS+=("${TLS_LDFLAGS[@]}")
else
  echo "OpenSSL/BoringSSL is required. Set OPENSSL_ROOT_DIR or configure pkg-config openssl." >&2
  exit 1
fi
if [[ "$(uname -s)" == Darwin ]]; then
  LDFLAGS+=(-framework Foundation)
fi
if [[ "${CROSS_TRANSFER_TEST_ONLY:-0}" != 1 ]]; then
OBJECTS=()
for source in "${SCRIPT_DIR}"/src/*.cc; do
  object="${BUILD_DIR}/$(basename "${source}" .cc).o"
  "${CXX}" "${CXXFLAGS[@]}" -c "${source}" -o "${object}"
  OBJECTS+=("${object}")
done
if [[ "$(uname -s)" == Darwin ]]; then
  "${CXX}" "${CXXFLAGS[@]}" -c "${SCRIPT_DIR}/src/platform_adapter.mm" -o "${BUILD_DIR}/platform_adapter_apple.o"
  OBJECTS+=("${BUILD_DIR}/platform_adapter_apple.o")
fi
for source in "${SCRIPT_DIR}"/tests/test_*.cc; do
  name="$(basename "${source}" .cc)"
  "${CXX}" "${CXXFLAGS[@]}" "${source}" "${OBJECTS[@]}" "${LDFLAGS[@]}" -o "${BUILD_DIR}/${name}"
done
fi
if [[ "${CROSS_TRANSFER_BUILD_ONLY:-0}" == 1 ]]; then exit 0; fi
for test in "${SCRIPT_DIR}"/tests/test_*.cc; do
  name="$(basename "${test}" .cc)"
  case "${name}" in test_https_relay|test_tls_transport|*relay_client*|*tls_client*) continue ;; esac
  echo "Running ${name}"
  "${BUILD_DIR}/${name}"
done
if [[ -f "${SCRIPT_DIR}/tests/test_https_relay.py" ]]; then
  python3 "${SCRIPT_DIR}/tests/test_https_relay.py" "${BUILD_DIR}/test_https_relay" "${BUILD_DIR}/test_tls_transport" "${BUILD_DIR}/test_test_job"
fi
echo "All cross_transfer tests passed."
