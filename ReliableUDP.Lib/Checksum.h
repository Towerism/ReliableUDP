// File: Checksum.h
// Martin Fracker
// CSCE 463-500 Spring 2017
#pragma once
#include <unordered_map>
#include <windows.h>

class Checksum
{
public:
  Checksum();

  DWORD CRC32(UCHAR* buf, size_t len);
  
private:
  std::unordered_map<DWORD, DWORD> crc_table;
};
