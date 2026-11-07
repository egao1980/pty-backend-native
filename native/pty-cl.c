/*
 * pty-cl: spawn a program on a pseudo-terminal. One C ABI for every OS:
 * forkpty-style fork/exec on Unix, ConPTY on Windows. Lisp (CFFI) calls these;
 * no Lisp code ever runs in a forked child.
 *
 * Return conventions:
 *   pty_cl_spawn   0 ok, else an errno / GetLastError code (also *out_errno)
 *   pty_cl_read    >0 bytes, 0 timeout, -1 EOF, -2 error (pty_cl_errno)
 *   pty_cl_write   bytes written (all of them), -1 error (pty_cl_errno)
 *   pty_cl_wait    1 exited, 0 timeout, -1 error
 *   timeout_ms < 0 waits forever.
 */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _GNU_SOURCE /* posix_openpt, grantpt, unlockpt, ptsname, nanosleep */
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define CL_API __declspec(dllexport)
#else
#define CL_API __attribute__((visibility("default")))
#endif

#define PTY_CL_ABI 1

CL_API int pty_cl_abi(void) { return PTY_CL_ABI; }

#if !defined(_WIN32)
/* ======================================================================
 * Unix
 * ====================================================================== */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/select.h>
#endif

/* Wait until FD is readable (or writable). 1 ready, 0 timeout, -1 error.
 * macOS poll() rejects tty devices (POLLNVAL), so use select() there. */
static int wait_fd(int fd, int for_write, int timeout_ms) {
#if defined(__APPLE__)
  fd_set set;
  struct timeval tv, *tvp = NULL;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  if (timeout_ms >= 0) {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    tvp = &tv;
  }
  int r = select(fd + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, tvp);
  return r < 0 ? -1 : (r > 0 ? 1 : 0);
#else
  struct pollfd pfd = {fd, (short)(for_write ? POLLOUT : POLLIN), 0};
  int r = poll(&pfd, 1, timeout_ms);
  return r < 0 ? -1 : (r > 0 ? 1 : 0);
#endif
}

typedef struct pty_cl {
  int master;
  pid_t pid;
  int exited;
  int code;    /* exit status if exited normally, else -1 */
  int signal;  /* terminating signal, else 0 */
  int last_errno;
} pty_cl_t;

static int set_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* Async-signal-safe helpers for the child. */
static size_t cl_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static void child_fail(int errpipe, int err) {
  ssize_t ignored = write(errpipe, &err, sizeof err);
  (void)ignored;
  _exit(127);
}

static const char *env_path(char *const envp[]) {
  for (char *const *e = envp; e && *e; e++) {
    const char *s = *e;
    if (s[0] == 'P' && s[1] == 'A' && s[2] == 'T' && s[3] == 'H' && s[4] == '=')
      return s + 5;
  }
  return "/usr/local/bin:/usr/bin:/bin";
}

CL_API int pty_cl_spawn(const char *file, char *const argv[], char *const envp[],
                        const char *cwd, uint16_t cols, uint16_t rows,
                        pty_cl_t **out, int *out_errno) {
  int err = 0, master = -1, errpipe[2] = {-1, -1};
  char *slave_name = NULL, *candidate = NULL;
  pid_t pid;
  struct winsize ws;
  struct rlimit rl;
  int max_fd;
  const char *path;
  size_t file_len, cand_cap;

  *out = NULL;
  master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) { err = errno; goto fail; }
  set_cloexec(master);
  {
    const char *name = ptsname(master);
    if (!name) { err = errno; goto fail; }
    slave_name = strdup(name);
    if (!slave_name) { err = ENOMEM; goto fail; }
  }
  /* The child sets the size on its slave: macOS rejects TIOCSWINSZ on a
   * master whose slave has never been opened (ENOTTY). */
  memset(&ws, 0, sizeof ws);
  ws.ws_col = cols;
  ws.ws_row = rows;

  if (pipe(errpipe) < 0) { err = errno; goto fail; }
  set_cloexec(errpipe[0]);
  set_cloexec(errpipe[1]);

  /* Everything the child needs is computed before fork. */
  path = env_path(envp);
  file_len = strlen(file);
  cand_cap = strlen(path) + file_len + 2;
  candidate = malloc(cand_cap);
  if (!candidate) { err = ENOMEM; goto fail; }
  max_fd = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
            rl.rlim_cur < 65536) ? (int)rl.rlim_cur : 65536;

  pid = fork();
  if (pid < 0) { err = errno; goto fail; }

  if (pid == 0) {
    /* Child: async-signal-safe calls only, then exec. */
    sigset_t all;
    int slave, fd, last = ENOENT;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);
    for (int s = 1; s < 32; s++) signal(s, SIG_DFL);

    if (setsid() < 0) child_fail(errpipe[1], errno);
    slave = open(slave_name, O_RDWR);
    if (slave < 0) child_fail(errpipe[1], errno);
#ifdef TIOCSCTTY
    if (ioctl(slave, TIOCSCTTY, 0) < 0) child_fail(errpipe[1], errno);
#endif
    if (ioctl(slave, TIOCSWINSZ, &ws) < 0) child_fail(errpipe[1], errno);
    if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0)
      child_fail(errpipe[1], errno);
    for (fd = 3; fd < max_fd; fd++)
      if (fd != errpipe[1]) close(fd);
    if (cwd && cwd[0] && chdir(cwd) < 0) child_fail(errpipe[1], errno);

    {
      int has_slash = 0;
      for (const char *c = file; *c; c++) if (*c == '/') { has_slash = 1; break; }
      if (has_slash) {
        execve(file, argv, envp);
        child_fail(errpipe[1], errno);
      }
    }
    for (const char *p = path;;) {
      const char *end = p;
      size_t n = 0;
      while (*end && *end != ':') end++;
      if (end == p) { candidate[n++] = '.'; }
      else { while (p < end) candidate[n++] = *p++; }
      candidate[n++] = '/';
      for (size_t i = 0; i < file_len; i++) candidate[n++] = file[i];
      candidate[n] = 0;
      execve(candidate, argv, envp);
      if (errno == EACCES) last = EACCES;
      else if (errno != ENOENT && errno != ENOTDIR && last != EACCES) last = errno;
      if (!*end) break;
      p = end + 1;
    }
    (void)cl_strlen;
    child_fail(errpipe[1], last);
  }

  /* Parent. */
  close(errpipe[1]);
  errpipe[1] = -1;
  {
    int child_err = 0;
    ssize_t n;
    do { n = read(errpipe[0], &child_err, sizeof child_err); } while (n < 0 && errno == EINTR);
    close(errpipe[0]);
    errpipe[0] = -1;
    if (n == (ssize_t)sizeof child_err) {
      int st;
      while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
      err = child_err ? child_err : ENOEXEC;
      goto fail;
    }
  }
  free(slave_name);
  free(candidate);
  {
    pty_cl_t *p = calloc(1, sizeof *p);
    if (!p) { kill(pid, SIGKILL); err = ENOMEM; goto fail; }
    p->master = master;
    p->pid = pid;
    p->code = -1;
    *out = p;
  }
  *out_errno = 0;
  return 0;

fail:
  if (errpipe[0] >= 0) close(errpipe[0]);
  if (errpipe[1] >= 0) close(errpipe[1]);
  if (master >= 0) close(master);
  free(slave_name);
  free(candidate);
  *out_errno = err;
  return err ? err : -1;
}

CL_API long pty_cl_read(pty_cl_t *p, uint8_t *buf, size_t len, int timeout_ms) {
  for (;;) {
    int r = wait_fd(p->master, 0, timeout_ms);
    if (r < 0) {
      if (errno == EINTR) continue;
      p->last_errno = errno;
      return -2;
    }
    if (r == 0) return 0;
    ssize_t n = read(p->master, buf, len);
    if (n > 0) return (long)n;
    if (n == 0) return -1;
    if (errno == EINTR || errno == EAGAIN) continue;
    if (errno == EIO) return -1;   /* Linux: slave side closed */
    p->last_errno = errno;
    return -2;
  }
}

CL_API long pty_cl_write(pty_cl_t *p, const uint8_t *buf, size_t len) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = write(p->master, buf + done, len - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) {
        wait_fd(p->master, 1, 100);
        continue;
      }
      p->last_errno = errno;
      return -1;
    }
    done += (size_t)n;
  }
  return (long)done;
}

CL_API int pty_cl_resize(pty_cl_t *p, uint16_t cols, uint16_t rows) {
  struct winsize ws;
  memset(&ws, 0, sizeof ws);
  ws.ws_col = cols;
  ws.ws_row = rows;
  if (ioctl(p->master, TIOCSWINSZ, &ws) < 0) { p->last_errno = errno; return -1; }
  return 0;
}

static int native_signal(int portable) {
  switch (portable) {
  case 1: return SIGHUP;
  case 2: return SIGINT;
  case 9: return SIGKILL;
  case 15: return SIGTERM;
  default: return portable;
  }
}

CL_API int pty_cl_signal(pty_cl_t *p, int sig) {
  if (p->exited) return 0;
  if (kill(p->pid, native_signal(sig)) < 0) { p->last_errno = errno; return -1; }
  return 0;
}

static void record_status(pty_cl_t *p, int st) {
  p->exited = 1;
  if (WIFEXITED(st)) { p->code = WEXITSTATUS(st); p->signal = 0; }
  else if (WIFSIGNALED(st)) { p->code = -1; p->signal = WTERMSIG(st); }
}

CL_API int pty_cl_wait(pty_cl_t *p, int timeout_ms, int *code, int *sig) {
  if (!p->exited) {
    int st;
    pid_t r;
    if (timeout_ms < 0) {
      do { r = waitpid(p->pid, &st, 0); } while (r < 0 && errno == EINTR);
      if (r < 0) { p->last_errno = errno; return -1; }
      record_status(p, st);
    } else {
      long waited = 0;
      for (;;) {
        r = waitpid(p->pid, &st, WNOHANG);
        if (r < 0 && errno != EINTR) { p->last_errno = errno; return -1; }
        if (r == p->pid) { record_status(p, st); break; }
        if (waited >= timeout_ms) return 0;
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, NULL);
        waited += 5;
      }
    }
  }
  *code = p->code;
  *sig = p->signal;
  return 1;
}

CL_API long pty_cl_pid(pty_cl_t *p) { return (long)p->pid; }
CL_API int pty_cl_errno(pty_cl_t *p) { return p->last_errno; }

CL_API void pty_cl_close(pty_cl_t *p) {
  if (!p) return;
  if (!p->exited) {
    int code, sig;
    kill(p->pid, SIGHUP);
    if (pty_cl_wait(p, 200, &code, &sig) == 0) {
      kill(p->pid, SIGKILL);
      pty_cl_wait(p, -1, &code, &sig);
    }
  }
  close(p->master);
  free(p);
}

#else
/* ======================================================================
 * Windows (ConPTY)
 * ====================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

typedef void *cl_hpcon;
typedef HRESULT (WINAPI *create_pc_fn)(COORD, HANDLE, HANDLE, DWORD, cl_hpcon *);
typedef HRESULT (WINAPI *resize_pc_fn)(cl_hpcon, COORD);
typedef void (WINAPI *close_pc_fn)(cl_hpcon);

static create_pc_fn p_create;
static resize_pc_fn p_resize;
static close_pc_fn p_close;

static int load_conpty(void) {
  if (p_create) return 1;
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  if (!k) return 0;
  p_create = (create_pc_fn)(void *)GetProcAddress(k, "CreatePseudoConsole");
  p_resize = (resize_pc_fn)(void *)GetProcAddress(k, "ResizePseudoConsole");
  p_close = (close_pc_fn)(void *)GetProcAddress(k, "ClosePseudoConsole");
  return p_create && p_resize && p_close;
}

typedef struct pty_cl {
  cl_hpcon hpc;
  HANDLE in_write, out_read, process, closer;
  ULONGLONG quiet_since; /* last output, or when the exit was first seen */
  int exit_seen;
  DWORD pid;
  int exited, code;
  int last_errno;
} pty_cl_t;

static wchar_t *utf16(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc(sizeof(wchar_t) * (size_t)n) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

/* Append ARG to CMD quoted per CommandLineToArgvW rules. */
static void append_arg(wchar_t **cmd, size_t *len, size_t *cap, const wchar_t *arg) {
  size_t need = *len + wcslen(arg) * 2 + 4;
  if (need > *cap) {
    *cap = need * 2;
    *cmd = realloc(*cmd, sizeof(wchar_t) * *cap);
  }
  wchar_t *c = *cmd;
  if (*len) c[(*len)++] = L' ';
  int quote = (*arg == 0) || wcspbrk(arg, L" \t\n\v\"") != NULL;
  if (!quote) {
    while (*arg) c[(*len)++] = *arg++;
    c[*len] = 0;
    return;
  }
  c[(*len)++] = L'"';
  for (const wchar_t *a = arg;; a++) {
    size_t backslashes = 0;
    while (*a == L'\\') { a++; backslashes++; }
    if (!*a) {
      for (size_t i = 0; i < backslashes * 2; i++) c[(*len)++] = L'\\';
      break;
    } else if (*a == L'"') {
      for (size_t i = 0; i < backslashes * 2 + 1; i++) c[(*len)++] = L'\\';
      c[(*len)++] = L'"';
    } else {
      for (size_t i = 0; i < backslashes; i++) c[(*len)++] = L'\\';
      c[(*len)++] = *a;
    }
  }
  c[(*len)++] = L'"';
  c[*len] = 0;
}

static wchar_t *env_block(char *const envp[]) {
  size_t cap = 1, len = 0;
  for (char *const *e = envp; e && *e; e++)
    cap += (size_t)MultiByteToWideChar(CP_UTF8, 0, *e, -1, NULL, 0);
  wchar_t *block = malloc(sizeof(wchar_t) * (cap + 1));
  if (!block) return NULL;
  for (char *const *e = envp; e && *e; e++)
    len += (size_t)MultiByteToWideChar(CP_UTF8, 0, *e, -1, block + len, (int)(cap - len));
  block[len] = 0;
  return block;
}

CL_API int pty_cl_spawn(const char *file, char *const argv[], char *const envp[],
                        const char *cwd, uint16_t cols, uint16_t rows,
                        pty_cl_t **out, int *out_errno) {
  HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
  cl_hpcon hpc = NULL;
  wchar_t *cmd = NULL, *env = NULL, *wcwd = NULL;
  size_t cmd_len = 0, cmd_cap = 0;
  STARTUPINFOEXW si;
  PROCESS_INFORMATION pi;
  SIZE_T attr_size = 0;
  DWORD err = 0;
  COORD size;
  (void)file;

  *out = NULL;
  if (!load_conpty()) { err = ERROR_CALL_NOT_IMPLEMENTED; goto fail; }
  if (!CreatePipe(&in_read, &in_write, NULL, 0) || !CreatePipe(&out_read, &out_write, NULL, 0)) {
    err = GetLastError();
    goto fail;
  }
  size.X = (SHORT)cols;
  size.Y = (SHORT)rows;
  if (FAILED(p_create(size, in_read, out_write, 0, &hpc))) { err = GetLastError(); goto fail; }

  for (char *const *a = argv; *a; a++) {
    wchar_t *w = utf16(*a);
    if (!w) { err = ERROR_NOT_ENOUGH_MEMORY; goto fail; }
    append_arg(&cmd, &cmd_len, &cmd_cap, w);
    free(w);
  }
  env = env_block(envp);
  if (cwd && cwd[0]) wcwd = utf16(cwd);

  memset(&si, 0, sizeof si);
  si.StartupInfo.cb = sizeof si;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
  si.lpAttributeList = malloc(attr_size);
  if (!si.lpAttributeList ||
      !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) ||
      !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                 hpc, sizeof hpc, NULL, NULL)) {
    err = GetLastError();
    goto fail;
  }
  memset(&pi, 0, sizeof pi);
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                      env, wcwd, &si.StartupInfo, &pi)) {
    err = GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    goto fail;
  }
  DeleteProcThreadAttributeList(si.lpAttributeList);
  free(si.lpAttributeList);
  CloseHandle(pi.hThread);
  /* The pseudo console holds its own references. */
  CloseHandle(in_read);
  CloseHandle(out_write);
  free(cmd);
  free(env);
  free(wcwd);

  pty_cl_t *p = calloc(1, sizeof *p);
  if (!p) { TerminateProcess(pi.hProcess, 1); err = ERROR_NOT_ENOUGH_MEMORY; goto fail_late; }
  p->hpc = hpc;
  p->in_write = in_write;
  p->out_read = out_read;
  p->process = pi.hProcess;
  p->pid = pi.dwProcessId;
  p->code = -1;
  *out = p;
  *out_errno = 0;
  return 0;

fail:
  if (in_read) CloseHandle(in_read);
  if (out_write) CloseHandle(out_write);
fail_late:
  if (hpc) p_close(hpc);
  if (in_write) CloseHandle(in_write);
  if (out_read) CloseHandle(out_read);
  free(cmd);
  free(env);
  free(wcwd);
  *out_errno = (int)err;
  return err ? (int)err : -1;
}

static DWORD WINAPI closer_thread(LPVOID hpc) {
  p_close((cl_hpcon)hpc);
  return 0;
}

/* After the child exits, ConPTY keeps the output pipe open until the pseudo
 * console is closed, and ClosePseudoConsole can block until its last output
 * is read. Close it on a thread and keep reading until the pipe breaks.
 * Closing too early drops output ConPTY has not rendered yet (a quick
 * `echo` vanishes), so wait until output has been quiet for QUIET_MS. */
#define QUIET_MS 200
static void start_closing(pty_cl_t *p) {
  if (p->hpc && !p->closer) {
    p->closer = CreateThread(NULL, 0, closer_thread, p->hpc, 0, NULL);
    p->hpc = NULL;
  }
}

CL_API long pty_cl_read(pty_cl_t *p, uint8_t *buf, size_t len, int timeout_ms) {
  ULONGLONG start = GetTickCount64();
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(p->out_read, NULL, 0, NULL, &avail, NULL)) {
      DWORD e = GetLastError();
      if (e == ERROR_BROKEN_PIPE) return -1;
      p->last_errno = (int)e;
      return -2;
    }
    if (avail > 0) {
      DWORD n = 0;
      if (!ReadFile(p->out_read, buf, (DWORD)(len < avail ? len : avail), &n, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE) return -1;
        p->last_errno = (int)e;
        return -2;
      }
      p->quiet_since = GetTickCount64();
      return (long)n;
    }
    if (WaitForSingleObject(p->process, 0) == WAIT_OBJECT_0) {
      ULONGLONG now = GetTickCount64();
      if (!p->exit_seen) { p->exit_seen = 1; p->quiet_since = now; }
      if (now - p->quiet_since >= QUIET_MS) start_closing(p);
    }
    if (timeout_ms >= 0 && GetTickCount64() - start >= (ULONGLONG)timeout_ms) return 0;
    Sleep(5);
  }
}

CL_API long pty_cl_write(pty_cl_t *p, const uint8_t *buf, size_t len) {
  size_t done = 0;
  while (done < len) {
    DWORD n = 0;
    if (!WriteFile(p->in_write, buf + done, (DWORD)(len - done), &n, NULL)) {
      p->last_errno = (int)GetLastError();
      return -1;
    }
    done += n;
  }
  return (long)done;
}

CL_API int pty_cl_resize(pty_cl_t *p, uint16_t cols, uint16_t rows) {
  COORD size;
  size.X = (SHORT)cols;
  size.Y = (SHORT)rows;
  if (!p->hpc) return 0;
  if (FAILED(p_resize(p->hpc, size))) { p->last_errno = (int)GetLastError(); return -1; }
  return 0;
}

CL_API int pty_cl_signal(pty_cl_t *p, int sig) {
  if (sig == 2) {  /* INT: what Ctrl-C would send */
    uint8_t ctrl_c = 3;
    return pty_cl_write(p, &ctrl_c, 1) == 1 ? 0 : -1;
  }
  if (!TerminateProcess(p->process, 128 + (UINT)sig)) {
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED && WaitForSingleObject(p->process, 0) == WAIT_OBJECT_0) return 0;
    p->last_errno = (int)e;
    return -1;
  }
  return 0;
}

CL_API int pty_cl_wait(pty_cl_t *p, int timeout_ms, int *code, int *sig) {
  if (!p->exited) {
    DWORD r = WaitForSingleObject(p->process, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    if (r == WAIT_TIMEOUT) return 0;
    if (r != WAIT_OBJECT_0) { p->last_errno = (int)GetLastError(); return -1; }
    DWORD c = 0;
    GetExitCodeProcess(p->process, &c);
    p->exited = 1;
    p->code = (int)c;
  }
  *code = p->code;
  *sig = 0;
  return 1;
}

CL_API long pty_cl_pid(pty_cl_t *p) { return (long)p->pid; }
CL_API int pty_cl_errno(pty_cl_t *p) { return p->last_errno; }

CL_API void pty_cl_close(pty_cl_t *p) {
  if (!p) return;
  if (WaitForSingleObject(p->process, 0) != WAIT_OBJECT_0) TerminateProcess(p->process, 1);
  start_closing(p);
  /* Drain so ClosePseudoConsole can finish, then wait for the closer. */
  {
    uint8_t buf[4096];
    ULONGLONG start = GetTickCount64();
    while (pty_cl_read(p, buf, sizeof buf, 50) >= 0 && GetTickCount64() - start < 2000) {}
  }
  if (p->closer) {
    WaitForSingleObject(p->closer, 2000);
    CloseHandle(p->closer);
  }
  CloseHandle(p->in_write);
  CloseHandle(p->out_read);
  CloseHandle(p->process);
  free(p);
}
#endif
