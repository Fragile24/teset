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

typedef enum {
  OUT_NONE,
  OUT_TRUNC,
  OUT_APPEND,
} OutputMode;

typedef struct {
  char *argv[MAX_ARGS];
  int argc;
  char *in_file;
  char *outfile;
  OutputMode out_mode;
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

typedef enum {
  PIPE_SINGLE,
  PIPE_FIRST,
  PIPE_MIDDLE,
  PIPE_LAST,
} PipeRole;

typedef enum {
  EXEC_PREPARE_STAGE,
  EXEC_FORK_STAGE,
  EXEC_REAP,
  EXEC_DONE,
} ExecState;

typedef enum {
  SHELL_READ,
  SHELL_PARSE,
  SHELL_DISPATCH,
  SHELL_EXIT,
} ShellState;

typedef enum {
  BUILTIN_NONE,
  BUILTIN_EXIT,
  BUILTIN_CD,
} BuiltinKind;

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
  cmd->out_mode = OUT_NONE;
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
      cmds[cur].out_mode = OUT_TRUNC;
      state = PARSE_ARG;
      break;

    case PARSE_EXPECT_APPEND:
      if (is_operator(tok)) {
        fprintf(stderr, "syntax error near '%s'\n", tok);
        return -1;
      }
      cmds[cur].outfile = tok;
      cmds[cur].out_mode = OUT_APPEND;
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

  int out_flags = 0;
  switch (cmd->out_mode) {
  case OUT_NONE:
    return;
  case OUT_TRUNC:
    out_flags = O_WRONLY | O_CREAT | O_TRUNC;
    break;
  case OUT_APPEND:
    out_flags = O_WRONLY | O_CREAT | O_APPEND;
    break;
  }

  int fd = open(cmd->outfile, out_flags, 0644);
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

static PipeRole classify_pipe_role(int index, int ncmds) {
  if (ncmds <= 1) {
    return PIPE_SINGLE;
  }
  if (index == 0) {
    return PIPE_FIRST;
  }
  if (index == ncmds - 1) {
    return PIPE_LAST;
  }
  return PIPE_MIDDLE;
}

static int role_has_prev(PipeRole role) {
  return role == PIPE_MIDDLE || role == PIPE_LAST;
}

static int role_has_next(PipeRole role) {
  return role == PIPE_FIRST || role == PIPE_MIDDLE;
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
  ExecState state = EXEC_PREPARE_STAGE;
  PipeRole role = PIPE_SINGLE;
  int stage = 0;
  int spawned = 0;
  int prev_fd = -1;
  int pipefd[2] = {-1, -1};

  while (state != EXEC_DONE) {
    switch (state) {
    case EXEC_PREPARE_STAGE:
      if (stage >= ncmds) {
        state = EXEC_REAP;
        break;
      }
      role = classify_pipe_role(stage, ncmds);
      pipefd[0] = -1;
      pipefd[1] = -1;
      if (role_has_next(role) && pipe(pipefd) == -1) {
        perror("pipe");
        if (prev_fd != -1) {
          close(prev_fd);
          prev_fd = -1;
        }
        state = EXEC_REAP;
        break;
      }
      state = EXEC_FORK_STAGE;
      break;

    case EXEC_FORK_STAGE: {
      pid_t pid = fork();
      if (pid < 0) {
        perror("fork");
        if (prev_fd != -1) {
          close(prev_fd);
          prev_fd = -1;
        }
        if (role_has_next(role)) {
          close(pipefd[0]);
          close(pipefd[1]);
        }
        state = EXEC_REAP;
        break;
      }

      if (pid == 0) {
        if (role_has_prev(role)) {
          if (dup2(prev_fd, STDIN_FILENO) < 0) {
            perror("dup2");
            _exit(1);
          }
          close(prev_fd);
        }
        if (role_has_next(role)) {
          close(pipefd[0]);
          if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            perror("dup2");
            _exit(1);
          }
          close(pipefd[1]);
        }
        apply_redirection(&cmds[stage]);
        struct sigaction sa = {0};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
        execvp(cmds[stage].argv[0], cmds[stage].argv);
        perror(cmds[stage].argv[0]);
        _exit(127);
      }

      pids[spawned++] = pid;
      if (role_has_prev(role)) {
        close(prev_fd);
        prev_fd = -1;
      }
      if (role_has_next(role)) {
        close(pipefd[1]);
        prev_fd = pipefd[0];
      }
      stage++;
      state = EXEC_PREPARE_STAGE;
      break;
    }

    case EXEC_REAP:
      reap_children(pids, spawned);
      state = EXEC_DONE;
      break;

    case EXEC_DONE:
      break;
    }
  }
}

void handle_sigint(int sig) {
  (void)sig;
  ssize_t n = write(STDOUT_FILENO, "\n", 1);
  (void)n;
}

static BuiltinKind classify_builtin(const Command *cmds, int ncmds) {
  if (ncmds != 1) {
    return BUILTIN_NONE;
  }
  if (strcmp(cmds[0].argv[0], "exit") == 0) {
    return BUILTIN_EXIT;
  }
  if (strcmp(cmds[0].argv[0], "cd") == 0) {
    return BUILTIN_CD;
  }
  return BUILTIN_NONE;
}

int main(void) {
  char str[200];
  char *argv[MAX_ARGS];
  Command cmds[MAX_CMDS];
  int ncmds = 0;

  struct sigaction sa = {0};
  sa.sa_handler = handle_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);

  ShellState state = SHELL_READ;
  while (state != SHELL_EXIT) {
    switch (state) {
    case SHELL_READ:
      printf("mysh> ");
      fflush(stdout);
      errno = 0;
      if (fgets(str, sizeof(str), stdin) == NULL) {
        if (errno == EINTR) {
          clearerr(stdin);
          state = SHELL_READ;
        } else {
          puts("end of file");
          state = SHELL_EXIT;
        }
        break;
      }
      state = SHELL_PARSE;
      break;

    case SHELL_PARSE: {
      size_t len = strlen(str);
      if (len > 0 && str[len - 1] == '\n') {
        str[len - 1] = '\0';
      }
      int argc = tokenize(str, argv, MAX_ARGS);
      if (argc == 0) {
        state = SHELL_READ;
        break;
      }
      ncmds = parse_pipeline(argv, argc, cmds, MAX_CMDS);
      state = (ncmds > 0) ? SHELL_DISPATCH : SHELL_READ;
      break;
    }

    case SHELL_DISPATCH:
      switch (classify_builtin(cmds, ncmds)) {
      case BUILTIN_EXIT:
        state = SHELL_EXIT;
        break;

      case BUILTIN_CD:
        if (cmds[0].argc < 2) {
          fprintf(stderr, "cd: missing argument\n");
        } else if (chdir(cmds[0].argv[1]) == -1) {
          perror("cd");
        }
        state = SHELL_READ;
        break;

      case BUILTIN_NONE:
        execute_pipeline(cmds, ncmds);
        state = SHELL_READ;
        break;
      }
      break;

    case SHELL_EXIT:
      break;
    }
  }

  return 0;
}
