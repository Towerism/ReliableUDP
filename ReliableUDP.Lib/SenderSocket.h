﻿// File: SenderSocket.h
// Martin Fracker
// CSCE 463-500 Spring 2017
#pragma once

#include <cstring>
#include <windows.h>
#include <mutex>
#include "Semaphore.h"
#include <deque>

#define MAGIC_PORT 22345 // receiver listens on this port
#define MAX_PKT_SIZE (1500-28) // maximum UDP packet size accepted by receiver 
  
// possible status codes from ss.Open, ss.Send, ss.Close
#define STATUS_OK 0 // no error
#define ALREADY_CONNECTED 1 // second call to ss.Open() without closing connection
#define NOT_CONNECTED 2 // call to ss.Send()/Close() without ss.Open()
#define INVALID_NAME 3 // ss.Open() with targetHost that has no DNS entry
#define FAILED_SEND 4 // sendto() failed in kernel
#define TIMEOUT 5 // timeout after all retx attempts are exhausted
#define FAILED_RECV 6 // recvfrom() failed in kernel

#define SELECT_TIMEOUT 99 // non-fatal timeout error 

#define MAGIC_PROTOCOL 0x8311AA

#define BITS_IN_MEGABIT 1e6

#define FORWARD_PATH 0
#define RETURN_PATH 1

#define MAX_RETX 5

#pragma pack(push, 1)
struct Flags {
  DWORD reserved : 5; // must be zero
  DWORD SYN : 1;
  DWORD ACK : 1;
  DWORD FIN : 1;
  DWORD magic : 24;
  Flags() { memset(this, 0, sizeof(*this)); magic = MAGIC_PROTOCOL; }
};
struct SenderDataHeader {
  Flags flags;
  DWORD seq; // must begin from 0
};
struct LinkProperties {
  // transfer parameters
  float RTT; // propagation RTT (in sec)
  float speed; // bottleneck bandwidth (in bits/sec)
  float pLoss[2]; // probability of loss in each direction
  DWORD bufferSize; // buffer size of emulated routers (in packets)
  LinkProperties() { memset(this, 0, sizeof(*this)); }
};
struct SenderSynHeader {
  SenderDataHeader sdh;
  LinkProperties lp;
};
struct ReceiverHeader {
  Flags flags;
  DWORD recvWnd; // receiver window for flow control (in pkts)
  DWORD ackSeq; // ack value = next expected sequence
};
#pragma pack(pop)

class SenderSocket
{
public:
  SenderSocket();
  ~SenderSocket();
  int ReceivePacket(char* packet, size_t packetLength, bool printTimestamp);

  int Open(const char* host, DWORD port, DWORD senderWindow, LinkProperties* lp);
  int Send(const char* buffer, DWORD bytes);
  int Close();

private:
  bool Connected = false;
  DWORD ConstructionTime;
  SOCKET Socket;
  struct sockaddr_in Remote;
  float Rto = 1.;
  int sndBase;
  UINT32 nextSeq;
  UINT32 sndWindow;
  UINT32 rcvWindow;
  std::thread AckThread;
  std::condition_variable cv;
  Semaphore FullSlots;
  Semaphore EmptySlots;
  std::mutex Mutex;
  bool KillAckThread = false;
  std::deque<char*> PacketBuffer;
  float oldDevRTT = 0, devRTT = 0, oldEstRTT = 0, estRTT = 0, time;

  bool RemoteInfoFromHost(const char* host, DWORD port);
  bool SendPacket(char* pkt, size_t pktLength);
  void PrintSynFinAttempt(const char* packetType, DWORD sequence, size_t maximumAttempts, size_t attempt);
  void PrintAckReception(const char* packetType, ReceiverHeader rh);
  void AckPackets();
  bool AckIsValid(DWORD ack) const;
  void StartTimer();
  void StopTimer();
  float CalculateRTO(float rtt);

  const char* Ip() const { return inet_ntoa(Remote.sin_addr); }
  float Time() const { return static_cast<float>(timeGetTime() - ConstructionTime) / 1000; }
};
