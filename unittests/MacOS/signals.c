#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <unistd.h>

static volatile int fixed_faults = 0;
static char* guarded_page = NULL;
static volatile sig_atomic_t alarm_hits = 0;
static volatile sig_atomic_t thread_hits = 0;
static volatile int used_alt_stack = 0;
static char alt_stack_memory[65536];
static sigjmp_buf ill_recover;

static void fix_protection(int signal_number, siginfo_t* info, void* context) {
  if ((char*)info->si_addr >= guarded_page && (char*)info->si_addr < guarded_page + 16384) {
    mprotect(guarded_page, 16384, PROT_READ | PROT_WRITE);
    fixed_faults++;
  }
}

static void skip_instruction(int signal_number, siginfo_t* info, void* raw_context) {
  ucontext_t* context = raw_context;
  char local_marker;
  used_alt_stack = (&local_marker >= alt_stack_memory && &local_marker < alt_stack_memory + sizeof(alt_stack_memory));
  context->uc_mcontext->__ss.__rip += 2;
  context->uc_mcontext->__ss.__rax = 1234;
}

static void on_alarm(int signal_number) { alarm_hits++; }
static void on_usr2(int signal_number) { thread_hits++; }
static void on_ill(int signal_number) { siglongjmp(ill_recover, 1); }

static void* sleepy_thread(void* unused) {
  while (thread_hits == 0) usleep(1000);
  return NULL;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = fix_protection;
  action.sa_flags = SA_SIGINFO;
  sigaction(SIGBUS, &action, NULL);
  sigaction(SIGSEGV, &action, NULL);
  guarded_page = mmap(NULL, 16384, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  guarded_page[100] = 42;
  printf("fault fixed %d value %d\n", fixed_faults, guarded_page[100]);

  stack_t alt_stack = {.ss_sp = alt_stack_memory, .ss_size = sizeof(alt_stack_memory), .ss_flags = 0};
  sigaltstack(&alt_stack, NULL);
  action.sa_sigaction = skip_instruction;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &action, NULL);
  sigaction(SIGBUS, &action, NULL);
  long rax_after = 0;
  __asm__ volatile("xor %%eax, %%eax\n movl (%%rax), %%eax\n" : "=a"(rax_after) : : "memory");
  printf("skipped fault rax %ld alt stack %d\n", rax_after, used_alt_stack);

  signal(SIGALRM, on_alarm);
  struct itimerval timer = {.it_interval = {0, 2000}, .it_value = {0, 2000}};
  setitimer(ITIMER_REAL, &timer, NULL);
  volatile uint64_t spin = 0;
  while (alarm_hits < 5) spin++;
  struct itimerval stop = {0};
  setitimer(ITIMER_REAL, &stop, NULL);
  printf("alarm hits during busy loop %d\n", alarm_hits >= 5);

  signal(SIGUSR2, on_usr2);
  pthread_t helper;
  pthread_create(&helper, NULL, sleepy_thread, NULL);
  usleep(10000);
  pthread_kill(helper, SIGUSR2);
  pthread_join(helper, NULL);
  printf("thread got usr2 %d\n", thread_hits);

  sigset_t block_usr2, old_mask;
  sigemptyset(&block_usr2);
  sigaddset(&block_usr2, SIGUSR2);
  sigprocmask(SIG_BLOCK, &block_usr2, &old_mask);
  raise(SIGUSR2);
  int before_unblock = thread_hits;
  sigset_t pending;
  sigpending(&pending);
  int was_pending = sigismember(&pending, SIGUSR2);
  sigprocmask(SIG_SETMASK, &old_mask, NULL);
  printf("blocked usr2 pending %d delivered after unblock %d\n", was_pending, thread_hits == before_unblock + 1);

  signal(SIGILL, on_ill);
  if (sigsetjmp(ill_recover, 1) == 0) {
    __asm__ volatile("ud2");
    printf("ud2 did nothing??\n");
  } else {
    printf("caught ud2\n");
  }
  return 0;
}
