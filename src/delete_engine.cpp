#include "delete_engine.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

extern char **environ;

namespace fc {

DeleteResult delete_direct(const std::vector<fs::path> &paths) {
    DeleteResult result;
    for (const auto &p : paths) {
        std::error_code ec;
        fs::remove_all(p, ec);
        if (!ec) {
            result.deleted_count++;
        } else if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
            result.permission_failed.push_back(p);
        } else {
            result.errors.push_back(p.filename().string() + ": " + ec.message());
        }
    }
    return result;
}

namespace {

// Deletion helper run under `pkexec`. Kept minimal and dependency-free
// (plain POSIX shell) rather than shelling back out to python3, since the
// app itself no longer requires a Python runtime.
constexpr const char *kPkexecHelper =
    "#!/bin/sh\n"
    "while IFS= read -r p; do\n"
    "  [ -z \"$p\" ] && continue\n"
    "  err=$(rm -rf -- \"$p\" 2>&1)\n"
    "  if [ $? -eq 0 ]; then\n"
    "    printf 'OK %s\\n' \"$p\"\n"
    "  else\n"
    "    printf 'ERR %s %s\\n' \"$p\" \"$err\"\n"
    "  fi\n"
    "done\n";

}  // namespace

DeleteResult delete_with_pkexec(const std::vector<fs::path> &paths) {
    DeleteResult result;
    if (paths.empty()) return result;

    char tmpl[] = "/tmp/filecleaner_helper_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        result.errors.push_back(std::string("failed to create helper script: ") + std::strerror(errno));
        return result;
    }
    std::string script = kPkexecHelper;
    if (write(fd, script.data(), script.size()) < 0) { /* best-effort */
    }
    close(fd);
    // Owner-only: this script is about to run as root via pkexec, so it
    // should not be group/world writable or even readable in the meantime
    // (the original Python helper left it world-readable+executable).
    chmod(tmpl, S_IRWXU);

    std::string input;
    for (const auto &p : paths) {
        input += p.string();
        input += '\n';
    }

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        result.errors.push_back("failed to create pipes for pkexec");
        unlink(tmpl);
        return result;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[1]);

    char arg0[] = "pkexec";
    char *argv[] = {arg0, tmpl, nullptr};
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, "pkexec", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        unlink(tmpl);
        result.errors.push_back(rc == ENOENT ? "pkexec binary missing from PATH"
                                              : std::string("failed to launch pkexec: ") + std::strerror(rc));
        return result;
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    std::thread writer([&]() {
        std::size_t off = 0;
        while (off < input.size()) {
            ssize_t n = write(in_pipe[1], input.data() + off, input.size() - off);
            if (n <= 0) break;
            off += static_cast<std::size_t>(n);
        }
        close(in_pipe[1]);
    });

    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = read(out_pipe[0], buf, sizeof(buf))) > 0) output.append(buf, static_cast<std::size_t>(n));
    close(out_pipe[0]);
    writer.join();

    int status = 0;
    waitpid(pid, &status, 0);
    unlink(tmpl);

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exit_code == 126) {
        // Authentication dialog dismissed/denied. Distinct from "nothing to
        // do" so the UI can tell the user why nothing happened, rather than
        // silently reporting success with zero deletions as the original
        // Python version did.
        result.errors.push_back("Authentication was cancelled or denied.");
        return result;
    }

    std::size_t pos = 0;
    while (pos < output.size()) {
        std::size_t eol = output.find('\n', pos);
        if (eol == std::string::npos) eol = output.size();
        std::string line = output.substr(pos, eol - pos);
        pos = eol + 1;

        if (line.rfind("OK ", 0) == 0) {
            result.deleted_count++;
        } else if (line.rfind("ERR ", 0) == 0) {
            result.errors.push_back(line.substr(4));
        }
    }

    if (exit_code != 0 && exit_code != 1) {
        result.errors.push_back("pkexec terminal process runtime fault: " + std::to_string(exit_code));
    }

    return result;
}

}  // namespace fc
