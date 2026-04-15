#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

// must match kernel module
#define CMD_REGISTER 1
#define CMD_GET_STATS 2

struct stats {
    int pid;
    unsigned long rss;
};

int main() {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    int pid = getpid();

    if (ioctl(fd, CMD_REGISTER, &pid) < 0) {
        perror("ioctl register");
        return 1;
    }

    struct stats s;

    if (ioctl(fd, CMD_GET_STATS, &s) < 0) {
        perror("ioctl stats");
        return 1;
    }

    printf("PID: %d RSS: %lu\n", s.pid, s.rss);

    close(fd);
    return 0;
}
