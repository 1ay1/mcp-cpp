#include <mcp/tools/util/sandbox.hpp>

#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/progress.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace mcp::tools::util::sandbox {

namespace fs = std::filesystem;

namespace {

// Single-process state. Set by init() at startup, read by every bash
// call afterwards. Atomics aren't strictly needed (set once, never
// flipped after main has handed off to maya), but they cost nothing
// and document the read-many lifecycle.
std::atomic<Mode>    g_mode{Mode::Auto};
std::atomic<Backend> g_backend{Backend::None};

// Host-installed sandbox (see sandbox.hpp). Plain globals: set once at
// startup, read on every tool run, never mutated concurrently.
HostSandbox& host_sandbox() { static HostSandbox hs; return hs; }

// Only the POSIX backends (Linux bwrap, macOS sandbox-exec) need the
// "can we run this binary?" probe — the Windows/unsupported branch
// just hard-codes Backend::None.
#if defined(__linux__) || defined(__APPLE__)

// One-shot probe: try to spawn `<exe> --version` and observe the
// outcome. Replaces a fragile PATH walk with the actual semantic
// check we care about ("can we run this binary?"). The result is
// thrown away — exit code 0 just means the binary exists and starts.
[[nodiscard]] bool can_invoke(const char* exe) {
    SubprocessOptions opts;
    opts.argv = std::vector<std::string>{exe, "--version"};
    opts.timeout = std::chrono::seconds{2};
    opts.max_bytes = 4096;
    auto r = Subprocess::run(std::move(opts));
    return r.started && r.exit_code == 0;
}

#endif // posix backends

// ---------------------------------------------------------------------
// Denial diagnostics -- PLATFORM-INDEPENDENT, and deliberately above the
// backend split below.
//
// These sit outside #if because both backends need them: the comment on
// looks_like_boundary() names sandbox-exec explicitly as a backend that
// returns EACCES. Leaving them inside the __linux__ arm compiled fine on
// Linux and broke the macOS release leg, which is the worst place for a
// guard to be wrong -- nothing a developer runs locally sees it.
// ---------------------------------------------------------------------
// Tell the model WHY a command failed, when the reason was us.
//
// A sandboxed failure reaches a tool as whatever the child printed:
// `cat: /etc/shadow: Permission denied`, `fatal: unable to access …`. That
// is indistinguishable from the file genuinely not being readable, so a
// model retries it, tries sudo, tries a different path, and burns turns
// against a boundary it cannot see. The sandbox knows the answer and was
// throwing it away.
//
// bwrap says nothing at all: the kernel returns EACCES and that is the whole
// story, so the note has to be synthesized from the fact that a sandbox was
// active and the command failed. A backend that explains itself on stderr
// needs no help here -- detect its prefix and leave the output alone.
//
// Deliberately conservative: appended only when the command FAILED and the
// output looks like a boundary refusal. A note on every failure would train
// the model to ignore it, which is worse than no note.
//
// "Permission denied" is NOT the main shape, which is what made the first
// version of this fire on nothing. bwrap does not deny a path — it declines
// to bind it, so the path simply is not there and the child reports
// `No such file or directory`. Measured:
//
//     $ cat /etc/shadow            (inside bwrap)
//     cat: /etc/shadow: No such file or directory
//
// That is the more dangerous message of the two, because a model reads it as
// "this file does not exist on this machine" and stops — or worse, tries to
// CREATE it. So an absent-path failure on an ABSOLUTE path outside the
// workspace is the primary trigger; EACCES/EPERM are kept for backends that
// do return them (sandbox-exec, Landlock).
[[nodiscard]] bool looks_like_boundary(std::string_view out) {
    for (std::string_view needle : {
             "Permission denied", "permission denied",
             "Operation not permitted", "EACCES", "EPERM",
             "Read-only file system",
             "No such file or directory", "no such file or directory"}) {
        if (out.find(needle) != std::string_view::npos) return true;
    }
    return false;
}

// Does the output mention an ABSOLUTE path that the sandbox would not have
// made visible? Without this, every `cat typo.txt` in the workspace would be
// blamed on the sandbox — the note has to be wrong far less often than it is
// right, or it becomes noise the model learns to skip.
//
// Absolute specifically. A first attempt scanned for any '/', which matched
// `./missing.txt` and `nope/inner.txt` and annotated two plain typos — the
// exact noise this guard exists to prevent. A relative path resolves inside
// the cwd, which is inside the grant, so it can never be the sandbox's
// doing.
[[nodiscard]] bool mentions_path_outside_workspace(std::string_view out) {
    const std::string ws = workspace_root().string();
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] != '/') continue;
        // Absolute means the '/' opens the token: preceded by start of line,
        // whitespace, or a quote — not by a path character.
        if (i > 0) {
            const char p = out[i - 1];
            const bool opens = p == ' ' || p == '\n' || p == '\t'
                            || p == '\'' || p == '"' || p == '(' || p == '=';
            if (!opens) continue;    // mid-path slash (./x, a/b) — not absolute
        }
        std::size_t j = i;
        while (j < out.size() && out[j] != ' ' && out[j] != '\n'
               && out[j] != '\t' && out[j] != ':' && out[j] != '\''
               && out[j] != '"')
            ++j;
        const auto cand = out.substr(i, j - i);
        if (cand.size() <= 1) continue;
        // Inside the workspace, or somewhere the sandbox deliberately grants?
        // Then the failure is real, not ours.
        if (!ws.empty() && cand.rfind(ws, 0) == 0) continue;
        if (cand.rfind("/tmp", 0) == 0 || cand.rfind("/var/tmp", 0) == 0
            || cand.rfind("/dev", 0) == 0 || cand.rfind("/proc", 0) == 0)
            continue;
        return true;
    }
    return false;
}

void annotate_sandbox_denial(SubprocessResult& r) {
    if (!is_active() || r.exit_code == 0) return;
    if (!looks_like_boundary(r.output)) return;
    if (!mentions_path_outside_workspace(r.output)) return;

    std::string note =
        "\n\n[sandbox] This command ran inside agentty's sandbox (";
    switch (detected_backend()) {
        case Backend::Bwrap:       note += "bwrap";        break;
        case Backend::SandboxExec: note += "sandbox-exec"; break;
        case Backend::None:        note += "none";         break;
    }
    note += "), so a missing or unreadable path may be the sandbox rather "
            "than the filesystem — paths outside the grant are not made "
            "visible, so they report as ABSENT rather than denied. Writable: "
            "the workspace (";
    note += workspace_root().string();
    note += ") and $TMPDIR. The network is available.\nIf the path is "
            "genuinely needed, ask the user to re-run with --sandbox off.";
    r.output += note;
}


#if defined(__linux__)

// A GENUINE minimal-sandbox probe. `bwrap --version` (the old check via
// can_invoke) only proved the BINARY EXISTS — it never creates a namespace, so
// it passed on hosts where bwrap is installed but unprivileged user namespaces
// are BLOCKED: Ubuntu 24.04's AppArmor `userns` restriction, RHEL/hardened
// `kernel.unprivileged_userns_clone=0` or `user.max_user_namespaces=0`, and
// many container/CI hosts. There every REAL command then died with
//     bwrap: setting up uid map: Permission denied
// while the tool host reported "sandbox: active (bwrap)" — GitHub issue #21.
//
// So we run the SAME namespace unshares a real command uses against a trivial
// /bin/true and require it to actually start AND exit 0. If it can't build the
// namespace here, it can't build it for the shell either, and we report no
// backend so Auto degrades to unsandboxed and On surfaces a clear error.
[[nodiscard]] bool bwrap_can_sandbox() {
    if (!can_invoke("bwrap")) return false;
    const std::vector<std::string> argv = {
        "bwrap",
        "--unshare-user", "--unshare-pid",
        "--ro-bind", "/usr", "/usr",
        "--ro-bind-try", "/bin", "/bin",
        "--ro-bind-try", "/lib", "/lib",
        "--ro-bind-try", "/lib64", "/lib64",
        "--proc", "/proc",
        "--dev", "/dev",
        "--die-with-parent",
        "--", "/bin/true",
    };
    auto r = run_argv_s(argv, /*max_bytes=*/4096, std::chrono::seconds{5});
    return r.started && !r.timed_out && r.exit_code == 0;
}

[[nodiscard]] Backend probe() {
    return bwrap_can_sandbox() ? Backend::Bwrap : Backend::None;
}

// Build the bwrap argv prefix. Workspace gets read-write bound to
// itself; system dirs are bound read-only so the shell can find
// /bin/sh, libc, /etc/resolv.conf, etc.; /tmp is a fresh tmpfs (no
// leakage into / from the host /tmp); /proc and /dev are minimal;
// network namespace is shared (so `git push` / `npm install` / `curl`
// keep working — sandboxing those would break legitimate agent flows
// users expect to work).
//
// The intent is "bash can do anything WITHIN your workspace, plus
// network, plus read system libs — but it cannot mutate /etc,
// /home/<other-projects>, ~/.ssh, /opt, etc." That's the threat model
// we're actually defending against: a compromised or sloppy model
// running `rm -rf ~` or `cat ~/.ssh/id_rsa` after one user approval.
//
// `--die-with-parent` ensures the sandbox dies if agentty dies — no
// detached zombies. `--unshare-pid` gives the child a clean PID
// namespace so kills work cleanly. `--new-session` so the child can't
// steal the controlling tty.
[[nodiscard]] std::vector<std::string> build_bwrap_argv(std::string_view shell_cmd) {
    std::string ws = workspace_root().string();
    std::vector<std::string> argv = {"bwrap"};

    auto push = [&](const char* a) { argv.emplace_back(a); };
    auto push_pair = [&](const char* k, std::string v) {
        argv.emplace_back(k);
        argv.emplace_back(std::move(v));
    };
    auto push_bind = [&](const char* k, const char* p) {
        argv.emplace_back(k);
        argv.emplace_back(p);
        argv.emplace_back(p);
    };

    // System dirs first: read-only. `--ro-bind-try` skips silently if
    // the path doesn't exist on this distro (e.g. /lib64 on Alpine).
    push_bind("--ro-bind",     "/usr");
    push_bind("--ro-bind",     "/bin");

    // /etc: bind ONLY the files a shell + toolchain + name resolution
    // actually need, not the whole tree. Binding all of /etc exposed
    // host secrets (/etc/shadow if readable, krb5 keytabs, corporate
    // config) to an "approved" bash call that also has --share-net —
    // a read+exfiltrate path the sandbox is supposed to close.
    // --ro-bind-try so a missing file on a given distro is skipped.
    {
        static constexpr const char* kEtcAllow[] = {
            "/etc/resolv.conf",   // DNS
            "/etc/hosts",
            "/etc/nsswitch.conf", // NSS resolution order
            "/etc/host.conf",
            "/etc/passwd",        // uid->name (git, shells)
            "/etc/group",
            "/etc/localtime",     // timestamps
            "/etc/ssl",           // TLS trust store (curl/git over https)
            "/etc/pki",           // RHEL/Fedora trust store
            "/etc/ca-certificates",
            "/etc/ca-certificates.conf",
            "/etc/gitconfig",     // system git config
            "/etc/profile",
            "/etc/alternatives",  // Debian toolchain symlinks
        };
        for (const char* f : kEtcAllow) {
            argv.emplace_back("--ro-bind-try");
            argv.emplace_back(f);
            argv.emplace_back(f);
        }
    }

    argv.emplace_back("--ro-bind-try");
    argv.emplace_back("/lib"); argv.emplace_back("/lib");
    argv.emplace_back("--ro-bind-try");
    argv.emplace_back("/lib64"); argv.emplace_back("/lib64");
    argv.emplace_back("--ro-bind-try");
    argv.emplace_back("/sbin"); argv.emplace_back("/sbin");
    argv.emplace_back("--ro-bind-try");
    argv.emplace_back("/opt"); argv.emplace_back("/opt");

    // User-local toolchains (GitHub issue #21). Many toolchains install OUTSIDE
    // /usr — webinstall.dev drops go/gofmt/node under ~/.local/opt and links
    // them into ~/.local/bin; rustup/cargo, go, nvm, pyenv, rbenv, asdf, bun,
    // and deno all live under $HOME. Without these binds an "approved" bash
    // call couldn't find the very tools the user asked the agent to run. Bind
    // them READ-ONLY via --ro-bind-try so a missing dir is skipped. Deliberately
    // narrow (named tool roots), NOT all of $HOME — that would re-expose
    // ~/.ssh / ~/.aws / ~/.config secrets the sandbox exists to protect.
    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::string h = home;
        // Narrow, tool-only roots. Deliberately EXCLUDES broad dirs that mix in
        // secrets/app-data: ~/.local/share (app data), ~/.npm (may cache an
        // _auth token), ~/.config (creds), ~/.local/state (logs/history).
        static constexpr const char* kToolSubdirs[] = {
            "/.local/bin", "/.local/opt", "/.local/lib", // webinstall.dev etc.
            "/.cargo/bin", "/.rustup",   // Rust (bin only from .cargo)
            "/go/bin", "/.go",           // Go (GOPATH bin + webinstall)
            "/.nvm",                     // Node version manager
            "/.pyenv", "/.rbenv", "/.asdf", // version managers
            "/.bun/bin", "/.deno/bin",   // Bun / Deno
            "/.dotnet", "/.sdkman/candidates", // .NET / JVM
        };
        for (const char* sub : kToolSubdirs) {
            std::string p = h + sub;
            argv.emplace_back("--ro-bind-try");
            argv.emplace_back(p);
            argv.emplace_back(std::move(p));
        }
    }

    // Pseudo-fs
    push_pair("--proc", "/proc");
    push_pair("--dev",  "/dev");

    // Fresh /tmp inside the sandbox. MUST come before the workspace
    // bind: when workspace lives under /tmp (common in test setups),
    // bwrap applies args in order and a later --tmpfs would wipe the
    // workspace overlay. Bind workspace LAST so it always wins.
    push_pair("--tmpfs", "/tmp");

    // Workspace: read-write. Bound LAST so it overlays any earlier
    // --tmpfs / --ro-bind that touches the same prefix.
    //
    // NOTE: with --workspace / the rw bind covers the entire host
    // filesystem, which defeats the point of the sandbox. We still
    // wrap (process/pid/session hardening + fresh /tmp/proc/dev keep
    // some value) but describe_state() reports the degraded posture so
    // the user isn't told they're "active (bwrap)" when they're not.
    argv.emplace_back("--bind");
    argv.emplace_back(ws);
    argv.emplace_back(ws);

    // Network: keep it. Removing this breaks git push / package
    // installs / curl — flows users explicitly want to work.
    push("--share-net");

    // Process / namespace / privilege hardening. Each --unshare severs a
    // kernel namespace so a command inside the sandbox can't observe or touch
    // the host's view of it (user/pid/ipc/uts/cgroup). --new-session detaches
    // the controlling tty (blocks TIOCSTI injection); --die-with-parent avoids
    // detached zombies. bwrap runs the payload with no ambient caps and
    // no_new_privs inside the userns, so a setuid binary can't escalate. Net is
    // kept (--share-net) so git/npm/curl work (accepted residual).
    push("--unshare-user");
    push("--unshare-pid");
    push("--unshare-ipc");
    push("--unshare-uts");
    push("--unshare-cgroup-try");
    push("--new-session");
    push("--die-with-parent");

    // Pass through the workspace-relative cwd so the shell starts where
    // the user expects. Default cwd would be / inside the sandbox.
    argv.emplace_back("--chdir");
    argv.emplace_back(ws);

    // The actual shell command
    argv.emplace_back("--");
    argv.emplace_back("/bin/sh");
    argv.emplace_back("-c");
    argv.emplace_back(std::string{shell_cmd});
    return argv;
}

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout,
                                           std::string_view cwd,
                                           const std::vector<std::pair<std::string, std::string>>& env) {
    SubprocessOptions opts;
    opts.argv = build_bwrap_argv(cmd);
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.cwd = std::string{cwd};
    opts.env = env;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    auto r = Subprocess::run(std::move(opts));
    annotate_sandbox_denial(r);
    return r;
}

// argv-form: wrap with the same bwrap prefix as the shell form, then
// append the user's argv after the `--` separator. No `sh -c`
// indirection — the args reach the child process exactly as given.
[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    if (user_argv.empty()) {
        SubprocessResult r;
        r.started = false; r.start_error = "empty argv";
        return r;
    }
    // Build prefix with no shell command, then splice the user's argv.
    // Same prefix the shell form uses. The builder ends with four elements
    // ("--", "/bin/sh", "-c", cmd), so the pop below matches its tail — but
    // assert the shape rather than trusting it, because a builder that
    // changed its tail would otherwise silently splice the user's argv into
    // the wrong position.
    auto wrapped = build_bwrap_argv("");
    // Pop the trailing 4 elements added by the builder ("--",
    // "/bin/sh", "-c", ""), then append user argv directly.
    if (wrapped.size() >= 4) wrapped.resize(wrapped.size() - 4);
    wrapped.emplace_back("--");
    for (const auto& a : user_argv) wrapped.push_back(a);

    SubprocessOptions opts;
    opts.argv = std::move(wrapped);
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    auto r = Subprocess::run(std::move(opts));
    annotate_sandbox_denial(r);
    return r;
}

#elif defined(__APPLE__)

[[nodiscard]] Backend probe() {
    return can_invoke("sandbox-exec") ? Backend::SandboxExec : Backend::None;
}

// Generate a minimal sandbox-exec profile. Allows reads broadly,
// limits writes to workspace + tmp + system caches, allows network
// (same rationale as bwrap: agent-typical commands need it).
//
// Apple deprecated `sandbox-exec` in public docs but the binary keeps
// working. The profile language is Scheme-ish; we keep it small so a
// future Apple removal is easy to spot.
[[nodiscard]] std::string build_profile(std::string_view workspace) {
    std::string p;
    p += "(version 1)\n";
    p += "(deny default)\n";
    // Process / signals
    p += "(allow process-exec)\n";
    p += "(allow process-fork)\n";
    p += "(allow signal (target same-sandbox))\n";
    // Reads: broad — same rationale as bwrap's ro-bind on system dirs.
    p += "(allow file-read*)\n";
    // Writes: workspace + tmp/cache regions only.
    p += "(allow file-write* (subpath \"" + std::string{workspace} + "\"))\n";
    p += "(allow file-write* (subpath \"/tmp\"))\n";
    p += "(allow file-write* (subpath \"/private/tmp\"))\n";
    p += "(allow file-write* (subpath \"/private/var/folders\"))\n";   // user caches
    p += "(allow file-write* (subpath \"/dev/null\"))\n";
    p += "(allow file-write* (subpath \"/dev/tty\"))\n";
    // Network: open. Restricting would break git push / curl / npm.
    p += "(allow network*)\n";
    p += "(allow system-socket)\n";
    p += "(allow mach-lookup)\n";
    p += "(allow iokit-open)\n";
    p += "(allow sysctl-read)\n";
    return p;
}

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout,
                                           std::string_view cwd,
                                           const std::vector<std::pair<std::string, std::string>>& env) {
    SubprocessOptions opts;
    auto profile = build_profile(workspace_root().string());
    opts.argv = std::vector<std::string>{
        "sandbox-exec", "-p", std::move(profile),
        "/bin/sh", "-c", std::string{cmd}
    };
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.cwd = std::string{cwd};
    opts.env = env;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    auto r = Subprocess::run(std::move(opts));
    annotate_sandbox_denial(r);
    return r;
}

[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    if (user_argv.empty()) {
        SubprocessResult r;
        r.started = false; r.start_error = "empty argv";
        return r;
    }
    SubprocessOptions opts;
    auto profile = build_profile(workspace_root().string());
    std::vector<std::string> argv{"sandbox-exec", "-p", std::move(profile)};
    for (const auto& a : user_argv) argv.push_back(a);
    opts.argv = std::move(argv);
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    auto r = Subprocess::run(std::move(opts));
    annotate_sandbox_denial(r);
    return r;
}

#else // Windows / unsupported

[[nodiscard]] Backend probe() { return Backend::None; }

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout,
                                           std::string_view cwd,
                                           const std::vector<std::pair<std::string, std::string>>& env) {
    // Should never be called — is_active() returns false on this
    // platform — but defensively fall through to the unsandboxed path.
    return run_command_s(std::string{cmd}, max_bytes, timeout, cwd, env);
}

[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    return run_argv_s(user_argv, max_bytes, timeout);
}

#endif

} // namespace

bool init(Mode requested) {
    g_mode.store(requested, std::memory_order_release);
    auto found = (requested == Mode::Off) ? Backend::None : probe();
    g_backend.store(found, std::memory_order_release);
    if (requested == Mode::On && found == Backend::None) {
        // Strict mode + no backend = init failure. Caller decides
        // whether to abort startup or downgrade silently.
        return false;
    }
    return true;
}

void set_host_sandbox(HostSandbox hs) { host_sandbox() = std::move(hs); }
bool has_host_sandbox() noexcept { return static_cast<bool>(host_sandbox().run); }

Mode    requested_mode()   noexcept { return g_mode.load(std::memory_order_acquire); }
Backend detected_backend() noexcept { return g_backend.load(std::memory_order_acquire); }

bool is_active() noexcept {
    if (requested_mode() == Mode::Off) return false;
    // A host sandbox IS a sandbox, even when our own probe found no backend:
    // the host may have an engine this library cannot reach on its own.
    return has_host_sandbox() || detected_backend() != Backend::None;
}

std::string describe_state() {
    auto m = requested_mode();
    auto b = detected_backend();
    if (m == Mode::Off) return "sandbox: off";
    // Name what will actually run the command.
    if (has_host_sandbox() && !host_sandbox().label.empty())
        return "sandbox: active (" + host_sandbox().label + ")";
    const char* tag = nullptr;
    switch (b) {
        case Backend::Bwrap:       tag = "bwrap";        break;
        case Backend::SandboxExec: tag = "sandbox-exec"; break;
        case Backend::None:        tag = nullptr;        break;
    }
    if (tag) {
        // --workspace / rw-binds the whole filesystem: still wrapped,
        // but no filesystem containment. Be honest about it.
        std::error_code wec;
        auto ws = fs::weakly_canonical(workspace_root(), wec);
        if (wec) ws = workspace_root();
        if (ws == ws.root_path())
            return std::string{"sandbox: degraded ("} + tag
                 + ", --workspace / gives no filesystem containment)";
        return std::string{"sandbox: active ("} + tag + ")";
    }
    if (m == Mode::On)
        return "sandbox: requested but no backend "
#if defined(__linux__)
               + std::string{can_invoke("bwrap")
                   ? "(bubblewrap present but unprivileged user namespaces are "
                     "blocked \xe2\x80\x94 e.g. Ubuntu 24.04 AppArmor userns "
                     "restriction or kernel.unprivileged_userns_clone=0; "
                     "allow userns or run with --sandbox off)"
                   : "(install bubblewrap)"};
#elif defined(__APPLE__)
               "(sandbox-exec missing \xe2\x80\x94 system integrity issue)";
#else
               "(unsupported on this platform)";
#endif
    // Mode::Auto + no backend → falling through unsandboxed
    return "sandbox: unavailable, running unsandboxed "
#if defined(__linux__)
           + std::string{can_invoke("bwrap")
               ? "(bubblewrap present but user namespaces are blocked \xe2\x80\x94 "
                 "allow unprivileged userns to enable containment)"
               : "(install bubblewrap to enable)"};
#elif defined(__APPLE__)
           "(sandbox-exec missing)";
#else
           "(no backend on this platform)";
#endif
}

SubprocessResult run_shell_command(std::string_view cmd,
                                   std::size_t max_bytes,
                                   std::chrono::seconds timeout,
                                   std::string_view cwd,
                                   const std::vector<std::pair<std::string, std::string>>& env) {
    // A host-installed sandbox takes precedence over our own backend: it is
    // the one the host's OTHER execution paths use, and two engines confining
    // two halves of the same session is the split this hook exists to close.
    if (has_host_sandbox()) {
        if (auto r = host_sandbox().run({"/bin/sh", "-c", std::string{cmd}},
                                        max_bytes, timeout, cwd, env))
            return std::move(*r);
        // nullopt => the host declined this one; fall through.
    }
    if (!is_active())
        return run_command_s(std::string{cmd}, max_bytes, timeout, cwd, env);
    return run_wrapped(cmd, max_bytes, timeout, cwd, env);
}

std::vector<std::string> prepare_shell_argv(std::string_view cmd) {
    if (is_active()) {
#if defined(__linux__)
        return build_bwrap_argv(cmd);
#elif defined(__APPLE__)
        return {"sandbox-exec", "-p", build_profile(workspace_root().string()),
                "/bin/sh", "-c", std::string{cmd}};
#endif
    }
#ifdef _WIN32
    return {"cmd.exe", "/d", "/s", "/c", std::string{cmd}};
#else
    return {"/bin/sh", "-lc", std::string{cmd}};
#endif
}

SubprocessResult run_argv(const std::vector<std::string>& argv,
                          std::size_t max_bytes,
                          std::chrono::seconds timeout) {
    if (has_host_sandbox()) {
        if (auto r = host_sandbox().run(argv, max_bytes, timeout, {}, {}))
            return std::move(*r);
    }
    if (!is_active())
        return run_argv_s(argv, max_bytes, timeout);
    return run_wrapped_argv(argv, max_bytes, timeout);
}

} // namespace mcp::tools::util::sandbox
