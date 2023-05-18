#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

/* n_of tcp accept()'s   */
static int n_tcp_connections = 0;
/* max n_of accept()'s   */
static const int kMaxTCP = 8;
/* +2 listeners @[0]&[1] */
static const int kMaxConnections = 2 + kMaxTCP;
/* udp listener at index */
static const int kIndexUDP = 0;
/* tcp listener at index */
static const int kIndexTCP = 1;
/* How many pending tcp  */
static const int kBacklog = 4;

static void ReadAndDiscardTCP(int sid);
static void SendTemperatureTCP(int sid);
static void CheckError(int status);
static float ReadPiTemp(void);

int main(int argc, char* argv[]) {
  int port;    /* argv[1], which port to listen on for tcp/udp. */
  int ret_val; /* Store return values here to error check. */

  /* SIGPIPE when a tcp socket gets closed; do this to not die. */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    fprintf(stderr, "Error, cannot ignore SIGPIPE\n");
    return 1;
  }

  if (argc < 2) {
    fprintf(stderr, "Error, missing port.\nUsage: ./server nnnn\n");
    return 1;
  }

  /* Attempt to read command line argument: port */
  errno = 0;
  char const* read_from = argv[1];
  char* read_to = NULL;
  port = strtol(read_from, &read_to, 10);
  /* n too large? n too small? didn't read chars? n negative? Exit. */
  if (errno != 0 || read_from == read_to) {
    fprintf(stderr, "Error, could not read port to listen on.\n");
    return 1;
  } else if (port < 0) {
    fprintf(stderr, "Error, port should be positive.\n");
    return 1;
  }

  /* Initialize our server address by IP and port */
  struct sockaddr_in our_address;
  our_address.sin_family = AF_INET;
  our_address.sin_port = htons(port);
  our_address.sin_addr.s_addr = INADDR_ANY;

  /* Create an array of struct pollfd: watched_fds
   * poll() will wake up when events in pollfd.events are triggered
   * first two elements are listeners for udp/tcp sockets
   * POLLIN on udp listener? Reply right away.
   * POLLIN on tcp listener? accept() if space in array of pollfd's */
  struct pollfd watched_fds[kMaxConnections];

  /* valid_fds tracks whether each connection is active, when looking
   * to place a new tcp socket file descriptor into watched_fds we
   * know we have space when n_tcp_connections < kMaxTCP. We find
   * where to place the new tcp socket descriptor by walking valid_fds
   * and finding an invalid address where we can overwrite an old
   * connection. on accept() we mark the index true, on error/hangup
   * we mark the index as false and decrement n_tcp_connections */
  bool valid_fds[kMaxConnections];
  /* Mark the listener sockets as valid */
  valid_fds[kIndexUDP] = true;
  valid_fds[kIndexTCP] = true;
  /* Mark all empty tcp connection slots as invalid */
  for (int i = 2; i < kMaxConnections; i++) { valid_fds[i] = false; }

  /* Initialize udp listener [0] */
  /* Get a socket descriptor to listen for udp */
  int sid_udp = socket(PF_INET, SOCK_DGRAM, 0);
  CheckError(sid_udp);
  /* Bind socket descriptor to our server address */
  ret_val = bind(sid_udp,
                (struct sockaddr*)&our_address,
                sizeof(our_address));
  CheckError(ret_val);
  /* watched_fds[0] is udp listener, listen for POLLIN */
  watched_fds[kIndexUDP].fd = sid_udp;
  watched_fds[kIndexUDP].events = POLLIN;

  /* Initialize the tcp listener [1] */
  /* Get a socket descriptor to listen for tcp */
  int sid_tcp = socket(PF_INET, SOCK_STREAM, 0);
  CheckError(sid_tcp);
  ret_val = bind(sid_tcp,
                    (struct sockaddr*)&our_address,
                    sizeof(our_address));
  CheckError(ret_val);
  /* listen on the tcp socket -- udp sockets do not listen */
  ret_val = listen(sid_tcp, kBacklog);
  CheckError(ret_val);
  /* watched_fds[1] is tcp listener, listen for POLLIN */
  watched_fds[kIndexTCP].fd = sid_tcp;
  watched_fds[kIndexTCP].events = POLLIN;
  /* watched_fds now configured with udp/tcp listeners */

  /* save the address of the last udp packet into udp_return_addr then
   * we can sendto() this address. */
  struct sockaddr_in udp_return_addr;
  udp_return_addr.sin_family = AF_INET;
  udp_return_addr.sin_port = htons(port);
  udp_return_addr.sin_addr.s_addr = INADDR_ANY;

  /* store address of the last tcp connection into tcp_return_addr */
  struct sockaddr_in tcp_return_addr;
  socklen_t tcp_return_addr_size;

  while (1) {
    /* poll() until one of our file descriptors is ready. */
    /* 2 + n_tcp_connections is number of fds we poll */
    poll(watched_fds, 2 + n_tcp_connections, -1);

    /* poll() awoke! */

    /* Server has awoken from poll(). Gameplan:
       1) UDP socket listener: Reply. Ignore errors.
       2) For each TCP connection: Check for errors.
          If error: Mark index in valid_fds false (invalid).
                    close() connection.
          No error: Reply.
       3) TCP socket listener: If space in watched_fds: accept()
          walk valid_fds[] and overwrite first invalid (eases defrag)
          increment n_tcp_connections for accept(). Reply.
       4) Defragment watched_fds[].
          All connections which were closed in (2) and marked invalid
          should be eliminated from watched_fds. watched_fds[] should
          contiguously store n_tcp_connections + 2 struct pollfd's
       5) Slumber in Poll()'s arms. */

    /* 1) UDP Listener: Reply, catch errors. */
    /* UDP(POLLIN) we want to reply right away */
    if (watched_fds[kIndexUDP].revents & POLLIN) {
      printf("UDP(POLLIN):\t");

      char recv_buf[4];
      char send_buf[7]; // automatic (stack) not gaurenteed 0
      socklen_t addr_len;

      recvfrom(watched_fds[kIndexUDP].fd, recv_buf,
                   sizeof(recv_buf), 0,
                   (struct sockaddr *)&udp_return_addr,
                   &addr_len);

      recv_buf[3] = '\0';
      printf("recv(%s)\n", recv_buf);

      // read file: /sys/class/thermal/thermal_zone0/temp
      // send back right away and go back to sleep
      float temp_C = ReadPiTemp();
      snprintf(send_buf, sizeof(send_buf), "%.3f", temp_C);

      sendto(watched_fds[kIndexUDP].fd, send_buf,
                 sizeof(send_buf), 0,
                 (struct sockaddr*)&udp_return_addr,
                 sizeof(udp_return_addr));
    }
    /* UDP Errors. */
    if (watched_fds[kIndexUDP].revents & POLLERR) {
      fprintf(stderr, "UDP(POLLERR)\n");
    }
    if (watched_fds[kIndexUDP].revents & POLLHUP) {
      fprintf(stderr, "UDP(POLLHUP)\n");
    }
    if (watched_fds[kIndexUDP].revents & POLLNVAL) {
      fprintf(stderr, "UDP(POLLNVAL)\n");
    }

    /* 2) TCP connections: check for errors, mark invalid or reply */
    /* For each tcp connection: close if errors. At this stage all
       connections are contiguous in watched_fds[2...9] Keep track of
       how many we close. After the for() loop we subtract this number
       of closed connections from our total, n_tcp_connections */
    int n_closed_connections = 0;
    for (int idx = 2; idx < 2 + n_tcp_connections; idx++) {
      if (watched_fds[idx].revents & POLLERR) {
        fprintf(stderr, "TCP[%d]\tPOLLERR\n", idx);
        valid_fds[idx] = false;
      }
      if (watched_fds[idx].revents & POLLHUP) {
        fprintf(stderr, "TCP[%d]\tPOLLHUP)\n", idx);
        valid_fds[idx] = false;
      }
      if (watched_fds[idx].revents & POLLNVAL) {
        fprintf(stderr, "TCP[%d]\tPOLLNVAL\n", idx);
        valid_fds[idx] = false;
      }
      /* Error on this file descriptor? close() it. */
      if (valid_fds[idx] == false) {
        fprintf(stderr, "error on %d, closing\n", idx);
        n_closed_connections++;
        close(watched_fds[idx].fd);
      } else {
        /* connection is valid and has no errors, is it talking? */
        if (watched_fds[idx].revents & POLLIN) {
          ReadAndDiscardTCP(watched_fds[idx].fd);
          SendTemperatureTCP(watched_fds[idx].fd);
        }
      }
    }
    n_tcp_connections -= n_closed_connections;

    /* 3) TCP Listener: accept() tcp connection if we have space. */
    if (watched_fds[kIndexTCP].revents & POLLIN) {
      /* new tcp connection, check if we can accept() */
      if (n_tcp_connections < kMaxTCP) {
        printf("accepting tcp conneciton\n");
        int tcp_socket = accept(sid_tcp,
                                (struct sockaddr*)&tcp_return_addr,
                                &tcp_return_addr_size);
        CheckError(tcp_socket);

        /* Find a place to store this new tcp socket descriptor: walk
         * valid_fd and overwrite first false index */
        for (int i = 2; i < 2 + kMaxTCP; i++) {
          if (!valid_fds[i]) {
            watched_fds[i].fd = tcp_socket;
            watched_fds[i].events = POLLIN;
            n_tcp_connections++;
            valid_fds[i] = true;
            break;
          }
        }

        /* Send a reply over this new socket */
        ReadAndDiscardTCP(tcp_socket);
        SendTemperatureTCP(tcp_socket);
      } else {
        /* Else: don't add connection, does this wake us up again? */
        printf("no space to store another connection\n");
      }
    }
    if (watched_fds[kIndexTCP].revents & POLLERR) {
      fprintf(stderr, "TCP(POLLERR)\n");
    }
    if (watched_fds[kIndexTCP].revents & POLLHUP) {
      fprintf(stderr, "TCP(POLLHUP)\n");
    }
    if (watched_fds[kIndexTCP].revents & POLLNVAL) {
      fprintf(stderr, "TCP(POLLNVAL)\n");
    }

    /* 4) Defragment watched_fds. We want a contiguous array of 2 +
       n_tcp_connections struct pollfd's. Example: if we have 2 TCP
       connections stored in watched_fds[2] and watched_fds[3], and we
       close watched_fds[2], we need to move watched_fds[3] into slot
       [2] so that poll() works */
    for (int lh_idx = 2; lh_idx < 2 + n_tcp_connections; lh_idx++) {
      if (!valid_fds[lh_idx]) {
        for (int rh_idx = lh_idx + 1; rh_idx < kMaxConnections; rh_idx++) {
          if (valid_fds[rh_idx]) {
            /* We looked ahead from lh_idx and found valid data at
             * rh_idx, move the data at rh_idx into [lh_idx] and mark
             * the file descriptor at rh_idx as invalid (it was just
             * moved) */
            valid_fds[lh_idx] = true;
            valid_fds[rh_idx] = false;
            watched_fds[lh_idx].fd = watched_fds[rh_idx].fd;
            printf("moved fd from %d to %d\n", rh_idx, lh_idx);
            break;
          }
        }
      }
    }
    /* 5) Now slumber in poll() until we have more alerts */
  }
  return 0;  /* Never reached. */
}

static void CheckError(int ret_val) {
  if (ret_val < 0) {
    printf("socket error: [%s]\n", strerror(errno));
    exit(1);
  }
}

/* Handle POLLIN, we don't really care about what message we got over
 * TCP, just read it into a small buffer on the stack and move on to
 * replying. */
static void ReadAndDiscardTCP(int sid) {
  char buf[4];
  recv(sid, &buf, sizeof(buf), 0);
  buf[3] = '\0';
}

static void SendTemperatureTCP(int sid) {
  // read file on disk and reply
  char send_buf[7];

  snprintf(send_buf, sizeof(send_buf), "%.3f", ReadPiTemp());
  //dprintf(sid, "%.3f", ReadPiTemp());

  int rem = sizeof(send_buf);
  send(sid, send_buf, rem, 0);
}

static float ReadPiTemp(void) {
  FILE* fp;
  float temp_mC = -1.0;
  fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
  if (fp == NULL) {
    fprintf(stderr, "Error: fopen() failed to read Pi Temp\n");
    return -1.0;
  }
  /* We are able to read the temperature: */
  fscanf(fp, "%f", &temp_mC);
  fclose(fp);
  return temp_mC / 1000;
}
