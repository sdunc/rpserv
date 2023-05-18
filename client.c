#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

static void CheckError(int status);

int main(int argc, char* argv[]) {
  /* argv[1] == ip_address */
  /* argv[2] == port */
  /* argv[3] == tcp/udp */
  /* argv[4] == count (OPTIONAL) */
  char* ip_address;
  int port = 5025;
  char protocol[4] = "udp";
  int count = 1;

  if (argc < 4 || argc > 5) {
    printf("Usage: ./client xxx.xxx.xxx.xxx port udp/tcp count=1\n");
    printf("using defaults: ./client 127.0.0.1 5025 udp 1\n");
    ip_address = "127.0.0.1";
  } else {
    /* We have the correct number of arguments. Now try to read 'em */
    /* arg1: ip_address. Is validated in inet_pton() call. */
    ip_address = argv[1];
    /* arg2: port. strtol() read and check for error */
    errno = 0;
    char* read_to = NULL;
    port = strtol(argv[2], &read_to, 10);
    if (errno != 0 || read_to == argv[1] || port < 0) {
      printf("Error, could not read valid port\n");
      return 1;
    }
    /* arg3: tcp/udp [case insensitive] */
    /* read expected 3 chars into protocol buffer as lower */
    /* exit if non alpha chars, validate later when creating socket */
    for (int i = 0; i < 3; i++) {
      if (!isalpha(argv[3][i])) {
          printf("Error, non alpha characters in udp/tcp argument\n");
          return 1;
      }
      protocol[i] = tolower(argv[3][i]);
    }
    protocol[4] = '\0';
    /* protocol contents are validated w/strcmp below. */
  }

  /* If the optional 5th argument was supplied, read that in too */
  if (argc == 5) {
    errno = 0;
    char* read_to = NULL;
    count = strtol(argv[4], &read_to, 10);
    if (errno != 0 || read_to == argv[4] || count < 0) {
      printf("Error, could not read valid count\n");
      return 1;
    }
  }

  //printf("port:\t[%d]\n",port);
  //printf("proto:\t[%s]\n", protocol);
  //printf("ip:\t[%s]\n", ip_address);
  //printf("count:\t[%d]\n", count);

  /* depending on protocol[] string, create the type of socket we want
   * or error if doesnt match either protocol */
  int sid;
  if (strcmp("udp", protocol)==0) {
    sid = socket(PF_INET, SOCK_DGRAM, 0);
  } else if (strcmp("tcp", protocol)==0) {
    sid = socket(PF_INET, SOCK_STREAM, 0);
  } else {
    printf("error, not a valid protocol, try tcp or udp\n");
    return 1;
  }

  /* Create address for server from command line arguments */
  struct sockaddr_in srv;
  memset(&(srv.sin_zero), 0, 8);
  int status = inet_pton(AF_INET, ip_address, &(srv.sin_addr));
  /* Check if the ip_address field was valid */
  CheckError(status);
  srv.sin_family = AF_INET;
  srv.sin_port = htons(port);

  /* Connect to the server address using our socket */
  /* even if we have a datagram socket, connect()'ing allows us to use
   * just send() and recv() instead of sendto() and recvfrom()
   * calls. */
  status = connect(sid, (struct sockaddr*)&srv, sizeof(srv));
  /* Check that we connected. If server is down: connection refused. */
  CheckError(status);

  /* We have connected to the server, let's send them a temperature
   * request count number of times */
  char send_buf[4];
  char recv_buf[7]; // "01.345[6=\0]"
  snprintf(send_buf, sizeof(send_buf) - 1, "%d", count);

  for (int i = count; i > 0; i--) {
    // send ith message
    // this may not send all bytes but we don't care.
    status = send(sid, send_buf, sizeof(send_buf), 0);
    CheckError(status);

    // get ith reply
    status = recv(sid, recv_buf, sizeof(recv_buf), 0);
    CheckError(status);

    recv_buf[6] = '\0';
    printf("%s\n", recv_buf);
    sleep(1);
  }

  close(sid);
  return 0;
}

static void CheckError(int status) {
  if (status < 0) {
    printf("socket error: [%s]\n", strerror(errno));
    exit(1);
  }
}
