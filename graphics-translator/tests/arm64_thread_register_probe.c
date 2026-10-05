#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>

static volatile uint64_t signal_x18;
static volatile sig_atomic_t delivered;

static void handler(int signal, siginfo_t *info, void *context) {
  (void)signal;
  (void)info;
  signal_x18 = ((ucontext_t *)context)->uc_mcontext->__ss.__x[18];
  delivered = 1;
}

static void *probe(void *arg) {
  const int clobber_el0 = arg != NULL;
  struct sigaction action = {0};
  uint64_t original, original_thread_id, before, after;
  const uint64_t value = UINT64_C(0x123456789abcdef0);
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  delivered = 0;
  if (sigaction(SIGUSR1, &action, NULL)) return (void *)(intptr_t)2;
  __asm__ volatile("mov %0, x18" : "=r"(original));
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(original_thread_id));
  __asm__ volatile("mov x18, %0" :: "r"(value));
  if (clobber_el0) __asm__ volatile("msr tpidr_el0, %0" :: "r"(value));
  __asm__ volatile("mov %0, x18" : "=r"(before));
  int status = raise(SIGUSR1);
  __asm__ volatile("mov %0, x18" : "=r"(after));
  __asm__ volatile("msr tpidr_el0, %0" :: "r"(original_thread_id));
  __asm__ volatile("mov x18, %0" :: "r"(original));
  int pass = !status && delivered && before == value && signal_x18 == value && after == (clobber_el0 ? 0 : value);
  printf("{\"x18_preserved\":%s,\"before\":\"%016" PRIx64 "\",\"signal\":\"%016" PRIx64 "\",\"after\":\"%016" PRIx64 "\"}\n",
         after == value ? "true" : "false", before, signal_x18, after);
  return (void *)(intptr_t)(pass ? 0 : 1);
}

int main(int argc, char **argv) {
  pthread_t thread;
  void *result;
  int clobber_el0 = argc == 2 && !strcmp(argv[1], "--clobber-el0");
  if (argc > 2 || (argc == 2 && !clobber_el0)) return 2;
  void *arg = clobber_el0 ? &clobber_el0 : NULL;
  if (probe(arg)) return 1;
  if (pthread_create(&thread, NULL, probe, arg) || pthread_join(thread, &result)) return 2;
  return (int)(intptr_t)result;
}
