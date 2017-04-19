#pragma once

#include <atomic>
#include <mutex>

class Semaphore {
public:
  Semaphore(int resources);

  void Wait();

  // sometimes you don't initially know how many resources you will consume.
  // call this function once you know.
  // Typically you call Wait. Then you realize that you consumed more than you thought you would.
  // Then you would call WaitDeferred with how many you actually consumed.
  // The decrease in resources is calculated `consumed - 1` because it is assumed Wait was called once already.
  void WaitDeferred(int consumed);

  void Signal(int resourcesReleased = 1);

  // Put a resource back
  void UnWait();

  int GetResources() const { return resources; }

private:
  std::atomic<int> resources;
  std::condition_variable cv;
  std::mutex cv_mtx;
};

