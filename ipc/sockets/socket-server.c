#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_types/_socklen_t.h>
#include <sys/_types/_ssize_t.h>
#include <sys/fcntl.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "unix_socket_example"
#define FILE_CHUNK_SIZE 256

void error(char *msg) {
  perror(msg);
  exit(1);
}

void unlink_sock(int _sig) {
  unlink(SOCK_PATH);
  exit(1);
}

int main(int argc, char *argv[]) {
  int sockfd;
  struct sockaddr_un serv_addr, cli_addr;
  int n;

  if (signal(SIGINT, unlink_sock) < 0)
    error("signal handler not registered");

  /* create socket */
  sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockfd < 0)
    error("ERROR opening socket");

  /* fill in socket addres */
  bzero((char *)&serv_addr, sizeof(serv_addr));
  serv_addr.sun_family = AF_UNIX;
  strcpy(serv_addr.sun_path, SOCK_PATH);

  /* bind socket to this address */
  if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    error("ERROR on binding");

  /* read file from client */
  printf("Server ready\n");

  socklen_t len = sizeof(cli_addr);

  int file_size;
  if (recvfrom(sockfd, &file_size, sizeof(file_size), 0,
               (struct sockaddr *)&cli_addr, &len) < 0)
    error("ERROR reading from socket");

  char filename[256];
  bzero(filename, 256);
  if (recvfrom(sockfd, filename, 255, 0, (struct sockaddr *)&cli_addr, &len) <
      0)
    error("ERROR reading from socket");

  char new_filename[512];
  strcpy(new_filename, "new_");
  strcat(new_filename, filename);
  FILE* file = fopen(new_filename, "w");

  if (file == NULL)
    error("unable to create output file");

  int bytes_read = 0;
  char buffer[FILE_CHUNK_SIZE];
  bzero(buffer, FILE_CHUNK_SIZE);
  ssize_t buffer_len;

  printf("\n---- FILE START ----\n\n");

  while (bytes_read < file_size &&
         (buffer_len = recvfrom(sockfd, buffer, FILE_CHUNK_SIZE, 0,
                                (struct sockaddr *)&cli_addr, &len)) >= 0) {
    bytes_read += buffer_len;

    write(STDOUT_FILENO, buffer, buffer_len); // print to console
    write(fileno(file), buffer, buffer_len);
    bzero(buffer, FILE_CHUNK_SIZE);
  }

  printf("\n\n---- FILE END ----\n\n");
  printf("received %d bytes\n", bytes_read);

  fclose(file);
  unlink(SOCK_PATH);

  return 0;
}
