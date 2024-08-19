@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "ROOT=%cd%"
set "OPENSSL_EXE=%ROOT%\openssl\openssl.exe"
set "SIGNTOOL_EXE=%ROOT%\signtool\signtool.exe"

set "FILE=%~1"
if not defined FILE (
  goto :end_script_with_err
)

set "FILENAME=%random%"
for %%A in ("%FILE%") do (
  set "FILENAME=%random%-%%~nxA"
)

:re_pvt
call :gen_rnd rr
set "PVT_FILE=%ROOT%\prvt-%rr%-%FILENAME%.pem"
:: parallel build can generate same rand number
if exist "%PVT_FILE%" (
  goto :re_pvt
)

:re_cer
call :gen_rnd rr
set "CER_FILE=%ROOT%\cert-%rr%-%FILENAME%.pem"
:: parallel build can generate same rand number
if exist "%CER_FILE%" (
  goto :re_cer
)

:re_pfx
call :gen_rnd rr
set "PFX_FILE=%ROOT%\cfx-%rr%-%FILENAME%.pfx"
:: parallel build can generate same rand number
if exist "%PFX_FILE%" (
  goto :re_pfx
)

call "%OPENSSL_EXE%" req -newkey rsa:2048 -nodes -keyout "%PVT_FILE%" -x509 -days 5525 -out "%CER_FILE%" ^
  -subj "/O=GSE/CN=GSE" ^
  -addext "extendedKeyUsage=codeSigning" ^
  -addext "basicConstraints=critical,CA:true" ^
  -addext "subjectAltName=email:GSE,DNS:GSE,DNS:GSE" ^
  -addext "keyUsage=digitalSignature,keyEncipherment" ^
  -addext "authorityKeyIdentifier=keyid,issuer:always" ^
  -addext "crlDistributionPoints=URI:GSE" ^
  -addext "subjectKeyIdentifier=hash" ^
  -addext "issuerAltName=issuer:copy" ^
  -addext "nsComment=GSE" ^
  -extensions v3_req
if %errorlevel% neq 0 (
  goto :end_script_with_err
)

call "%OPENSSL_EXE%" pkcs12 -export -out "%PFX_FILE%" -inkey "%PVT_FILE%" -in "%CER_FILE%" -passout pass:

del /f /q "%CER_FILE%"
del /f /q "%PVT_FILE%"

if %errorlevel% neq 0 (
  goto :end_script_with_err
)

call "%SIGNTOOL_EXE%" sign /d "GSE" /fd sha256 /f "%PFX_FILE%" /p "" "%FILE%"

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
    set "_r=!random!"
  )
  endlocal
  set "%~1=%random%"
  exit /b
