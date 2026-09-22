#include <cstdio>    // IWYU pragma: keep
#include <cstdlib>
#include <csignal>
#include <readline/history.h>
#include <readline/readline.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <sys/wait.h>
#include <iostream>
#include <linux/limits.h>
#include <fcntl.h>
#include <unordered_map>

constexpr int READ_END = 0;
constexpr int WRITE_END = 1;

char *line;

struct command {
  std::vector<std::vector<std::string>> args;
  bool background = false;
  std::string stdinFile;
  std::string stdoutFile;
};

enum class Redirect { None, Stdin, Stdout };

command parse(char* line);
std::vector<char*> to_argv(std::vector<std::string>& args);
void printCommands(command& cmd);

pid_t shellPgid;
volatile sig_atomic_t foregroundPgid = -1;

int selfPipe[2];
std::unordered_map<pid_t, std::string> jobNames;  // pid description, for the notification

void sighandler(int signum) {
  if (signum == SIGINT) {
    if (foregroundPgid > 0) {
      kill(-foregroundPgid, SIGINT);   // relay to whichever job is in the foreground
    }
  } else if (signum == SIGCHLD) {
    pid_t pid;
    while ((pid = waitpid(-1, nullptr, WNOHANG)) > 0) {
        write(selfPipe[1], &pid, sizeof(pid));
    }
  }
}

int main(int argc, char *argv[]) {
  using_history(); // Set up readline history()

  shellPgid = getpid();
  setpgid(shellPgid, shellPgid);
  tcsetpgrp(STDIN_FILENO, shellPgid);

  signal(SIGTTOU, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGINT, sighandler);
  signal(SIGCHLD, sighandler);

  pipe(selfPipe);
  fcntl(selfPipe[0], F_SETFL, O_NONBLOCK);  // so draining "background processes finished" pipe never blocks the shell
  fcntl(selfPipe[1], F_SETFL, O_NONBLOCK);  // so the handler's write() never blocks either

  while (true) {
    pid_t finished;
    while (read(selfPipe[0], &finished, sizeof(finished)) == sizeof(finished)) {
        std::cout << "[background] " << finished;
        auto it = jobNames.find(finished);
        if (it != jobNames.end()) {
            std::cout << " (" << it->second << ")";
            jobNames.erase(it);
        }
        std::cout << " finished\n";
    }

    line = readline("winry> ");

    if (line == nullptr) {
      // Ctrl+D was pressed — exit the shell
      break;
    }

    command cmd = parse(line);
    if (!cmd.args.empty()) add_history(line);
    free(line); // readline allocates a new string each time, must be freed
    if (cmd.args.empty()) continue; // empty line: just show the prompt again
    //printCommands(cmd);

    bool bad = false;
    for (auto& c : cmd.args) {
      if (c.empty()) {
        bad = true;
      }
    }
    if (bad) { std::cerr << "syntax error in command\n"; continue; }

    // Handle exit and cd specially since they are not processes
    if (cmd.args[0][0] == "exit") {
      break;
    } else if (cmd.args[0][0] == "cd") {
      if (cmd.args[0].size() > 1) {
        chdir(cmd.args[0][1].c_str());
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
        } else {
            perror("getcwd() error");
            return 1;
        }
      }
      continue;
    }

    // Pair of FDs for every pipe between commands
    std::vector<int> fd(2 * (cmd.args.size() - 1));

    // Create all pipes before forking
    for (size_t i = 0; i + 1 < cmd.args.size(); i++) {
      pipe(&fd[i * 2]);
    }

    pid_t groupId = -1;

    // For each command typed by user that will result in processes
    for (size_t i = 0; i < cmd.args.size(); i++) {
      std::vector<char*> execArgv = to_argv(cmd.args[i]);

      pid_t pid = fork();

      if (pid < 0) {
        perror("fork");
        break;
      }

      if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        setpgid(0, i == 0 ? 0 : groupId); // also done in the parent, to dodge the fork race

        if (cmd.background) {
          jobNames[pid] = cmd.args[i][0];
        }

        // FILE REDIRECTION
        if (i == 0 && !cmd.stdinFile.empty()) {
          int fdIn = open(cmd.stdinFile.c_str(), O_RDONLY);
          if (fdIn < 0) { perror("open"); _exit(1); }
          dup2(fdIn, STDIN_FILENO);
          close(fdIn);
        }

        // FILE REDIRECTION
        if (i == cmd.args.size() - 1 && !cmd.stdoutFile.empty()) {
          int fdOut = open(cmd.stdoutFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if (fdOut < 0) { perror("open"); _exit(1); }
          dup2(fdOut, STDOUT_FILENO);
          close(fdOut);
        }

        if (i > 0) { // If not the first command, read from pipe i-1 (to the left)
          dup2(fd[2 * (i - 1) + READ_END], STDIN_FILENO);
        }
        if (i < cmd.args.size() - 1) { // If not the last command, write to pipe i (to the right)
          dup2(fd[2 * i + WRITE_END], STDOUT_FILENO);
        }
        for (int descriptor : fd) {
          close(descriptor);
        }
        execvp(execArgv[0], execArgv.data());
        perror("execvp"); // only reached if exec failed
        _exit(127);
      }

      if (i == 0) groupId = pid;
      setpgid(pid, groupId); // also done in the child, to dodge the fork race
    }

    for (int descriptor : fd) {
      close(descriptor);
    }

    if (!cmd.background) {
      foregroundPgid = groupId;
      tcsetpgrp(STDIN_FILENO, groupId);          // hand the terminal to the job
      while (waitpid(-groupId, nullptr, 0) > 0); // block until the whole group is done
      tcsetpgrp(STDIN_FILENO, shellPgid);        // take it back
      foregroundPgid = -1;
    }
    // background jobs just keep running; SIGCHLD reaps them whenever they exit
  }

  return 0;
}

command parse(char* line) {
  command cmd{};
  std::string token{};
  std::vector<std::vector<std::string>> fullCommand{};
  std::vector<std::string> singleCommand{};
  Redirect pending = Redirect::None;

  auto flush = [&]() {
    if (token.empty()) return;
    if (pending == Redirect::Stdin) {
      cmd.stdinFile = token;
    } else if (pending == Redirect::Stdout) {
      cmd.stdoutFile = token;
    } else {
      singleCommand.emplace_back(token);
    }
    token.clear();
  };

  while (*line != '\0') {
    if (*line == ' ') {
      flush();
      pending = Redirect::None;
    } else if (*line == '|') {
      flush();
      pending = Redirect::None;
      fullCommand.emplace_back(singleCommand);
      singleCommand.clear();
    } else if (*line == '&') {
      cmd.background = true;
    } else if (*line == '<') {
      flush();
      pending = Redirect::Stdin;
    } else if (*line == '>') {
      flush();
      pending = Redirect::Stdout;
    } else {
      token += *line;
    }
    line++;
  }

  flush();
  if (!singleCommand.empty()) fullCommand.emplace_back(singleCommand);

  cmd.args = fullCommand;

  return cmd;
}

void printCommands(command& cmd) {
  for (auto& arg : cmd.args) {
    for (auto& s : arg) {
      std::cout << s << " ";
    }
    std::cout << std::endl;
  }

  std::cout << "stdin:      " << (cmd.stdinFile.empty() ? "<none>" : cmd.stdinFile) << std::endl;
  std::cout << "stdout:     " << (cmd.stdoutFile.empty() ? "<none>" : cmd.stdoutFile) << std::endl;
  std::cout << "Background: " << cmd.background << std::endl;
}

std::vector<char*> to_argv(std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (auto &s : args) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);
    return argv;
}