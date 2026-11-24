#define _POSIX_C_SOURCE 200809L

#include <semaphore.h>
#include <stddef.h>

#define BLOCK_SIZE 4096

typedef struct {
    sem_t to_child1;
    sem_t to_child2;
    sem_t to_parent;
    size_t length;
    int stop;
    char data[BLOCK_SIZE];
} Shared;
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static int wait_sem(sem_t *sem) {
    while (sem_wait(sem) == -1) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: child1 FILE\n");
        return 1;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    Shared *shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    if (close(fd) == -1) {
        perror("close");
        munmap(shared, sizeof(Shared));
        return 1;
    }

    int success = 1;
    for (;;) {
        if (wait_sem(&shared->to_child1) == -1) {
            perror("sem_wait");
            success = 0;
            break;
        }
        if (shared->stop) {
            if (sem_post(&shared->to_child2) == -1) {
                perror("sem_post");
                success = 0;
            }
            break;
        }
        for (size_t i = 0; i < shared->length; i++) {
            if (shared->data[i] >= 'a' && shared->data[i] <= 'z')
                shared->data[i] = (char)(shared->data[i] - 'a' + 'A');
        }
        if (sem_post(&shared->to_child2) == -1) {
            perror("sem_post");
            success = 0;
            break;
        }
    }

    if (munmap(shared, sizeof(Shared)) == -1) {
        perror("munmap");
        success = 0;
    }
    return success ? 0 : 1;
}
