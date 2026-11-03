@echo off
rem Starts the Boro Video Wizard without a console window hanging around.
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -STA -File "%~dp0BoroWizard.ps1"
