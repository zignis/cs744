#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_INPUT_SIZE 1024
#define MAX_TOKEN_SIZE 64
#define MAX_NUM_TOKENS 64
#define MAX_CHILD_PROCS 64

/* Splits the string by space and returns the array of tokens
 *
 */
char **tokenize(char *line) {
  char **tokens = (char **)malloc(MAX_NUM_TOKENS * sizeof(char *));
  char *token = (char *)malloc(MAX_TOKEN_SIZE * sizeof(char));
  int i, tokenIndex = 0, tokenNo = 0;

  for (i = 0; i < strlen(line); i++) {
    char readChar = line[i];

    if (readChar == ' ' || readChar == '\n' || readChar == '\t') {
      token[tokenIndex] = '\0';
      if (tokenIndex != 0) {
        tokens[tokenNo] = (char *)malloc(MAX_TOKEN_SIZE * sizeof(char));
        strcpy(tokens[tokenNo++], token);
        tokenIndex = 0;
      }
    } else {
      token[tokenIndex++] = readChar;
    }
  }

  free(token);
  tokens[tokenNo] = NULL;
  return tokens;
}

int find_and_remove_last_token(char **tokens, const char *to_find) {
  int idx = 0;
  char *last_token = tokens[0];

  while (1) {
    if (tokens[idx] != NULL) {
      last_token = tokens[idx];
      idx++;
    } else {
      break;
    }
  }

  if (strcmp(last_token, to_find) == 0) {
    free(tokens[idx - 1]);
    tokens[idx - 1] = NULL;
    return 1;
  }

  return 0;
}

void split_chunks_at(char **tokens, char ***out, int *exec_mode) {
  char *serial_delim = "&&";
  char *parallel_delim = "&&&";
  char **cur = (char **)malloc(MAX_NUM_TOKENS * sizeof(char *));
  int idx = 0, out_idx = 0, cur_idx = 0;

  while (1) {
    if (tokens[idx] != NULL && *exec_mode == 0) {
      if (strcmp(tokens[idx], serial_delim) == 0)
        *exec_mode = 1;
      else if (strcmp(tokens[idx], parallel_delim) == 0)
        *exec_mode = 2;
    }

    if (tokens[idx] != NULL && strcmp(tokens[idx], serial_delim) != 0 &&
        strcmp(tokens[idx], parallel_delim) != 0) {
      cur[cur_idx++] = tokens[idx];
    } else {
      // delimiter encountered

      if (cur_idx > 0) {
        cur[cur_idx] = NULL;
        out[out_idx++] = cur;
      } else {
        free(cur);
      }

      if (!tokens[idx])
        break;

      free(tokens[idx]); // dealloc the delimiter
      cur = (char **)malloc(MAX_NUM_TOKENS * sizeof(char *));
      cur_idx = 0;
    }

    idx++;
  }

  out[out_idx] = NULL;
}

void free_tokens(char **tokens) {
  for (int i = 0; tokens[i] != NULL; i++) {
    free(tokens[i]);
  }

  free(tokens);
}

void free_tokens_and_exit(char **tokens, int exit_code) {
  free_tokens(tokens);
  exit(exit_code);
}

static void catch_sigint(int _sig) {}

int main(int argc, char *argv[]) {
  // ignore SIGINTs for shell process.
  //
  // although child processes inherit parent's signal handlers, when
  // a child calls exec syscall the handlers are reset to their defaults
  // inside the child
  if (signal(SIGINT, catch_sigint) == SIG_ERR) {
    printf("failed to install signal handler\n");
    exit(EXIT_FAILURE);
  }

  char line[MAX_INPUT_SIZE];
  char **tokens;
  char cwd[256];
  int bg_children[64] = {};
  int child_count = 0;
  int fg_procs[64] = {};
  int fg_proc_count = 0;

  while (1) {
    /* BEGIN: TAKING INPUT */
    bzero(line, sizeof(line));

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      printf("error getting cwd\n");
      free_tokens_and_exit(tokens, EXIT_FAILURE);
    }

    printf("%s $ ", cwd);
    scanf("%[^\n]", line);
    getchar();

    /* END: TAKING INPUT */

    line[strlen(line)] = '\n'; // terminate with new line
    tokens = tokenize(line);

    if (tokens[0] != NULL) {
      // reap dead background children
      for (int i = 0; i < child_count; i++) {
        int child_pid = bg_children[i];
        int exit_status;

        // if WNOHANG is specified and there are no stopped or exited children,
        // 0 is returned
        if (waitpid(child_pid, &exit_status, WNOHANG) != 0) {
          printf("Shell: Background process finished with EXITSTATUS:%d\n",
                 exit_status);

          // remove pid from array
          for (int j = i; j < child_count - 1; j++) {
            // shift
            bg_children[j] = bg_children[j + 1];
          }

          child_count--;
        }
      }

      char ***commands =
          malloc(32 * sizeof(char **)); // max 32 commands per line
      int exec_mode = 0; // 0=default, 1=serial, 2=parallel
      split_chunks_at(tokens, commands, &exec_mode);

      for (int i = 0; commands[i]; i++) {
        char **cmd = commands[i];

        if (cmd[0] == NULL)
          continue;

        if (strcmp(cmd[0], "cd") == 0) {
          if (chdir(cmd[1]) < 0) {
            printf("invalid directory\n");
          }
        } else if (strcmp(cmd[0], "exit") == 0) {
          int exit_code = 0;

          // kill all background child processes
          for (int i = 0; i < child_count; i++) {
            int child_pid = bg_children[i];
            int status = kill(child_pid, SIGKILL);

            if (status != 0) {
              printf("failed to kill child with pid: %d\n", child_pid);
              exit_code = EXIT_FAILURE; // exit with errors
            }
          }

          free(commands);
          exit(exit_code);
        } else {
          int is_bg_proc = find_and_remove_last_token(cmd, "&");
          pid_t pid = fork();

          if (pid < 0) {
            printf("error forking\n");
            free(commands);
            exit(EXIT_FAILURE);
          } else if (pid == 0) {
            // move background processes to their own group
            if (is_bg_proc) {
              // move each process to its own process group
              if (setpgid(0, 0) != 0) {
                printf("failed to set process pgid");
              }
            }

            if (execvp(cmd[0], cmd) < 0) {
              printf("command not found\n");
            }
          } else {
            if (is_bg_proc) {
              bg_children[child_count++] = pid;
            } else if (exec_mode == 2) {
              fg_procs[fg_proc_count++] = pid;
            } else {
              int status;
              waitpid(pid, &status, 0);

              if (status != 0) {
                printf("EXITSTATUS: %d\n", status);
              }
            }
          }
        }
      }

      // wait for all parallel processes
      if (exec_mode == 2) {
        for (int i = 0; i < fg_proc_count; i++) {
          int status;
          waitpid(fg_procs[i], &status, 0);

          if (status != 0) {
            printf("EXITSTATUS: %d\n", status);
          }
        }
      }

      free(commands);
    } else {
      free_tokens(tokens);
    }
  }

  return 0;
}
