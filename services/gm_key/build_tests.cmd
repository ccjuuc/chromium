@echo off
setlocal
rem Run from a Visual Studio x64 Native Tools prompt. No SDK or USBKey needed.
rem Usage: services\gm_key\build_tests.cmd [output-directory]
for %%I in ("%~dp0..\..") do set "GM_KEY_ROOT=%%~fI"
if "%~1"=="" (
  set "GM_KEY_OUT=%GM_KEY_ROOT%\out\gm-key-tests"
) else (
  for %%I in ("%~1") do set "GM_KEY_OUT=%%~fI"
)
if not exist "%GM_KEY_OUT%" mkdir "%GM_KEY_OUT%"
if errorlevel 1 exit /b 1
set "GM_KEY_FLAGS=/nologo /std:c++20 /EHsc /W4 /WX /O2 /MD /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
pushd "%GM_KEY_OUT%"
cl %GM_KEY_FLAGS% /I"%GM_KEY_ROOT%" /LD "%GM_KEY_ROOT%\services\gm_key\test\fake_skf.cc" /Fegm_fake_skf.dll /link /INCREMENTAL:NO
if errorlevel 1 goto fail
cl %GM_KEY_FLAGS% /I"%GM_KEY_ROOT%" /DGM_FAKE_MISSING_EXPORT /LD "%GM_KEY_ROOT%\services\gm_key\test\fake_skf.cc" /Fegm_fake_skf_missing.dll /link /INCREMENTAL:NO
if errorlevel 1 goto fail
cl %GM_KEY_FLAGS% /I"%GM_KEY_ROOT%" /DGM_KEY_TESTING "%GM_KEY_ROOT%\services\gm_key\skf_driver.cc" "%GM_KEY_ROOT%\services\gm_key\test\skf_driver_test.cc" /Fegm_skf_test.exe /link wintrust.lib bcrypt.lib /INCREMENTAL:NO
if errorlevel 1 goto fail
cl %GM_KEY_FLAGS% /I"%GM_KEY_ROOT%" "%GM_KEY_ROOT%\services\gm_key\skf_driver.cc" "%GM_KEY_ROOT%\services\gm_key\skf_probe.cc" /Fegm_skf_probe.exe /link wintrust.lib bcrypt.lib /INCREMENTAL:NO
if errorlevel 1 goto fail
gm_skf_test.exe "%GM_KEY_OUT%\gm_fake_skf.dll"
if errorlevel 1 goto fail
popd
exit /b 0
:fail
popd
exit /b 1
