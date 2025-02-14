extern "C" {
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
} // extern "C"

#include <cerrno>
#include <cstring>
#include <iostream>

#define REQUIRE(NAME, EXPR)                                                    \
  if (-1 == (EXPR)) {                                                          \
    const int err = errno;                                                     \
    std::cout << NAME << " failed with: " << std::strerror(err) << '\n';       \
    return err;                                                                \
  }

int main() {
  int listenfd, connfd;
  sockaddr_in serv_addr = {};
  char buf[1025] = {};
  ssize_t count;

  REQUIRE("socket()", listenfd = socket(AF_INET, SOCK_STREAM, 0));

  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(1337);

  REQUIRE("bind()",
          bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)));
  REQUIRE("listen()", listen(listenfd, 10));

  REQUIRE("accept()", connfd = accept(listenfd, NULL, NULL));

  for (;;) {
    // TODO: use io_uring instead

    REQUIRE("recv()", count = recv(connfd, buf, sizeof buf, 0));
    if (count == 0) {
      std::cout << "No more data to read from connection.\n";
      return 0;
    }

    ssize_t sent = 0;
    do {
      REQUIRE("send()", sent += send(connfd, buf + sent, count - sent, 0));
    } while (count != sent);
  }
}
