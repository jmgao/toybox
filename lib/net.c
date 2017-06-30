#include "toys.h"

int xsocket(int domain, int type, int protocol)
{
  int fd = socket(domain, type, protocol);

  if (fd < 0) perror_exit("socket %x %x", type, protocol);
  fcntl(fd, F_SETFD, FD_CLOEXEC);

  return fd;
}

void xsetsockopt(int fd, int level, int opt, void *val, socklen_t len)
{
  if (-1 == setsockopt(fd, level, opt, val, len)) perror_exit("setsockopt");
}

int xgetaddrinfo(char *host, char *port, int family, int socktype, int protocol,
                 int flags, int (*cb)(struct addrinfo*, void*), void *cookie)
{
  int rc;
  struct addrinfo info, *ai, *ai_head;

  memset(&info, 0, sizeof(info));
  info.ai_family = family;
  info.ai_socktype = socktype;
  info.ai_protocol = protocol;
  info.ai_flags = flags;

  rc = getaddrinfo(host, port, &info, &ai);
  if (rc || !ai) {
    error_exit("getaddrinfo '%s%s%s': %s", host, port ? ":" : "", port ? port : "",
      rc ? gai_strerror(rc) : "not found");
  }

  for (ai_head = ai; ai; ai = ai->ai_next) {
    rc = cb(ai, cookie);
    if (rc != -1) break;
  }
  freeaddrinfo(ai_head);

  return rc;
}

static int _xconnect(struct addrinfo* ai, void *unused) {
  int fd;

  fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd == -1) {
    return -1;
  } else if (!connect(fd, ai->ai_addr, ai->ai_addrlen)) {
    close(fd);
    return -1;
  }

  return fd;
}

int xconnect(char *host, char *port, int family, int socktype, int protocol,
             int flags)
{
  int rc = xgetaddrinfo(host, port, family, socktype, protocol, flags, _xconnect, 0);
  if (rc == -1) perror_exit("connect");
  return rc;
}

int xpoll(struct pollfd *fds, int nfds, int timeout)
{
  int i;

  for (;;) {
    if (0>(i = poll(fds, nfds, timeout))) {
      if (toys.signal) return i;
      if (errno != EINTR && errno != ENOMEM) perror_exit("xpoll");
      else if (timeout>0) timeout--;
    } else return i;
  }
}

// Loop forwarding data from in1 to out1 and in2 to out2, handling
// half-connection shutdown. timeouts return if no data for X miliseconds.
// Returns 0: both closed, 1 shutdown_timeout, 2 timeout
int pollinate(int in1, int in2, int out1, int out2, int timeout, int shutdown_timeout)
{
  struct pollfd pollfds[2];
  int i, pollcount = 2;

  memset(pollfds, 0, 2*sizeof(struct pollfd));
  pollfds[0].events = pollfds[1].events = POLLIN;
  pollfds[0].fd = in1;
  pollfds[1].fd = in2;

  // Poll loop copying data from each fd to the other one.
  for (;;) {
    if (!xpoll(pollfds, pollcount, timeout)) return pollcount;

    for (i=0; i<pollcount; i++) {
      if (pollfds[i].revents & POLLIN) {
        int len = read(pollfds[i].fd, libbuf, sizeof(libbuf));
        if (len<1) pollfds[i].revents = POLLHUP;
        else xwrite(i ? out2 : out1, libbuf, len);
      }
      if (pollfds[i].revents & POLLHUP) {
        // Close half-connection.  This is needed for things like
        // "echo GET / | netcat landley.net 80"
        // Note that in1 closing triggers timeout, in2 returns now.
        if (i) {
          shutdown(pollfds[0].fd, SHUT_WR);
          pollcount--;
          timeout = shutdown_timeout;
        } else return 0;
      }
    }
  }
}
