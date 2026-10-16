#include "epoll_loop.h"

// Create the epoll file descriptor used to monitor all registered sockets.
EpollLoop::EpollLoop() : epoll_fd(epoll_create1(0)) {
    if (epoll_fd == -1) {
        perror("epoll_create1");
    }
}

// Close the epoll file descriptor when the event loop is destroyed.
EpollLoop::~EpollLoop() {
    if (epoll_fd != -1) {
        close(epoll_fd);
    }
}

// Add one socket to the epoll interest list.
bool EpollLoop::add_fd(int fd, unsigned int events) {
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1) {
        perror("epoll_ctl(ADD)");
        return false;
    }

    return true;
}

// Remove one socket from the epoll interest list.
bool EpollLoop::remove_fd(int fd) {
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        perror("epoll_ctl(DEL)");
        return false;
    }

    return true;
}

// Update one socket's interest mask without removing it from epoll.
bool EpollLoop::modify_fd(int fd, unsigned int events) {
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == -1) {
        perror("epoll_ctl(MOD)");
        return false;
    }

    return true;
}

// Wait for socket activity and call the server handler for each ready socket.
bool EpollLoop::run(const function<void(int, unsigned int)>& callback,
                    const function<void()>& on_iteration,
                    const function<bool()>& should_stop) {
    if (epoll_fd == -1) {
        return false;
    }

    epoll_event events[64];
    while (!should_stop()) {
        const int event_count = epoll_wait(epoll_fd, events, 64, 100);
        if (event_count == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            return false;
        }

        for (int index = 0; index < event_count; ++index) {
            callback(events[index].data.fd, events[index].events);
        }
        on_iteration();
        if (should_stop()) break;
    }

    return true;
}
