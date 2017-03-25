// File: main.cpp
// Martin Fracker
// CSCE 463-500 Spring 2017

#include <iostream>
#include <libraries.h>

#include <ArgumentParser.h>
#include <SenderSocket.h>

void printUsage()
{
  std::cout << "Usage: ReliableUDP <host> <power> <window> <rtt> <forward loss> <return loss> <bottleneck>\n";
  std::exit(EXIT_FAILURE);
}

void mainError(const char* errorFormat, ...)
{
  printf("%-8s", "Main: ");
  va_list args;
  va_start(args, errorFormat);
  vprintf(errorFormat, args);
  va_end(args);
  printf("\n");
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
  UINT64 dwordBufSize = (UINT64)1 << args.Power;
  DWORD *dwordBuf = new DWORD[dwordBufSize]; // user-requested buffer
  for (UINT64 i = 0; i < dwordBufSize; i++) // required initialization
    dwordBuf[i] = i;
  SenderSocket ss; // instance of your class
  int status;
  LinkProperties lp;
  lp.RTT = args.RTT;
  lp.speed = BITS_IN_MEGABIT * args.BandwidthBottleneck;
  lp.pLoss[FORWARD_PATH] = args.LossForward;
  lp.pLoss[RETURN_PATH] = args.LossReturn;
  if ((status = ss.Open(args.Host, MAGIC_PORT, args.WindowSize, &lp)) != STATUS_OK)
    mainError("connect failed with status %d", status);
  char *charBuf = (char*)dwordBuf; // this buffer goes into socket
  UINT64 byteBufferSize = dwordBufSize << 2; // convert to bytes
  UINT64 off = 0; // current position in buffer
  while (off < byteBufferSize) {
    // decide the size of next chunk
    int bytes = min(byteBufferSize - off, MAX_PKT_SIZE - sizeof(SenderDataHeader));
    // send chunk into socket
    if ((status = ss.Send(charBuf + off, bytes)) != STATUS_OK)
      // error handing: print status and quit
      off += bytes;
  }
  if ((status = ss.Close()) != STATUS_OK)
    mainError("close failed with status %d", status);
  return 0;
}
