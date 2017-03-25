// File: ArgumentParser.h
// Martin Fracker
// CSCE 463-500 Spring 2017
#pragma once
#include <windows.h>

struct Arguments
{
  bool Valid = true;
  char* Host = nullptr;
  UINT64 Power = 0;
  UINT64 WindowSize = 0;
  float RTT = 0;
  float LossForward = 0.;
  float LossReturn = 0.;
  float BandwidthBottleneck = 0.;
};

class ArgumentParser
{
public:
  ArgumentParser(int argc, char** argv) : argc(argc), argv(argv) {}

  Arguments Parse() const;

private:
  int argc;
  char** argv;
};
