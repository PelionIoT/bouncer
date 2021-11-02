#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

void proxy_loop(int fd0, int fd1)
{
    int event_count, i, rc, other_fd;
    const int MAX_EVENTS = 1024;
    struct epoll_event event, events[MAX_EVENTS];
    char buf[1024];

    int epoll_fd = epoll_create1(0);

    if (epoll_fd == -1) {
        perror("Failed to create epoll");
        goto out1;
    }

    event.events = EPOLLIN;
    event.data.fd = fd0;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd0, &event)) {
        perror("Failed to add fd to epoll");
        goto out;
    }

    event.events = EPOLLIN;
    event.data.fd = fd1;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd1, &event)) {
        perror("Failed to add fd to epoll");
        goto out;
    }

    while(1) {
        event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, 30000);
        if (event_count == -1) {
            perror("Failed to wait for epoll events");
            goto out;
        }
        for (i = 0; i < event_count; i++) {
            rc = read(events[i].data.fd, buf, sizeof(buf));
            fprintf(stderr, "Read %u bytes from fd %d: %.*s\n", rc, events[i].data.fd, rc, buf);
            if (rc == -1) {
                perror("Error while reading from socket");
                goto out;
            } else if (rc == 0) {
                fprintf(stderr, "Got EOF from fd %d.\n", events[i].data.fd);
                goto out;
            }

            other_fd = (events[i].data.fd == fd0 ? fd1 : fd0);
            rc = write(other_fd, buf, rc);
            if (rc == -1) {
                perror("Failed to write to socket");
                goto out;
            }
        }
    }

out:
    if (close(epoll_fd)) {
        perror("Failed to close epoll fd");
    }
out1:
    return;
}

int connect_to_docker(char *docker_sock_path)
{
    struct sockaddr_un docker_sockaddr;
    int docker_fd = -1, rc;

    // Create a socket FD
    docker_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (docker_fd == -1) {
        perror("Failed to create docker socket fd");
        return -1;
    }

    // Connect to docker.sock
    memset(&docker_sockaddr, 0, sizeof docker_sockaddr);
    docker_sockaddr.sun_family = AF_UNIX;
    strncpy(docker_sockaddr.sun_path, docker_sock_path, sizeof docker_sockaddr.sun_path);
    rc = connect(docker_fd, (struct sockaddr *)&docker_sockaddr, sizeof(docker_sockaddr));
    if (rc == -1) {
        perror("Failed to connect to docker socket");
        close(docker_fd);
        return -1;
    }
    fprintf(stderr, "Connected to docker.sock at %s\n", docker_sockaddr.sun_path);

    return docker_fd;
}

int main(int argc, char **argv)
{
    int listen_fd, docker_fd, rc = 0, accept_fd;
    struct sockaddr_un listen_sockaddr;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <listen socket path> <docker.sock path>\n", argv[0]);
        rc = 1;
        goto out3;
    }
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("Failed to create listening socket fd\n");
        rc = 1;
        goto out3;
    }

    memset(&listen_sockaddr, 0, sizeof listen_sockaddr);

    // Open the listening socket
    listen_sockaddr.sun_family = AF_UNIX;
    strncpy(listen_sockaddr.sun_path, argv[1], sizeof listen_sockaddr.sun_path);
    unlink(listen_sockaddr.sun_path);
    rc = bind(listen_fd, (struct sockaddr *)&listen_sockaddr, sizeof(listen_sockaddr));
    if (rc == -1) {
        perror("Failed to bind listening socket");
        goto out1;
    }
    fprintf(stderr, "Bound socket to %s\n", listen_sockaddr.sun_path);

    // Accept a connection on the proxy sock
    if (listen(listen_fd, 1) == -1) {
        perror("Failed to listen on proxy socket");
        goto out;
    }
    fprintf(stderr, "Listening at %s\n", listen_sockaddr.sun_path);

    while (1) {
        if ((accept_fd = accept(listen_fd, NULL, NULL)) == -1) {
            perror("Failed to accept connection on proxy socket");
            continue;
        }

        docker_fd = connect_to_docker(argv[2]);
        if (docker_fd == -1) {
            fprintf(stderr, "Failed to connect to docker.sock at %s\n", argv[2]);
            close(accept_fd);
            continue;
        }

        proxy_loop(accept_fd, docker_fd);
    }

out:
    close(docker_fd);
out1:
    unlink(listen_sockaddr.sun_path);
out2:
    close(listen_fd);
out3:
    return rc;
}
