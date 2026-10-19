@echo off
call "D:\BuildTool\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cl.exe /nologo /EHsc /W4 /utf-8 /std:c++20 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /I"C:\users\mayn\Desktop\hotspot\PowerToysRunStandalone\cpp\src" /Fo"C:\users\mayn\Desktop\hotspot\PowerToysRunStandalone\cpp\verify\out\\" /Fe"C:\users\mayn\Desktop\hotspot\PowerToysRunStandalone\cpp\verify\out\path_query_test.exe" "C:\users\mayn\Desktop\hotspot\PowerToysRunStandalone\cpp\verify\path_query_test.cpp" "C:\users\mayn\Desktop\hotspot\PowerToysRunStandalone\cpp\src\path_query.cpp" "C:\users\mayn\Desktop\hotspot\PowerToysRunStandalone\cpp\src\file_rank.cpp" "C:\users\mayn\Desktop\hotspot\PowerToysRunStandalone\cpp\src\winutil.cpp" user32.lib shell32.lib shlwapi.lib advapi32.lib ole32.lib
if errorlevel 1 exit /b 1
"C:\users\mayn\Desktop\hotspot\PowerToysRunStandalone\cpp\verify\out\path_query_test.exe"
exit /b %errorlevel%