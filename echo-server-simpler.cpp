extern "C" {
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
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
    return err;                                                   \
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
  enum Operation { SPLICE, SEND, RECV };
  std::int64_t bytes_desired : 33;
  Operation op : 3;
  int from_fd : 14;
  int to_fd : 14;
};

static_assert(sizeof(IOEntryContext) == 8);

void io_uring_prep(io_uring_sqe *sqe, IOEntryContext io_ctx, int flags = 0,
                   char *buffer = nullptr) {
  switch (io_ctx.op) {
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

struct RawMetrics {
  std::uint64_t bytes_sent = 0;
  std::uint64_t short_reads = 0;
  std::uint64_t short_writes_echo = 0;
  std::chrono::steady_clock::duration cpu_user =
      std::chrono::steady_clock::duration();
  std::chrono::steady_clock::duration cpu_system =
      std::chrono::steady_clock::duration();
  std::uint64_t page_faults_minor = 0;
  std::uint64_t page_faults_major = 0;
  std::uint64_t yields = 0;
  std::uint64_t preempts = 0;
};

struct Snapshot : public RawMetrics {
  std::chrono::steady_clock::time_point when;
};

struct Metrics : public RawMetrics {
  Snapshot snapshot;
};

int get_resource_usage(RawMetrics &raw) {
  rusage usage = {};
  POSIX_REQUIRE(getrusage(RUSAGE_SELF, &usage));

  using namespace std::chrono;
  raw.cpu_user =
      seconds(usage.ru_utime.tv_sec) + microseconds(usage.ru_utime.tv_usec);
  raw.cpu_system =
      seconds(usage.ru_stime.tv_sec) + microseconds(usage.ru_stime.tv_usec);
  raw.page_faults_minor = usage.ru_minflt;
  raw.page_faults_major = usage.ru_majflt;
  raw.yields = usage.ru_nvcsw;
  raw.preempts = usage.ru_nivcsw;

  return 0;
}

/* man(7) documentation relevant to the above:

       ru_utime
              This is the total amount of time spent executing in user
              mode, expressed in a timeval structure (seconds plus
              microseconds).

       ru_stime
              This is the total amount of time spent executing in kernel
              mode, expressed in a timeval structure (seconds plus
              microseconds).

       ru_minflt
              The number of page faults serviced without any I/O
              activity; here I/O activity is avoided by “reclaiming” a
              page frame from the list of pages awaiting reallocation.

       ru_majflt
              The number of page faults serviced that required I/O
              activity.

       ru_nvcsw (since Linux 2.6)
              The number of times a context switch resulted due to a
              process voluntarily giving up the processor before its time
              slice was completed (usually to await availability of a
              resource).

       ru_nivcsw (since Linux 2.6)
              The number of times a context switch resulted due to a
              higher priority process becoming runnable or because the
              current process exceeded its time slice.
*/

std::string log_snapshot_diff(std::chrono::steady_clock::time_point start,
                              std::chrono::steady_clock::time_point now,
                              const Metrics &metrics) {
  using namespace std::chrono;

  const auto diff = [&](const auto &mem_ptr) {
    return (metrics.*mem_ptr - metrics.snapshot.*mem_ptr);
  };
  const auto scaled_diff = [&](const auto &mem_ptr) {
    return diff(mem_ptr) * seconds(1) / (now - metrics.snapshot.when);
  };

  std::ostringstream sstream;
  sstream << (now - start) / milliseconds(1) << " milliseconds\t"
          << scaled_diff(&RawMetrics::bytes_sent) / 1'000'000 << " MB/s\t"
          << scaled_diff(&RawMetrics::short_reads) << " short_reads/s\t"
          << scaled_diff(&RawMetrics::short_writes_echo)
          << " short_writes_echo/s\t"
          // Note: NOT per second (at least not necessarily)
          << diff(&RawMetrics::cpu_user) / milliseconds(1)
          << " cpu_user_milliseconds\t"
          // Note: NOT per second (at least not necessarily)
          << diff(&RawMetrics::cpu_system) / milliseconds(1)
          << " cpu_system_milliseconds\t"
          << scaled_diff(&RawMetrics::page_faults_minor)
          << " minor_page_faults/s\t"
          << scaled_diff(&RawMetrics::page_faults_major)
          << " major_page_faults/s\t" << scaled_diff(&RawMetrics::yields)
          << " yields/s\t" << scaled_diff(&RawMetrics::preempts)
          << " preempts/s\n";
  return sstream.str();
}

// Consume from `conn1fd` and duplicate all data onto `connfd1`. Use `splice()`
// using the pipe `pipe1fds` to prevent any copies of data into user space.
int server_splice(int bufsize, int conn1fd, int (&pipe1fds)[2]) {
  const auto interval = std::chrono::seconds(1);
  const auto start = std::chrono::steady_clock::now();
  Metrics metrics;
  metrics.snapshot.when = start;

  const int splice_size = bufsize;
  std::ofstream log("log");

  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now - metrics.snapshot.when >= interval) {
      URING_REQUIRE(get_resource_usage(metrics));
      const std::string message = log_snapshot_diff(start, now, metrics);
      std::cout << message << std::flush;
      log << message << std::flush;
      metrics.snapshot.when = now;
      static_cast<RawMetrics &>(metrics.snapshot) = metrics;
    }

    const ssize_t bytes_in = splice(conn1fd, nullptr, pipe1fds[1], nullptr, splice_size, 0);
    POSIX_REQUIRE(bytes_in);
    if (bytes_in == 0) {
       // TODO: might leave data in the pipe
       std::cerr << "Nothing more to read.\n";
       return 0;
    }
    if (bytes_in < splice_size) {
      ++metrics.short_reads;
    }
    const ssize_t bytes_out = splice(pipe1fds[0], nullptr, conn1fd, nullptr, bytes_in, 0);
    POSIX_REQUIRE(bytes_out);
    metrics.bytes_sent += bytes_out;
  }

  return 0;
}

// Consume from `conn1fd` and duplicate all data onto `connfd1`.
// Use `recv()` and `send()` with a buffer in user space.
int server_recvsend(int bufsize, int conn1fd) {
  const auto interval = std::chrono::seconds(1);
  const auto start = std::chrono::steady_clock::now();

  Metrics metrics;
  metrics.snapshot.when = start;

  std::ofstream log("log");
  std::vector<char> buffer(bufsize);

  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now - metrics.snapshot.when >= interval) {
      URING_REQUIRE(get_resource_usage(metrics));
      const std::string message = log_snapshot_diff(start, now, metrics);
      std::cout << message << std::flush;
      log << message << std::flush;
      metrics.snapshot.when = now;
      static_cast<RawMetrics &>(metrics.snapshot) = metrics;
    }

    int bytes_to_send = recv(conn1fd, buffer.data(), buffer.size(), 0);
    POSIX_REQUIRE(bytes_to_send);
    if (bytes_to_send == 0) {
      std::cerr << "Nothing more to read.\n";
      return 0;
    }
    if (std::size_t(bytes_to_send) < buffer.size()) {
      ++metrics.short_reads;
    }

    for (int sent = 0;;) {
      const int rc = send(conn1fd, buffer.data(), bytes_to_send - sent, 0);
      POSIX_REQUIRE(rc);
      sent += rc;
      metrics.bytes_sent += rc;
      if (sent < bytes_to_send) {
        // This should only happend on account of a signal.
        ++metrics.short_writes_echo;
      } else {
        break;
      }
    }
  }

  return 0;
}

void usage(std::ostream &out, const char *argv0) {
  out << "usage: " << argv0
      << " <recvsend | splice> <tcp | unix> <#pages>\n"
         "\nfor example: "
      << argv0 << " recvsend tcp 16\n";
}

int main(int argc, char *argv[]) {
  enum { RECVSEND, SPLICE } server_mode;
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
  } else if (arg == "splice") {
    server_mode = SPLICE;
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

  const int rc = [&]() {
    POSIX_REQUIRE(pipe(pipe1fds));
    URING_REQUIRE(listen1fd = net->server_socket(1));

    // fork() to client_source_and_sink(...).
    switch (fork()) {
      case 0:
        // child
        // TODO: Should close all file descriptors except 0 and 1, but meh.
        std::exit(client_source_and_sink(bufsize, *net, listen1fd));
      case -1: {
        const int err = errno;
        std::cerr << "error forking to client_source_and_sink(): "
                  << std::strerror(err) << '\n';
        return err;
      }
    }

    std::cerr << "Waiting for echo client to connect on echo socket.\n";
    POSIX_REQUIRE(conn1fd = accept(listen1fd, NULL, NULL));
    std::cerr << "Echo connection established.\n\n";

    switch (server_mode) {
      case RECVSEND:
        return server_recvsend(bufsize, conn1fd);
      case SPLICE:
        return server_splice(bufsize, conn1fd, pipe1fds);
      default:
        std::unreachable();
    }
  }();

  for (const int fd : {conn1fd, listen1fd, pipe1fds[0], pipe1fds[1]}) {
    if (fd >= 0) {
      close(fd);
    }
  }

  wait(0);

  return rc;
}
