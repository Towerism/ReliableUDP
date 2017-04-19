#include "Semaphore.h"



Semaphore::Semaphore(int resources) : resources(resources) {}

void Semaphore::Wait()
{
  std::unique_lock<std::mutex> cv_lock(cv_mtx);
  cv.wait(cv_lock, [&] { return resources > 0; });
  --resources;
}

void Semaphore::WaitDeferred(int consumed)
{
  std::unique_lock<std::mutex> cv_lock(cv_mtx);
  if (resources < consumed - 1)
    return;
  resources -= consumed - 1;
}

void Semaphore::Signal(int resourcesReleased)
{
  std::unique_lock<std::mutex> cv_lock(cv_mtx);
  resources += resourcesReleased;
  cv.notify_all();
}

void Semaphore::UnWait()
{
  std::unique_lock<std::mutex> cv_lock(cv_mtx);
  ++resources;
}

