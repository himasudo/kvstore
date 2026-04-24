#include "server.h"
#include "parser.h"
#include "encoder.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <unordered_map>
#include <string>
#include <sys/eventfd.h>
#include <atomic>
#include <chrono>

extern int shutdown_fd;
extern std::atomic<bool> keep_running;

int run_server(Dispatcher& dispatcher) {

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(6379);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 128);

    auto set_nonblocking = [](int fd) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    };
    set_nonblocking(server_fd);

    int epfd = epoll_create1(0);

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

    if (shutdown_fd != -1) {
        epoll_event ev_shutdown{};
        ev_shutdown.events = EPOLLIN;
        ev_shutdown.data.fd = shutdown_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, shutdown_fd, &ev_shutdown);
    }

    std::unordered_map<int, std::string> buffers;
    std::unordered_map<int, std::chrono::steady_clock::time_point> last_active;
    auto close_connection = [&](int fd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        buffers.erase(fd);
        last_active.erase(fd);
    };
    epoll_event events[64];

    while (keep_running) {
        int n = epoll_wait(epfd, events, 64, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        bool should_shutdown = false;

        for (int i = 0; i < n; i++) {
            int fd              = events[i].data.fd;
            uint32_t events_mask = events[i].events;

            if (fd == shutdown_fd) {
                should_shutdown = true;
                break; 
            }
            if (fd == server_fd) {
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int cfd = accept(server_fd, (sockaddr*)&client_addr, &len);
                    if (cfd < 0) break;

                    set_nonblocking(cfd);
                    epoll_event cev{};
                    cev.events  = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                    buffers[cfd] = "";

                    last_active[cfd] = std::chrono::steady_clock::now();
                }

            } else if (events_mask & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                close_connection(fd);

            } else if (events_mask & EPOLLIN) {
                auto& accum = buffers[fd];
                
                last_active[fd] = std::chrono::steady_clock::now();

                while (true) {
                    char chunk[4096];
                    int r = read(fd, chunk, sizeof(chunk));
                    if (r > 0)  { accum.append(chunk, r); continue; }
                    if (r == 0) { break; }
                    if (errno == EAGAIN) break;
                    break;
                }

                if (accum.size() > MAX_BUFFER_SIZE) {
                    close_connection(fd);
                    continue;
                }

                while (!accum.empty()) {
                    auto result = parse(std::string_view(accum));
                    if (!result) break;

                    accum.clear();

                    auto dresult         = dispatcher.dispatch(*result);
                    std::string response = encode(dresult);

                    size_t sent = 0;
                    while (sent < response.size()) {
                        int w = write(fd,
                                      response.data() + sent,
                                      response.size() - sent);
                        if (w < 0) {
                            if (errno == EINTR) continue;
                            break;
                        }
                        sent += w;
                    }
                }
            }
        }
        if (should_shutdown) {
            break;
        }

        auto now = std::chrono::steady_clock::now();
        std::vector<int> stale_fds;

        for (const auto& [fd, timestamp]: last_active) {
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - timestamp).count();
            if (idle > 30) {
                stale_fds.push_back(fd);
            }
        }

        for (int stale_fd : stale_fds) {
            close_connection(stale_fd);
        }
    }

    close(epfd);
    close(server_fd);
    return 0;
}