extern "C" {
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
}  // extern "C"

#include <liburing.h>

#include <cerrno>
#include <cstring>
#include <iostream>

#define POSIX_REQUIRE(NAME, EXPR)                                \
  if (-1 == (EXPR)) {                                            \
    const int err = errno;                                       \
    std::cout << __LINE__ << ": " << NAME                        \
              << " failed with: " << std::strerror(err) << '\n'; \
    return err;                                                  \
  }

#define URING_REQUIRE(NAME, EXPR)                                 \
  if (const int err = (EXPR); err < 0) {                          \
    std::cout << __LINE__ << ": " << NAME                         \
              << " failed with: " << std::strerror(-err) << '\n'; \
    return -err;                                                  \
  }

#define PTR_REQUIRE(NAME, EXPR)                           \
  if (!(EXPR)) {                                          \
    std::cout << __LINE__ << ": " << NAME << " failed\n"; \
    return -1;                                            \
  }

int main() {
  int listenfd = -1, connfd = -1;
  int pipefds[2] = {-1, -1};
  sockaddr_in serv_addr = {};

  io_uring ring;

  const int rc = [&]() {
    URING_REQUIRE("io_uring_queue_init()", io_uring_queue_init(8, &ring, 0));
    POSIX_REQUIRE("pipe()", pipe(pipefds));
    POSIX_REQUIRE("socket()", listenfd = socket(AF_INET, SOCK_STREAM, 0));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(1337);

    POSIX_REQUIRE("bind()", bind(listenfd, (struct sockaddr *)&serv_addr,
                                 sizeof(serv_addr)));
    POSIX_REQUIRE("listen()", listen(listenfd, 10));

    POSIX_REQUIRE("accept()", connfd = accept(listenfd, NULL, NULL));

    for (;;) {
      constexpr int splice_size = 1024;
      io_uring_sqe *sqe;
      io_uring_cqe *cqe;

      PTR_REQUIRE("io_uring_get_sqe()", sqe = io_uring_get_sqe(&ring));
      io_uring_prep_splice(sqe, connfd, -1, pipefds[1], -1, splice_size, 0);
      // sqe->flags |= IOSQE_IO_LINK;
      sqe->flags |= IOSQE_IO_HARDLINK;

      PTR_REQUIRE("io_uring_get_sqe()", sqe = io_uring_get_sqe(&ring));
      io_uring_prep_splice(sqe, pipefds[0], -1, connfd, -1, splice_size, 0);

      io_uring_submit(&ring);

      for (int i = 0; i < 2; i++) {
        URING_REQUIRE("io_uring_wait_cqe()", io_uring_wait_cqe(&ring, &cqe));
        URING_REQUIRE("cqe->res", cqe->res);
        const int result = cqe->res;
        std::cout << "Result of the operation i=" << i << ": " << result
                  << '\n';
        io_uring_cqe_seen(&ring, cqe);
        if (i == 0 && result == 0) {
          std::cout << "Nothing more to read.\n";
          return 0;
        }
      }
    }

    return 0;
  }();

  io_uring_queue_exit(&ring);
  for (const int fd : {connfd, listenfd, pipefds[0], pipefds[1]}) {
    if (fd >= 0) {
      close(fd);
    }
  }

  return rc;
}
