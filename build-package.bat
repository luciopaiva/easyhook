﻿echo off
call "C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC\vcvarsall.bat" x86_amd64
msbuild build.proj /t:Clean;BeforeBuild
msbuild build.proj /t:Build
msbuild build.proj /t:ExportSource;PreparePackage;Package
pause