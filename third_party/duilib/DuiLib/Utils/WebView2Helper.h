#ifndef __WEBVIEW2HELPER_H__
#define __WEBVIEW2HELPER_H__

#pragma once

#include <Windows.h>
#include <unknwn.h>
#include <wchar.h>
#include "VersionHelpers.h"

namespace DuiLib
{
	namespace WebView2Helper
	{

		typedef HRESULT(__stdcall *PFN_CreateCoreWebView2EnvironmentWithOptions)(
			LPCWSTR browserExecutableFolder,
			LPCWSTR userDataFolder,
			IUnknown* environmentOptions,
			IUnknown* environmentCreatedHandler);

		typedef HRESULT(__stdcall *PFN_GetAvailableCoreWebView2BrowserVersionString)(
			LPCWSTR browserExecutableFolder,
			LPWSTR* versionInfo);










		inline HMODULE LoadWebView2Loader()
		{
			static HMODULE s_hModule = NULL;
			if (s_hModule != NULL) return s_hModule;


			s_hModule = ::LoadLibraryW(L"WebView2Loader.dll");
			if (s_hModule != NULL) return s_hModule;


			WCHAR szBase[MAX_PATH] = { 0 };
			WCHAR szPath[MAX_PATH] = { 0 };
			const WCHAR* szArch =
#ifdef _WIN64
				L"x64";
#else
				L"x86";
#endif

			if (::GetModuleFileNameW(NULL, szBase, MAX_PATH) > 0) {
				WCHAR* pSlash = wcsrchr(szBase, L'\\');
				if (pSlash != NULL) *pSlash = 0;


				swprintf_s(szPath, MAX_PATH, L"%s\\3rd\\WebView2\\lib\\%s\\WebView2Loader.dll", szBase, szArch);
				s_hModule = ::LoadLibraryW(szPath);
				if (s_hModule != NULL) return s_hModule;


				swprintf_s(szPath, MAX_PATH, L"%s\\..\\3rd\\WebView2\\lib\\%s\\WebView2Loader.dll", szBase, szArch);
				s_hModule = ::LoadLibraryW(szPath);
				if (s_hModule != NULL) return s_hModule;


				swprintf_s(szPath, MAX_PATH, L"%s\\WebView2Loader.dll", szBase);
				s_hModule = ::LoadLibraryW(szPath);
				if (s_hModule != NULL) return s_hModule;
			}


			HMODULE hSelf = ::GetModuleHandleW(L"DuiLib.dll");
			if (hSelf != NULL && ::GetModuleFileNameW(hSelf, szBase, MAX_PATH) > 0) {
				WCHAR* pSlash = wcsrchr(szBase, L'\\');
				if (pSlash != NULL) *pSlash = 0;
				swprintf_s(szPath, MAX_PATH, L"%s\\WebView2Loader.dll", szBase);
				s_hModule = ::LoadLibraryW(szPath);
				if (s_hModule != NULL) return s_hModule;
			}
			return s_hModule;
		}


		inline PFN_CreateCoreWebView2EnvironmentWithOptions GetCreateEnvironmentFunc()
		{
			HMODULE hModule = LoadWebView2Loader();
			if (hModule == NULL) return NULL;
			return (PFN_CreateCoreWebView2EnvironmentWithOptions)::GetProcAddress(hModule, "CreateCoreWebView2EnvironmentWithOptions");
		}


		inline PFN_GetAvailableCoreWebView2BrowserVersionString GetVersionFunc()
		{
			HMODULE hModule = LoadWebView2Loader();
			if (hModule == NULL) return NULL;
			return (PFN_GetAvailableCoreWebView2BrowserVersionString)::GetProcAddress(hModule, "GetAvailableCoreWebView2BrowserVersionString");
		}


		inline bool IsWebView2RuntimeAvailable()
		{
			PFN_GetAvailableCoreWebView2BrowserVersionString pfnGetVersion = GetVersionFunc();
			if (pfnGetVersion == NULL) return false;
			LPWSTR lpszVersion = NULL;
			HRESULT hr = pfnGetVersion(NULL, &lpszVersion);
			if (SUCCEEDED(hr) && lpszVersion != NULL) {
				::CoTaskMemFree(lpszVersion);
				return true;
			}
			return false;
		}


		inline bool IsWebView2Supported()
		{
			return IsWindows10OrGreater() && IsWebView2RuntimeAvailable();
		}
	}
}

#endif
