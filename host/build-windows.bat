@echo off
rem Builds the Windows host (window version, no console window).
go test
if errorlevel 1 exit /b 1
go build -ldflags "-H=windowsgui -s -w" -o echost.exe
if errorlevel 1 exit /b 1
echo Done: echost.exe
