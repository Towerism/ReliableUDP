// File: printing.cpp
// Martin Fracker
// CSCE 463-500 Spring 2017

#include "printing.h"

#include <cstdarg>
#include <cstdio>

void PrintDebug(char* format, ...)
{
#if _DEBUG
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
#endif
}
