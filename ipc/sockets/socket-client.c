#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_types/_ssize_t.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "unix_socket_example"
#define FILE_CHUNK_SIZE 256

void error(char *msg) {
  perror(msg);
  exit(0);
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    printf("missing filename argument\n");
    exit(1);
  }

  char *filename = argv[1];
  int sockfd, portno;
  struct sockaddr_un serv_addr;

  /* create socket, get sockfd handle */
  sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockfd < 0)
    error("ERROR opening socket");

  /* fill in server address */
  bzero((char *)&serv_addr, sizeof(serv_addr));
  serv_addr.sun_family = AF_UNIX;
  strcpy(serv_addr.sun_path, SOCK_PATH);

  /* handle file */
  int file = open(filename, O_RDONLY);
  if (file < 0)
    error("unknown file");

  struct stat file_stats;
  if (fstat(file, &file_stats) < 0)
    error("failed to read file");

  /* send file size to server */
  int file_size = file_stats.st_size;
  printf("Sending file size (%d B)...\n", file_size);
  if (sendto(sockfd, &file_size, sizeof(file_size), 0,
             (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    error("ERROR writing to socket");

  printf("Sending file name (%s)...\n", filename);
  if (sendto(sockfd, filename, strlen(filename), 0,
             (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    error("ERROR writing to socket");

  /* send file chunks */
  int chunks_sent = 0;
  char buffer[FILE_CHUNK_SIZE];
  ssize_t buffer_len;
  bzero(buffer, FILE_CHUNK_SIZE);

  while ((buffer_len = read(file, buffer, FILE_CHUNK_SIZE)) > 0) {
    if (sendto(sockfd, buffer, buffer_len, 0, (struct sockaddr *)&serv_addr,
               sizeof(serv_addr)) < 0)
      error("ERROR sending chunk");

    chunks_sent++;
  }

  printf("file sent (%d chunks)\n", chunks_sent);

  close(file);
  close(sockfd);

  return 0;
}
