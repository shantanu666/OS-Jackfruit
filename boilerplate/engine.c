#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <signal.h>

#define STACK_SIZE (1024 * 1024)

// FIFO paths
#define FIFO_REQ "/tmp/container_req"
#define FIFO_RES "/tmp/container_res"

// ioctl commands
#define CMD_REGISTER 1
#define CMD_GET_STATS 2

// ================= CONTAINER STRUCT =================
#define MAX_CONTAINERS 10

typedef struct {
    char id[32];
    pid_t pid;
    int running;
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

// ================= STATS STRUCT =================
struct stats {
    int pid;
    unsigned long rss;
};

// ================= CONTAINER MAIN =================
static int container_main(void *arg) {
    char **argv = (char **)arg;

    char *rootfs = argv[0];
    char *cmd = argv[1];
    int pipe_fd = *((int *)argv[2]);

    dup2(pipe_fd, STDOUT_FILENO);
    dup2(pipe_fd, STDERR_FILENO);
    close(pipe_fd);

    if (chroot(rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

    execlp(cmd, cmd, NULL);

    perror("exec");
    return 1;
}

// ================= SUPERVISOR =================
static int run_supervisor(const char *rootfs)
{
    printf("Supervisor started\n");

    unlink(FIFO_REQ);
    unlink(FIFO_RES);

    umask(0);
    mkfifo(FIFO_REQ, 0666);
    mkfifo(FIFO_RES, 0666);

    char buffer[256];

    while (1) {

        int fd = open(FIFO_REQ, O_RDONLY);
        if (fd < 0) continue;

        int n = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);

        if (n <= 0) continue;

        buffer[n] = '\0';

        // ================= START =================
        if (strncmp(buffer, "start", 5) == 0) {

            char id[32], rootfs_path[128], cmd[128];
            sscanf(buffer, "start %s %s %s", id, rootfs_path, cmd);

            int pipefd[2];
            pipe(pipefd);

            int *arg_pipe = malloc(sizeof(int));
            *arg_pipe = pipefd[1];

            char *child_args[] = { rootfs_path, cmd, (char *)arg_pipe, NULL };

            char *stack = malloc(STACK_SIZE);

            pid_t pid = clone(
                container_main,
                stack + STACK_SIZE,
                CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                child_args
            );

            printf("Container %s started with PID %d\n", id, pid);

            // register with kernel module
            int fdm = open("/dev/container_monitor", O_RDWR);
            if (fdm >= 0) {
                ioctl(fdm, CMD_REGISTER, &pid);
                close(fdm);
            }

            // store
            strcpy(containers[container_count].id, id);
            containers[container_count].pid = pid;
            containers[container_count].running = 1;
            container_count++;

            close(pipefd[1]);

            mkdir("logs", 0777);

            char path[128];
            sprintf(path, "logs/%s.log", id);

            FILE *f = fopen(path, "w");

            char buf[256];
            int bytes;

            while ((bytes = read(pipefd[0], buf, sizeof(buf))) > 0) {
                fwrite(buf, 1, bytes, f);
                fflush(f);
            }

            fclose(f);
            close(pipefd[0]);
        }

        // ================= PS =================
        else if (strncmp(buffer, "ps", 2) == 0) {

            int fdw = open(FIFO_RES, O_WRONLY);

            char out[512] = "ID\tPID\tSTATUS\n";

            for (int i = 0; i < container_count; i++) {
                char line[128];
                sprintf(line, "%s\t%d\t%s\n",
                        containers[i].id,
                        containers[i].pid,
                        containers[i].running ? "running" : "stopped");
                strcat(out, line);
            }

            write(fdw, out, strlen(out));
            close(fdw);
        }

        // ================= STOP =================
        else if (strncmp(buffer, "stop", 4) == 0) {

            char id[32];
            sscanf(buffer, "stop %s", id);

            for (int i = 0; i < container_count; i++) {
                if (strcmp(containers[i].id, id) == 0) {

                    kill(containers[i].pid, SIGTERM);
                    sleep(1);
                    kill(containers[i].pid, SIGKILL);

                    containers[i].running = 0;
                }
            }
        }
    }

    return 0;
}

// ================= CLIENT COMMANDS =================
int cmd_start(int argc, char *argv[]) {
    int fd = open(FIFO_REQ, O_WRONLY);

    char buf[256];
    sprintf(buf, "start %s %s %s", argv[2], argv[3], argv[4]);

    write(fd, buf, strlen(buf));
    close(fd);

    return 0;
}

int cmd_ps() {
    int fd = open(FIFO_REQ, O_WRONLY);
    write(fd, "ps", 2);
    close(fd);

    fd = open(FIFO_RES, O_RDONLY);

    char buf[512];
    int n = read(fd, buf, sizeof(buf)-1);
    buf[n] = '\0';

    printf("%s", buf);

    close(fd);
    return 0;
}

int cmd_stop(int argc, char *argv[]) {
    int fd = open(FIFO_REQ, O_WRONLY);

    char buf[128];
    sprintf(buf, "stop %s", argv[2]);

    write(fd, buf, strlen(buf));
    close(fd);

    return 0;
}

int cmd_logs(int argc, char *argv[]) {
    char path[128];
    sprintf(path, "logs/%s.log", argv[2]);

    FILE *f = fopen(path, "r");

    char buf[256];
    while (fgets(buf, sizeof(buf), f))
        printf("%s", buf);

    fclose(f);
    return 0;
}

int cmd_stats() {
    int fd = open("/dev/container_monitor", O_RDWR);

    struct stats s;
    ioctl(fd, CMD_GET_STATS, &s);

    printf("PID: %d RSS: %lu\n", s.pid, s.rss);

    close(fd);
    return 0;
}

// ================= MAIN =================
int main(int argc, char *argv[]) {

    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor(argv[2]);

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stats") == 0)
        return cmd_stats();

    return 0;
}

