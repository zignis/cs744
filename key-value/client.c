#include "common.h"
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_CMD_ARGS 128

typedef struct {
  CommandKind kind;
  char *args[MAX_CMD_ARGS];
  int argc;
} Command;

void free_command(Command *cmd) {
  if (cmd == NULL)
    return;
  for (int i = 0; i < cmd->argc; i++) {
    free(cmd->args[i]);
  }
  free(cmd);
}

int read_word(FILE *fd, char *word, int word_size) {
  int curr;
  int i = 0;
  bzero(word, word_size);

  while (1) {
    curr = getc(fd);
    if (curr == EOF)
      return -1;
    if (curr == ' ' || curr == '\n')
      break;
    if (i < word_size - 1)
      word[i++] = (char)curr;
  }

  return 0;
}

Command *parse_command(FILE *fd, int batch_mode, int *eof_ptr) {
  Command *cmd = malloc(sizeof(Command));
  if (cmd == NULL)
    return NULL;

  char command[256];
  if (read_word(fd, command, 256) == -1) {
    *eof_ptr = 1;
    return NULL;
  };
  CommandKind kind;

  if (strlen(command) == 0) {
    free(cmd);
    return NULL;
  } else if (strcmp(command, "connect") == 0) {
    kind = CONNECT;
  } else if (strcmp(command, "disconnect") == 0) {
    kind = DISCONNECT;
  } else if (strcmp(command, "create") == 0) {
    kind = CREATE;
  } else if (strcmp(command, "update") == 0) {
    kind = UPDATE;
  } else if (strcmp(command, "read") == 0) {
    kind = READ;
  } else if (strcmp(command, "delete") == 0) {
    kind = DELETE;
  } else {
    return NULL; // invalid command
  }

  cmd->kind = kind;
  cmd->argc = 0;

  if (kind != CONNECT && kind != DISCONNECT) {
    char key[128];
    read_word(fd, key, 128);
    cmd->args[cmd->argc++] = strdup(key);

    if (kind == CREATE || kind == UPDATE) {
      // read value size
      char value_size[128];
      read_word(fd, value_size, 128);
      cmd->args[cmd->argc++] = strdup(value_size);

      // read value
      int val_size = atoi(value_size);
      int lookahead_buf = 5;
      char value[val_size + 5];
      bzero(value, val_size + 5);
      fgets(value, val_size + 4, fd);
      cmd->args[cmd->argc++] = strdup(value);
    }
  } else if (kind == CONNECT) {
    char buffer[256];
    bzero(buffer, 256);
    fgets(buffer, 255, fd);
    char *token = strtok(buffer, " ");
    cmd->args[cmd->argc++] = strdup(token);

    while ((token = strtok(NULL, " ")) != NULL) {
      if (cmd->argc >= MAX_CMD_ARGS) {
        free_command(cmd);
        return NULL;
      }
      cmd->args[cmd->argc++] = strdup(token);
    }
  }

  // display command
  if (batch_mode) {
    printf("%s", command);
    for (int i = 0; i < cmd->argc; i++) {
      printf(" %s", cmd->args[i]);
    }
    printf("\n");
  }

  return cmd;
}

void error(char *msg) {
  perror(msg);
  exit(0);
}

int main(int argc, char *argv[]) {
  int sockfd, portno, n;
  int connected = 0;
  int eof_ptr = 0;
  struct sockaddr_in serv_addr;
  struct hostent *server;
  char buffer[256];

  if (argc < 2) {
    fprintf(stderr, "ERROR, operation mode not provided\n");
    exit(1);
  }

  int interactive_mode = (strcmp(argv[1], "interactive") == 0);
  FILE *fd = stdin;

  // batch mode
  if (!interactive_mode) {
    if (argc != 3) {
      fprintf(stderr, "ERROR, file not provided\n");
      exit(1);
    }

    fd = fopen(argv[2], "r");
    if (fd == NULL) {
      fprintf(stderr, "ERROR, cannot open file\n");
      exit(1);
    }
  }

  /* ask user for input */
  while (1) {
    printf("$ ");
    Command *cmd = parse_command(fd, !interactive_mode, &eof_ptr);

    if (eof_ptr == 1)
      break;

    if (cmd == NULL) {
      printf("> unknown command\n");
      continue;
    }

    // send command to server
    if (cmd->kind != CONNECT && cmd->kind != DISCONNECT) {
      if (connected == 0) {
        printf("> not connected to server\n");
        free_command(cmd);
        continue;
      } else {
        printf("\n___SENDING_CMD_____\n");
        int kind = htonl(cmd->kind);
        if (write(sockfd, &kind, sizeof(kind)) < 0) {
          error("error writing to socket");
        }
      }
    }

    if (cmd->kind == CONNECT) {
      if (connected == 1) {
        printf("> already connected to a server\n");
        free_command(cmd);
        continue;
      }

      if (cmd->argc < 2) {
        printf("> invalid args\n");
        free_command(cmd);
        continue;
      }

      /* create socket */
      sockfd = socket(AF_INET, SOCK_STREAM, 0);
      if (sockfd < 0)
        error("ERROR opening socket");

      /* connect to server */
      server = gethostbyname(cmd->args[0]);
      portno = atoi(cmd->args[1]);

      if (server == NULL) {
        fprintf(stderr, "> no such host\n");
        free_command(cmd);
        continue;
      }

      bzero((char *)&serv_addr, sizeof(serv_addr));
      serv_addr.sin_family = AF_INET;
      bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr,
            server->h_length);
      serv_addr.sin_port = htons(portno);

      if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) <
          0) {
        perror("> error connecting\n");
        free_command(cmd);
        continue;
      }

      connected = 1;
      printf("> connected\n");
      free_command(cmd);
      continue;
    } else if (cmd->kind == DISCONNECT) {
      if (connected == 0) {
        printf("> already disconnected\n");
        free_command(cmd);
        continue;
      }

      close(sockfd);
      connected = 0;
      printf("> disconnected\n");
      free_command(cmd);
      continue;
    } else if (cmd->kind == CREATE || cmd->kind == UPDATE) {
      if (cmd->argc < 3) {
        printf("> invalid args\n");
        free_command(cmd);
        continue;
      }

      int key = htonl(atoi(cmd->args[0]));
      int value_size = atoi(cmd->args[1]);
      char *value = cmd->args[2];

      // write key
      if (write(sockfd, &key, sizeof(key)) < 0) {
        error("error writing to socket");
      }

      // write value size
      int net_value_size = htonl(value_size);
      if (write(sockfd, &net_value_size, sizeof(net_value_size)) < 0) {
        error("error writing to socket");
      }

      // write value
      int num_chunks = ceil((float)value_size / MAX_CHUNK_SIZE);

      for (int i = 0; i < num_chunks; i++) {
        char buffer[MAX_CHUNK_SIZE + 1];
        bzero(buffer, sizeof(buffer));
        int start = i * MAX_CHUNK_SIZE;
        int length = (value_size - start > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE
                                                           : value_size - start;
        strncpy(buffer, value + start, length);
        buffer[length] = '\0';

        if (write(sockfd, buffer, length) < 0) {
          error("error writing to socket");
        }
      }
    } else if (cmd->kind == READ || cmd->kind == DELETE) {
      if (cmd->argc != 1) {
        printf("> missing key\n");
        free_command(cmd);
        continue;
      }

      int key = htonl(atoi(cmd->args[0]));
      if (write(sockfd, &key, sizeof(key)) < 0)
        error("error writing to socket");
    } else {
      printf("> invalid command\n");
      free_command(cmd);
      continue;
    }

    free_command(cmd);

    /* read reply from server */
    bzero(buffer, 256);
    if (read(sockfd, buffer, 255) < 0)
      error("error reading from socket");
    printf("> %s\n", buffer);
  }

  if (connected == 1) {
    close(sockfd);
  }

  if (!interactive_mode && fd != NULL) {
    fclose(fd);
  }

  return 0;
}
