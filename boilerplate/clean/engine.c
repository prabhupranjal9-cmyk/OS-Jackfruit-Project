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
#include <sys/ioctl.h>
#include "monitor_ioctl.h"

#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_BUFFER_CAPACITY 16
#define LOG_CHUNK_SIZE 256

typedef struct {
    char container_id[50];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    int head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    bounded_buffer_t log_buffer;
    pthread_t logger_thread;
} supervisor_ctx_t;


// ================= BUFFER =================
int bounded_buffer_push(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    if(b->count == LOG_BUFFER_CAPACITY){
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == 0)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}


// ================= LOGGER =================
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (1) {
        bounded_buffer_pop(&ctx->log_buffer, &item);

        char path[100];
        mkdir(item.container_id, 0777);

        sprintf(path, "./%s/log.txt", item.container_id);

        FILE *f = fopen(path, "a");
        if (f) {
            fwrite(item.data, 1, item.length, f);
            fclose(f);
        }
    }
    return NULL;
}


// ================= SUPERVISOR =================
int run_supervisor()
{
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    pthread_mutex_init(&ctx.log_buffer.mutex, NULL);
    pthread_cond_init(&ctx.log_buffer.not_empty, NULL);
    pthread_cond_init(&ctx.log_buffer.not_full, NULL);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    unlink(CONTROL_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {

        int client_fd = accept(server_fd, NULL, NULL);

        char buffer[256] = {0};
        int n = read(client_fd, buffer, sizeof(buffer) - 1);

        if (n <= 0) {
            close(client_fd);
            continue;
        }

        buffer[n] = '\0';

        if (strncmp(buffer, "start", 5) == 0) {

            char id[50], rootfs[100], cmd[100];
            sscanf(buffer, "start %s %s %s", id, rootfs, cmd);

            int pipefd[2];
            pipe(pipefd);

            pid_t pid = fork();

            // ===== PARENT =====
            if (pid > 0) {

                int fd = open("/dev/container_monitor", O_RDWR);
                if (fd >= 0) {
                    struct monitor_request req;
                    req.pid = pid;
                    req.soft_limit_bytes = 40 * 1024 * 1024;
                    req.hard_limit_bytes = 64 * 1024 * 1024;
                    ioctl(fd, MONITOR_REGISTER, &req);
                    close(fd);
                }

                close(pipefd[1]);

                FILE *f = fopen("./containers.txt","a");
                if (f) {
                    fprintf(f, "%s %d\n", id, pid);
                    fclose(f);
                }

                // FIX: copy id safely
                char *id_copy = strdup(id);

                // 🔥 READER THREAD
                pthread_t reader_thread;

                void *reader_func(void *arg) {
                    int fd = ((int*)arg)[0];
                    char *cid = (char*)(((void**)arg)[1]);
                    char buf[LOG_CHUNK_SIZE];

                    while (1) {
                        int n = read(fd, buf, sizeof(buf));
                        if (n <= 0) break;

                        log_item_t item;
                        memset(&item, 0, sizeof(item));

                        strncpy(item.container_id, cid, sizeof(item.container_id)-1);
                        memcpy(item.data, buf, n);
                        item.length = n;

                        bounded_buffer_push(&ctx.log_buffer, &item);
                    }

                    close(fd);
                    free(cid);
                    free(arg);
                    return NULL;
                }

                void **args = malloc(2 * sizeof(void*));
                int *fd_ptr = malloc(sizeof(int));
                *fd_ptr = pipefd[0];
                args[0] = fd_ptr;
                args[1] = id_copy;

                pthread_create(&reader_thread, NULL, reader_func, args);
            }

            // ===== CHILD =====
            if (pid == 0) {

                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);

                close(pipefd[0]);
                close(pipefd[1]);

                chroot(rootfs);
                chdir("/");

                mount("proc", "/proc", "proc", 0, NULL);

                execlp(cmd, cmd, NULL);

                perror("exec failed");
                exit(1);
            }

            write(client_fd, "OK\n", 3);
            close(client_fd);
        }
    }
    return 0;
}


// ================= CLIENT =================
int send_request(char *msg)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    write(sock, msg, strlen(msg));
    shutdown(sock, SHUT_WR);

    char buffer[100];
    int n = read(sock, buffer, sizeof(buffer) - 1);

    if (n > 0) {
        buffer[n] = '\0';
        printf("Response: %s\n", buffer);
    }

    close(sock);
    return 0;
}


// ================= CLI =================
int cmd_start(int argc, char *argv[])
{
    char msg[256];
    sprintf(msg, "start %s %s %s", argv[2], argv[3], argv[4]);
    return send_request(msg);
}

int cmd_ps()
{
    FILE *f = fopen("containers.txt", "r");

    char id[50];
    int pid;

    printf("Containers:\n");

    while (f && fscanf(f, "%s %d", id, &pid) == 2) {
        printf("ID: %s PID: %d\n", id, pid);
    }

    if (f) fclose(f);
    return 0;
}

int cmd_stop(int argc, char *argv[])
{
    FILE *f = fopen("containers.txt", "r");
    FILE *temp = fopen("temp.txt", "w");

    char id[50];
    int pid;

    while (f && fscanf(f, "%s %d", id, &pid) == 2) {
        if (strcmp(id, argv[2]) == 0) {
            kill(pid, SIGKILL);
            printf("Stopped %s\n", id);
        } else {
            fprintf(temp, "%s %d\n", id, pid);
        }
    }

    if (f) fclose(f);
    if (temp) fclose(temp);

    remove("containers.txt");
    rename("temp.txt", "containers.txt");
    return 0;
}

int cmd_logs(int argc, char *argv[])
{
    char path[100];
    sprintf(path, "%s/log.txt", argv[2]);

    FILE *f = fopen(path, "r");
    char line[256];

    while (f && fgets(line, sizeof(line), f)) {
        printf("%s", line);
    }

    if (f) fclose(f);
    return 0;
}

int cmd_run(int argc, char *argv[]) {
    char msg[256];
    sprintf(msg, "start %s %s %s", argv[2], argv[3], argv[4]);

    send_request(msg);

    while (1) sleep(1);
}


// ================= MAIN =================
void handle_sigchld(int sig){
  while(waitpid(-1,NULL,WNOHANG)>0);
}

int main(int argc, char *argv[])
{  
   signal(SIGCHLD,handle_sigchld);

    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor();

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    return 0;
}
