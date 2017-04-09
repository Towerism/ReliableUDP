// File: printing.h
// Martin Fracker
// CSCE 463-500 Spring 2017

#include <cstdarg>
#include <cstdio>

inline void PrintDebug(char* format, ...)
{
#if _DEBUG
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
#endif
}
