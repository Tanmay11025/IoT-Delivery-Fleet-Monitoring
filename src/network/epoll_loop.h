#pragma once

#include <functional>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <sys/epoll.h>
#include <unistd.h>

using namespace std;

class EpollLoop {
public:
    EpollLoop();
    ~EpollLoop();

    EpollLoop(const EpollLoop&) = delete;
    EpollLoop& operator=(const EpollLoop&) = delete;

    // Watch a file descriptor for the requested epoll events.
    bool add_fd(int fd, unsigned int events);

    // Stop watching a file descriptor.
    bool remove_fd(int fd);

    // Wait for ready descriptors and pass each one to the callback.
    bool run(const function<void(int, unsigned int)>& callback);

private:
    int epoll_fd = -1;
};
