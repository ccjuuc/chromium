# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
"""Host tools and private-key permissions for portable macOS CRX packaging."""

import ctypes
import os
from pathlib import Path
import shutil


def find_openssl(requested=None):
    candidates = [requested] if requested else [shutil.which("openssl")]
    if not requested and os.name == "nt":
        candidates += [str(Path(os.environ.get("ProgramFiles", "C:/Program Files")) /
                           "Git/mingw64/bin/openssl.exe")]
    for candidate in candidates:
        if candidate:
            resolved = shutil.which(str(candidate))
            if resolved:
                return Path(resolved).absolute()
    raise ValueError("OpenSSL is required; pass --openssl with its executable path")


def _windows_security():
    from ctypes import wintypes
    security = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    pointer = ctypes.c_void_p
    security.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    security.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, pointer,
                                           wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    security.ConvertSidToStringSidW.argtypes = [pointer, ctypes.POINTER(wintypes.LPWSTR)]
    security.GetNamedSecurityInfoW.argtypes = [wintypes.LPWSTR, ctypes.c_int, wintypes.DWORD,
                                             ctypes.POINTER(pointer), ctypes.POINTER(pointer),
                                             ctypes.POINTER(pointer), ctypes.POINTER(pointer),
                                             ctypes.POINTER(pointer)]
    security.GetNamedSecurityInfoW.restype = wintypes.DWORD
    security.GetAce.argtypes = [pointer, wintypes.DWORD, ctypes.POINTER(pointer)]
    security.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(pointer), ctypes.POINTER(wintypes.DWORD)]
    security.GetSecurityDescriptorDacl.argtypes = [pointer, ctypes.POINTER(wintypes.BOOL),
                                                  ctypes.POINTER(pointer), ctypes.POINTER(wintypes.BOOL)]
    security.SetNamedSecurityInfoW.argtypes = [wintypes.LPWSTR, ctypes.c_int, wintypes.DWORD,
                                             pointer, pointer, pointer, pointer]
    security.SetNamedSecurityInfoW.restype = wintypes.DWORD
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.LocalFree.argtypes = [pointer]
    kernel.LocalFree.restype = pointer

    def sid_string(sid):
        text = wintypes.LPWSTR()
        if not security.ConvertSidToStringSidW(sid, ctypes.byref(text)):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            return text.value
        finally:
            kernel.LocalFree(text)

    token = wintypes.HANDLE()
    if not security.OpenProcessToken(kernel.GetCurrentProcess(), 8, ctypes.byref(token)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        needed = wintypes.DWORD()
        security.GetTokenInformation(token, 1, None, 0, ctypes.byref(needed))
        buffer = ctypes.create_string_buffer(needed.value)
        if not security.GetTokenInformation(token, 1, buffer, len(buffer), ctypes.byref(needed)):
            raise ctypes.WinError(ctypes.get_last_error())
        current_sid = sid_string(pointer.from_buffer(buffer).value)
    finally:
        kernel.CloseHandle(token)
    return security, kernel, sid_string, current_sid


def check_private_key_acl(path):
    """Require key access to be limited to its user, SYSTEM and Administrators."""
    from ctypes import wintypes
    security, kernel, sid_string, current_sid = _windows_security()
    owner, acl, descriptor = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
    result = security.GetNamedSecurityInfoW(str(path), 1, 5, ctypes.byref(owner), None,
                                            ctypes.byref(acl), None, ctypes.byref(descriptor))
    if result:
        raise ctypes.WinError(result)
    try:
        trusted = {current_sid, "S-1-5-18", "S-1-5-32-544"}
        if not owner or sid_string(owner) not in {current_sid, "S-1-5-32-544"} or not acl:
            raise ValueError("CRX signing key must have a private, owned Windows ACL")
        count = ctypes.c_ushort.from_address(acl.value + 4).value
        for index in range(count):
            ace = ctypes.c_void_p()
            if not security.GetAce(acl, index, ctypes.byref(ace)):
                raise ctypes.WinError(ctypes.get_last_error())
            kind, flags = ctypes.string_at(ace, 2)
            if flags & 8 or kind == 1:  # Inherit-only or deny ACE.
                continue
            if kind != 0 or sid_string(ace.value + 8) not in trusted:
                raise ValueError("CRX signing key must have a private Windows ACL; remove other principals")
    finally:
        kernel.LocalFree(descriptor)


def make_private_directory(path):
    """Protect a new temporary directory before creating a key snapshot in it."""
    Path(path).mkdir(mode=0o700)
    if os.name != "nt":
        return
    from ctypes import wintypes
    security, kernel, _, current_sid = _windows_security()
    sddl = f"D:P(A;OICI;FA;;;{current_sid})(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
    descriptor, acl = ctypes.c_void_p(), ctypes.c_void_p()
    if not security.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, 1, ctypes.byref(descriptor), None):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        present, defaulted = wintypes.BOOL(), wintypes.BOOL()
        if not security.GetSecurityDescriptorDacl(descriptor, ctypes.byref(present),
                                                  ctypes.byref(acl), ctypes.byref(defaulted)):
            raise ctypes.WinError(ctypes.get_last_error())
        result = security.SetNamedSecurityInfoW(str(path), 1, 0x80000004, None, None, acl, None)
        if result:
            raise ctypes.WinError(result)
    finally:
        kernel.LocalFree(descriptor)
