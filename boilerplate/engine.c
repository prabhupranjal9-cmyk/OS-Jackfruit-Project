#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sched.h>
#include <sys/ioctl.h>
#include "monitor_ioctl.h"

#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define STACK_SIZE (1024 * 1024)
#define LOG_BUF 256
#define MAX_CONTAINERS 20

// ================= CONTAINER =================
struct container_info {
    char id[50];
    pid_t pid;
    char state[20];
};

struct container_info containers[MAX_CONTAINERS];
int container_count = 0;

// ================= CHILD =================
int child_func(void *arg)
{
    char **args = (char **)arg;

    char *rootfs = args[0];
    int write_fd = atoi(args[1]);
    char *cmd = args[2];

    dup2(write_fd, STDOUT_FILENO);
    dup2(write_fd, STDERR_FILENO);
    close(write_fd);

    sethostname("container", 9);
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    if (chroot(rootfs) != 0) {
        perror("chroot failed");
        exit(1);
    }

    chdir("/");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    // ✅ FIX: execute command properly
    // parse command into arguments
char *argv_exec[10];
int i = 0;

char *token = strtok(cmd, " ");
while (token != NULL && i < 9) {
    argv_exec[i++] = token;
    token = strtok(NULL, " ");
}
argv_exec[i] = NULL;

// execute directly (VERY IMPORTANT)
execv(argv_exec[0], argv_exec);

    perror("exec failed");
    exit(1);
}

// ================= LOGGER =================
void *logger_thread(void *arg)
{
    char **args = (char **)arg;
    int fd = atoi(args[0]);
    char *id = args[1];

    char path[100];
    mkdir(id, 0777);
    sprintf(path, "%s/log.txt", id);

    FILE *f = fopen(path, "a");
    if (!f) return NULL;

    char buf[LOG_BUF];

    while (1) {
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;

        fwrite(buf, 1, n, f);
        fflush(f);
    }

    fclose(f);
    close(fd);
    return NULL;
}

// ================= SIGNAL =================
void sigchld_handler(int sig)
{
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

// ================= SUPERVISOR =================
int run_supervisor()
{
    signal(SIGCHLD, sigchld_handler);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    unlink(CONTROL_PATH);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {

        int client_fd = accept(server_fd, NULL, NULL);

        char buffer[256] = {0};
        int n = read(client_fd, buffer, sizeof(buffer)-1);
        buffer[n] = '\0';

        // ================= START =================
        if (strncmp(buffer, "start", 5) == 0) {

            char id[50], rootfs[100], cmd[150];

            // ✅ FIX: capture full command properly
            sscanf(buffer, "start %s %s %[^\n]", id, rootfs, cmd);

            int pipefd[2];
            pipe(pipefd);

            char *stack = malloc(STACK_SIZE);

            char fd_str[10];
            sprintf(fd_str, "%d", pipefd[1]);

            char *args[4];
            args[0] = rootfs;
            args[1] = fd_str;
            args[2] = cmd;
            args[3] = NULL;

            pid_t pid = clone(child_func,
                              stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              args);

            if (pid < 0) {
                perror("clone failed");
                write(client_fd, "FAIL\n", 5);
                close(client_fd);
                continue;
            }

            printf("Started %s PID=%d\n", id, pid);

            strcpy(containers[container_count].id, id);
            containers[container_count].pid = pid;
            strcpy(containers[container_count].state, "running");
            container_count++;

            // ================= MONITOR =================
            int fd = open("/dev/container_monitor", O_RDWR);

            if (fd < 0) {
                perror("monitor open failed");
            } else {
                printf("Monitor opened for PID %d\n", pid);

                struct monitor_request req = {0};
                req.pid = pid;
                req.soft_limit_bytes = 100 * 1024 * 1024;
                req.hard_limit_bytes = 200 * 1024 * 1024;

                strncpy(req.container_id, id, sizeof(req.container_id)-1);

                int ret = ioctl(fd, MONITOR_REGISTER, &req);

                if (ret < 0)
                    perror("ioctl failed");
                else
                    printf("Monitor registered PID %d\n", pid);

                close(fd);
            }

            close(pipefd[1]);

            pthread_t t;
            char *log_args[2];
            char fd_read[10];
            sprintf(fd_read, "%d", pipefd[0]);

            log_args[0] = strdup(fd_read);
            log_args[1] = strdup(id);

            pthread_create(&t, NULL, logger_thread, log_args);

            write(client_fd, "OK\n", 3);
            close(client_fd);
        }

        // ================= PS =================
        else if (strncmp(buffer, "ps", 2) == 0) {

            char output[1024] = {0};
            strcat(output, "ID\tPID\tSTATE\n");

            for (int i = 0; i < container_count; i++) {
                char line[100];
                sprintf(line, "%s\t%d\t%s\n",
                        containers[i].id,
                        containers[i].pid,
                        containers[i].state);
                strcat(output, line);
            }

            write(client_fd, output, strlen(output));
            close(client_fd);
        }

        // ================= STOP =================
        else if (strncmp(buffer, "stop", 4) == 0) {

            char id[50];
            sscanf(buffer, "stop %s", id);

            for (int i = 0; i < container_count; i++) {
                if (strcmp(containers[i].id, id) == 0) {
                    kill(containers[i].pid, SIGKILL);
                    strcpy(containers[i].state, "stopped");
                }
            }

            write(client_fd, "STOPPED\n", 8);
            close(client_fd);
        }

        // ================= LOGS =================
        else if (strncmp(buffer, "logs", 4) == 0) {

            char id[50];
            sscanf(buffer, "logs %s", id);

            char path[100];
            sprintf(path, "%s/log.txt", id);

            FILE *f = fopen(path, "r");

            if (!f) {
                write(client_fd, "No logs\n", 8);
            } else {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    write(client_fd, line, strlen(line));
                }
                fclose(f);
            }

            close(client_fd);
        }
    }
}

// ================= CLIENT =================
int send_request(char *msg)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect failed");
        return 1;
    }

    write(sock, msg, strlen(msg));
    shutdown(sock, SHUT_WR);

    char buffer[1024];
    int n;

    while ((n = read(sock, buffer, sizeof(buffer)-1)) > 0) {
        buffer[n] = '\0';
        printf("%s", buffer);
    }

    close(sock);
    return 0;
}

// ================= MAIN =================
int main(int argc, char *argv[])
{
    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor();

    if (strcmp(argv[1], "ps") == 0)
        return send_request("ps");

    char msg[256] = {0};

    for (int i = 1; i < argc; i++) {
        strcat(msg, argv[i]);
        strcat(msg, " ");
    }

    return send_request(msg);
}
