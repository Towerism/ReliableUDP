// File: main.cpp
// Martin Fracker
// CSCE 463-500 Spring 2017

#include <iostream>
#include <libraries.h>

#include <ArgumentParser.h>
#include <SenderSocket.h>
#include <Checksum.h>

void printUsage()
{
  std::cout << "Usage: ReliableUDP <host> <power> <window> <rtt> <forward loss> <return loss> <bottleneck>\n";
  std::exit(EXIT_FAILURE);
}

void vMainInfo(const char* infoFormat, va_list args)
{
  printf("%-8s", "Main: ");
  vprintf(infoFormat, args);
}

void mainInfo(const char* infoFormat, ...)
{
  va_list args;
  va_start(args, infoFormat);
  vMainInfo(infoFormat, args);
  va_end(args);
}

void mainError(const char* errorFormat, ...)
{
  va_list args;
  va_start(args, errorFormat);
  vMainInfo(errorFormat, args);
  va_end(args);
  std::exit(EXIT_FAILURE);
}

int main(int argc, char* argv[])
{
  ArgumentParser parser(argc, argv);
  auto args = parser.Parse();
  if (!args.Valid)
  {
    printUsage();
  }
  mainInfo("sender W = %llu, RTT %g sec, loss %g / %g, link %g Mbps\n", args.WindowSize, args.RTT, args.LossForward, args.LossReturn, args.BandwidthBottleneck);
  mainInfo("initializing DWORD array with 2^%llu elements... ", args.Power);
  auto time = timeGetTime();
  UINT64 dwordBufSize = (UINT64)1 << args.Power;
  DWORD *dwordBuf = new DWORD[dwordBufSize]; // user-requested buffer
  for (UINT64 i = 0; i < dwordBufSize; i++) // required initialization
    dwordBuf[i] = i;
  printf("done in %lu ms\n", timeGetTime() - time);
  SenderSocket ss; // instance of your class
  int status;
  LinkProperties lp;
  lp.RTT = args.RTT;
  lp.speed = BITS_IN_MEGABIT * args.BandwidthBottleneck;
  lp.pLoss[FORWARD_PATH] = args.LossForward;
  lp.pLoss[RETURN_PATH] = args.LossReturn;
  if ((status = ss.Open(args.Host, MAGIC_PORT, args.WindowSize, &lp)) != STATUS_OK)
    mainError("connect failed with status %d\n", status);
  mainInfo("connected to %s in %.3f sec, pkt size %d bytes\n", args.Host, ss.GetEstRTT(), MAX_PKT_SIZE);
  auto t = timeGetTime();
  char *charBuf = (char*)dwordBuf; // this buffer goes into socket
  UINT64 byteBufferSize = dwordBufSize << 2; // convert to bytes
  UINT64 off = 0; // current position in buffer
  while (off < byteBufferSize) {
    // decide the size of next chunk
    int bytes = min(byteBufferSize - off, MAX_PKT_SIZE - sizeof(SenderDataHeader));
    // send chunk into socket
    if ((status = ss.Send(charBuf + off, bytes)) != STATUS_OK)
      mainError("send failed with status %d\n", status);
    off += bytes;
  }
  float transferTime;
  if ((status = ss.Close(transferTime)) != STATUS_OK)
    mainError("close failed with status %d\n", status);
  Checksum cs;
  DWORD check = cs.CRC32((UCHAR*)charBuf, byteBufferSize);
  auto bitsTransferred = static_cast<float>(byteBufferSize * BITS_IN_BYTE);
  auto transferRate = bitsTransferred / transferTime / BITS_IN_KILOBIT;
  mainInfo("transfer finished in %.3f sec, %.2f Kbps checksum %X\n", transferTime, transferRate, check);
  auto packetsSent = static_cast<float>(90);//ceil(static_cast<float>(byteBufferSize) / static_cast<float>(MAX_PKT_SIZE)) 
  mainInfo("packets sent %d\n", ceil(static_cast<float>(byteBufferSize) / static_cast<float>(MAX_PKT_SIZE)));
  auto idealRate = bitsTransferred / packetsSent / static_cast<float>(ss.GetEstRTT()) / BITS_IN_KILOBIT;
  mainInfo("estRTT %.3f, ideal rate %.2f Kbps\n", ss.GetEstRTT(), idealRate);
  return 0;
}
