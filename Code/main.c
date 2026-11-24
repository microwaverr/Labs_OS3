#define _POSIX_C_SOURCE 200809L

#include <semaphore.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BLOCK_SIZE 4096

typedef struct {
    sem_t to_child1;
    sem_t to_child2;
    sem_t to_parent;
    size_t length;
    int stop;
    char data[BLOCK_SIZE];
} Shared;

static pid_t start_child(const char *program, const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(program, program, path, (char *)NULL);
        perror("execl");
        _exit(127);
    }
    return pid;
}

static int check_child(pid_t *pid, const char *name) {
    int status;
    pid_t result;
    do {
        result = waitpid(*pid, &status, WNOHANG);
    } while (result == -1 && errno == EINTR);

    if (result == 0) return 0;
    if (result == -1) {
        perror("waitpid");
        return -1;
    }

    fprintf(stderr, "%s finished before processing was completed\n", name);
    *pid = -1;
    return -1;
}

static int wait_for_result(sem_t *sem, pid_t *pid1, pid_t *pid2) {
    for (;;) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) == -1) {
            perror("clock_gettime");
            return -1;
        }
        deadline.tv_sec++;

        if (sem_timedwait(sem, &deadline) == 0) return 0;
        if (errno == EINTR) continue;
        if (errno != ETIMEDOUT) {
            perror("sem_timedwait");
            return -1;
        }
        if (check_child(pid1, "child1") == -1 ||
            check_child(pid2, "child2") == -1) return -1;
    }
}

static int write_all(const char *data, size_t length) {
    while (length > 0) {
        ssize_t written = write(STDOUT_FILENO, data, length);
        if (written == -1 && errno == EINTR) continue;
        if (written <= 0) {
            perror("write");
            return -1;
        }
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

static int wait_child(pid_t pid) {
    int status;
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        perror("waitpid");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child %ld terminated with an error\n", (long)pid);
        return -1;
    }
    return 0;
}

static void stop_child(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {}
}

int main(void) {
    char path[] = "/tmp/oslab3-XXXXXX";
    int fd = -1;
    int created = 0;
    int sem_count = 0;
    int success = 0;
    Shared *shared = MAP_FAILED;
    pid_t pid1 = -1;
    pid_t pid2 = -1;
    char *line = NULL;
    size_t capacity = 0;

    if (access("./child1", X_OK) == -1 ||
        access("./child2", X_OK) == -1) {
        perror("child program is unavailable");
        goto cleanup;
    }

    fd = mkstemp(path);
    if (fd == -1) {
        perror("mkstemp");
        goto cleanup;
    }
    created = 1;

    if (ftruncate(fd, sizeof(Shared)) == -1) {
        perror("ftruncate");
        goto cleanup;
    }

    shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE,
                  MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) {
        perror("mmap");
        goto cleanup;
    }
    if (close(fd) == -1) {
        fd = -1;
        perror("close");
        goto cleanup;
    }
    fd = -1;

    memset(shared, 0, sizeof(Shared));
    if (sem_init(&shared->to_child1, 1, 0) == -1) {
        perror("sem_init");
        goto cleanup;
    }
    sem_count = 1;
    if (sem_init(&shared->to_child2, 1, 0) == -1) {
        perror("sem_init");
        goto cleanup;
    }
    sem_count = 2;
    if (sem_init(&shared->to_parent, 1, 0) == -1) {
        perror("sem_init");
        goto cleanup;
    }
    sem_count = 3;

    pid1 = start_child("./child1", path);
    if (pid1 == -1) {
        perror("fork child1");
        goto cleanup;
    }
    pid2 = start_child("./child2", path);
    if (pid2 == -1) {
        perror("fork child2");
        goto cleanup;
    }

    ssize_t size;
    while ((size = getline(&line, &capacity, stdin)) != -1) {
        size_t offset = 0;
        while (offset < (size_t)size) {
            size_t count = (size_t)size - offset;
            if (count > BLOCK_SIZE) count = BLOCK_SIZE;

            memcpy(shared->data, line + offset, count);
            shared->length = count;
            if (sem_post(&shared->to_child1) == -1) {
                perror("sem_post");
                goto cleanup;
            }
            if (wait_for_result(&shared->to_parent, &pid1, &pid2) == -1)
                goto cleanup;
            if (write_all(shared->data, shared->length) == -1)
                goto cleanup;

            offset += count;
        }
    }
    if (ferror(stdin)) {
        perror("getline");
        goto cleanup;
    }

    shared->stop = 1;
    if (sem_post(&shared->to_child1) == -1) {
        perror("sem_post");
        goto cleanup;
    }

    if (wait_child(pid1) == -1) {
        pid1 = -1;
        goto cleanup;
    }
    pid1 = -1;
    if (wait_child(pid2) == -1) {
        pid2 = -1;
        goto cleanup;
    }
    pid2 = -1;
    success = 1;

cleanup:
    free(line);
    if (!success) {
        stop_child(pid1);
        stop_child(pid2);
    }
    if (shared != MAP_FAILED) {
        if (sem_count >= 3) sem_destroy(&shared->to_parent);
        if (sem_count >= 2) sem_destroy(&shared->to_child2);
        if (sem_count >= 1) sem_destroy(&shared->to_child1);
        if (munmap(shared, sizeof(Shared)) == -1) {
            perror("munmap");
            success = 0;
        }
    }
    if (fd != -1 && close(fd) == -1) {
        perror("close");
        success = 0;
    }
    if (created && unlink(path) == -1) {
        perror("unlink");
        success = 0;
    }
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
