#include "pipe_lab.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Pipes carry bytes: retry interrupted calls and transfer the entire value. */
static int write_all(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size > 0) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { perror("write"); return 1; }
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *data, size_t size) {
    char *p = data;
    while (size > 0) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { perror("read"); return 1; }
        if (n == 0) { fprintf(stderr, "unexpected EOF\n"); return 1; }
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int wait_child(pid_t pid) {
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno == EINTR) continue;
        perror("waitpid");
        return 1;
    }
    return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}

static void finish_child(int status) {
    if (fflush(stdout) == EOF) status = 1;
    _exit(status);
}


/*
 * Task A – one-way pipe
 *
 * The parent writes the string "ping" into a pipe.
 * The child reads it and prints exactly:
 *   child received: ping
 * The parent then waits for the child and prints exactly:
 *   parent: sent ping
 *
 * Required steps in the child:
 *   1. close fd[1] (unused write end)
 *   2. read the message into a buffer
 *   3. null-terminate the buffer and print "child received: <message>"
 *   4. close fd[0]
 *
 * Required steps in the parent:
 *   1. close fd[0] (unused read end)
 *   2. write the string "ping" into fd[1]
 *   3. close fd[1]
 *   4. waitpid() for the child
 *   5. print "parent: sent ping"
 */
int task_one_way(void) {
    int fd[2];
    if (pipe(fd) == -1) { perror("pipe"); return 1; }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(fd[0]); close(fd[1]);
        return 1;
    }
    if (pid == 0) {
        close(fd[1]);
        char buffer[5];
        int failed = read_all(fd[0], buffer, 4);
        if (!failed) {
            buffer[4] = '\0';
            printf("child received: %s\n", buffer);
        }
        close(fd[0]);
        finish_child(failed);
    }
    close(fd[0]);
    int failed = write_all(fd[1], "ping", 4);
    close(fd[1]);
    int child_failed = wait_child(pid);
    if (failed || child_failed) return 1;
    printf("parent: sent ping\n");
    return 0;
}

/*
 * Task B – two-way pipe
 *
 * Pipes are unidirectional, so bidirectional communication needs TWO pipes:
 *   p2c[2]: parent writes → child reads
 *   c2p[2]: child writes  → parent reads
 *
 * The parent sends the integer 21 through p2c.
 * The child reads it, doubles it to 42, and prints exactly:
 *   child computed: 42
 * The child sends the result back through c2p.
 * The parent reads the result and prints exactly:
 *   parent received: 42
 *
 * Required: close every pipe end that is not used in each process.
 * Hint: write/read a raw int with write(fd, &val, sizeof(val)).
 */
int task_two_way(void) {
    int p2c[2], c2p[2];
    if (pipe(p2c) == -1) { perror("pipe"); return 1; }
    if (pipe(c2p) == -1) {
        perror("pipe");
        close(p2c[0]); close(p2c[1]);
        return 1;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(p2c[0]); close(p2c[1]);
        close(c2p[0]); close(c2p[1]);
        return 1;
    }
    if (pid == 0) {
        close(p2c[1]);
        close(c2p[0]);
        int value;
        int failed = read_all(p2c[0], &value, sizeof(value));
        close(p2c[0]);
        if (!failed) {
            value *= 2;
            printf("child computed: %d\n", value);
            fflush(stdout);
            failed = write_all(c2p[1], &value, sizeof(value));
        }
        close(c2p[1]);
        finish_child(failed);
    }
    close(p2c[0]);
    close(c2p[1]);
    int value = 21;
    int failed = write_all(p2c[1], &value, sizeof(value));
    close(p2c[1]);
    if (!failed) failed = read_all(c2p[0], &value, sizeof(value));
    close(c2p[0]);
    int child_failed = wait_child(pid);
    if (failed || child_failed) return 1;
    printf("parent received: %d\n", value);
    return 0;
}

/*
 * Task C – EOF limitation
 *
 * When every write end of a pipe is closed, the next read() returns 0.
 * This is how a reader detects end-of-stream (EOF).
 *
 * Sequence:
 *   parent: write "hello" (5 bytes) into the pipe, then CLOSE the write end
 *   child:  read in a loop until read() returns 0; track total bytes
 *           print exactly: child: got EOF after 5 bytes
 *
 * The loop must check the return value of read() on every iteration.
 * The child should NOT assume a single read delivers all the bytes.
 */
int task_eof(void) {
    int fd[2];
    if (pipe(fd) == -1) { perror("pipe"); return 1; }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(fd[0]); close(fd[1]);
        return 1;
    }
    if (pid == 0) {
        close(fd[1]);
        char buffer[16];
        size_t total = 0;
        int failed = 0;
        for (;;) {
            ssize_t n = read(fd[0], buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { perror("read"); failed = 1; break; }
            if (n == 0) break;
            total += (size_t)n;
        }
        if (!failed) printf("child: got EOF after %zu bytes\n", total);
        close(fd[0]);
        finish_child(failed);
    }
    close(fd[0]);
    int failed = write_all(fd[1], "hello", 5);
    close(fd[1]);
    int child_failed = wait_child(pid);
    return failed || child_failed;
}

/*
 * Task D – SIGPIPE / EPIPE limitation
 *
 * Writing to a pipe with no readers delivers SIGPIPE to the writer,
 * killing it unless the signal is ignored.  With SIGPIPE ignored,
 * write() returns -1 and sets errno to EPIPE.
 *
 * A synchronisation pipe (sync_fd) is used so the parent only writes
 * after the child has already closed the read end of data_fd.
 *
 * Required sequence:
 *   child:  close data_fd[0] and data_fd[1]
 *           write one byte to sync_fd[1] (ack), close sync_fd[1], exit
 *   parent: close sync_fd[1]
 *           read one byte from sync_fd[0] (blocks until child sends ack)
 *           close sync_fd[0] and data_fd[0]
 *           call signal(SIGPIPE, SIG_IGN)
 *           write one byte to data_fd[1]; check if errno == EPIPE
 *           print exactly: parent: broken pipe detected
 *           close data_fd[1], waitpid
 */
int task_broken_pipe(void) {
    int data_fd[2], sync_fd[2];
    if (pipe(data_fd) == -1) { perror("pipe"); return 1; }
    if (pipe(sync_fd) == -1) {
        perror("pipe");
        close(data_fd[0]); close(data_fd[1]);
        return 1;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(data_fd[0]); close(data_fd[1]);
        close(sync_fd[0]); close(sync_fd[1]);
        return 1;
    }
    if (pid == 0) {
        close(data_fd[0]);
        close(data_fd[1]);
        close(sync_fd[0]);
        int failed = write_all(sync_fd[1], "A", 1);
        close(sync_fd[1]);
        finish_child(failed);
    }
    close(sync_fd[1]);
    char ack;
    int failed = read_all(sync_fd[0], &ack, 1);
    close(sync_fd[0]);
    close(data_fd[0]);
    if (!failed) {
        void (*old_handler)(int) = signal(SIGPIPE, SIG_IGN);
        if (old_handler == SIG_ERR) {
            perror("signal");
            failed = 1;
        } else {
            ssize_t n;
            do {
                n = write(data_fd[1], "X", 1);
            } while (n == -1 && errno == EINTR);
            if (n == -1 && errno == EPIPE) {
                printf("parent: broken pipe detected\n");
            } else {
                fprintf(stderr, "expected EPIPE\n");
                failed = 1;
            }
            if (signal(SIGPIPE, old_handler) == SIG_ERR) {
                perror("signal");
                failed = 1;
            }
        }
    }
    close(data_fd[1]);
    int child_failed = wait_child(pid);
    return failed || child_failed;
}

/* ------------------------------------------------------------------ */
int main(void) {
    if (task_one_way() != 0)     { fprintf(stderr, "task_one_way failed\n");     return 1; }
    if (task_two_way() != 0)     { fprintf(stderr, "task_two_way failed\n");     return 1; }
    if (task_eof() != 0)         { fprintf(stderr, "task_eof failed\n");         return 1; }
    if (task_broken_pipe() != 0) { fprintf(stderr, "task_broken_pipe failed\n"); return 1; }
    printf("all tasks done\n");
    return 0;
}

