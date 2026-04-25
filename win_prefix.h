// Max SDK expects windows.h to be already included, but this isn't the case
// with MinGW.

#define WIN32_LEAN_AND_MEAN

#define _hypot hypot

#include <windows.h>
