#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void primes(int read_fd) {
    int prime;
    if (read(read_fd, &prime, sizeof(int)) != sizeof(int)) {
        close(read_fd);
        exit(0);
    }
    printf("prime %d\n",prime);
    int p[2];
    pipe(p);
    int pid = fork();
    if(pid < 0) {
        fprintf(2,"fork error\n");
        exit(1);
    }
    if(pid == 0){
        close(p[1]);
        close(read_fd);
        primes(p[0]);
        close(p[0]);
        exit(0);
    } else {
        close(p[0]);
        int num;
        while (read(read_fd, &num, sizeof(num)) > 0) {
            if (num % prime != 0) {
                write(p[1], &num, sizeof(num));
            }
        }
        close(read_fd);
        close(p[1]);
        wait(0);
    }
}

int main() {
    
int p[2];
    pipe(p);
    int pid = fork();
    if(pid < 0) {
        fprintf(2,"fork error\n");
        exit(1);
    }
    if (pid == 0) {
        close(p[1]);
        primes(p[0]);
        close(p[0]);
        exit(0);
    } else {
        close(p[0]);
        for (int i = 2; i <= 280; i++) {
            if(write(p[1], &i, sizeof(int)) != sizeof(int)) {
                fprintf(2,"write error\n");
                exit(1);
            }
        }
        close(p[1]);
        wait(0);
    }
    exit(0);
}
