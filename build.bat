@echo off
:: Thin shim. The Windows build lives in build.ps1 - keeping a second full
:: implementation here would only let the two drift. This exists so double
:: clicking still works and so `build.bat --clean` keeps doing what it always
:: did; every argument is forwarded untouched.
::
:: -ExecutionPolicy Bypass applies to this process only. It does not change the
:: machine policy, and it is what lets the script run on a default Windows
:: install where the policy is Restricted.
setlocal
set "PS_SCRIPT=%~dp0build.ps1"
if not exist "%PS_SCRIPT%" (
    echo [ERROR] build.ps1 not found next to build.bat: "%PS_SCRIPT%"
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" %*
set "BUILD_RC=%errorlevel%"

:: Pause only for a double click, so the window does not vanish with the result
:: still in it. Explorer launches a .bat as:  cmd.exe /c ""C:\path\build.bat" "
:: The doubled quote after /c is what separates that from a deliberate
:: `cmd /c build.bat` from a terminal or CI, which must return without blocking.
:: Testing for a bare "/c" would stall every scripted run, including PowerShell's
:: own `.\build.bat`, which also shells out through cmd /c.
::
:: Substring comparison rather than findstr: findstr's \" escaping does not
:: survive being handed a pattern containing doubled quotes, and silently never
:: matches. Delayed expansion is only switched on here, after %* was consumed,
:: so an argument containing '!' is not mangled on its way to PowerShell.
setlocal EnableDelayedExpansion
set "CL=!CMDCMDLINE!"
set "STRIPPED=!CL:/c ""=!"
if not "!STRIPPED!"=="!CL!" pause
endlocal

exit /b %BUILD_RC%
