/* netcat.c - Forward stdin/stdout to a file or network connection.
 *
 * Copyright 2007 Rob Landley <rob@landley.net>
 *
 * TODO: udp, ipv6, genericize for telnet/microcom/tail-f

USE_NETCAT(OLDTOY(nc, netcat, TOYFLAG_USR|TOYFLAG_BIN))
USE_NETCAT(NEWTOY(netcat, USE_NETCAT_LISTEN("^tlL")"w#W#p:s:q#f:46"USE_NETCAT_LISTEN("[!tlL][!Lw]")"[!46]", TOYFLAG_BIN))

config NETCAT
  bool "netcat"
  default y
  help
    usage: netcat [-u] [-wpq #] [-s addr] {IPADDR PORTNUM|-f FILENAME}

    -4, -6	force IPv4, IPv6
    -f	use FILENAME (ala /dev/ttyS0) instead of network
    -p	local port number
    -q	quit SECONDS after EOF on stdin, even if stdout hasn't closed yet
    -s	local source address
    -w	SECONDS timeout to establish connection
    -W	SECONDS timeout for idle connection

    Use "stty 115200 -F /dev/ttyS0 && stty raw -echo -ctlecho" with
    netcat -f to connect to a serial port.

config NETCAT_LISTEN
  bool "netcat server options (-let)"
  default y
  depends on NETCAT
  help
    usage: netcat [-t] [-lL COMMAND...]

    -l	listen for one incoming connection
    -L	listen for multiple incoming connections (server mode)
    -t	allocate tty (must come before -l or -L)

    The command line after -l or -L is executed (as a child process) to handle
    each incoming connection. If blank -l waits for a connection and forwards
    it to stdin/stdout. If no -p specified, -l prints port it bound to and
    backgrounds itself (returning immediately).

    For a quick-and-dirty server, try something like:
    netcat -s 127.0.0.1 -p 1234 -tL /bin/bash -l
*/

#define FOR_netcat
#include "toys.h"

GLOBALS(
  char *filename;        // -f read from filename instead of network
  long quit_delay;       // -q Exit after EOF from stdin after # seconds.
  char *source_address;  // -s Bind to a specific source address.
  char *port;            // -p Bind to a specific source port.
  long idle;             // -W Wait # seconds for more data
  long wait;             // -w Wait # seconds for a connection.
)

static void timeout(int signum)
{
  if (TT.wait) error_exit("Timeout");
  // This should be xexit() but would need siglongjmp()...
  exit(0);
}

static void set_alarm(int seconds)
{
  xsignal(SIGALRM, seconds ? timeout : SIG_DFL);
  alarm(seconds);
}

static int socket_bind(struct addrinfo *ai, void *fd_) {
  int fd = *(int*)fd_;
  return bind(fd, ai->ai_addr, ai->ai_addrlen);
}

static int socket_connect(struct addrinfo *ai, void *unused) {
  int fd, rc;

  fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd == -1) return -1;

  fcntl(fd, F_SETFD, FD_CLOEXEC);
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &rc, sizeof(rc));

  if (TT.source_address || TT.port) {
    rc = xgetaddrinfo(TT.source_address, TT.port, ai->ai_family,
      ai->ai_socktype, ai->ai_protocol, ai->ai_flags, socket_bind, &fd);
    if (rc != 0) {
      close(fd);
      return -1;
    }
  }

  rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
  if (rc == -1) {
    close(fd);
    return -1;
  }
  return fd;
}

static int socket_server(struct addrinfo *ai, void *unused) {
  int fd, rc;

  fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd == -1) return -1;

  fcntl(fd, F_SETFD, FD_CLOEXEC);
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &rc, sizeof(rc));

  if (bind(fd, ai->ai_addr, ai->ai_addrlen)) {
    close(fd);
    return -1;
  }

  return fd;
}

void netcat_main(void)
{
  struct sockaddr_in *address = (void *)toybuf;
  int sockfd = -1, in1 = 0, in2 = 0, out1 = 1, out2 = 1;
  int family = 0;
  pid_t child;

  if (toys.optflags&FLAG_4) family = AF_INET;
  if (toys.optflags&FLAG_6) family = AF_INET6;

  // Addjust idle and quit_delay to miliseconds or -1 for no timeout
  TT.idle = TT.idle ? TT.idle*1000 : -1;
  TT.quit_delay = TT.quit_delay ? TT.quit_delay*1000 : -1;

  set_alarm(TT.wait);

  // The argument parsing logic can't make "<2" conditional on other
  // arguments like -f and -l, so we do it by hand here.
  if ((toys.optflags&FLAG_f) ? toys.optc :
      (!(toys.optflags&(FLAG_l|FLAG_L)) && toys.optc!=2))
        help_exit("bad argument count");

  if (TT.filename) in1 = out2 = xopen(TT.filename, O_RDWR);
  else if (!CFG_NETCAT_LISTEN || !(toys.optflags&(FLAG_L|FLAG_l))) {
    // Dial out
    sockfd = xgetaddrinfo(toys.optargs[0], toys.optargs[1], family,
      SOCK_STREAM, 0, 0, socket_connect, 0);
    if (sockfd < 0) perror_exit("connect");

    in1 = out2 = sockfd;
  } else {
    socklen_t len = sizeof(*address);

    if (TT.source_address || TT.port) {
      sockfd = xgetaddrinfo(TT.source_address, TT.port, family, SOCK_STREAM, 0, 0,
        socket_server, &sockfd);
    } else {
      sockfd = xsocket(family ? family : AF_INET, SOCK_STREAM, 0);
    }

    if (sockfd == -1) perror_exit("bind");

    if (listen(sockfd, 5)) error_exit("listen");
    if (!TT.port) {
      getsockname(sockfd, (struct sockaddr *)address, &len);
      printf("%d\n", SWAP_BE16(address->sin_port));
      fflush(stdout);
      // Return immediately if no -p and -Ll has arguments, so wrapper
      // script can use port number.
      if (CFG_TOYBOX_FORK && toys.optc && xfork()) goto cleanup;
    }

    for (;;) {
      child = 0;
      len = sizeof(*address); // gcc's insane optimizer can overwrite this
      in1 = out2 = accept(sockfd, (struct sockaddr *)address, &len);

      if (in1<0) perror_exit("accept");

      // We can't exit this loop or the optimizer's "liveness analysis"
      // combines badly with vfork() to corrupt or local variables
      // (the child's call stack gets trimmed and the next function call
      // stops the variables the parent tries to re-use next loop)
      // So there's a bit of redundancy here

      // We have a connection. Disarm timeout.
      set_alarm(0);

      if (toys.optc) {
        // Do we need a tty?

// TODO nommu, and -t only affects server mode...? Only do -t with optc
//      if (CFG_TOYBOX_FORK && (toys.optflags&FLAG_t))
//        child = forkpty(&fdout, NULL, NULL, NULL);
//      else

        // Do we need to fork and/or redirect for exec?

        if (toys.optflags&FLAG_L) {
          toys.stacktop = 0;
          child = vfork();
        }
        if (child<0) error_msg("vfork failed\n");
        else {
          if (child) {
            close(in1);
            continue;
          }
          dup2(in1, 0);
          dup2(in1, 1);
          if (toys.optflags&FLAG_L) dup2(in1, 2);
          if (in1>2) close(in1);
          xexec(toys.optargs);
        }
      }

      pollinate(in1, in2, out1, out2, TT.idle, TT.quit_delay);
      close(in1);
    }
  }

  // We have a connection. Disarm timeout.
  set_alarm(0);

  pollinate(in1, in2, out1, out2, TT.idle, TT.quit_delay);

cleanup:
  if (CFG_TOYBOX_FREE) {
    close(in1);
    close(sockfd);
  }
}
