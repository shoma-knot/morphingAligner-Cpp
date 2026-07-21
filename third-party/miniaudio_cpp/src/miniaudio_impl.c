/* Compile miniaudio implementation as C.
   This file is intentionally kept separate from the C++ wrapper
   so that miniaudio is compiled as pure C (avoiding potential
   issues with C++ compilers and miniaudio's C code). */

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
