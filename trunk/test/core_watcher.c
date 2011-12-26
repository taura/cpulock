/* 
 * core_watcher.c
 *
 * ./core_watcher SERIAL_ID N_THREADS TIME
 */


#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

double cur_time() {
  struct timeval tp[1];
  gettimeofday(tp, NULL);
  return tp->tv_sec + 1.0E-6 * tp->tv_usec;
}

typedef struct quantum_record {
  double a;			/* from */
  double b;			/* to */
  int cpu;			/* on */
} quantum_record, * quantum_record_t;

#define MAX_RECORDS 10000

void bind_to_core(int core) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(core, &mask);
  if (sched_setaffinity(syscall(SYS_gettid), sizeof(cpu_set_t), &mask) == -1) {
    perror("sched_setaffinity"); exit(1);
  }
}

typedef struct thread_arg {
  int id;
  int tid;
  int cpu;
  double dt;
  pthread_t pthread_id;
} * thread_arg_t;

void * thread_main(void * arg_) {
  thread_arg_t arg = arg_;
  int core = arg->cpu;
  if (core != -1) {
    bind_to_core(core);
  }
  int id = arg->id;
  int tid = arg->tid;
  double dt = arg->dt;
  quantum_record_t qr = malloc(MAX_RECORDS * sizeof(quantum_record));
  bzero(qr, MAX_RECORDS * sizeof(quantum_record));

  double start_t = cur_time();
  int start_core = sched_getcpu();

  double last_t = start_t;	/* last time we saw the clock */
  double quantum_began = start_t; /* the time at which we think the current
				    time quantum began */
  int quantum_core = start_core;

  int n_quantums = 0;
  while (last_t < start_t + dt) {
    double t = cur_time();
    int core = sched_getcpu();
    if (t - last_t >= 1.0E-4 || core != quantum_core) {
      /* I have not run for some time (> 1.us).
	 somebody else running on this core? */
      qr[n_quantums].a = quantum_began;
      qr[n_quantums].b = last_t;
      qr[n_quantums].cpu = quantum_core;
      n_quantums++;
      /* new quantum just began */
      quantum_began = t;
      quantum_core = core;
    }
    last_t = t;
    if (n_quantums == MAX_RECORDS) break;
  }
  if (n_quantums < MAX_RECORDS) {
    qr[n_quantums].a = quantum_began;
    qr[n_quantums].b = last_t;
    qr[n_quantums].cpu = quantum_core;
    n_quantums++;
  }

  int i;
  for (i = 0; i < n_quantums; i++) {
    printf("run_%d,%f,%f,%d,id=%d;tid=%d\n", 
	   id, qr[i].a, qr[i].b, qr[i].cpu, id, tid);
  }
  return 0;
}
  
int * parse_cpus() {
  char * cpus_locked = getenv("CPUS_LOCKED");
  if (cpus_locked == NULL) { 
    fprintf(stderr, "environment variable CPUS_LOCKED not set\n"); 
    exit(1); 
  }
  int n = 0;
  char * p = cpus_locked;
  while (p) {
    n++;
    p = strchr(p, ',');
    if (p) p++;
  }
  int * a = malloc(sizeof(int) * (n + 1));
  if (a == NULL) { perror("malloc"); exit(1); }
  int i;
  p = cpus_locked;
  for (i = 0; i < n; i++) {
    char * q = strchr(p, ',');
    if (i < n - 1) {
      assert(q);
      a[i] = atoi(strndup(p, q - p));
      p = q + 1;
    } else {
      a[i] = atoi(strdup(p));
    }
  }
  a[n] = -1;
  return a;
}


int main(int argc, char ** argv) {
  int id = atoi(argv[1]);
  int n_threads = atoi(argv[2]);
  double dt = atof(argv[3]);
  thread_arg_t args = malloc(sizeof(struct thread_arg) * n_threads);
  int * cpus = parse_cpus();
  int i;
  for (i = 0; i < n_threads; i++) {
    args[i].id = id;
    args[i].tid = i;
    args[i].dt = dt;
    args[i].cpu = cpus[i];
    pthread_create(&args[i].pthread_id, NULL, thread_main, &args[i]);
  }
  for (i = 0; i < n_threads; i++) {
    void * retval;
    pthread_join(args[i].pthread_id, &retval);
  }
  return 0;
}
  
