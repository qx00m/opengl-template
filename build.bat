@echo off

pushd ..\build

rem C4201: nonstandard extension used: nameless struct/union
rem C4204: nonstandard extension used: non-constant aggregate initializer

del code_*.pdb >nul 2>nul
echo compiling > build.lock
cl /FC /GS- /LD /O2 /Oi /utf-8 /wd4201 /wd4204 /Wall /WX /Z7 /nologo ..\src\code.c /link /DEBUG /DLL /NOENTRY /NODEFAULTLIB /OPT:ICF /OPT:REF /PDB:code_%RANDOM%.pdb /SUBSYSTEM:WINDOWS
del build.lock

cl /FC /GS- /O2 /Oi /utf-8 /wd4201 /wd4204 /Wall /WX /Z7 /nologo ..\src\main.c /link /DEBUG /ENTRY:WinEntry /NODEFAULTLIB /OPT:ICF /OPT:REF /SUBSYSTEM:WINDOWS kernel32.lib user32.lib gdi32.lib opengl32.lib

popd