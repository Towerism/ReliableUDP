// File: SenderSocket.h
// Martin Fracker
// CSCE 463-500 Spring 2017
#pragma once

#include <cstring>
#include <windows.h>
#include <mutex>
#include <vector>
#include "Semaphore.h"

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

#define FAST_RETX 97 // non-fatal timeout error 
#define INVALID_ACK 98 //non-fatal ack error
#define SELECT_TIMEOUT 99 // non-fatal timeout error 

#define MAGIC_PROTOCOL 0x8311AA

#define BITS_IN_MEGABIT 1e6
#define BITS_IN_KILOBIT 1000
#define BYTES_IN_MEGABYTE (1 << 20)
#define BITS_IN_BYTE 8

#define FORWARD_PATH 0
#define RETURN_PATH 1

#define MAX_RETX 50 

#pragma pack(push, 1)
struct Flags {
  DWORD Reserved : 5; // must be zero
  DWORD Syn : 1;
  DWORD Ack : 1;
  DWORD Fin : 1;
  DWORD Magic : 24;
  Flags() { memset(this, 0, sizeof(*this)); Magic = MAGIC_PROTOCOL; }
};
struct SenderDataHeader {
  Flags Flags;
  DWORD Sequence; // must begin from 0
};
struct LinkProperties {
  // transfer parameters
  float Rtt; // propagation Rtt (in sec)
  float Speed; // bottleneck bandwidth (in bits/sec)
  float LossProbability[2]; // probability of loss in each direction
  DWORD BufferSize; // buffer size of emulated routers (in packets)
  LinkProperties() { memset(this, 0, sizeof(*this)); }
};
struct SenderSynHeader {
  SenderDataHeader SenderDataHeader;
  LinkProperties LinkProperties;
};
struct ReceiverHeader {
  Flags Flags;
  DWORD ReceiverWindow; // receiver window for flow control (in pkts)
  DWORD AckSequence; // ack value = next expected sequence
};
#pragma pack(pop)

struct PacketBufferElement
{
  std::string Packet;
  size_t PacketLength;
  float TimeStamp;
};

class SenderSocket
{
public:
  SenderSocket();
  ~SenderSocket();
  int ReceivePacket(char* packet, size_t packetLength, bool printTimestamp);

  int Open(const char* host, DWORD port, DWORD senderWindow, LinkProperties* lp);
  int Send(const char* buffer, DWORD bytes);
  int Close(float* transferTime);

  float GetEstRTT() const { return EstimatedRtt; }

private:
  std::atomic<float> TransferTimeStart, TransferTimeEnd;
  int Status = STATUS_OK;
  std::atomic<bool> Connected = false;
  DWORD ConstructionTime;
  SOCKET Socket;
  struct sockaddr_in Remote;
  int dupack = 0;
  float Rto = 1.;
  std::atomic<int> SenderBase;
  std::atomic<size_t> BytesAcked = 0;
  std::atomic<UINT32> NextSequence;
  std::atomic<UINT32> CurrentSequence = 0;
  UINT32 SenderWindow;
  UINT32 ReceiverWindow;
  std::thread AckThread;
  std::thread StatsThread;
  std::condition_variable Condition;
  std::condition_variable FullSlotsCondition;
  std::mutex FullSlotsMutex;
  bool FinSent = false;
  std::atomic<size_t> FastRetransmissions = 0;
  Semaphore FullSlots;
  Semaphore EmptySlots;
  std::atomic<bool> WindowMovedForwardSinceLastSend = true;
  float CalculateTimeout();
  std::mutex Mutex;
  int Timeouts = 0;
  std::atomic<size_t> TotalTimeouts = 0;
  std::atomic<UINT32> EffectiveWindow;
  bool KillAckThread = false;
  std::vector<PacketBufferElement> PacketBuffer;
  std::atomic<float> OldRttDeviation = 0, RttDeviation = 0, OldEstimatedRtt = 0, EstimatedRtt = 0, TimeMark;
  float Timeout = 1;

  bool RemoteInfoFromHost(const char* host, DWORD port);
  bool SendPacket(const char* pkt, size_t pktLength, bool bypassSemaphore = false, int sequenceOverride = -1);
  void PrintSendAttempt(const char* packetType, DWORD sequence, size_t maximumAttempts, size_t attempt);
  void PrintAckReception(const char* packetType, ReceiverHeader rh);
  void PrintAckReceptionNonDebug(const char* packetType, ReceiverHeader rh);
  void AckPackets();
  void PrintStats() const;
  bool AckIsValid(DWORD ack, bool isFin) const;
  void StartTimer();
  void RecordRto(float rtt);
  void WaitUntilConnectedOrAborted();
  void WaitUntilDisconnectedOrAborted();
  float GetTimeStamp(int sequence);

  const char* Ip() const { return inet_ntoa(Remote.sin_addr); }
  float Time() const { return static_cast<float>(timeGetTime() - ConstructionTime) / 1000; }
};
