// File: SenderSocket.cpp
// Martin Fracker
// CSCE 463-500 Spring 2017
#include "SenderSocket.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include "../ReliableUDP/printing.h"

#define BETA 0.25
#define ALPHA 0.125

SenderSocket::SenderSocket() 
  : ConstructionTime(timeGetTime()), sndBase(-1), nextSeq(0), sndWindow(1), FullSlots(0), EmptySlots(1)
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
  return static_cast<int>(ack) > sndBase && ack <= nextSeq;
}

void SenderSocket::StartTimer()
{
  time = Time();
}

void SenderSocket::StopTimer()
{
  estRTT = Time() - time;
}

void SenderSocket::RecordRto(float rtt)
{
  estRTT = (1 - ALPHA) * oldEstRTT + ALPHA * rtt;
  devRTT = (1 - BETA) * oldDevRTT + BETA * abs(rtt - estRTT);
  Rto = estRTT + 4 * max(devRTT, 0.010);
  oldEstRTT = estRTT;
  oldDevRTT = devRTT;
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
  synHeader.lp.bufferSize = senderWindow + MAX_RETX;
  synHeader.sdh.flags.SYN = 1;
  synHeader.sdh.seq = 0;
  if (!SendPacket((char*)(&synHeader), sizeof(SenderSynHeader)))
    return FAILED_SEND;
  WaitUntilConnectedOrAborted();
  return status;
}

bool SenderSocket::SendPacket(const char* pkt, size_t pktLength, bool bypassSemaphore)
{
  if (!bypassSemaphore)
    EmptySlots.Wait();
  std::unique_lock<std::mutex> lock(Mutex); // will guarantee unlock upon destruction
  StartTimer();
  if (status != STATUS_OK)
    return false;
  PrintDebug("[%6.3f] --> ", Time());
  SenderDataHeader* sdh = (SenderDataHeader*)pkt;
  sdh->seq = max(0, sndBase);
  if (sdh->flags.FIN)
  {
    sndBase -= 1;
#ifndef _DEBUG
  PrintDebug("[%6.3f] --> ", Time());
#endif
    PrintSendAttempt("FIN", sdh->seq, MAX_RETX, timeouts + 1);
  } else if (sdh->flags.SYN)
  {
    PrintSendAttempt("SYN", sdh->seq, MAX_RETX, timeouts + 1);
  } else
  {
    PrintSendAttempt("data", sdh->seq, MAX_RETX, timeouts + 1);
    if (sdh->seq == 0)
      transferTimeStart = Time();
  }
  if (sendto(Socket, pkt, pktLength, 0, (struct sockaddr*)(&Remote), sizeof(Remote)) == SOCKET_ERROR)
  {
    printf("failed sendto with %d\n", WSAGetLastError());
    status = FAILED_SEND;
    return false;
  }
  PacketBuffer.push_back({ std::string(pkt, pktLength), pktLength });
  lock.unlock();
  if (!bypassSemaphore)
    FullSlots.Signal();
  status = STATUS_OK;
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
    if ((recvfrom(Socket, packet, packetLength, 0, (struct sockaddr*)(&senderAddr), &senderAddrSize)) == SOCKET_ERROR) {
      printf("failed recvfrom with %d\n", WSAGetLastError());
      return FAILED_RECV;
    }
    if (AckIsValid(((ReceiverHeader*)packet)->ackSeq))
      return STATUS_OK;
    else
      return INVALID_ACK;
  }
  return TIMEOUT;
}

void SenderSocket::PrintSendAttempt(const char* packetType, DWORD sequence, size_t maximumAttempts, size_t attempt)
{
  PrintDebug("%s %d (attempt %zu of %zu, Rto %.3f)\n", packetType, sequence, attempt, maximumAttempts, Rto);
}

void SenderSocket::PrintAckReception(const char* packetType, ReceiverHeader rh)
{
  PrintDebug("%s %d window %X", packetType, rh.ackSeq, rh.recvWnd);
}

void SenderSocket::PrintAckReceptionNonDebug(const char* packetType, ReceiverHeader rh)
{
  printf("%s %d window %X", packetType, rh.ackSeq, rh.recvWnd);
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
  char pkt[MAX_PKT_SIZE];
  SenderDataHeader senderHeader;
  memcpy(pkt + sizeof(SenderDataHeader), buffer, bytes);
  memcpy(pkt, &senderHeader, sizeof(SenderDataHeader));
  SendPacket(pkt, bytes + sizeof(SenderDataHeader));
  return status;
}

void SenderSocket::WaitUntilConnectedOrAborted()
{
  std::unique_lock<std::mutex> lock(Mutex);
  cv.wait(lock, [&] { return Connected || status != STATUS_OK; });
}

void SenderSocket::WaitUntilDisconnectedOrAborted()
{
  std::unique_lock<std::mutex> lock(Mutex);
  cv.wait(lock, [&] { return !Connected || status != STATUS_OK; });
}

int SenderSocket::Close(float& transferTime)
{
  if (!Connected)
    return NOT_CONNECTED;
  SenderSynHeader synHeader;
  synHeader.sdh.flags.FIN = 1;
  synHeader.sdh.seq = 0;
  if (!SendPacket((char*)(&synHeader), sizeof(SenderSynHeader)))
    return FAILED_SEND;
  WaitUntilDisconnectedOrAborted();
  transferTime = transferTimeEnd - transferTimeStart;
  return STATUS_OK;
}

void SenderSocket::AckPackets()
{
  ReceiverHeader rh;
  auto lastReleased = 0;
  int receiveResult;
  while (!KillAckThread) {
    FullSlots.Wait();
    do {
      receiveResult = ReceivePacket((char*)(&rh), sizeof(rh), true);
      if (receiveResult == INVALID_ACK)
        continue;
      std::unique_lock<std::mutex> lock(Mutex);
      if (receiveResult == TIMEOUT)
      {
        {
          PacketBufferElement bufferElem;
          if (PacketBuffer.size() > 0)
            bufferElem = PacketBuffer.front();
          else {
            lock.unlock();
            lock.release();
            continue;
          }
          ++timeouts;
          PacketBuffer.pop_front();
          lock.unlock();
          lock.release();
          SendPacket(bufferElem.Packet.c_str(), bufferElem.PacketLength, true);
        }
      } else if (receiveResult != STATUS_OK) {
        status = receiveResult;
        cv.notify_one();
        EmptySlots.Signal();
        return;
      }
      if (timeouts >= MAX_RETX - 1)
      {
        status = receiveResult;
        cv.notify_one();
        EmptySlots.Signal();
        return;
      }
      FullSlots.Signal();
      if (receiveResult != TIMEOUT) {
        lock.unlock();
        lock.release();
      }
    } while (receiveResult != STATUS_OK);
    timeouts = 0;
    if (static_cast<int>(rh.ackSeq) > sndBase) {
      FullSlots.Wait();
      std::unique_lock<std::mutex> lock(Mutex);
      ++nextSeq;
      sndBase = rh.ackSeq;
      auto effectiveWindow = min(sndWindow, rh.recvWnd);
      auto newReleased = sndBase + effectiveWindow + lastReleased;
      PrintDebug("[%6.3f] <-- ", Time());
      if (rh.flags.SYN) {
        PrintAckReception("SYN-ACK", rh);
        StopTimer();
        Rto = min(1, 2 * estRTT);
        PrintDebug("; setting initial RTO to %.3f\n", Rto);
        Connected = true;
        cv.notify_one();
      } else
      { 
        if (rh.flags.FIN) {
          PrintAckReceptionNonDebug("FIN-ACK", rh);
          printf("\n");
          Connected = false;
          cv.notify_one();
          KillAckThread = true;
        } else {
          PrintAckReception("ACK", rh);
          transferTimeEnd = Time();
          PrintDebug("\n");
        }
        StopTimer();
        RecordRto(estRTT);
      }
      PacketBuffer.pop_front();
      lock.unlock();
      EmptySlots.Signal();
      lastReleased += newReleased;
    }
  }
}
