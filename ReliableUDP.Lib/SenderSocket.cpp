// File: SenderSocket.cpp
// Martin Fracker
// CSCE 463-500 Spring 2017
#include "SenderSocket.h"
#include <windows.h>
#include <cstdio>
#include <string>

#define BETA 0.25
#define ALPHA 0.125

SenderSocket::SenderSocket() 
  : ConstructionTime(timeGetTime()), sndBase(-1), nextSeq(0), FullSlots(0), EmptySlots(1)
{
  WSADATA wsaData;
  WORD wVersionRequested = MAKEWORD(2, 2);
  if (WSAStartup(wVersionRequested, &wsaData) != 0) {
    printf("WSAStartup error %d\n", WSAGetLastError());
    std::exit(EXIT_FAILURE);
  }
  Socket = socket(AF_INET, SOCK_DGRAM, 0);
  if (Socket == INVALID_SOCKET) {
    printf("socket() generated error %d\n", WSAGetLastError());
    std::exit(EXIT_FAILURE);
  }
  struct sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_port = htons(0);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(Socket, (struct sockaddr*)(&local), sizeof(local)) == SOCKET_ERROR) {
    printf("bind() failed with error %d\n", WSAGetLastError());
    std::exit(EXIT_FAILURE);
  }
  // ensure non-blocking mode is off
  ioctlsocket(Socket, FIONBIO, nullptr);
  // start ack thread
  AckThread = std::thread(&SenderSocket::AckPackets, this);
}

SenderSocket::~SenderSocket()
{
  AckThread.join();
  WSACleanup();
}

bool SenderSocket::AckIsValid(DWORD ack) const
{
  return static_cast<int>(ack) >= sndBase && ack <= nextSeq;
}

void SenderSocket::StartTimer()
{
}

void SenderSocket::StopTimer()
{
}

float SenderSocket::CalculateRTO(float rtt)
{
  estRTT = (1 - ALPHA) * oldEstRTT + ALPHA * rtt;
  devRTT = (1 - BETA) * oldDevRTT + BETA * abs(rtt - estRTT);
  Rto = estRTT + 4 * devRTT;
  oldEstRTT = estRTT;
  oldDevRTT = devRTT;
  return Rto;
}

int SenderSocket::Open(const char* host, DWORD port, DWORD senderWindow, LinkProperties* lp)
{
  if (Connected)
    return ALREADY_CONNECTED;
  if (!RemoteInfoFromHost(host, port)) 
    return INVALID_NAME;
  sndWindow = senderWindow;
  SenderSynHeader synHeader;
  synHeader.lp = *lp;
  synHeader.lp.bufferSize = senderWindow + 50;
  synHeader.sdh.flags.SYN = 1;
  synHeader.sdh.seq = 0;
  for (size_t attempt = 1; attempt <= 3; ++attempt) {
#if _DEBUG
    printf("[%6.3f] --> ", Time());
#endif
    if (!SendPacket((char*)(&synHeader), sizeof(SenderSynHeader)))
      return FAILED_SEND;
    PrintSynFinAttempt("SYN", 0, 3, attempt);
#if _DEBUG
    printf(" to %s\n", Ip());
#endif
    return STATUS_OK;
  }
  return TIMEOUT;
}

bool SenderSocket::SendPacket(char* pkt, size_t pktLength)
{
  EmptySlots.Wait();
  std::unique_lock<std::mutex> lock(Mutex); // will guarantee unlock upon destruction
  time = Time();
  if (sendto(Socket, pkt, pktLength, 0, (struct sockaddr*)(&Remote), sizeof(Remote)) == SOCKET_ERROR)
  {
    printf("failed sendto with %d\n", WSAGetLastError());
    return false;
  }
  PacketBuffer.push_back(pkt);
  lock.unlock();
  FullSlots.Signal();
  return true;
}

int SenderSocket::ReceivePacket(char* packet, size_t packetLength, bool printTimestamp = false)
{
  float t;
  struct sockaddr_in senderAddr;
  int senderAddrSize = sizeof(senderAddr);

  fd_set readers;
  FD_ZERO(&readers);
  FD_SET(Socket, &readers);
  struct timeval timeout;
  timeout.tv_sec = Rto;
  timeout.tv_usec = (Rto - timeout.tv_sec) * 1000000;
  if (select(Socket, &readers, nullptr, nullptr, &timeout) > 0u) {
#if _DEBUG
    printf("[%6.3f] <-- ", Time());
#endif
    if ((recvfrom(Socket, packet, packetLength, 0, (struct sockaddr*)(&senderAddr), &senderAddrSize)) == SOCKET_ERROR) {
      printf("failed recvfrom with %d\n", WSAGetLastError());
      return FAILED_RECV;
    }
    if (AckIsValid(((ReceiverHeader*)packet)->ackSeq))
      return STATUS_OK;
  }
  return SELECT_TIMEOUT;
}

void SenderSocket::PrintSynFinAttempt(const char* packetType, DWORD sequence, size_t maximumAttempts, size_t attempt)
{
#if _DEBUG
  printf("%s %d (attempt %zu of %zu, Rto %.3f)", packetType, sequence, attempt, maximumAttempts, Rto);
#endif
}

void SenderSocket::PrintAckReception(const char* packetType, ReceiverHeader rh)
{
#if _DEBUG
  printf("%s %d window %x", packetType, rh.ackSeq, rh.recvWnd);
#endif
}

bool SenderSocket::RemoteInfoFromHost(const char* host, DWORD port)
{
  // structure used in DNS lookups
  struct hostent* hostname;

  // structure for connecting to server
  DWORD IP = inet_addr(host);
  // first assume that the string is an IP address
  if (IP == INADDR_NONE) {
    // if not a valid IP, then do a DNS lookup
    if ((hostname = gethostbyname(host)) == NULL) {
      return false;
    }
    // take the first IP address and copy into sin_addr
    memcpy((char *)&(Remote.sin_addr), hostname->h_addr, hostname->h_length);
  } else {
    // if a valid IP, directly drop its binary version into sin_addr
    Remote.sin_addr.S_un.S_addr = IP;
  }

  // setup the port # and protocol type
  Remote.sin_family = AF_INET;
  Remote.sin_port = htons(port); // host-to-network flips the byte order
  return true;
}

int SenderSocket::Send(const char* buffer, DWORD bytes) {
  std::unique_lock<std::mutex> lock(Mutex);
  cv.wait(lock, [&] { return Connected; });
  return -1;
}

int SenderSocket::Close()
{
  if (!Connected)
    return NOT_CONNECTED;
  SenderSynHeader synHeader;
  synHeader.sdh.flags.FIN = 1;
  synHeader.sdh.seq = 0;
  sndBase -= 1;
  for (size_t attempt = 1; attempt <= 5; ++attempt) {
#if _DEBUG
    printf("[%6.3f] --> ", Time());
#endif
    if (!SendPacket((char*)(&synHeader), sizeof(SenderSynHeader)))
      return FAILED_SEND;
    PrintSynFinAttempt("FIN", 0, 5, attempt);
#if _DEBUG
    printf("\n");
#endif
    return STATUS_OK;
  }
  return TIMEOUT;
}

void SenderSocket::AckPackets()
{
  ReceiverHeader rh;
  auto lastReleased = 0;
  int receiveResult;
  while (!KillAckThread) {
    do {
      receiveResult = ReceivePacket((char*)(&rh), sizeof(rh), true);
    } while (receiveResult != STATUS_OK);
    if (static_cast<int>(rh.ackSeq) > sndBase) {
      FullSlots.Wait();
      Mutex.lock();
      sndBase = rh.ackSeq;
      auto effectiveWindow = min(sndWindow, rh.recvWnd);
      auto newReleased = sndBase + effectiveWindow + lastReleased;
      PacketBuffer.pop_front();
      if (rh.flags.SYN && rh.flags.ACK) {
        PrintAckReception("SYN-ACK", rh);
        auto rtt = Time() - time;
        Rto = max(1, 2 * rtt);
#if _DEBUG
        printf("; setting initial RTO to %.3f\n", Rto);
#endif
        Connected = true;
        cv.notify_one();
      } else if (rh.flags.FIN && rh.flags.ACK) {
        PrintAckReception("FIN-ACK", rh);
#if _DEBUG
        printf("\n");
#endif
        Connected = false;
        KillAckThread = true;
      }
      Mutex.unlock();
      EmptySlots.Signal();
      lastReleased += newReleased;
    }
  }
}
