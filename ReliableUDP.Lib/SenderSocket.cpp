// File: SenderSocket.cpp
// Martin Fracker
// CSCE 463-500 Spring 2017
#include "SenderSocket.h"
#include <windows.h>
#include <cstdio>
#include <string>

SenderSocket::SenderSocket() : ConstructionTime(timeGetTime())
{
  WSADATA wsaData;
  WORD wVersionRequested = MAKEWORD(2, 2);
  if (WSAStartup(wVersionRequested, &wsaData) != 0) {
    printf("WSAStartup error %d\n", WSAGetLastError());
    std::exit(EXIT_FAILURE);
  }
  Socket = socket(AF_INET, SOCK_DGRAM, 0);
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
}

SenderSocket::~SenderSocket()
{
  WSACleanup();
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
    printf("[%6.3f] <-- ", Time());
    if ((recvfrom(Socket, packet, packetLength, 0, (struct sockaddr*)(&senderAddr), &senderAddrSize)) == SOCKET_ERROR) {
      printf("failed recvfrom with %d", WSAGetLastError());
      return FAILED_RECV;
    }
    return STATUS_OK;
  }
  return SELECT_TIMEOUT;
}

int SenderSocket::Open(const char* host, DWORD port, DWORD senderWindow, LinkProperties* lp)
{
  if (Connected)
    return ALREADY_CONNECTED;
  if (!RemoteInfoFromHost(host, port)) 
    return INVALID_NAME;
  SenderSynHeader synHeader;
  synHeader.lp = *lp;
  synHeader.lp.bufferSize = senderWindow + 50;
  synHeader.sdh.flags.SYN = 1;
  synHeader.sdh.seq = 0;
  for (size_t attempt = 1; attempt <= 3; ++attempt) {
    float t = Time();
    printf("[%6.3f] --> ", Time());
    if (!SendPacket((char*)(&synHeader), sizeof(SenderSynHeader)))
      return FAILED_SEND;
    PrintSynFinAttempt("SYN", 0, 3, attempt);
    printf(" to %s\n", Ip());
    ReceiverHeader rh;
    int receiveResult = ReceivePacket((char*)(&rh), sizeof(rh), true);
    if (receiveResult == SELECT_TIMEOUT) 
      continue;
    if (receiveResult == FAILED_RECV)
      return FAILED_RECV;
    PrintSynFinReception("SYN-ACK", rh);
    lp->RTT = Time() - t;
    Rto = 3 * lp->RTT;
    printf("; setting initial RTO to %.3f\n", Rto);
    Connected = true;
    return STATUS_OK;
  }
  return TIMEOUT;
}

bool SenderSocket::SendPacket(char* pkt, size_t pktLength)
{
  if (sendto(Socket, pkt, pktLength, 0, (struct sockaddr*)(&Remote), sizeof(Remote)) == SOCKET_ERROR)
  {
    printf("failed sendto with %d", WSAGetLastError());
    return false;
  }
  return true;
}

void SenderSocket::PrintSynFinAttempt(const char* packetType, DWORD sequence, size_t maximumAttempts, size_t attempt)
{
  printf("%s %d (attempt %zu of %zu, Rto %.3f)", packetType, sequence, attempt, maximumAttempts, Rto);
}

void SenderSocket::PrintSynFinReception(const char* packetType, ReceiverHeader rh)
{
  printf("%s %d window %d", packetType, rh.ackSeq, rh.recvWnd);
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

int SenderSocket::Send(const char* buffer, DWORD bytes)
{
  return -1;
}

int SenderSocket::Close()
{
  if (!Connected)
    return NOT_CONNECTED;
  SenderSynHeader synHeader;
  synHeader.sdh.flags.FIN = 1;
  synHeader.sdh.seq = 0;
  for (size_t attempt = 1; attempt <= 5; ++attempt) {
    printf("[%6.3f] --> ", Time());
    if (!SendPacket((char*)(&synHeader), sizeof(SenderSynHeader)))
      return FAILED_SEND;
    PrintSynFinAttempt("FIN", 0, 5, attempt);
    printf("\n");
    ReceiverHeader rh;
    int receiveResult = ReceivePacket((char*)(&rh), sizeof(rh), true);
    if (receiveResult == SELECT_TIMEOUT) 
      continue;
    if (receiveResult == SOCKET_ERROR)
      return FAILED_RECV;
    PrintSynFinReception("FIN-ACK", rh);
    printf("\n");
    Connected = false;
    return STATUS_OK;
  }
  return TIMEOUT;
}
