#!/bin/env bash

PATH=$PATH:/c/Users/johnk/Programming/libjuice/build
PATH="/c/Program Files (x86)/Dr. Memory/bin:$PATH"
./build/Debug/sdlarch.exe /c/Users/johnk/Programming/ArchVizTinyHouse/Plugins/UnrealLibretro/MyCores/nestopia_libretro.dll '/c/Users/johnk/Programming/ArchVizTinyHouse/Plugins/UnrealLibretro/MyROMs/Metroid (USA).nes' & ./build/Debug/sdlarch.exe /c/Users/johnk/Programming/ArchVizTinyHouse/Plugins/UnrealLibretro/MyCores/nestopia_libretro.dll '/c/Users/johnk/Programming/ArchVizTinyHouse/Plugins/UnrealLibretro/MyROMs/Metroid (USA).nes'