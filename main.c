#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS 64
#define MAX_CMDS 16
typedef struct {
  char *argv[MAX_ARGS];
  int argc;
  char *in_file;
  char *outfile;
  int append;
} Command;

int tokenize(char *line, char **argv, int max_args) {
  int argc = 0;
  char *p = line;

  while (*p != '\0' && argc < max_args - 1) {
    while (*p == ' ' || *p == '\t') {
      p++;
    }

    if (*p == '\0') {
      break;
    }

    argv[argc++] = p;

    while (*p != '\0' && *p != '\t' && *p != ' ') {
      p++;
    }

    if (*p != '\0') {
      *p = '\0';
      p++;
    }
  }

  argv[argc] = NULL;
  return argc;
}

int parse_command(char **tokens, int ntokens, Command *cmd) {
  cmd->argc = 0;
  cmd->in_file = NULL;
  cmd->outfile = NULL;
  cmd->append = 0;
  for (int i = 0; i < ntokens; i++) {
    if (strcmp(tokens[i], "<") == 0 || strcmp(tokens[i], ">") == 0 ||
        strcmp(tokens[i], ">>") == 0) {
      if (i + 1 >= ntokens) {
        fprintf(stderr, "syntax error\n");
        return -1;
      } else {
        if (strcmp(tokens[i], "<") == 0) {
          cmd->in_file = tokens[++i];
        } else {
          if (strcmp(tokens[i], ">") == 0) {
            cmd->append = 0;
          } else if (strcmp(tokens[i], ">>") == 0) {
            cmd->append = 1;
          }
          cmd->outfile = tokens[++i];
        }
      }
    } else if (cmd->argc < MAX_ARGS - 1) {
      cmd->argv[cmd->argc++] = tokens[i];
    }
  }
  cmd->argv[cmd->argc] = NULL;
  return 0;
}

void apply_redirection(const Command *cmd) {
  if (cmd->in_file != NULL) {
    int fd = open(cmd->in_file, O_RDONLY);
    if (fd < 0) {
      perror(cmd->in_file);
      _exit(1);
    }
    if (dup2(fd, STDIN_FILENO) < 0) {
      perror("dup2");
      close(fd);
      _exit(1);
    }
    close(fd);
  }
  if (cmd->outfile != NULL) {
    int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
    int fd = open(cmd->outfile, flags, 0644);
    if (fd < 0) {
      perror(cmd->outfile);
      _exit(1);
    }
    if (dup2(fd, STDOUT_FILENO) < 0) {
      perror("dup2");
      close(fd);
      _exit(1);
    }
    close(fd);
  }
}

void execute_pipeline(const Command *cmds, int ncmds) {
  pid_t pids[MAX_CMDS] = {0};
  int prev_fd = -1;
  for (int i = 0; i < ncmds; i++) {
    int pipefd[2] = {-1, -1};
    if (i < ncmds - 1 && pipe(pipefd) == -1) {
      perror("pipe");
      if (prev_fd != -1)
        close(prev_fd);
      for (int j = 0; j < i; j++) {
        while (waitpid(pids[j], NULL, 0) == -1 && errno == EINTR) {
        }
      }
      return;
    }
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      if (prev_fd != -1) {
        close(prev_fd);
      }
      if (i < ncmds - 1) {
        close(pipefd[0]);
        close(pipefd[1]);
      }
      for (int j = 0; j < i; j++) {
        while (waitpid(pids[j], NULL, 0) == -1 && errno == EINTR) {
        }
      }
      return;
    }
    if (pid == 0) {
      if (prev_fd != -1) {
        if (dup2(prev_fd, STDIN_FILENO) < 0) {
          perror("dup2");
          _exit(1);
        }
        close(prev_fd);
      }
      if (i < ncmds - 1) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
          perror("dup2");
          _exit(1);
        }
        close(pipefd[1]);
      }
      apply_redirection(&cmds[i]);
      struct sigaction sa = {0};
      sa.sa_handler = SIG_DFL;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = 0;
      sigaction(SIGINT, &sa, NULL);
      execvp(cmds[i].argv[0], cmds[i].argv);
      perror(cmds[i].argv[0]);
      _exit(127);
    } else if (pid > 0) {
      pids[i] = pid;
      if (prev_fd != -1) {
        close(prev_fd);
      }
      if (i < ncmds - 1) {
        close(pipefd[1]);
        prev_fd = pipefd[0];
      }
    }
  }
  for (int i = 0; i < ncmds; i++) {
    int status;
    while (waitpid(pids[i], &status, 0) == -1 && errno == EINTR) {
    }
  }
}
int parse_pipeline(char **tokens, int ntokens, Command *cmds, int max_cmds) {
  int start = 0;
  int ncmds = 0;

  for (int i = 0; i <= ntokens; i++) {
    if (i == ntokens || strcmp(tokens[i], "|") == 0) {
      int len = i - start;
      if (len == 0) {
        fprintf(stderr, "syntax error\n");
        return -1;
      }
      if (ncmds >= max_cmds) {
        fprintf(stderr, "too many commands in pipeline\n");
        return -1;
      }
      if (parse_command(tokens + start, len, &cmds[ncmds]) != 0 ||
          cmds[ncmds].argc == 0) {
        return -1;
      }
      ncmds++;
      start = i + 1;
    }
  }
  return ncmds;
}

void handle_sigint(int sig) {
  (void)sig;
  ssize_t n = write(STDOUT_FILENO, "\n", 1);
  (void)n;
}

int main(void) {
  char str[200];
  char *argv[MAX_ARGS];

  struct sigaction sa = {0};

  sa.sa_handler = handle_sigint;
  sigemptyset(&sa.sa_mask);

  sa.sa_flags = 0;

  sigaction(SIGINT, &sa, NULL);

  while (1) {
    printf("mysh> ");
    fflush(stdout);
    errno = 0;
    if (fgets(str, sizeof(str), stdin) == NULL) {
      if (errno == EINTR) {
        clearerr(stdin);
        continue;
      } else {
        puts("end of file");
        return 0;
      }
    }
    size_t len = strlen(str);
    if (len > 0 && str[len - 1] == '\n') {
      str[len - 1] = '\0';
    }
    int argc = tokenize(str, argv, MAX_ARGS);
    if (argc == 0) {
      continue;
    }
    Command cmds[MAX_CMDS];
    int ncmds = parse_pipeline(argv, argc, cmds, MAX_CMDS);
    if (ncmds <= 0) {
      continue;
    }
    if (ncmds == 1) {
      if (strcmp(cmds[0].argv[0], "exit") == 0) {
        return 0;
      } else if (strcmp(cmds[0].argv[0], "cd") == 0) {
        if (cmds[0].argc < 2) {
          fprintf(stderr, "cd: missing argument\n");
        } else if (chdir(cmds[0].argv[1]) == -1) {
          perror("cd");
        }
        continue;
      }
    }
    execute_pipeline(cmds, ncmds);
  }
}
