#include "Semaphore.h"



Semaphore::Semaphore(int resources) : resources(resources) {}

void Semaphore::Wait()
{
  std::unique_lock<std::mutex> cv_lock(cv_mtx);
  cv.wait(cv_lock, [&] { return resources > 0; });
  --resources;
}

void Semaphore::Signal()
{
  std::unique_lock<std::mutex> cv_lock(cv_mtx);
  ++resources;
  cv.notify_all();
}

