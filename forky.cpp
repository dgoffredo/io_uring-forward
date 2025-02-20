extern "C" {
#include <sys/wait.h>
#include <unistd.h>
}  // extern "C"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

int main() {
  const int num_children = 10;

  for (int i = 1; i < num_children + 1; ++i) {
    switch (::fork()) {
      case 0:
        // std::this_thread::sleep_for(std::chrono::seconds(i));
        std::cout << "I am child " << i << '\n';
        return 0;
      case -1: {
        const int err = errno;
        std::cerr << "error forking: " << std::strerror(err) << '\n';
        return err;
      }
    }
  }

  for (int i = 0; i < num_children; ++i) {
    ::wait(0);
    std::cout << "I am the parent, handling a terminated child.\n";
  }
  std::cout << "I am the parent, exiting.\n";
}
