#include "server.h"
#include "parser.h"
#include "encoder.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#ifdef KVSTORE_ENABLE_NET_FAULTS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

extern int shutdown_fd;
extern std::atomic<bool> keep_running;

namespace {

constexpr size_t OUTPUT_HIGH_WATER = 512 * 1024;
constexpr size_t MAX_OUTPUT_BUFFER = 4 * 1024 * 1024;


#ifdef KVSTORE_ENABLE_NET_FAULTS
bool take_net_fault(const char* name) {
    static std::atomic<bool> fired{false};
    const char* configured = std::getenv("KVSTORE_NET_FAULT");
    if (!configured || std::strcmp(configured, name) != 0) return false;

    bool expected = false;
    if (!fired.compare_exchange_strong(expected, true)) return false;
    std::fprintf(stderr, "NET_FAULT_TRIGGERED:%s\n", name);
    return true;
}
#endif

ssize_t socket_read(int fd, void* buf, size_t count) {
#ifdef KVSTORE_ENABLE_NET_FAULTS
    if (take_net_fault("read_eintr")) { errno = EINTR; return -1; }
    if (take_net_fault("read_eagain")) { errno = EAGAIN; return -1; }
#endif
    return ::read(fd, buf, count);
}

ssize_t socket_send(int fd, const void* buf, size_t count) {
#ifdef KVSTORE_ENABLE_NET_FAULTS
    if (take_net_fault("send_eintr")) { errno = EINTR; return -1; }
    if (take_net_fault("send_eagain")) { errno = EAGAIN; return -1; }
    if (take_net_fault("send_eio")) { errno = EIO; return -1; }
    if (count > 1 && take_net_fault("send_short")) {
        return ::send(fd, buf, count / 2, MSG_NOSIGNAL);
    }
#endif
    return ::send(fd, buf, count, MSG_NOSIGNAL);
}

int socket_accept(int listener) {
#ifdef KVSTORE_ENABLE_NET_FAULTS
    if (take_net_fault("accept_eintr")) { errno = EINTR; return -1; }
    if (take_net_fault("accept_eagain")) { errno = EAGAIN; return -1; }
#endif
    return ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
}

struct Connection {
    std::string input;
    std::string output;
    size_t sent = 0;
    bool read_closed = false;
    bool close_after_write = false;
    std::chrono::steady_clock::time_point last_active = std::chrono::steady_clock::now();

    size_t pending() const { return output.size() - sent; }

    void queue(std::string_view response) {
        if (sent != 0) {
            output.erase(0, sent);
            sent = 0;
        }
        output.append(response);
    }
};

bool flush_output(int fd, Connection& conn) {
    while (conn.pending() != 0) {
        ssize_t n = socket_send(fd, conn.output.data() + conn.sent,
                                conn.pending());
        if (n > 0) {
            conn.sent += static_cast<size_t>(n);
            conn.last_active = std::chrono::steady_clock::now();
        } else if (n == -1 && errno == EINTR) {
            continue;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            return false;
        }
    }

    if (conn.sent == conn.output.size()) {
        conn.output.clear();
        conn.sent = 0;
    }
    return true;
}

void protocol_error(Connection& conn) {
    conn.input.clear();
    conn.queue("-ERR protocol error\r\n");
    conn.close_after_write = true;
}

void process_input(Connection& conn, Dispatcher& dispatcher) {
    while (!conn.input.empty() && !conn.close_after_write &&
           conn.pending() < OUTPUT_HIGH_WATER) {
        ParseResult parsed = parse_one(conn.input);
        if (std::holds_alternative<ParseIncomplete>(parsed)) {
            if (conn.read_closed) protocol_error(conn);
            return;
        }
        if (std::holds_alternative<ParseError>(parsed)) {
            protocol_error(conn);
            return;
        }

        ParseComplete& complete = std::get<ParseComplete>(parsed);
        conn.input.erase(0, complete.consumed);

        std::string response;
        try {
            response = encode(dispatcher.dispatch(complete.command));
        } catch (const std::exception&) {
            conn.queue("-ERR command failed\r\n");
            conn.close_after_write = true;
            conn.input.clear();
            return;
        }

        if (response.size() > MAX_OUTPUT_BUFFER - conn.pending()) {
            conn.queue("-ERR response exceeds output limit\r\n");
            conn.close_after_write = true;
            conn.input.clear();
            return;
        }
        conn.queue(response);
    }
}

bool update_interest(int epfd, int fd, const Connection& conn) {
    epoll_event ev{};
    if (!conn.read_closed && !conn.close_after_write &&
        conn.pending() < OUTPUT_HIGH_WATER) {
        ev.events |= EPOLLIN | EPOLLRDHUP;
    }
    if (conn.pending() != 0) ev.events |= EPOLLOUT;
    ev.data.fd = fd;

    if (ev.events == 0) return true; // Caller closes drained EOF connections.
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

} // namespace

int run_server(Dispatcher& dispatcher) {
    int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd == -1) return -1;

    int opt = 1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(6379);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1 ||
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1 ||
        bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1 ||
        listen(server_fd, 128) == -1) {
        close(server_fd);
        return -1;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        close(server_fd);
        return -1;
    }

    epoll_event listen_ev{};
    listen_ev.events = EPOLLIN;
    listen_ev.data.fd = server_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &listen_ev) == -1) {
        close(epfd);
        close(server_fd);
        return -1;
    }

    if (shutdown_fd != -1) {
        epoll_event shutdown_ev{};
        shutdown_ev.events = EPOLLIN;
        shutdown_ev.data.fd = shutdown_fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, shutdown_fd, &shutdown_ev) == -1) {
            close(epfd);
            close(server_fd);
            return -1;
        }
    }

    std::unordered_map<int, Connection> clients;
    auto close_client = [&](int fd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        clients.erase(fd);
    };

    epoll_event events[64]{};
    while (keep_running) {
        int ready = epoll_wait(epfd, events, 64, 1000);
        if (ready == -1) {
            if (errno == EINTR) continue;
            break;
        }

        bool should_shutdown = false;
        for (int i = 0; i < ready; ++i) {
            int fd = events[i].data.fd;
            uint32_t mask = events[i].events;

            if (fd == shutdown_fd) {
                should_shutdown = true;
                break;
            }

            if (fd == server_fd) {
                if (mask & (EPOLLERR | EPOLLHUP)) {
                    should_shutdown = true;
                    break;
                }
                while (true) {
                    int client_fd = socket_accept(server_fd);
                    if (client_fd == -1) {
                        if (errno == EINTR) continue;
                        break;
                    }

                    epoll_event client_ev{};
                    client_ev.events = EPOLLIN | EPOLLRDHUP;
                    client_ev.data.fd = client_fd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev) == -1) {
                        close(client_fd);
                        continue;
                    }
                    clients.emplace(client_fd, Connection{});
                }
                continue;
            }

            auto it = clients.find(fd);
            if (it == clients.end()) continue;
            Connection& conn = it->second;

            if (mask & EPOLLERR) {
                close_client(fd);
                continue;
            }

            bool healthy = true;
            if ((mask & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) &&
                !conn.read_closed && !conn.close_after_write &&
                conn.pending() < OUTPUT_HIGH_WATER) {
                while (true) {
                    char chunk[4096];
                    ssize_t n = socket_read(fd, chunk, sizeof(chunk));
                    if (n > 0) {
                        conn.last_active = std::chrono::steady_clock::now();
                        conn.input.append(chunk, static_cast<size_t>(n));
                        process_input(conn, dispatcher);

                        if (conn.input.size() > MAX_BUFFER_SIZE) {
                            protocol_error(conn);
                        }
                        if (conn.close_after_write ||
                            conn.pending() >= OUTPUT_HIGH_WATER) break;
                    } else if (n == 0) {
                        conn.read_closed = true;
                        break;
                    } else if (errno == EINTR) {
                        continue;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    } else {
                        healthy = false;
                        break;
                    }
                }
            }

            if (healthy) {
                // Buffered commands must make progress even if the kernel has
                // no more readable socket bytes to wake EPOLLIN.
                while (true) {
                    const size_t input_before = conn.input.size();
                    const size_t pending_before = conn.pending();
                    if (!conn.close_after_write &&
                        conn.pending() < OUTPUT_HIGH_WATER) {
                        process_input(conn, dispatcher);
                    }

                    if (!flush_output(fd, conn)) {
                        healthy = false;
                        break;
                    }

                    // EAGAIN waits for EPOLLOUT; incomplete input waits for
                    // EPOLLIN. Flushing queued bytes is progress too: it may
                    // make room to parse more already-buffered commands.
                    if (conn.pending() != 0 || conn.close_after_write ||
                        conn.input.empty() ||
                        (conn.input.size() == input_before && pending_before == 0)) {
                        break;
                    }
                }
            }

            if (!healthy ||
                ((conn.close_after_write || conn.read_closed) && conn.pending() == 0)) {
                close_client(fd);
                continue;
            }

            if (!update_interest(epfd, fd, conn)) close_client(fd);
        }

        if (should_shutdown) break;

        auto now = std::chrono::steady_clock::now();
        std::vector<int> stale;
        for (const auto& [fd, conn] : clients) {
            if (now - conn.last_active > std::chrono::seconds(30)) {
                stale.push_back(fd);
            }
        }
        for (int fd : stale) close_client(fd);
    }

    for (const auto& [fd, conn] : clients) {
        (void)conn;
        close(fd);
    }
    close(epfd);
    close(server_fd);
    return 0;
}
