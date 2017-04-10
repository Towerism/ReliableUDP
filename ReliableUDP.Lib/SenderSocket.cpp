// File: SenderSocket.cpp
// Martin Fracker
// CSCE 463-500 Spring 2017
#include "SenderSocket.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include "printing.h"

#define BETA 0.25
#define ALPHA 0.125

SenderSocket::SenderSocket() 
  : ConstructionTime(timeGetTime()), SenderBase(-1), NextSequence(0), SenderWindow(1), FullSlots(0), EmptySlots(1), EffectiveWindow(1)
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
  StatsThread.join();
  WSACleanup();
}

bool SenderSocket::AckIsValid(DWORD ack, bool isFin) const
{
  if (isFin)
  {
    ack += 1;
  }
  return static_cast<int>(ack) > SenderBase && ack <= NextSequence;
}

void SenderSocket::StartTimer()
{
  TimeMark = Time();
}

void SenderSocket::StopTimer()
{
  EstimatedRtt = Time() - TimeMark;
}

void SenderSocket::RecordRto(float rtt)
{
  EstimatedRtt = (1 - ALPHA) * OldEstimatedRtt + ALPHA * rtt;
  RttDeviation = (1 - BETA) * OldRttDeviation + BETA * abs(rtt - EstimatedRtt);
  Rto = EstimatedRtt + 4 * max(RttDeviation.load(), 0.010);
  OldEstimatedRtt.store(EstimatedRtt.load());
  OldRttDeviation.store(RttDeviation.load());
}

int SenderSocket::Open(const char* host, DWORD port, DWORD senderWindow, LinkProperties* lp)
{
  if (Connected)
    return ALREADY_CONNECTED;
  if (!RemoteInfoFromHost(host, port))
    return INVALID_NAME;
  SenderWindow = 1;
  SenderSynHeader synHeader;
  synHeader.LinkProperties = *lp;
  synHeader.LinkProperties.BufferSize = senderWindow + MAX_RETX;
  synHeader.SenderDataHeader.Flags.Syn = 1;
  synHeader.SenderDataHeader.Sequence = 0;
  if (!SendPacket((char*)(&synHeader), sizeof(SenderSynHeader)))
    return FAILED_SEND;
  WaitUntilConnectedOrAborted();
  StatsThread = std::thread(&SenderSocket::PrintStats, this);
  return Status;
}

bool SenderSocket::SendPacket(const char* pkt, size_t pktLength, bool bypassSemaphore)
{
  if (!bypassSemaphore)
    EmptySlots.Wait();
  std::unique_lock<std::mutex> lock(Mutex); // will guarantee unlock upon destruction
  StartTimer();
  if (Status != STATUS_OK)
    return false;
  PrintDebug("[%6.3f] --> ", Time());
  SenderDataHeader* sdh = (SenderDataHeader*)pkt;
  sdh->Sequence = max(0, SenderBase.load());
  if (sdh->Flags.Fin)
  {
    PrintSendAttempt("FIN", sdh->Sequence, MAX_RETX, Timeouts + 1);
  } else if (sdh->Flags.Syn)
  {
    PrintSendAttempt("SYN", sdh->Sequence, MAX_RETX, Timeouts + 1);
  } else
  {
    PrintSendAttempt("data", sdh->Sequence, MAX_RETX, Timeouts + 1);
    if (sdh->Sequence == 0)
      TransferTimeStart = Time();
  }
  if (sendto(Socket, pkt, pktLength, 0, (struct sockaddr*)(&Remote), sizeof(Remote)) == SOCKET_ERROR)
  {
    printf("failed sendto with %d\n", WSAGetLastError());
    Status = FAILED_SEND;
    return false;
  }
  PacketBuffer.push_back({ std::string(pkt, pktLength), pktLength });
  lock.unlock();
  if (!bypassSemaphore)
    FullSlots.Signal();
  Status = STATUS_OK;
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
    ReceiverHeader* rh = (ReceiverHeader*)packet;
    if (AckIsValid(rh->AckSequence, rh->Flags.Fin))
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
  PrintDebug("%s %d window %X", packetType, rh.AckSequence, rh.ReceiverWindow);
}

void SenderSocket::PrintAckReceptionNonDebug(const char* packetType, ReceiverHeader rh)
{
  printf("%s %d window %X", packetType, rh.AckSequence, rh.ReceiverWindow);
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
  return Status;
}

void SenderSocket::WaitUntilConnectedOrAborted()
{
  std::unique_lock<std::mutex> lock(Mutex);
  Condition.wait(lock, [&] { return Connected || Status != STATUS_OK; });
}

void SenderSocket::WaitUntilDisconnectedOrAborted()
{
  std::unique_lock<std::mutex> lock(Mutex);
  Condition.wait(lock, [&] { return !Connected || Status != STATUS_OK; });
}

int SenderSocket::Close(float* transferTime)
{
  if (!Connected)
    return NOT_CONNECTED;
  SenderSynHeader synHeader;
  synHeader.SenderDataHeader.Flags.Fin = 1;
  synHeader.SenderDataHeader.Sequence = 0;
  if (!SendPacket((char*)(&synHeader), sizeof(SenderSynHeader)))
    return FAILED_SEND;
  WaitUntilDisconnectedOrAborted();
  *transferTime = TransferTimeEnd - TransferTimeStart;
  return Status;
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
          else
            continue;
          ++Timeouts;
          ++TotalTimeouts;
          PacketBuffer.pop_front();
          lock.unlock();
          lock.release();
          SendPacket(bufferElem.Packet.c_str(), bufferElem.PacketLength, true);
        }
      } else if (receiveResult != STATUS_OK) {
        Status = receiveResult;
        Connected = false;
        Condition.notify_one();
        EmptySlots.Signal();
        return;
      }
      if (Timeouts >= MAX_RETX - 1)
      {
        Status = receiveResult;
        Connected = false;
        Condition.notify_one();
        EmptySlots.Signal();
        return;
      }
      FullSlots.Signal();
      if (receiveResult != TIMEOUT) {
        lock.unlock();
        lock.release();
      }
    } while (receiveResult != STATUS_OK);
    BytesAcked += PacketBuffer.front().PacketLength - sizeof(SenderDataHeader);
    Timeouts = 0;
    if (AckIsValid(rh.AckSequence, rh.Flags.Fin)) {
      FullSlots.Wait();
      std::unique_lock<std::mutex> lock(Mutex);
      ++NextSequence;
      SenderBase = rh.AckSequence;
      EffectiveWindow = min(SenderWindow, rh.ReceiverWindow);
      auto newReleased = SenderBase + EffectiveWindow + lastReleased;
      PrintDebug("[%6.3f] <-- ", Time());
      if (rh.Flags.Syn) {
        PrintAckReception("SYN-ACK", rh);
        StopTimer();
        Rto = min(1, 2 * EstimatedRtt);
        PrintDebug("; setting initial RTO to %.3f\n", Rto);
        Connected = true;
        Condition.notify_one();
      } else
      { 
        if (rh.Flags.Fin) {
#ifndef _DEBUG
  printf("[%6.3f] <-- ", Time());
#endif
          PrintAckReceptionNonDebug("FIN-ACK", rh);
          printf("\n");
          Connected = false;
          Condition.notify_one();
          KillAckThread = true;
        } else {
          PrintAckReception("ACK", rh);
          TransferTimeEnd = Time();
          PrintDebug("\n");
        }
        StopTimer();
        RecordRto(EstimatedRtt);
      }
      PacketBuffer.pop_front();
      lock.unlock();
      EmptySlots.Signal();
      lastReleased += newReleased;
    }
  }
}

void SenderSocket::PrintStats() const
{
  const UINT64 interval = 2;
  UINT64 seconds = interval;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(interval));
    if (!Connected)
      break;
    auto megabytesAcked = static_cast<float>(BytesAcked.load()) / BYTES_IN_MEGABYTE;
    auto megabitsAcked = megabytesAcked * BITS_IN_BYTE;
    auto elapsedTime = Time() - TransferTimeStart;
    auto rate = megabitsAcked / elapsedTime;
    printf("[%2llu] B %6d (%5.1f MB) N %6d T %zu F %d W %d S %.3f Mbps RTT %.3f\n", seconds, SenderBase.load(), megabytesAcked, NextSequence.load(), TotalTimeouts.load(), 0, EffectiveWindow.load(), rate, EstimatedRtt.load());
    seconds += interval;
  }
}
