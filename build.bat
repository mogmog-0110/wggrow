@echo off
rem wggrow build. Run from an "x64 Native Tools Command Prompt for VS 2022"
rem (so cl / link are on PATH), or via a shell that has set the MSVC env.
rem Prereq: run  powershell -File fetch_deps.ps1  once to fetch Agility SDK + DXC.
cd /d %~dp0
set DXC=third_party\dxc\build\native\bin\x64\dxc.exe
set AG=third_party\agility\build\native\include
if not exist build mkdir build
if not exist build\D3D12 mkdir build\D3D12

echo [1/3] compiling shaders (DXIL SM6.8) ...
%DXC% -T lib_6_8               -Fo build\wg_grow.dxil   shaders\wg_grow.hlsl   || exit /b 1
%DXC% -T cs_6_6  -E CSMain     -Fo build\raymarch.dxil  shaders\raymarch.hlsl  || exit /b 1
%DXC% -T cs_6_6  -E CSMain     -Fo build\composite.dxil shaders\composite.hlsl || exit /b 1

echo [2/3] compiling exe ...
cl /nologo /std:c++20 /EHsc /utf-8 /O2 /Fobuild\ src\main.cpp /I%AG% /Ithird_party\stb /Fe:build\wggrow.exe /link d3d12.lib dxgi.lib || exit /b 1

echo [3/3] deploying Agility runtime ...
copy /Y third_party\agility\build\native\bin\x64\D3D12Core.dll     build\D3D12\ >nul
copy /Y third_party\agility\build\native\bin\x64\d3d12SDKLayers.dll build\D3D12\ >nul

echo done.  run:  build\wggrow.exe --preset coral --out out.png
