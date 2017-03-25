#include "ArgumentParser.h"
#include <string>

Arguments ArgumentParser::Parse() const
{
  Arguments args;
  if (argc != 8)
  {
    args.Valid = false;
    return args;
  }
  try
  {
    args.Host = argv[1];
    args.Power = std::stoul(argv[2]);
    args.WindowSize = std::stoul(argv[3]);
    args.RTT = std::stof(argv[4]);
    args.LossForward = std::stof(argv[5]);
    args.LossReturn = std::stof(argv[6]);
    args.BandwidthBottleneck = std::stof(argv[7]);
  } catch(...)
  {
    args.Valid = false;
  }
  return args;
}
