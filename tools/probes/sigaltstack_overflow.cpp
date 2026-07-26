// Verifies the alternate signal stack actually lets a stack-overflow SIGSEGV be
// reported, which is the case that silently produced nothing before.
#include <signal.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char* altStack;
static void handler(int sig, siginfo_t* info, void* ctx)
{
    void* bt[32];
    int n = backtrace(bt, 32);
    // formatting on the alt stack is the thing being tested
    char msg[128];
    snprintf(msg, sizeof(msg), "handler ran on alt stack: sig=%d frames=%d addr=%p\n",
             sig, n, info ? info->si_addr : NULL);
    write(STDERR_FILENO, msg, strlen(msg));
    _Exit(42);
}
static int depth = 0;
static void recurse() { char pad[4096]; pad[0] = (char)depth++; recurse(); (void)pad; }

int main(int argc, char** argv)
{
    int useAltStack = (argc > 1 && strcmp(argv[1], "altstack") == 0);
    if (useAltStack) {
        altStack = (char*)malloc(SIGSTKSZ * 8);
        stack_t ss{}; ss.ss_sp = altStack; ss.ss_size = SIGSTKSZ * 8; ss.ss_flags = 0;
        sigaltstack(&ss, nullptr);
    }
    struct sigaction a{};
    sigfillset(&a.sa_mask);
    a.sa_flags = SA_SIGINFO | (useAltStack ? SA_ONSTACK : 0);
    a.sa_sigaction = handler;
    sigaction(SIGSEGV, &a, nullptr);
    sigaction(SIGBUS, &a, nullptr);
    recurse();
    return 0;
}
