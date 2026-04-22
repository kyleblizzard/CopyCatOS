// CopyCatOS — by Kyle Blizzard at Blizzard.show

// moonbase-app-stub — the static ELF at the head of every single-file
// `.app`. The .app's on-disk layout is:
//
//   [stub bytes] [squashfs image] [appimg trailer]
//
// When the user runs a .app, the kernel loads this stub at entry. The
// stub's only job is to hand control to moonbase-launch, passing the
// absolute path of the .app file as the bundle argument.
//
// Built -nostdlib -static -fno-pic so:
//   * no libc → no glibc version coupling → same stub works on every
//     CopyCatOS host, Fedora / Arch / Debian-derived, today and five
//     years from now.
//   * no PIE / no relocations → the ELF is ~2 KiB and the packer can
//     concatenate it verbatim.
//   * no startup files → _start is provided inline below, reads
//     argc/argv/envp off the stack per the x86_64 System V ABI, and
//     hands them to stub_main().
//
// Architecture note: this file is x86_64-only (Legion Go S + Intel
// MacBook Pro are both x86_64). ARM64 support is a follow-up — the
// syscall numbers and the entry-asm change, nothing else.
//
// Why exec via /bin/sh instead of rolling PATH search ourselves:
// `execve` doesn't do PATH lookup; we'd have to iterate $PATH by
// hand in nostdlib, which is ~80 LoC of fiddly string handling. The
// shell already has bulletproof PATH search. The fork-exec cost is
// ~1 ms and moonbase-launch runs for the lifetime of the app. POSIX
// requires /bin/sh; every Linux distro ships it. The stub stays
// tiny; the cost is a single extra exec.

#if !defined(__x86_64__)
#error "moonbase-app-stub only supports x86_64 in v1"
#endif

typedef long ssize_t;
typedef unsigned long size_t;

// Linux x86_64 syscall numbers.
#define SYS_write     1
#define SYS_readlink  89
#define SYS_execve    59
#define SYS_exit      60

// --------------------------------------------------------------------
// syscall wrappers
// --------------------------------------------------------------------
//
// x86_64 syscall calling convention:
//   rax = syscall number; rdi, rsi, rdx, r10, r8, r9 = args 1..6.
//   rcx and r11 are clobbered by the `syscall` instruction.
//
// Return values > -4096 are treated as errors (negated errno) per the
// kernel ABI. The stub uses `sys_*` functions' raw return directly and
// doesn't bother recovering errno — error paths just bail with a
// fixed diagnostic.

static inline long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "0"(n), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return ret;
}

static ssize_t sys_write(int fd, const void *buf, size_t n) {
    return (ssize_t)sys3(SYS_write, (long)fd, (long)buf, (long)n);
}

static ssize_t sys_readlink(const char *path, char *buf, size_t cap) {
    return (ssize_t)sys3(SYS_readlink, (long)path, (long)buf, (long)cap);
}

static long sys_execve(const char *filename, char *const argv[],
                       char *const envp[]) {
    return sys3(SYS_execve, (long)filename, (long)argv, (long)envp);
}

__attribute__((noreturn))
static void sys_exit(int code) {
    __asm__ volatile("syscall" :: "a"((long)SYS_exit), "D"((long)code));
    __builtin_unreachable();
}

// --------------------------------------------------------------------
// minimal libc substitutes
// --------------------------------------------------------------------

static size_t s_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

__attribute__((noreturn))
static void die(const char *msg) {
    sys_write(2, msg, s_len(msg));
    sys_exit(127);
}

// --------------------------------------------------------------------
// stub_main — resolve /proc/self/exe, exec /bin/sh
// --------------------------------------------------------------------
//
// argv[0] is ignored — it's the name the kernel used to launch this
// stub (e.g. "./TextEdit.app" or a PATH-resolved absolute path), and
// we want the canonical resolved path of the `.app` file itself.
// readlink("/proc/self/exe") gives us that. Everything from argv[1]
// onwards is forwarded verbatim.
//
// The shell gets:
//   /bin/sh -c 'exec moonbase-launch "$0" "$@"' <bundle-path> <args...>
//
// Inside the shell, $0 is <bundle-path>, "$@" is the original <args>,
// and `exec moonbase-launch "$0" "$@"` lets the shell do its own PATH
// lookup for moonbase-launch and exec-replaces its own PID so the
// launcher becomes the .app's process (no orphaned shell).

// Upper bound on forwarded argc. Linux's MAX_ARG_STRINGS is 131072 but
// practical limits on argv size are much lower (kernel enforces 128
// KiB of argv+envp combined by default). 4096 argv slots is already
// well past any real-world launcher invocation; keeping it bounded
// lets us use a static buffer so the stub touches zero heap.
#define STUB_MAX_ARGV 4096

// PATH_MAX on Linux is 4096; 4097 covers the trailing NUL.
#define STUB_PATH_MAX 4097

static char exe_path[STUB_PATH_MAX];
static char *new_argv[STUB_MAX_ARGV];

int stub_main(int argc, char **argv, char **envp) {
    // argv[0] is intentionally unused — we re-derive the canonical
    // bundle path via readlink("/proc/self/exe") below, since argv[0]
    // is only as trustworthy as whatever string the launcher put
    // there. argv[1..] carries the user's forwarded flags.

    // Resolve /proc/self/exe → canonical path to the .app file. After
    // the execve to /bin/sh below, /proc/self/exe would point at sh,
    // so this has to happen first.
    ssize_t n = sys_readlink("/proc/self/exe", exe_path, STUB_PATH_MAX - 1);
    if (n <= 0) die("moonbase-app-stub: readlink /proc/self/exe failed\n");
    exe_path[n] = '\0';

    // Bound check: 4 fixed argv slots (sh, -c, script, bundle-path)
    // plus the (argc - 1) forwarded args plus a NULL terminator.
    if (argc < 1 || argc - 1 > STUB_MAX_ARGV - 5) {
        die("moonbase-app-stub: too many arguments\n");
    }

    new_argv[0] = (char *)"sh";
    new_argv[1] = (char *)"-c";
    new_argv[2] = (char *)"exec moonbase-launch \"$0\" \"$@\"";
    new_argv[3] = exe_path;
    // Forward argv[1..argc-1] to positions 4..argc+2.
    for (int i = 1; i < argc; i++) {
        new_argv[3 + i] = argv[i];
    }
    new_argv[3 + argc] = (char *)0;

    sys_execve("/bin/sh", new_argv, envp);

    // If execve returned, it failed. /bin/sh should always be present
    // on a POSIX system; if it isn't, this host is broken regardless
    // of CopyCatOS.
    die("moonbase-app-stub: execve /bin/sh failed (is /bin/sh missing?)\n");
}

// --------------------------------------------------------------------
// _start — handwritten entry
// --------------------------------------------------------------------
//
// x86_64 System V process entry state:
//   rsp -> argc
//   rsp+8 -> argv[0]
//   rsp+8+(argc+1)*8 -> envp[0]
//
// Load each into the usual arg registers and call stub_main. If
// stub_main returns (it shouldn't — execve or die), use its return
// value as the exit code.
//
// Emitted as a top-level naked assembly block (not __attribute__((naked)),
// which GCC doesn't accept on x86_64). The .global makes the symbol
// visible to ld so -e _start resolves; .type marks it as a function
// for correct DWARF/CFI handling (harmless even with -Wl,--build-id=none).

__asm__ (
    ".global _start\n"
    ".type _start, @function\n"
    "_start:\n"
    "    xor %rbp, %rbp\n"                 // clear frame pointer
    "    mov (%rsp), %rdi\n"               // argc
    "    lea 8(%rsp), %rsi\n"              // argv
    "    lea 8(%rsi, %rdi, 8), %rdx\n"     // envp = argv + (argc+1)*8
    "    and $-16, %rsp\n"                 // align stack to 16 bytes
    "    call stub_main\n"
    "    mov %rax, %rdi\n"                 // exit code = stub_main ret
    "    mov $60, %rax\n"                  // SYS_exit
    "    syscall\n"
    "    hlt\n"                            // unreachable
);
