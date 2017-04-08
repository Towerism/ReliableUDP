#pragma once

#include <atomic>
#include <mutex>

class Semaphore {
public:
  Semaphore(int resources);

  void Wait();

  void Signal();

private:
  std::atomic<int> resources;
  std::condition_variable cv;
  std::mutex cv_mtx;
};

