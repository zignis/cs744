#include "common.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define HASHMAP_SIZE 512
#define Q_BUFFER_SIZE 64
#define WORKER_POOL_SIZE 16

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t clients_available = PTHREAD_COND_INITIALIZER;

int clients[Q_BUFFER_SIZE] = {0};
pthread_t workers[WORKER_POOL_SIZE] = {0};
int workers_len = 0;

typedef struct Pair {
  int key;
  char *value;
  struct Pair *next;
} Pair;

Pair *kv_store[HASHMAP_SIZE];

int hash_key(int key) { return key % HASHMAP_SIZE; }
int is_valid_pair(Pair *p) { return strlen(p->value) != 0; }

char *insert(int key, char *value) {
  int index = hash_key(key);

  pthread_mutex_lock(&lock);
  // check for duplicates
  Pair *kv_pair = kv_store[index];

  while (kv_pair != NULL) {
    if (kv_pair->key == key) {
      pthread_mutex_unlock(&lock);
      return "key already present";
    }
    kv_pair = kv_pair->next;
  };

  // allocate new pair
  Pair *pair = (Pair *)malloc(sizeof(Pair));
  char *copied_value = strdup(value);

  if (copied_value == NULL) {
    pthread_mutex_unlock(&lock);
    return "internal error";
  }

  pair->key = key;
  pair->value = copied_value;
  pair->next = kv_store[index];
  kv_store[index] = pair;

  pthread_mutex_unlock(&lock);

  return NULL;
}

char *update(int key, char *new_value) {
  int index = hash_key(key);
  pthread_mutex_lock(&lock);
  Pair *pair = kv_store[index];

  while (pair) {
    if (pair->key == key) {
      char *copied_value = strdup(new_value);
      if (copied_value == NULL) {
        pthread_mutex_unlock(&lock);
        return "internal error";
      }

      free(pair->value);
      pair->value = copied_value;
      pthread_mutex_unlock(&lock);

      return NULL;
    }

    pair = pair->next;
  }

  pthread_mutex_unlock(&lock);
  return "key not found";
}

Pair *get(int key) {
  int index = hash_key(key);
  pthread_mutex_lock(&lock);
  Pair *pair = kv_store[index];

  while (pair) {
    if (pair->key == key)
      break;
    pair = pair->next;
  }

  pthread_mutex_unlock(&lock);
  return pair;
}

char *delete_key(int key) {
  int index = hash_key(key);
  pthread_mutex_lock(&lock);
  Pair *pair = kv_store[index];
  Pair *prev = NULL;

  while (pair != NULL) {
    if (pair->key == key) {
      if (prev) {
        prev->next = pair->next;
      } else {
        kv_store[index] = pair->next;
      }

      free(pair->value);
      free(pair);

      pthread_mutex_unlock(&lock);
      return NULL;
    }

    prev = pair;
    pair = pair->next;
  }

  pthread_mutex_unlock(&lock);
  return "key not found";
}

void error(char *msg) {
  perror(msg);
  exit(1);
}

int write_sock(int sockfd, char *msg) {
  if (write(sockfd, msg, strlen(msg)) < 0) {
    perror("ERROR writing to socket");
    return -1;
  }
  return 0;
}

void *handle_client(void *arg) {
  pthread_t tid = pthread_self();

  while (1) {
    pthread_mutex_lock(&q_lock);
    pthread_cond_wait(&clients_available, &q_lock);

    int sockfd = -1;
    for (int i = 0; i < Q_BUFFER_SIZE; i++) {
      if (clients[i] != 0) {
        sockfd = clients[i];
        clients[i] = 0;
        break;
      }
    }

    pthread_mutex_unlock(&q_lock);

    if (sockfd == -1)
      continue;

    printf("[thread][%d]:: serving client %d\n", tid, sockfd);

    while (1) {
      // read command
      int cmd_kind;
      int x = read(sockfd, &cmd_kind, sizeof(cmd_kind));
      if (x < 0) {
        perror("ERROR reading from socket");
        break;
      } else if (x == 0) {
        break; // client disconnected
      }
      cmd_kind = ntohl(cmd_kind);

      printf("[%d::%d]:: handling command %d\n", tid, sockfd, cmd_kind);

      // read key
      int key;
      if (read(sockfd, &key, sizeof(key)) < 0) {
        perror("ERROR reading from socket");
        break;
      }
      key = ntohl(key);

      if (cmd_kind == CREATE || cmd_kind == UPDATE) {
        // read value size
        int value_size;
        if (read(sockfd, &value_size, sizeof(value_size)) < 0) {
          perror("ERROR reading from socket");
          break;
        }
        value_size = ntohl(value_size);

        // assemble value
        char *value = malloc(value_size + 1);
        if (value == NULL) {
          perror("malloc failed");
          break;
        }

        int received = 0;
        while (received < value_size) {
          int bytes_to_read = (value_size - received > MAX_CHUNK_SIZE)
                                  ? MAX_CHUNK_SIZE
                                  : (value_size - received);

          int x;
          if ((x = read(sockfd, value + received, bytes_to_read)) <= 0) {
            perror("ERROR reading chunk");
            free(value);
            break;
          }

          received += x;
        }

        value[value_size] = '\0';

        char *err_msg;
        if ((err_msg = cmd_kind == CREATE ? insert(key, value)
                                          : update(key, value)) != NULL) {
          if (write_sock(sockfd, err_msg) == -1)
            break;
        } else {
          if (write_sock(sockfd, "OK") == -1)
            break;
        }
      } else if (cmd_kind == READ) {
        Pair *pair;
        if ((pair = get(key)) == NULL) {
          if (write_sock(sockfd, "key not found") == -1)
            break;
        } else {
          if (write_sock(sockfd, pair->value) == -1)
            break;
        }
      } else if (cmd_kind == DELETE) {
        char *err_msg;
        if ((err_msg = delete_key(key)) != NULL) {
          if (write_sock(sockfd, err_msg) == -1)
            break;
        } else {
          if (write_sock(sockfd, "OK") == -1)
            break;
        }
      } else {
        if (write_sock(sockfd, "unknown command") == -1)
          break;
      }
    }

    printf("[thread][%d]:: disconnecting client %d\n", tid, sockfd);
    close(sockfd);
    sockfd = -1;
  };

  return NULL;
}

void add_client(int sockfd) {
  pthread_mutex_lock(&q_lock);

  for (int i = 0; i < Q_BUFFER_SIZE; i++) {
    if (clients[i] == 0) {
      clients[i] = sockfd;
      pthread_cond_signal(&clients_available);
      break;
    }
  }

  pthread_mutex_unlock(&q_lock);
}

int main(int argc, char *argv[]) {
  int sockfd, newsockfd, portno, clilen;
  struct sockaddr_in serv_addr, cli_addr;
  int n1, n2;
  if (argc < 3) {
    fprintf(stderr, "ERROR, no host/port provided\n");
    exit(1);
  }

  /* create socket */

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0)
    error("ERROR opening socket");

  /* fill in host and port number to listen on.
   */

  bzero((char *)&serv_addr, sizeof(serv_addr));
  char *host = argv[1];
  portno = atoi(argv[2]);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(portno);

  if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
    // hostname resolution
    struct hostent *server = gethostbyname(host);

    if (server == NULL) {
      fprintf(stderr, "invalid host\n");
      exit(1);
    }

    bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr,
          server->h_length);
  }

  /* bind socket to this port number on this machine */

  if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    error("ERROR on binding");

  /* listen for incoming connection requests */

  listen(sockfd, 5);
  clilen = sizeof(cli_addr);

  /* create worker threads */
  for (int i = 0; i < WORKER_POOL_SIZE; i++) {
    pthread_t thread_id;
    pthread_create(&thread_id, NULL, handle_client, NULL);
    workers[workers_len++] = thread_id;
  }

  /* accept a new request, create a newsockfd */

  while (1) {
    newsockfd =
        accept(sockfd, (struct sockaddr *)&cli_addr, (socklen_t *)&clilen);
    if (newsockfd < 0)
      error("ERROR on accept");
    add_client(newsockfd);
  }

  close(sockfd);

  return 0;
}
