# Copyright 2026 The Xenon Overlay Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$CredentialRef
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') {
    throw 'Windows Credential Manager is required.'
}
if ([Text.Encoding]::UTF8.GetByteCount($CredentialRef) -gt 512 -or $CredentialRef.Contains([char]0)) {
    throw 'CredentialRef must have 1..512 UTF-8 bytes and no NUL.'
}
if (-not $PSCmdlet.ShouldProcess($CredentialRef, 'Store the current user Xenon CA bearer credential')) {
    return
}

if (-not ('Xenon.ManagedCaCredential' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Xenon {
    public static class ManagedCaCredential {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct Credential {
            public uint Flags;
            public uint Type;
            public string TargetName;
            public string Comment;
            public long LastWritten;
            public uint CredentialBlobSize;
            public IntPtr CredentialBlob;
            public uint Persist;
            public uint AttributeCount;
            public IntPtr Attributes;
            public string TargetAlias;
            public string UserName;
        }
        [DllImport("advapi32.dll", EntryPoint = "CredWriteW", CharSet = CharSet.Unicode,
            SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool Write(ref Credential credential, uint flags);
    }
}
'@
}

$secret = Read-Host 'Managed CA bearer token' -AsSecureString
$plainBuffer = [IntPtr]::Zero
$blob = [IntPtr]::Zero
$tokenBytes = $null
try {
    $plainBuffer = [Runtime.InteropServices.Marshal]::SecureStringToGlobalAllocUnicode($secret)
    $plainText = [Runtime.InteropServices.Marshal]::PtrToStringUni($plainBuffer)
    if ($plainText -cnotmatch '\A[A-Za-z0-9._~+/-]+=*\z' -or $plainText.Length -gt 2560) {
        throw 'Token must contain 1..2560 RFC 6750 bearer-token characters (Windows credential limit).'
    }
    $tokenBytes = [Text.Encoding]::UTF8.GetBytes($plainText)
    $plainText = $null
    $blob = [Runtime.InteropServices.Marshal]::AllocHGlobal($tokenBytes.Length)
    [Runtime.InteropServices.Marshal]::Copy($tokenBytes, 0, $blob, $tokenBytes.Length)
    $credential = New-Object Xenon.ManagedCaCredential+Credential
    $credential.Type = 1 # CRED_TYPE_GENERIC
    $credential.TargetName = $CredentialRef
    $credential.CredentialBlobSize = $tokenBytes.Length
    $credential.CredentialBlob = $blob
    $credential.Persist = 2 # CRED_PERSIST_LOCAL_MACHINE: current user, across logons.
    $credential.UserName = 'Xenon'
    if (-not [Xenon.ManagedCaCredential]::Write([ref]$credential, 0)) {
        throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
    Write-Output "Stored credential '$CredentialRef' for the current Windows user."
} finally {
    $plainText = $null
    if ($null -ne $tokenBytes) {
        [Array]::Clear($tokenBytes, 0, $tokenBytes.Length)
        if ($blob -ne [IntPtr]::Zero) {
            [Runtime.InteropServices.Marshal]::Copy($tokenBytes, 0, $blob, $tokenBytes.Length)
        }
    }
    if ($blob -ne [IntPtr]::Zero) {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($blob)
    }
    if ($plainBuffer -ne [IntPtr]::Zero) {
        [Runtime.InteropServices.Marshal]::ZeroFreeGlobalAllocUnicode($plainBuffer)
    }
    $secret.Dispose()
}
