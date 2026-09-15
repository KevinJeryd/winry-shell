#include <cstdio> // IWYU pragma: keep
#include <readline/history.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <sys/wait.h>
#include <iostream>
#include <linux/limits.h>

char *line;

struct command {
  std::vector<std::vector<std::string>> args;
  bool background;
};

command parse(char* line);
std::vector<char*> to_argv(std::vector<std::string>& args);
void printCommands(command& cmd);
void sighandler(int signum) {
  if (signum == SIGINT) {
    std::cout << "Caught CTRL+C" << std::endl;
  }
}

int main(int argc, char *argv[]) {
  while (true) {
    signal(SIGINT, sighandler);
    line = readline("winry> ");

    if (line == nullptr) {
      // Ctrl+D was pressed — exit the shell
      break;
    }

    command cmd = parse(line);
    free(line); // readline allocates a new string each time, must be freed
    if (cmd.args.empty()) continue; // empty line: just show the prompt again
    //printCommands(cmd);

    bool bad = false;
    for (auto& c : cmd.args) {
      if (c.empty()) {
        bad = true;
      }
    }
    if (bad) { std::cerr << "syntax error in command \n"; continue; }

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

    // Pair of FDs for every process
    std::vector<int> fd(2 * (cmd.args.size() - 1));

    // Create all pipes before forking
    for (int i = 0; i + 1 < cmd.args.size(); i++) {
      pipe(&fd[i * 2]);
    }

    std::vector<pid_t> pids; // Needed for non-blocking waiting

    // For each command typed by user that will result in processes
    for (int i = 0; i < cmd.args.size(); i++) {
      std::vector<char*> argv = to_argv(cmd.args[i]);

      pid_t pid = fork();

      if (pid < 0) {
        perror("fork");
        break;
      }

      if (pid == 0) {
        if (i > 0) { // If not the first command, read from pipe i-1 (to the left)
          dup2(fd[2 * (i - 1)], 0);
        }
        if (i < cmd.args.size() - 1) { // If not the last command, write to pipe i (to the right)
          dup2(fd[2 * i + 1], 1);
        }
        for (int descriptor : fd) {
          close(descriptor);
        }
        execvp(argv[0], argv.data());
        perror("execvp"); // only reached if exec failed
        _exit(127);
      }
      pids.push_back(pid);
    }

    for (int descriptor : fd) {
      close(descriptor);
    }
    for (pid_t p : pids) waitpid(p, nullptr, 0);
  }

  return 0;
}

command parse(char* line) {
  command cmd{};
  std::string token{};
  std::vector<std::vector<std::string>> fullCommand{};
  std::vector<std::string> singleCommand{};

  while (*line != '\0') {
    if (*line == ' ') {
      if (!token.empty()) {
        singleCommand.emplace_back(token);
      }
      token.clear();
    } else if (*line == '|') {
      if (!token.empty()) singleCommand.emplace_back(token);
      token.clear();
      fullCommand.emplace_back(singleCommand);
      singleCommand.clear();
    } else if (*line == '&') {
      cmd.background = true;
    }
    else {
      token += *line;
    }
    line++;
  }

  if (!token.empty()) singleCommand.emplace_back(token);
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
