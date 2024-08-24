@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "ROOT=%cd%"
set "OPENSSL_EXE=%ROOT%\openssl\openssl.exe"
set "SIGNTOOL_EXE=%ROOT%\signtool\signtool.exe"

set "OPENSSL_CONF=%ROOT%\openssl.cnf"

set "FILE_PATH=%~1"
if not defined FILE_PATH (
  goto :end_script_with_err
)

set "FILE_NAME=%RANDOM%"
for %%A in ("%FILE_PATH%") do (
  set "FILE_NAME=%RANDOM%-%%~nxA"
)

:re_pvt
call :gen_rnd rr
set "PVT_FILE=%ROOT%\pvt-%rr%-%FILE_NAME%.pem"
:: parallel build can generate same rand number
if exist "%PVT_FILE%" (
  goto :re_pvt
)

:re_cer
call :gen_rnd rr
set "CER_FILE=%ROOT%\cert-%rr%-%FILE_NAME%.pem"
:: parallel build can generate same rand number
if exist "%CER_FILE%" (
  goto :re_cer
)

:re_pfx
call :gen_rnd rr
set "PFX_FILE=%ROOT%\cfx-%rr%-%FILE_NAME%.pfx"
:: parallel build can generate same rand number
if exist "%PFX_FILE%" (
  goto :re_pfx
)

call "%OPENSSL_EXE%" req -x509 -noenc -days 5525 -newkey rsa:4096 -keyout "%PVT_FILE%" -out "%CER_FILE%" ^
  -subj "/CN=GSE/OU=GSE/O=GSE/C=UK" ^
  -addext "basicConstraints=critical,CA:true" ^
  -addext "keyUsage=digitalSignature" ^
  -addext "subjectKeyIdentifier=hash" ^
  -addext "authorityKeyIdentifier=keyid:always,issuer:always" ^
  -addext "subjectAltName=email:GSE,URI:GSE" ^
  -addext "issuerAltName=issuer:copy" ^
  -addext "extendedKeyUsage=codeSigning" ^
  -addext "crlDistributionPoints=URI:GSE" ^
  -extensions v3_req
if %errorlevel% neq 0 (
  goto :end_script_with_err
)

call "%OPENSSL_EXE%" pkcs12 -export -passout pass: -inkey "%PVT_FILE%" -in "%CER_FILE%" -out "%PFX_FILE%"

del /f /q "%CER_FILE%"
del /f /q "%PVT_FILE%"

if %errorlevel% neq 0 (
  goto :end_script_with_err
)

call "%SIGNTOOL_EXE%" sign /d "GSE" /fd sha256 /f "%PFX_FILE%" /p "" "%FILE_PATH%"

del /f /q "%PFX_FILE%"

if %errorlevel% neq 0 (
  goto :end_script_with_err
)

:: exit without error
:end_script
  endlocal
  exit /b 0

:: exit with error
:end_script_with_err
  endlocal
  exit /b 1

:: when every project is built in parallel '/MP' with Visual Studio,
:: the regular random variable might be the same, causing racing
:: this will waste some time and hopefully generate a different number
:: 1: (ref) out random number
:gen_rnd
  setlocal EnableDelayedExpansion
  for /l %%A in (1, 1, 10) do (
    set "_r=!RANDOM!"
  )
  endlocal
  set "%~1=%RANDOM%"
  exit /b
