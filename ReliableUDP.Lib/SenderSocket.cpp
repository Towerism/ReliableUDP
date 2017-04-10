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
  : ConstructionTime(timeGetTime()), sndBase(-1), nextSeq(0), sndWindow(1), FullSlots(0), EmptySlots(1), EffectiveWindow(1)
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
  Rto = estRTT + 4 * max(devRTT.load(), 0.010);
  oldEstRTT.store(estRTT.load());
  oldDevRTT.store(devRTT.load());
}

int SenderSocket::Open(const char* host, DWORD port, DWORD senderWindow, LinkProperties* lp)
{
  if (Connected)
    return ALREADY_CONNECTED;
  if (!RemoteInfoFromHost(host, port))
    return INVALID_NAME;
  sndWindow = 1;
  SenderSynHeader synHeader;
  synHeader.lp = *lp;
  synHeader.lp.bufferSize = senderWindow + MAX_RETX;
  synHeader.sdh.flags.SYN = 1;
  synHeader.sdh.seq = 0;
  if (!SendPacket((char*)(&synHeader), sizeof(SenderSynHeader)))
    return FAILED_SEND;
  WaitUntilConnectedOrAborted();
  StatsThread = std::thread(&SenderSocket::PrintStats, this);
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
  sdh->seq = max(0, sndBase.load());
  if (sdh->flags.FIN)
  {
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
    ReceiverHeader* rh = (ReceiverHeader*)packet;
    if (AckIsValid(rh->ackSeq, rh->flags.FIN))
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

int SenderSocket::Close(float* transferTime)
{
  if (!Connected)
    return NOT_CONNECTED;
  SenderSynHeader synHeader;
  synHeader.sdh.flags.FIN = 1;
  synHeader.sdh.seq = 0;
  if (!SendPacket((char*)(&synHeader), sizeof(SenderSynHeader)))
    return FAILED_SEND;
  WaitUntilDisconnectedOrAborted();
  *transferTime = transferTimeEnd - transferTimeStart;
  return status;
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
          ++timeouts;
          ++TotalTimeouts;
          PacketBuffer.pop_front();
          lock.unlock();
          lock.release();
          SendPacket(bufferElem.Packet.c_str(), bufferElem.PacketLength, true);
        }
      } else if (receiveResult != STATUS_OK) {
        status = receiveResult;
        Connected = false;
        cv.notify_one();
        EmptySlots.Signal();
        return;
      }
      if (timeouts >= MAX_RETX - 1)
      {
        status = receiveResult;
        Connected = false;
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
    BytesAcked += PacketBuffer.front().PacketLength - sizeof(SenderDataHeader);
    timeouts = 0;
    if (AckIsValid(rh.ackSeq, rh.flags.FIN)) {
      FullSlots.Wait();
      std::unique_lock<std::mutex> lock(Mutex);
      ++nextSeq;
      sndBase = rh.ackSeq;
      EffectiveWindow = min(sndWindow, rh.recvWnd);
      auto newReleased = sndBase + EffectiveWindow + lastReleased;
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
#ifndef _DEBUG
  printf("[%6.3f] <-- ", Time());
#endif
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

void SenderSocket::PrintStats()
{
  const UINT64 interval = 2;
  UINT64 seconds = interval;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(interval));
    if (!Connected)
      break;
    auto megabytesAcked = static_cast<float>(BytesAcked.load()) / BYTES_IN_MEGABYTE;
    auto megabitsAcked = megabytesAcked * BITS_IN_BYTE;
    auto elapsedTime = Time() - transferTimeStart;
    auto rate = megabitsAcked / elapsedTime;
    printf("[%2llu] B %6d (%5.1f MB) N %6d T %zu F %d W %d S %.3f Mbps RTT %.3f\n", seconds, sndBase.load(), megabytesAcked, nextSeq.load(), TotalTimeouts.load(), 0, EffectiveWindow.load(), rate, estRTT.load());
    seconds += interval;
  }
}