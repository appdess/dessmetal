@echo off
REM Helper script for Windows Packaging with Signing
REM Place this in the root of the repo (next to NeuralAmpModeler folder)

SET "PROJECT_ROOT=%~dp0\NeuralAmpModeler"
CD "%PROJECT_ROOT%\scripts"

REM Set up your signing variables here if needed, or ensure they are in the environment
REM Example: SET SIGNTOOL_PATH="C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\signtool.exe"
REM Example: SET PFX_FILE="C:\path\to\cert.p12"
REM Example: SET PFX_PASS="password"

echo Building Installer for Windows...
call makedist-win.bat full installer

REM Note: The current makedist-win.bat has signing commands commented out. 
REM You may need to uncomment lines 116 in NeuralAmpModeler/scripts/makedist-win.bat 
REM and configure the path to your .p12 file.
pause
