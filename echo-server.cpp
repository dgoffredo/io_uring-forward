extern "C" {
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
}  // extern "C"

#include <liburing.h>

#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>

#define POSIX_REQUIRE(EXPR)                                      \
  if (-1 == (EXPR)) {                                            \
    const int err = errno;                                       \
    std::cout << __LINE__ << ": " << #EXPR                       \
              << " failed with: " << std::strerror(err) << '\n'; \
    return err;                                                  \
  }

#define URING_REQUIRE(EXPR)                                       \
  if (const int err = (EXPR); err < 0) {                          \
    std::cout << __LINE__ << ": " << #EXPR                        \
              << " failed with: " << std::strerror(-err) << '\n'; \
    return -err;                                                  \
  }

#define PTR_REQUIRE(EXPR)                                  \
  if (!(EXPR)) {                                           \
    std::cout << __LINE__ << ": " << #EXPR << " failed\n"; \
    return -1;                                             \
  }

struct alignas(8) IOEntryContext {
  enum Operation {
    TEE,
    SPLICE_FROM_SOCKET,
    SPLICE_TO_SOCKET,
    SENDMSG,
    RECVMSG
  };
  std::int64_t bytes_desired : 33;
  Operation op : 3;
  int from_fd : 14;
  int to_fd : 14;
};

static_assert(sizeof(IOEntryContext) == 8);

int main() {
  int listen1fd = -1, conn1fd = -1;
  int pipe1fds[2] = {-1, -1};
  int listen2fd = -1, conn2fd = -1;
  int pipe2fds[2] = {-1, -1};
  sockaddr_in serv_addr = {};
  const std::uint16_t echo_port = 1337;
  const std::uint16_t observer_port = 1338;

  io_uring ring;

  const int rc = [&]() {
    URING_REQUIRE(io_uring_queue_init(8, &ring, 0));
    POSIX_REQUIRE(pipe(pipe1fds));
    POSIX_REQUIRE(pipe(pipe2fds));
    POSIX_REQUIRE(listen1fd = socket(AF_INET, SOCK_STREAM, 0));
    POSIX_REQUIRE(listen2fd = socket(AF_INET, SOCK_STREAM, 0));

    const int enable = 1;
    POSIX_REQUIRE(setsockopt(listen1fd, SOL_SOCKET, SO_REUSEADDR, &enable,
                             sizeof enable));
    POSIX_REQUIRE(setsockopt(listen2fd, SOL_SOCKET, SO_REUSEADDR, &enable,
                             sizeof enable));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serv_addr.sin_port = htons(echo_port);

    POSIX_REQUIRE(bind(listen1fd, (sockaddr *)&serv_addr, sizeof(serv_addr)));
    POSIX_REQUIRE(listen(listen1fd, 1));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serv_addr.sin_port = htons(observer_port);

    POSIX_REQUIRE(bind(listen2fd, (sockaddr *)&serv_addr, sizeof(serv_addr)));
    POSIX_REQUIRE(listen(listen2fd, 1));

    std::cout << "Waiting for observer client to connect on port "
              << observer_port << ".\n";
    POSIX_REQUIRE(conn2fd = accept(listen2fd, NULL, NULL));
    std::cout << "Connection established.\n";

    std::cout << "Waiting for echo client to connect on port " << echo_port
              << ".\n";
    POSIX_REQUIRE(conn1fd = accept(listen1fd, NULL, NULL));
    std::cout << "Connection established.\n";

    const auto interval = std::chrono::seconds(5);
    const auto start = std::chrono::steady_clock::now();
    auto last_flush = start;
    std::uint64_t last_snapshot = 0;
    constexpr int splice_size = 4096;

    for (std::uint64_t bytes_sent = 0;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_flush >= interval) {
        std::cout << std::chrono::duration_cast<std::chrono::seconds>(now -
                                                                      start)
                         .count()
                  << " s\t"
                  << ((bytes_sent - last_snapshot) * (now - last_flush) /
                      std::chrono::seconds(1) / 1'000'000.0)
                  << " MB/s" << std::endl;
        last_flush = now;
        last_snapshot = bytes_sent;
      }

      io_uring_sqe *sqe;
      io_uring_cqe *cqe;
      IOEntryContext io_ctx;

      PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
      io_uring_prep_splice(sqe, conn1fd, -1, pipe1fds[1], -1, splice_size, 0);
      io_ctx.op = IOEntryContext::SPLICE_FROM_SOCKET;
      io_ctx.bytes_desired = splice_size;
      io_ctx.from_fd = conn1fd;
      io_ctx.to_fd = pipe1fds[1];
      io_uring_sqe_set_data64(sqe, std::bit_cast<std::uint64_t>(io_ctx));
      sqe->flags |= IOSQE_IO_HARDLINK;

      PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
      io_uring_prep_tee(sqe, pipe1fds[0], pipe2fds[1], splice_size, 0);
      io_ctx.op = IOEntryContext::TEE;
      io_ctx.bytes_desired = splice_size;
      io_ctx.from_fd = pipe1fds[0];
      io_ctx.to_fd = pipe2fds[1];
      io_uring_sqe_set_data64(sqe, std::bit_cast<std::uint64_t>(io_ctx));

      // std::cout << "Submitting two chained operations to io_uring.\n";
      io_uring_submit(&ring);

      int bytes_to_send;

      for (int i = 0; i < 2; i++) {
        // std::cout << "Waiting for a completion from io_uring.\n";
        URING_REQUIRE(io_uring_wait_cqe(&ring, &cqe));
        // URING_REQUIRE(cqe->res);
        const int result = cqe->res;
        io_ctx = std::bit_cast<IOEntryContext>(io_uring_cqe_get_data64(cqe));
        io_uring_cqe_seen(&ring, cqe);
        if (result < io_ctx.bytes_desired) {
          std::cout << "Result of the operation i=" << i << ": " << result
                    << '\n';
        }
        if (io_ctx.op == IOEntryContext::SPLICE_FROM_SOCKET &&
            io_ctx.from_fd == conn1fd && result == 0) {
          std::cout << "Nothing more to read.\n";
          return 0;
        }
        if (i == 0) {
          bytes_to_send = result;
        } else {
          bytes_to_send = std::min(bytes_to_send, result);
        }
      }

      PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
      io_uring_prep_splice(sqe, pipe1fds[0], -1, conn1fd, -1, bytes_to_send, 0);
      io_ctx.op = IOEntryContext::SPLICE_TO_SOCKET;
      io_ctx.bytes_desired = bytes_to_send;
      io_ctx.from_fd = pipe1fds[0];
      io_ctx.to_fd = conn1fd;
      io_uring_sqe_set_data64(sqe, std::bit_cast<std::uint64_t>(io_ctx));

      PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
      io_uring_prep_splice(sqe, pipe2fds[0], -1, conn2fd, -1, bytes_to_send, 0);
      io_ctx.op = IOEntryContext::SPLICE_TO_SOCKET;
      io_ctx.bytes_desired = bytes_to_send;
      io_ctx.from_fd = pipe2fds[0];
      io_ctx.to_fd = conn2fd;
      io_uring_sqe_set_data64(sqe, std::bit_cast<std::uint64_t>(io_ctx));

      // std::cout << "Submitting two unchained operations to io_uring.\n";
      io_uring_submit(&ring);

      for (int j = 0; j < 2; j++) {
        // std::cout << "Waiting for a completion from io_uring.\n";
        URING_REQUIRE(io_uring_wait_cqe(&ring, &cqe));
        URING_REQUIRE(cqe->res);
        const int result = cqe->res;
        io_ctx = std::bit_cast<IOEntryContext>(io_uring_cqe_get_data64(cqe));
        io_uring_cqe_seen(&ring, cqe);
        bytes_sent += result;
        if (result < io_ctx.bytes_desired) {
          std::cout << "Result of the operation j=" << j << ": " << result
                    << ", so going to submit another splice.\n";
          io_ctx.bytes_desired -= result;
          PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
          io_uring_prep_splice(sqe, io_ctx.from_fd, -1, io_ctx.to_fd, -1,
                               io_ctx.bytes_desired, 0);
          io_uring_sqe_set_data64(sqe, std::bit_cast<std::uint64_t>(io_ctx));
          io_uring_submit(&ring);
          --j;
        }
      }
    }

    return 0;
  }();

  io_uring_queue_exit(&ring);
  for (const int fd : {conn1fd, listen1fd, pipe1fds[0], pipe1fds[1], conn2fd,
                       listen2fd, pipe2fds[0], pipe2fds[1]}) {
    if (fd >= 0) {
      close(fd);
    }
  }

  return rc;
}
