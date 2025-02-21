extern "C" {
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
}  // extern "C"

#include <liburing.h>

#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <ostream>
#include <sstream>
#include <utility>
#include <vector>

template <typename To, typename From>
To bit_cast(const From& from) {
  return *reinterpret_cast<const To*>(&from);
}

#define POSIX_REQUIRE(EXPR)                                      \
  if (-1 == (EXPR)) {                                            \
    const int err = errno;                                       \
    std::cerr << __LINE__ << ": " << #EXPR                       \
              << " failed with: " << std::strerror(err) << '\n'; \
    return err;                                                  \
  }

#define URING_REQUIRE(EXPR)                                       \
  if (const int err = (EXPR); err < 0) {                          \
    std::cerr << __LINE__ << ": " << #EXPR                        \
              << " failed with: " << std::strerror(-err) << '\n'; \
    return -err;                                                  \
  }

#define PTR_REQUIRE(EXPR)                                  \
  if (!(EXPR)) {                                           \
    std::cerr << __LINE__ << ": " << #EXPR << " failed\n"; \
    return -1;                                             \
  }

struct alignas(8) IOEntryContext {
  enum Operation { TEE, SPLICE, SEND, RECV };
  std::int64_t bytes_desired : 33;
  Operation op : 3;
  int from_fd : 14;
  int to_fd : 14;
};

static_assert(sizeof(IOEntryContext) == 8);

void io_uring_prep(io_uring_sqe *sqe, IOEntryContext io_ctx, int flags = 0,
                   char *buffer = nullptr) {
  switch (io_ctx.op) {
    case IOEntryContext::TEE:
      io_uring_prep_tee(sqe, io_ctx.from_fd, io_ctx.to_fd, io_ctx.bytes_desired,
                        0);
      break;
    case IOEntryContext::SPLICE:
      io_uring_prep_splice(sqe, io_ctx.from_fd, -1, io_ctx.to_fd, -1,
                           io_ctx.bytes_desired, flags);
      break;
    case IOEntryContext::SEND:
      io_uring_prep_send(sqe, io_ctx.to_fd, buffer, io_ctx.bytes_desired,
                         flags);
      break;
    case IOEntryContext::RECV:
      io_uring_prep_recv(sqe, io_ctx.from_fd, buffer, io_ctx.bytes_desired,
                         flags);
      break;
    default:
      __builtin_unreachable();
  }
  io_uring_sqe_set_data64(sqe, bit_cast<std::uint64_t>(io_ctx));
}

// Connect to 127.0.0.1:<port> and `recv()` continuously, discarding all data.
int client_sink(int port) {
  int sock;
  POSIX_REQUIRE(sock = socket(AF_INET, SOCK_STREAM, 0));

  sockaddr_in serv_addr = {};
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  serv_addr.sin_port = htons(port);
  POSIX_REQUIRE(connect(sock, (sockaddr *)&serv_addr, sizeof(serv_addr)));

  std::vector<char> buffer(4096 * 16);
  for (;;) {
    int rc;
    POSIX_REQUIRE(rc = recv(sock, buffer.data(), buffer.size(), MSG_TRUNC));
    if (rc == 0) {
      return 0;
    }
  }
}

// Connect to 127.0.0.1:<port> and concurrently `send()` zeros and `recv()`,
// discarding all received data.
int client_source_and_sink(int port) {
  io_uring ring;
  URING_REQUIRE(io_uring_queue_init(8, &ring, 0));

  int sock;
  POSIX_REQUIRE(sock = socket(AF_INET, SOCK_STREAM, 0));

  sockaddr_in serv_addr = {};
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  serv_addr.sin_port = htons(port);
  POSIX_REQUIRE(connect(sock, (sockaddr *)&serv_addr, sizeof(serv_addr)));

  io_uring_sqe *sqe;
  io_uring_cqe *cqe;
  IOEntryContext io_ctx = {};
  std::vector<char> buffer(4096 * 16);
  std::vector<char> payload(4096 * 16);

  const auto prep_send = [&]() {
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      std::cerr << "Panic on line " << __LINE__ << '\n' << std::flush;
      std::abort();
    }
    io_ctx.op = IOEntryContext::SEND;
    io_ctx.to_fd = sock;
    io_ctx.bytes_desired = payload.size();
    io_uring_prep(sqe, io_ctx, 0, payload.data());
  };

  const auto prep_recv = [&]() {
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      std::cerr << "Panic on line " << __LINE__ << '\n' << std::flush;
      std::abort();
    }
    io_ctx.op = IOEntryContext::RECV;
    io_ctx.from_fd = sock;
    io_ctx.bytes_desired = buffer.size();
    io_uring_prep(sqe, io_ctx, MSG_TRUNC, buffer.data());
  };

  prep_send();
  prep_recv();
  io_uring_submit(&ring);

  for (;;) {
    URING_REQUIRE(io_uring_wait_cqe(&ring, &cqe));
    URING_REQUIRE(cqe->res);
    const int result = cqe->res;
    io_ctx = bit_cast<IOEntryContext>(io_uring_cqe_get_data64(cqe));
    io_uring_cqe_seen(&ring, cqe);
    switch (io_ctx.op) {
      case IOEntryContext::RECV:
        if (result == 0) {
          // Server hung up.
          return 0;
        }
        prep_recv();
        io_uring_submit(&ring);
        break;
      case IOEntryContext::SEND:
        prep_send();
        io_uring_submit(&ring);
        break;
      default:
        std::abort();
    }
  }
}

// Consume from `conn1fd` and duplicate all data onto `connfd1` and `connfd2`.
// Use `splice()` and `tee()`, involving the pipes `pipe1fds` and `pipe2fds`,
// to prevent any copies of data into user space.
int server_splicetee(io_uring &ring, int conn1fd, int conn2fd,
                     int (&pipe1fds)[2], int (&pipe2fds)[2]) {
  const auto interval = std::chrono::seconds(5);
  const auto start = std::chrono::steady_clock::now();
  auto last_flush = start;
  std::uint64_t last_snapshot = 0;
  constexpr int splice_size = 4096 * 16;
  std::ofstream log("log");

  for (std::uint64_t bytes_sent = 0;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_flush >= interval) {
      std::ostringstream sstream;
      sstream << std::chrono::duration_cast<std::chrono::seconds>(now - start)
                     .count()
              << " s\t"
              << ((bytes_sent - last_snapshot) * (now - last_flush) /
                  std::chrono::seconds(1) / 1'000'000.0)
              << " MB/s\n";
      const auto message = sstream.str();
      std::cout << message << std::flush;
      log << message << std::flush;
      last_flush = now;
      last_snapshot = bytes_sent;
    }

    io_uring_sqe *sqe;
    io_uring_cqe *cqe;
    IOEntryContext io_ctx;

    PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
    io_ctx.op = IOEntryContext::SPLICE;
    io_ctx.bytes_desired = splice_size;
    io_ctx.from_fd = conn1fd;
    io_ctx.to_fd = pipe1fds[1];
    io_uring_prep(sqe, io_ctx);
    sqe->flags |= IOSQE_IO_HARDLINK;

    PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
    io_ctx.op = IOEntryContext::TEE;
    io_ctx.bytes_desired = splice_size;
    io_ctx.from_fd = pipe1fds[0];
    io_ctx.to_fd = pipe2fds[1];
    io_uring_prep(sqe, io_ctx);

    // std::cerr << "Submitting two chained operations to io_uring.\n";
    io_uring_submit(&ring);

    int bytes_to_send;

    for (int i = 0; i < 2; i++) {
      // std::cerr << "Waiting for a completion from io_uring.\n";
      URING_REQUIRE(io_uring_wait_cqe(&ring, &cqe));
      URING_REQUIRE(cqe->res);
      const int result = cqe->res;
      io_ctx = bit_cast<IOEntryContext>(io_uring_cqe_get_data64(cqe));
      io_uring_cqe_seen(&ring, cqe);
      /*if (result < io_ctx.bytes_desired) {
        std::cerr << "Result of the operation i=" << i << ": " << result
                  << '\n';
      }*/
      if (io_ctx.op == IOEntryContext::SPLICE && io_ctx.from_fd == conn1fd &&
          result == 0) {
        std::cerr << "Nothing more to read.\n";
        return 0;
      }
      if (i == 0) {
        bytes_to_send = result;
      } else {
        // TODO: Can we tee() less than we splice()? Maybe if conn2 is slow?
        bytes_to_send = std::min(bytes_to_send, result);
      }
    }

    PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
    io_ctx.op = IOEntryContext::SPLICE;
    io_ctx.bytes_desired = bytes_to_send;
    io_ctx.from_fd = pipe1fds[0];
    io_ctx.to_fd = conn1fd;
    io_uring_prep(sqe, io_ctx);

    PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
    io_ctx.op = IOEntryContext::SPLICE;
    io_ctx.bytes_desired = bytes_to_send;
    io_ctx.from_fd = pipe2fds[0];
    io_ctx.to_fd = conn2fd;
    io_uring_prep(sqe, io_ctx);

    // std::cerr << "Submitting two unchained operations to io_uring.\n";
    io_uring_submit(&ring);

    for (int j = 0; j < 2; j++) {
      // std::cerr << "Waiting for a completion from io_uring.\n";
      URING_REQUIRE(io_uring_wait_cqe(&ring, &cqe));
      // TODO: handle EINTR
      URING_REQUIRE(cqe->res);
      const int result = cqe->res;
      io_ctx = bit_cast<IOEntryContext>(io_uring_cqe_get_data64(cqe));
      io_uring_cqe_seen(&ring, cqe);
      bytes_sent += result;
      if (result < io_ctx.bytes_desired) {
        // TODO: This should only happen on account of a signal.
        // std::cerr << "Result of the operation j=" << j << ": " << result
        //          << ", so going to submit another splice.\n";
        io_ctx.bytes_desired -= result;
        PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
        io_uring_prep(sqe, io_ctx);
        io_uring_submit(&ring);
        --j;
      }
    }
  }

  return 0;
}

// Consume from `conn1fd` and duplicate all data onto `connfd1` and `connfd2`.
// Use `recv()` and `send()` with a buffer in user space.
int server_recvsend(io_uring &ring, int conn1fd, int conn2fd) {
  const auto interval = std::chrono::seconds(5);
  const auto start = std::chrono::steady_clock::now();
  auto last_flush = start;
  std::uint64_t last_snapshot = 0;
  std::ofstream log("log");

  std::vector<char> buffer(4096 * 16);

  for (std::uint64_t bytes_sent = 0;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_flush >= interval) {
      std::ostringstream sstream;
      sstream << std::chrono::duration_cast<std::chrono::seconds>(now - start)
                     .count()
              << " s\t"
              << ((bytes_sent - last_snapshot) * (now - last_flush) /
                  std::chrono::seconds(1) / 1'000'000.0)
              << " MB/s\n";
      const auto message = sstream.str();
      std::cout << message << std::flush;
      log << message << std::flush;
      last_flush = now;
      last_snapshot = bytes_sent;
    }

    int bytes_to_send = recv(conn1fd, buffer.data(), buffer.size(), 0);
    POSIX_REQUIRE(bytes_to_send);
    if (bytes_to_send == 0) {
      std::cerr << "Nothing more to read.\n";
      return 0;
    }

    io_uring_sqe *sqe;
    io_uring_cqe *cqe;
    IOEntryContext io_ctx;

    PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
    io_ctx.op = IOEntryContext::SEND;
    io_ctx.bytes_desired = bytes_to_send;
    io_ctx.to_fd = conn1fd;
    io_uring_prep(sqe, io_ctx, 0, buffer.data());

    PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
    io_ctx.op = IOEntryContext::SEND;
    io_ctx.bytes_desired = bytes_to_send;
    io_ctx.to_fd = conn2fd;
    io_uring_prep(sqe, io_ctx, 0, buffer.data());

    // std::cerr << "Submitting two unchained send() operations to io_uring.\n";
    io_uring_submit(&ring);

    for (int j = 0; j < 2; j++) {
      // std::cerr << "Waiting for a completion from io_uring. j=" << j << '\n';
      URING_REQUIRE(io_uring_wait_cqe(&ring, &cqe));
      // TODO: handle EINTR
      URING_REQUIRE(cqe->res);
      const int result = cqe->res;
      io_ctx = bit_cast<IOEntryContext>(io_uring_cqe_get_data64(cqe));
      io_uring_cqe_seen(&ring, cqe);
      bytes_sent += result;
      if (result < io_ctx.bytes_desired) {
        // TODO: This should only happen on account of a signal.
        // std::cerr << "Result of the operation j=" << j << ": " << result
        //           << ", so going to submit another send().\n";
        io_ctx.bytes_desired -= result;
        PTR_REQUIRE(sqe = io_uring_get_sqe(&ring));
        io_uring_prep(sqe, io_ctx, 0, buffer.data() + result);
        io_uring_submit(&ring);
        --j;
      }
    }
  }

  return 0;
}

void usage(std::ostream &out, const char *argv0) {
  out << "usage: " << argv0 << " <recvsend | splicetee>\n";
}

int main(int argc, char *argv[]) {
  enum { RECVSEND, SPLICETEE } server_mode;
  if (argc != 2) {
    usage(std::cerr, argv[0]);
    return 1;
  }
  const std::string_view arg{argv[1]};
  if (arg == "-h" || arg == "--help") {
    usage(std::cout, argv[0]);
    return 0;
  }
  if (arg == "recvsend") {
    server_mode = RECVSEND;
  } else if (arg == "splicetee") {
    server_mode = SPLICETEE;
  } else {
    usage(std::cerr, argv[0]);
    return 2;
  }

  int listen1fd = -1, conn1fd = -1;
  int pipe1fds[2] = {-1, -1};
  int listen2fd = -1, conn2fd = -1;
  int pipe2fds[2] = {-1, -1};
  sockaddr_in serv_addr = {};
  std::uint16_t echo_port; // to be determined by bind()
  std::uint16_t observer_port; // to be determined by bind()

  io_uring ring;

  const int rc = [&]() {
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
    serv_addr.sin_port = htons(0);

    POSIX_REQUIRE(bind(listen1fd, (sockaddr *)&serv_addr, sizeof(serv_addr)));
    POSIX_REQUIRE(listen(listen1fd, 1));

    socklen_t dummy;
    getsockname(listen1fd, (sockaddr*) &serv_addr, &dummy);
    echo_port = ntohs(serv_addr.sin_port);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serv_addr.sin_port = htons(observer_port);

    POSIX_REQUIRE(bind(listen2fd, (sockaddr *)&serv_addr, sizeof(serv_addr)));
    POSIX_REQUIRE(listen(listen2fd, 1));

    getsockname(listen2fd, (sockaddr*) &serv_addr, &dummy);
    observer_port = ntohs(serv_addr.sin_port);

    // fork() to client_sink(1338).
    switch (fork()) {
      case 0:
        // child
        // TODO: Should close all file descriptors except 0 and 1, but meh.
        std::exit(client_sink(observer_port));
      case -1: {
        const int err = errno;
        std::cerr << "error forking to client_sink(1338): "
                  << std::strerror(err) << '\n';
        return err;
      }
    }

    // fork() to client_source_and_sink(1337).
    switch (fork()) {
      case 0:
        // child
        // TODO: Should close all file descriptors except 0 and 1, but meh.
        std::exit(client_source_and_sink(echo_port));
      case -1: {
        const int err = errno;
        std::cerr << "error forking to client_sink(1338): "
                  << std::strerror(err) << '\n';
        return err;
      }
    }

    URING_REQUIRE(io_uring_queue_init(8, &ring, 0));

    std::cerr << "Waiting for observer client to connect on port "
              << observer_port << ".\n";
    POSIX_REQUIRE(conn2fd = accept(listen2fd, NULL, NULL));
    std::cerr << "Connection established.\n";

    std::cerr << "Waiting for echo client to connect on port " << echo_port
              << ".\n";
    POSIX_REQUIRE(conn1fd = accept(listen1fd, NULL, NULL));
    std::cerr << "Connection established.\n";

    switch (server_mode) {
      case RECVSEND:
        return server_recvsend(ring, conn1fd, conn2fd);
      case SPLICETEE:
        return server_splicetee(ring, conn1fd, conn2fd, pipe1fds, pipe2fds);
      default:
        __builtin_unreachable();
    }
  }();

  io_uring_queue_exit(&ring);
  for (const int fd : {conn1fd, listen1fd, pipe1fds[0], pipe1fds[1], conn2fd,
                       listen2fd, pipe2fds[0], pipe2fds[1]}) {
    if (fd >= 0) {
      close(fd);
    }
  }

  wait(0);

  return rc;
}
