#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

struct proxy_pair {
    int fd_in;  // The socket to read from.  If fd_in == listen_fd, this is the listening socket.
    int fd_out; // The socket to write to
    struct proxy_pair *partner; // The other proxy_pair that is related to this one.
};

int connect_to_docker(char *docker_sock_path);

int add_fd_pair_to_proxy(int epoll_fd, int fd0, int fd1)
{
    struct epoll_event event;
    struct proxy_pair *pair1, *pair2;
    event.events = EPOLLIN;
    pair1 = (struct proxy_pair *)malloc(sizeof(struct proxy_pair));
    if (pair1 == NULL) {
        fprintf(stderr, "Failed to allocate proxy pair\n");
        return -1;
    }
    pair2 = (struct proxy_pair *)malloc(sizeof(struct proxy_pair));
    if (pair2 == NULL) {
        fprintf(stderr, "Failed to allocate proxy pair\n");
        free(pair1);
        return -1;
    }

    pair1->fd_in = fd0;
    pair1->fd_out = fd1;
    pair1->partner = pair2;
    event.data.ptr = pair1;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd0, &event)) {
        perror("Failed to add fd to epoll");
        return -1;
    }

    event.events = EPOLLIN;
    pair2->fd_in = fd1;
    pair2->fd_out = fd0;
    pair2->partner = pair1;
    event.data.ptr = pair2;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd1, &event)) {
        perror("Failed to add fd to epoll");
        return -1;
    }

    fprintf(stderr, "Added %d %d to epoll\n", fd0, fd1);

    return 0;
}

void proxy_loop(int listen_fd, char *docker_sock_path)
{
    int epoll_fd, accept_fd, docker_fd, event_count, i, rc, other_fd;
    const int MAX_EVENTS = 1024;
    struct epoll_event event, events[MAX_EVENTS];
    char buf[1024];
    struct proxy_pair listen_pair, *pair;

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("Failed to create epoll");
        goto out2;
    }

    listen_pair.fd_in = listen_fd;
    listen_pair.fd_out = -1;
    event.data.ptr = &listen_pair;
    event.events = EPOLLIN;
    rc = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event);
    if (rc == -1) {
        perror("Error adding listen socket to epoll");
        goto out1;
    }

    while (1) {
        event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, 30000);
        if (event_count == -1) {
            perror("Failed to wait for epoll events");
            break;
        }
        for (i = 0; i < event_count; i++) {
            int done = 0;
            pair = (struct proxy_pair *)events[i].data.ptr;
            fprintf(stderr, "got epoll event: %d %d\n", pair->fd_in, pair->fd_out);
            if (events[i].events & EPOLLHUP) {
                fprintf(stderr, "epoll HUP\n");
                done = 1;
            }
            if ((events[i].events & EPOLLERR) ||
                (!(events[i].events & EPOLLIN))) {
                    fprintf(stderr, "epoll error: %u\n", events[i].events);
                    goto out1;
            }
            if (pair->fd_in == listen_fd) {
                // Got a new connection
                if ((accept_fd = accept(listen_fd, NULL, NULL)) == -1) {
                    perror("Failed to accept connection on proxy socket");
                    continue;
                }

                docker_fd = connect_to_docker(docker_sock_path);
                if (docker_fd == -1) {
                    fprintf(stderr, "Failed to connect to docker.sock at %s\n", docker_sock_path);
                    close(accept_fd);
                    continue;
                }

                if (add_fd_pair_to_proxy(epoll_fd, accept_fd, docker_fd) == -1) {
                    goto out1;
                }
            } else {
                rc = read(pair->fd_in, buf, sizeof(buf));
                fprintf(stderr, "Read %u bytes from fd %d: %.*s\n", rc, pair->fd_in, rc, buf);
                if (rc == -1) {
                    perror("Error while reading from socket");
                    break;
                } else if (rc == 0) {
                    fprintf(stderr, "Got EOF from fd %d.\n", pair->fd_in);
                    done = 1;
                }

                rc = write(pair->fd_out, buf, rc);
                if (rc == -1) {
                    perror("Failed to write to socket");
                    done = 1;
                }

            }

            if (done) {
                // close()ing a file descriptor makes epoll remove it from the interest list
                close(pair->fd_in);
                close(pair->fd_out);
                free(pair->partner);
                free(pair);
            }
        }
    }

out1:
    close(epoll_fd);
out2:
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
    rc = connect(docker_fd, (struct sockaddr *) &docker_sockaddr, sizeof(docker_sockaddr));
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
    int listen_fd, rc = 0;
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
    rc = bind(listen_fd, (struct sockaddr *) &listen_sockaddr, sizeof(listen_sockaddr));
    if (rc == -1) {
        perror("Failed to bind listening socket");
        goto out1;
    }
    fprintf(stderr, "Bound socket to %s\n", listen_sockaddr.sun_path);

    // Accept a connection on the proxy sock
    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("Failed to listen on proxy socket");
        goto out1;
    }
    fprintf(stderr, "Listening at %s\n", listen_sockaddr.sun_path);

    proxy_loop(listen_fd, argv[2]);

out1:
    unlink(listen_sockaddr.sun_path);
out2:
    close(listen_fd);
out3:
    return rc;
}
