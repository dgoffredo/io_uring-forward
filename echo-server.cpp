extern "C" {
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
}  // extern "C"

#include <liburing.h>
#include <stdlib.h>  // mkdtemp

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#define POSIX_REQUIRE(EXPR)                                      \
  if (-1 == (EXPR)) {                                            \
    const int err = errno;                                       \
    std::cerr << __LINE__ << ": " << #EXPR                       \
              << " failed with: " << std::strerror(err) << '\n'; \
    return -err;                                                 \
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

class Net {
 public:
  virtual ~Net() = default;

  // Return a listening socket bound to a local address, or return `-errno` if
  // an error occurs.
  virtual int server_socket(int backlog) = 0;

  // Return a socket connected to the address to which `server_fd` is bound, or
  // return `-errno` if an error occurs.
  virtual int client_socket(int server_fd) = 0;
};

template <typename Address, int family>
int client_socket(int server_fd) {
  Address addr = {};
  socklen_t len = sizeof addr;
  POSIX_REQUIRE(getsockname(server_fd, (sockaddr *)&addr, &len));

  int sock;
  POSIX_REQUIRE(sock = socket(family, SOCK_STREAM, 0));
  POSIX_REQUIRE(connect(sock, (sockaddr *)&addr, sizeof(addr)));

  return sock;
}

class TCP : public Net {
 public:
  int server_socket(int backlog) override {
    int sock;
    sockaddr_in serv_addr = {};

    POSIX_REQUIRE(sock = socket(AF_INET, SOCK_STREAM, 0));

    const int enable = 1;
    POSIX_REQUIRE(
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serv_addr.sin_port = htons(0);

    POSIX_REQUIRE(bind(sock, (sockaddr *)&serv_addr, sizeof(serv_addr)));
    POSIX_REQUIRE(listen(sock, backlog));

    return sock;
  }

  int client_socket(int server_fd) override {
    return ::client_socket<sockaddr_in, AF_INET>(server_fd);
  }
};

namespace fs = std::filesystem;

fs::path mktemp_dir() {
  std::string raw = fs::temp_directory_path() / "echo-server-XXXXXX";
  if (!mkdtemp(raw.data())) {
    const int err = errno;
    std::cerr << "Unable to create directory: " << std::strerror(err) << '\n';
    std::abort();
  }

  return raw;
}

class Unix : public Net {
  fs::path dir;
  int counter;

 public:
  Unix() : dir(mktemp_dir()), counter(0) {}

  ~Unix() { fs::remove_all(dir); }

  int server_socket(int backlog) override {
    const std::string sockname = dir / ("sock" + std::to_string(++counter));

    int sock;
    POSIX_REQUIRE(sock = socket(AF_UNIX, SOCK_STREAM, 0));

    sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    const auto size = std::min(sockname.size(), sizeof addr.sun_path);
    std::copy_n(sockname.data(), size, addr.sun_path);

    POSIX_REQUIRE(bind(sock, (sockaddr *)&addr, sizeof(addr)));
    POSIX_REQUIRE(listen(sock, backlog));

    return sock;
  }

  int client_socket(int server_fd) override {
    return ::client_socket<sockaddr_un, AF_UNIX>(server_fd);
  }
};

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
      std::unreachable();
  }
  io_uring_sqe_set_data64(sqe, std::bit_cast<std::uint64_t>(io_ctx));
}

// Connect and `recv()` continuously, discarding all data.
int client_sink(int bufsize, Net &net, int server_sock) {
  int sock;
  URING_REQUIRE(sock = net.client_socket(server_sock));

  std::vector<char> buffer(bufsize);
  for (;;) {
    int rc;
    POSIX_REQUIRE(rc = recv(sock, buffer.data(), buffer.size(), MSG_TRUNC));
    if (rc == 0) {
      return 0;
    }
  }
}

// Connect and concurrently `send()` zeros and `recv()`, discarding all
// received data.
int client_source_and_sink(int bufsize, Net &net, int server_sock) {
  io_uring ring;
  URING_REQUIRE(io_uring_queue_init(8, &ring, 0));

  int sock;
  URING_REQUIRE(sock = net.client_socket(server_sock));

  io_uring_sqe *sqe;
  io_uring_cqe *cqe;
  IOEntryContext io_ctx = {};
  std::vector<char> buffer(bufsize);
  std::vector<char> payload(bufsize);

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
    io_ctx = std::bit_cast<IOEntryContext>(io_uring_cqe_get_data64(cqe));
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
int server_splicetee(int bufsize, io_uring &ring, int conn1fd, int conn2fd,
                     int (&pipe1fds)[2], int (&pipe2fds)[2]) {
  const auto interval = std::chrono::seconds(5);
  const auto start = std::chrono::steady_clock::now();
  struct {
    std::chrono::steady_clock::time_point when;
    std::uint64_t bytes_sent = 0;
    std::uint64_t short_reads = 0;
    std::uint64_t short_writes_echo = 0;
    std::uint64_t short_writes_observer = 0;
    std::uint64_t short_writes_pipe = 0;
  } snapshot;
  snapshot.when = start;

  const int splice_size = bufsize;
  std::ofstream log("log");

  std::uint64_t bytes_sent = 0;
  std::uint64_t short_reads = 0;
  std::uint64_t short_writes_echo = 0;
  std::uint64_t short_writes_observer = 0;
  std::uint64_t short_writes_pipe = 0;

  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now - snapshot.when >= interval) {
      std::ostringstream sstream;
      sstream << std::chrono::duration_cast<std::chrono::seconds>(now - start)
                     .count()
              << " s\t"
              << ((bytes_sent - snapshot.bytes_sent) * std::chrono::seconds(1) /
                  (now - snapshot.when) / 1'000'000)
              << " MB/s\t"
              << ((short_reads - snapshot.short_reads) *
                  std::chrono::seconds(1) / (now - snapshot.when))
              << " short_reads/s\t"
              << ((short_writes_echo - snapshot.short_writes_echo) *
                  std::chrono::seconds(1) / (now - snapshot.when))
              << " short_writes_echo/s\t"
              << ((short_writes_observer - snapshot.short_writes_observer) *
                  std::chrono::seconds(1) / (now - snapshot.when))
              << " short_writes_observer/s\t"
              << ((short_writes_pipe - snapshot.short_writes_pipe) *
                  std::chrono::seconds(1) / (now - snapshot.when))
              << " short_writes_pipe/s\n";
      const auto message = sstream.str();
      std::cout << message << std::flush;
      log << message << std::flush;
      snapshot.when = now;
      snapshot.bytes_sent = bytes_sent;
      snapshot.short_reads = short_reads;
      snapshot.short_writes_echo = short_writes_echo;
      snapshot.short_writes_observer = short_writes_observer;
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
      io_ctx = std::bit_cast<IOEntryContext>(io_uring_cqe_get_data64(cqe));
      io_uring_cqe_seen(&ring, cqe);
      if (result < io_ctx.bytes_desired) {
        switch (io_ctx.op) {
          case IOEntryContext::SPLICE:
            ++short_reads;
            break;
          case IOEntryContext::TEE:
            ++short_writes_pipe;
            break;
          default:
            break;
        }
      }
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
      io_ctx = std::bit_cast<IOEntryContext>(io_uring_cqe_get_data64(cqe));
      io_uring_cqe_seen(&ring, cqe);
      bytes_sent += result;
      if (result < io_ctx.bytes_desired) {
        // TODO: This should only happen on account of a signal.
        if (io_ctx.to_fd == conn1fd) {
          ++short_writes_echo;
        } else if (io_ctx.to_fd == conn2fd) {
          ++short_writes_observer;
        }
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
int server_recvsend(int bufsize, io_uring &ring, int conn1fd, int conn2fd) {
  const auto interval = std::chrono::seconds(5);
  const auto start = std::chrono::steady_clock::now();

  struct {
    std::chrono::steady_clock::time_point when;
    std::uint64_t bytes_sent = 0;
    std::uint64_t short_reads = 0;
    std::uint64_t short_writes_echo = 0;
    std::uint64_t short_writes_observer = 0;
    std::uint64_t short_writes_pipe = 0;
  } snapshot;
  snapshot.when = start;

  std::ofstream log("log");

  std::uint64_t bytes_sent = 0;
  std::uint64_t short_reads = 0;
  std::uint64_t short_writes_echo = 0;
  std::uint64_t short_writes_observer = 0;
  std::uint64_t short_writes_pipe = 0;

  std::vector<char> buffer(bufsize);

  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now - snapshot.when >= interval) {
      std::ostringstream sstream;
      sstream << std::chrono::duration_cast<std::chrono::seconds>(now - start)
                     .count()
              << " s\t"
              << ((bytes_sent - snapshot.bytes_sent) * std::chrono::seconds(1) /
                  (now - snapshot.when) / 1'000'000)
              << " MB/s\t"
              << ((short_reads - snapshot.short_reads) *
                  std::chrono::seconds(1) / (now - snapshot.when))
              << " short_reads/s\t"
              << ((short_writes_echo - snapshot.short_writes_echo) *
                  std::chrono::seconds(1) / (now - snapshot.when))
              << " short_writes_echo/s\t"
              << ((short_writes_observer - snapshot.short_writes_observer) *
                  std::chrono::seconds(1) / (now - snapshot.when))
              << " short_writes_observer/s\t"
              << ((short_writes_pipe - snapshot.short_writes_pipe) *
                  std::chrono::seconds(1) / (now - snapshot.when))
              << " short_writes_pipe/s\n";
      const auto message = sstream.str();
      std::cout << message << std::flush;
      log << message << std::flush;
      snapshot.when = now;
      snapshot.bytes_sent = bytes_sent;
      snapshot.short_reads = short_reads;
      snapshot.short_writes_echo = short_writes_echo;
      snapshot.short_writes_observer = short_writes_observer;
    }

    int bytes_to_send = recv(conn1fd, buffer.data(), buffer.size(), 0);
    POSIX_REQUIRE(bytes_to_send);
    if (bytes_to_send == 0) {
      std::cerr << "Nothing more to read.\n";
      return 0;
    }
    if (std::size_t(bytes_to_send) < buffer.size()) {
      ++short_reads;
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
      io_ctx = std::bit_cast<IOEntryContext>(io_uring_cqe_get_data64(cqe));
      io_uring_cqe_seen(&ring, cqe);
      bytes_sent += result;
      if (result < io_ctx.bytes_desired) {
        // TODO: This should only happen on account of a signal.
        if (io_ctx.to_fd == conn1fd) {
          ++short_writes_echo;
        } else if (io_ctx.to_fd == conn2fd) {
          ++short_writes_observer;
        }
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
  out << "usage: " << argv0
      << " <recvsend | splicetee> <tcp | unix> <#pages>\n"
         "\nfor example: "
      << argv0 << " recvsend tcp 16\n";
}

int main(int argc, char *argv[]) {
  enum { RECVSEND, SPLICETEE } server_mode;
  std::unique_ptr<Net> net;
  int bufsize;

  if (argc != 4) {
    usage(std::cerr, argv[0]);
    return 1;
  }
  std::string_view arg{argv[1]};
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
  arg = argv[2];
  if (arg == "-h" || arg == "--help") {
    usage(std::cout, argv[0]);
    return 0;
  }
  if (arg == "tcp") {
    net = std::make_unique<TCP>();
  } else if (arg == "unix") {
    net = std::make_unique<Unix>();
  } else {
    usage(std::cerr, argv[0]);
    return 2;
  }
  arg = argv[3];
  bufsize = std::stoi(std::string{arg}) * getpagesize();

  int listen1fd = -1, conn1fd = -1;
  int pipe1fds[2] = {-1, -1};
  int listen2fd = -1, conn2fd = -1;
  int pipe2fds[2] = {-1, -1};

  io_uring ring;

  const int rc = [&]() {
    POSIX_REQUIRE(pipe(pipe1fds));
    POSIX_REQUIRE(pipe(pipe2fds));
    URING_REQUIRE(listen1fd = net->server_socket(1));
    URING_REQUIRE(listen2fd = net->server_socket(1));

    // fork() to client_sink(1338).
    switch (fork()) {
      case 0:
        // child
        // TODO: Should close all file descriptors except 0 and 1, but meh.
        std::exit(client_sink(bufsize, *net, listen2fd));
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
        std::exit(client_source_and_sink(bufsize, *net, listen1fd));
      case -1: {
        const int err = errno;
        std::cerr << "error forking to client_sink(1338): "
                  << std::strerror(err) << '\n';
        return err;
      }
    }

    URING_REQUIRE(io_uring_queue_init(8, &ring, 0));

    std::cerr << "Waiting for observer client to connect on observer socket.\n";
    POSIX_REQUIRE(conn2fd = accept(listen2fd, NULL, NULL));
    std::cerr << "Observer connection established.\n";

    std::cerr << "Waiting for echo client to connect on echo socket.\n";
    POSIX_REQUIRE(conn1fd = accept(listen1fd, NULL, NULL));
    std::cerr << "Echo connection established.\n\n";

    switch (server_mode) {
      case RECVSEND:
        return server_recvsend(bufsize, ring, conn1fd, conn2fd);
      case SPLICETEE:
        return server_splicetee(bufsize, ring, conn1fd, conn2fd, pipe1fds,
                                pipe2fds);
      default:
        std::unreachable();
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
