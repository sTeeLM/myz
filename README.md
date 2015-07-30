# myz
i686-w64-mingw32-gcc -o myz.exe -D_FILE_OFFSET_BITS=64 main.c -L lib/win32/ -llzma 
gcc -o myz -D_FILE_OFFSET_BITS=64 main.c -llzma
