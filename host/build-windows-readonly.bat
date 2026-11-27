@echo off
rem Builds the READ-ONLY Windows host: it can only share a folder. The code that
rem receives or writes files for a client is not compiled in at all.
go test -tags readonly
if errorlevel 1 exit /b 1
go build -tags readonly -ldflags "-H=windowsgui -s -w" -o echost-readonly.exe
if errorlevel 1 exit /b 1
echo Done: echost-readonly.exe
