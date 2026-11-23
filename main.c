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

typedef enum {
  LEX_SPACE,
  LEX_WORD,
} LexState;

typedef enum {
  PARSE_ARG,
  PARSE_EXPECT_IN,
  PARSE_EXPECT_OUT,
  PARSE_EXPECT_APPEND,
} ParseState;

int tokenize(char *line, char **argv, int max_args) {
  int argc = 0;
  LexState state = LEX_SPACE;

  for (char *p = line; *p != '\0'; p++) {
    int is_ws = (*p == ' ' || *p == '\t');
    switch (state) {
    case LEX_SPACE:
      if (!is_ws) {
        if (argc >= max_args - 1) {
          argv[argc] = NULL;
          return argc;
        }
        argv[argc++] = p;
        state = LEX_WORD;
      }
      break;

    case LEX_WORD:
      if (is_ws) {
        *p = '\0';
        state = LEX_SPACE;
      }
      break;
    }
  }

  argv[argc] = NULL;
  return argc;
}

static int is_operator(const char *tok) {
  return strcmp(tok, "|") == 0 || strcmp(tok, "<") == 0 ||
         strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0;
}

static void command_init(Command *cmd) {
  cmd->argc = 0;
  cmd->in_file = NULL;
  cmd->outfile = NULL;
  cmd->append = 0;
}

int parse_pipeline(char **tokens, int ntokens, Command *cmds, int max_cmds) {
  if (ntokens == 0 || max_cmds <= 0) {
    return 0;
  }

  ParseState state = PARSE_ARG;
  int cur = 0;
  command_init(&cmds[cur]);

  for (int i = 0; i < ntokens; i++) {
    char *tok = tokens[i];

    switch (state) {
    case PARSE_ARG:
      if (strcmp(tok, "|") == 0) {
        if (cmds[cur].argc == 0) {
          fprintf(stderr, "syntax error near '|'\n");
          return -1;
        }
        cmds[cur].argv[cmds[cur].argc] = NULL;
        if (cur + 1 >= max_cmds) {
          fprintf(stderr, "too many commands in pipeline\n");
          return -1;
        }
        cur++;
        command_init(&cmds[cur]);
      } else if (strcmp(tok, "<") == 0) {
        state = PARSE_EXPECT_IN;
      } else if (strcmp(tok, ">") == 0) {
        state = PARSE_EXPECT_OUT;
      } else if (strcmp(tok, ">>") == 0) {
        state = PARSE_EXPECT_APPEND;
      } else if (cmds[cur].argc < MAX_ARGS - 1) {
        cmds[cur].argv[cmds[cur].argc++] = tok;
      }
      break;

    case PARSE_EXPECT_IN:
      if (is_operator(tok)) {
        fprintf(stderr, "syntax error near '%s'\n", tok);
        return -1;
      }
      cmds[cur].in_file = tok;
      state = PARSE_ARG;
      break;

    case PARSE_EXPECT_OUT:
      if (is_operator(tok)) {
        fprintf(stderr, "syntax error near '%s'\n", tok);
        return -1;
      }
      cmds[cur].outfile = tok;
      cmds[cur].append = 0;
      state = PARSE_ARG;
      break;

    case PARSE_EXPECT_APPEND:
      if (is_operator(tok)) {
        fprintf(stderr, "syntax error near '%s'\n", tok);
        return -1;
      }
      cmds[cur].outfile = tok;
      cmds[cur].append = 1;
      state = PARSE_ARG;
      break;
    }
  }

  if (state != PARSE_ARG) {
    fprintf(stderr, "syntax error: missing filename\n");
    return -1;
  }
  if (cmds[cur].argc == 0) {
    fprintf(stderr, "syntax error: empty command\n");
    return -1;
  }

  cmds[cur].argv[cmds[cur].argc] = NULL;
  return cur + 1;
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

static void reap_children(const pid_t *pids, int count) {
  for (int i = 0; i < count; i++) {
    int status;
    while (waitpid(pids[i], &status, 0) == -1 && errno == EINTR) {
    }
  }
}

void execute_pipeline(const Command *cmds, int ncmds) {
  pid_t pids[MAX_CMDS] = {0};
  int prev_fd = -1;

  for (int i = 0; i < ncmds; i++) {
    int has_prev = (prev_fd != -1);
    int has_next = (i < ncmds - 1);
    int pipefd[2] = {-1, -1};

    if (has_next && pipe(pipefd) == -1) {
      perror("pipe");
      if (has_prev) {
        close(prev_fd);
      }
      reap_children(pids, i);
      return;
    }

    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      if (has_prev) {
        close(prev_fd);
      }
      if (has_next) {
        close(pipefd[0]);
        close(pipefd[1]);
      }
      reap_children(pids, i);
      return;
    }

    if (pid == 0) {
      if (has_prev) {
        if (dup2(prev_fd, STDIN_FILENO) < 0) {
          perror("dup2");
          _exit(1);
        }
        close(prev_fd);
      }
      if (has_next) {
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
    }

    pids[i] = pid;
    if (has_prev) {
      close(prev_fd);
    }
    if (has_next) {
      close(pipefd[1]);
      prev_fd = pipefd[0];
    }
  }

  reap_children(pids, ncmds);
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
