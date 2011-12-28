/* 
 * cpulock.c
 */

#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CREATE_THREAD 0

#define MAX_RESOURCES 128

/* res group is an array of integers */
typedef struct intset {
  int sz;
  int n;
  int * a;
} intset, * intset_t;

/* request is an array of res_group + number of requested groups 
   among them */
typedef struct request {
  int n_required;   /* number of REQUIRED groups <= n_groups */
  int sz_groups;
  int n_groups;	    /* number of acceptable groups <= MAX_RESOURCES */
  intset_t * groups;		/* array of groups */
} request, * request_t;

typedef struct reply {
  char acquired[MAX_RESOURCES];	/* acquired[i] <=> resource i acquired */
} reply, * reply_t;

typedef struct res_state {
  pid_t pid;			/* lock holder pid. 0 if free */
  double since;
} res_state, * res_state_t;

typedef struct respool {
  int initialized;		/* 1 if initialization is done */
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int op_count;	  /* number of times the acquire has been completed */
  int n_resources;		/* number of resources */
  res_state states[MAX_RESOURCES]; /* 0 or 1 */
  int work_area[MAX_RESOURCES];	/* work area for testing proper
				   mutual exclusion */
} respool, * respool_t;

typedef struct cmdline_args {
  request_t req;		/* resource requested */
  request_t req_self;		/* resource requested for this process */
  char * res_file;		/* resource pool file */
  int n_configured_resources;	/* (optional) no of resources to be 
				   configured (default: no. of cores) */
  int show_resource;		/* 1 if show resource pool and exit */
  char * var;			/* environment variable to communicate
				   allocated resources */
  int bind;
  int reset;			/* reset resource file and exit */
  int release_orphan;		/* release orphan entries and exit */
  int verbosity;		/* verbosity level */
  char * progname;		/* program name */
  char ** cmd;			/* cmd to run */
} cmdline_args, * cmdline_args_t;

typedef struct global_vars {
  cmdline_args_t args;
} global_vars;

global_vars gv;

/* trivial utilities */
static void die_e(char * msg, int e) {
  fprintf(stderr, 
	  "%s: %s: %s\n", 
	  gv.args->progname, msg, strerror(e));
  exit(1);
}

static void die(char * msg) {
  die_e(msg, errno);
}

static void * my_alloc(size_t sz) {
  void * a = malloc(sz);
  if (a == NULL) die("malloc");
  return a;
}

double cur_time() {
  struct timeval tp[1];
  gettimeofday(tp, NULL);
  return tp->tv_sec + 1.0E-6 * tp->tv_usec;
}

/* 
 * intset (set of integers)
 */

/* make empty intset */
static intset_t mk_intset() {
  intset_t is = my_alloc(sizeof(intset));
  is->n = 0;
  is->sz = 4;
  is->a = my_alloc(sizeof(int) * is->sz);
  return is;
}

/* ensure is->n < is->sz */
static void intset_ensure_size(intset_t is) {
  if (is->n == is->sz) {
    int * a = is->a;
    int sz = is->sz;
    int new_sz = sz * 2;
    int * new_a = my_alloc(sizeof(int) * new_sz);
    bcopy(a, new_a, sizeof(int) * sz);
    is->sz = new_sz;
    is->a = new_a;
  }
}

/* add x to intset is */
static void intset_add(intset_t is, int x) {
  intset_ensure_size(is);
  is->a[is->n] = x;
  is->n++;
}

/* make an intset each element of which 
   is incremented by x.
   mk_shifted_set({ 0, 6, 7 }, 3) = { 3, 9, 10 };*/
intset_t mk_shifted_set(intset_t is, int x) {
  intset_t is2 = mk_intset();
  int i;
  for (i = 0; i < is->n; i++) {
    intset_add(is2, is->a[i] + x);
  }
  return is2;
}


/*  
 * request
 */

static request_t mk_request() {
  request_t req = my_alloc(sizeof(request));
  req->n_required = -1;
  req->n_groups = 0;
  req->sz_groups = 4;
  req->groups = my_alloc(sizeof(intset_t) * req->sz_groups);
  return req;
}

/* ensure REQ has enough size to add an intset */
static void request_ensure_size(request_t req) {
  if (req->n_groups == req->sz_groups) {
    intset_t * groups = req->groups;
    int sz = req->sz_groups;
    int new_sz = sz * 2;
    intset_t * new_groups = my_alloc(sizeof(intset_t) * new_sz);
    bcopy(groups, new_groups, sizeof(intset_t) * sz);
    req->sz_groups = new_sz;
    req->groups = new_groups;
  }
}

/* add set of int IS to request REQ */
static void request_add_group(request_t req, intset_t is) {
  request_ensure_size(req);
  req->groups[req->n_groups] = is;
  req->n_groups++;
}

/* make a random request.
   each element is included with probability p.
   each group has exactly one element. */
static request_t mk_random_request(unsigned short xsub[3], 
				   double p, int n_resources) {
  int i;
  request_t req = mk_request();
  for (i = 0; i < n_resources; i++) {
    if (erand48(xsub) < p) {
      intset_t g = mk_intset();
      intset_add(g, i);
      request_add_group(req, g);
    }
  }
  return req;
}

static reply_t mk_reply() {
  return my_alloc(sizeof(reply));
}

/* 
  parser for resource allocation request
  it is a slight extension to taskset 
  example:
  (1) -c 4,5,6,7 -r 2
        request any two from { 4, 5, 6, 7 }.
  (2) -c 0-3,7-9 -r 5
        request any five from { 0, 1, 2, 3, 7, 8, 9 }
  (3) -c 0/24-23/47 -r 3
      0/24 means 0 and 24 are combined;
      they are obtained together only when both are free.
      0/24-23/47 means 0 and 24 are combined, so are 1 and 25,
      2 and 26, ... up to 23 and 47.  in this case, the /47 
      may be omitted (0/24-23 has the same meaning as 0/24-23/47).
      altogether this requests
      any three pairs from { 0/24, 1/25, 2/26, ..., 23/47 }.
      for example, you may end up with obtaining 
      0, 2, 8, 24, 26, and 32, or
      3, 7, 10, 27, 31, and 34.

this is the syntax for the string that comes after -c
 
   request := range  ( ',' range )*
   range   := group  ( '-' group )?
   group   := num    ( '/' num   )* 
 */


/* character stream */
typedef struct char_stream {
  char * a;			/* underlying string */
  int i;			/* the current position */
  int ok_pos;			/* position successfully parsed so far */
} char_stream, * char_stream_t;

/* make a character stream returning characters in A */
static char_stream_t mk_char_stream(char * a) {
  char_stream_t cs = my_alloc(sizeof(char_stream));
  cs->a = a;
  cs->i = 0;
  cs->ok_pos = 0;
  return cs;
}

static inline int cur_char(char_stream_t s) {
  return s->a[s->i];
}

static inline int next_char(char_stream_t s) {
  assert(s->a[s->i] != '\n');
  s->i++;
  return cur_char(s);
}

/* mark the current position as having been parsed successfully */
static inline void set_ok_pos(char_stream_t s) {
  s->ok_pos = s->i;
}

static void parse_error(char_stream_t s, char * msg) {
  int i;
  fprintf(stderr, "%s: invalid resource list: %s\n", 
	  gv.args->progname, msg);
  fprintf(stderr, "  %s", s->a);
  for (i = 0; i < 2 + s->ok_pos; i++) fputc(' ', stderr);
  for (     ; i < 2 + s->i; i++) fputc('^', stderr);
  fputc('\n', stderr);
}

/* get a non-negative number or return -1 */
static int parse_int(char_stream_t s) {
  int x = 0;
  int n_digits = 0;
  while (isdigit(cur_char(s))) {
    n_digits++;
    x = x * 10 + (cur_char(s) - '0');
    next_char(s);
  }
  if (n_digits == 0) { 
    parse_error(s, "expected a digit"); 
    return -1; 
  }
  return x;
}

/* get an num/num/../num and return intset_t or NULL */
static intset_t parse_group(char_stream_t s) {
  intset_t group = mk_intset();
  while (1) {
    int x = parse_int(s);
    if (x == -1) return NULL;
    intset_add(group, x);
    if (cur_char(s) == '/') next_char(s);
    else break;
  } 
  return group;
}

/* get a single group (e.g., 0/6/12/24) or group-group
   e.g., (0/6/12/24-5/11/17/29).
   put the parsed range into req */
static int parse_range(char_stream_t s, request_t req) {
  intset_t g = parse_group(s);
  if (g == NULL) return -1;	/* NG */
  intset_t h = NULL;
  if (cur_char(s) == '-') {
    next_char(s);
    h = parse_group(s);
  } 
  /* now we have group - group. make sense of it and
     translate it into a list of groups */
  /* 0/24-23/47 (g = 0/24, h=23/47) */
  int range_length;
  assert(g->n >= 1);
  if (h) {
    assert(h->n >= 1);
    /* get the length of the range */
    range_length = h->a[0] - g->a[0] + 1; /* 23 - 0 + 1 */
    /* check if g and h are consistent:
       OK: 0/24-5/29
       NG: 0/24-5/30 */
    if (g->n == h->n) {
      int i;
      for (i = 0; i < g->n; i++) {
	if (h->a[i] - g->a[i] + 1 != range_length) {
	  parse_error(s, "invalid resource groups"); 
	  return -1; /* NG */
	}
      }
    } else if (h->n != 1) {
      parse_error(s, "invalid resource group"); 
      return -1; /* NG */
    }
  } else {
    range_length = 1;
  }
  {
    /* now it has been succefully parsed
       add groups, 0/24, 1/25, 2/26, ... */
    int i;
    for (i = 0; i < range_length; i++) {
      intset_t g_ = mk_shifted_set(g, i);
      request_add_group(req, g_);
    }
  }
  return 0;			/* OK */
}

/* parse the entire resource list, like:
   0,3-5,6/30-9,12/36-18,42 
   kind of string */
static request_t parse_resource_list(char * arg, request_t req) {
  char_stream_t s = mk_char_stream(arg);
  if (parse_range(s, req) == -1) return NULL;
  set_ok_pos(s);
  while (cur_char(s) == ',') {
    next_char(s);
    if (parse_range(s, req) == -1) return NULL;
    set_ok_pos(s);
  }
  if (cur_char(s) != '\0') { 
    next_char(s);
    parse_error(s, "junk at the end of resource list"); 
    return NULL; 
  }
  return req;
}

/* 
 * resource pool
 */

/* initialize resource pool object */
static int init_respool(respool_t res, int n_resources) {
  int i;
  pthread_mutexattr_t mattr;
  pthread_condattr_t cattr;
  int e;
  if ((e = pthread_mutexattr_init(&mattr))) die_e("pthread_mutexattr_init", e);
  if ((e = pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED)))
    die_e("pthread_mutexattr_setpshared", e);
  if ((e = pthread_mutex_init(&res->mutex, &mattr)))
    die_e("pthread_mutex_init", e);
  if ((e = pthread_condattr_init(&cattr)))
    die_e("pthread_condattr_init", e);
  if ((e = pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED)))
    die_e("pthread_condattr_setpshared", e);
  if ((e = pthread_cond_init(&res->cond, &cattr)) )
    die_e("pthread_cond_init", e);
  res->op_count = 0;
  res->n_resources = n_resources;
  double ct = cur_time();
  for (i = 0; i < MAX_RESOURCES; i++) {
    res->states[i].pid = 0;
    res->states[i].since = ct;
  }
  __sync_synchronize();
  res->initialized = 1;
  return 1;			/* OK */
}

int find_process(pid_t pid) {
  if (kill(pid, 0) == -1) {
    return 0;
  } else {
    return 1;
  }
}

static void release_orphans_in_respool(respool_t res) {
  double tolerance = 3.0;
  double ct = cur_time();
  int i;
  for (i = 0; i < res->n_resources; i++) {
    pid_t p = res->states[i].pid;
    double since = res->states[i].since;
    if (p && ct - since > tolerance && find_process(p) == 0) {
      fprintf(stderr, 
	      "%s: resource [%d] held by non-existing process %d since %.3f (for %.3f sec).\n"
	      "Release it\n",
	      gv.args->progname, i, p, since, ct - since);
      res->states[i].pid = 0;
      res->states[i].since = ct;
    }
  }
}

static void show_respool(respool_t res, char * filename) {
  double ct = cur_time();
  if (filename) {
    printf("filename: %s\n", filename);
  } else {
    printf("filename: (memory)\n");
  }
  printf(" initialized: %d\n", res->initialized);
  printf(" op_count: %d\n", res->op_count);
  printf(" n_resources: %d\n", res->n_resources);
  int i;
  printf(" states:\n");
  for (i = 0; i < res->n_resources; i++) {
    pid_t p = res->states[i].pid;
    if (p == 0) {
      printf("  [%d]: since %.3f free\n", i, res->states[i].since);
    } else if (find_process(p)) {
      printf("  [%d]: since %.3f (for %.3f sec) held by %d \n", 
	     i, res->states[i].since, ct - res->states[i].since, res->states[i].pid);
    } else {
      printf("! [%d]: since %.3f (for %.3f sec) held by %d (non existent)\n", 
	     i, res->states[i].since, ct - res->states[i].since, res->states[i].pid);
    }
  }
  if (0) {
    printf(" work_area:\n");
    for (i = 0; i < res->n_resources; i++) {
      printf("  [%d]: %d\n", i, res->work_area[i]);
    }
  }
}

/* try to mmap resource file
   - file must exist; otherwise it dies
   - when it exists but appears not have been initialized
   (size is zero or initialized flag not set), return NULL; 
   the caller will retry or give up 
*/
static void * try_map_respool_file(char * filename) {
  int fd = open(filename, O_RDWR);
  if (fd == -1) die("open");
  off_t sz = lseek(fd, 0, SEEK_END);
  if (sz == -1) die("lseek");
  /* size is zero. the owner must be initializing it */
  if (sz == 0) {
    fprintf(stderr, 
	    "%s: map file '%s' not initialized (size zero)\n", 
	    gv.args->progname, filename);
    close(fd);
    return NULL;
  }
  /* if sz is not zero yet it does not match the structure
     size, it is a bug or users' using different versions due to 
     changes in structure or layout */
  if (sz != sizeof(respool)) {
    fprintf(stderr, 
	    "%s: fatal: map file '%s' has a wrong size\n"
	    "(file size = %ld != sizeof(resource) = %lu).\n"
	    "If you changed resource structure or MAX_RESOURCES,\n"
	    "remove the file and do it again\n", 
	    gv.args->progname, filename, sz, sizeof(respool));
    exit(1);
  }
  /* now the file is the right size, let's map it */
  respool_t res = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (res == MAP_FAILED) die("mmap");
  /* the owner must be initializing it */
  if (res->initialized == 0) {
    if (gv.args->verbosity >= 1) {
      fprintf(stderr, 
	      "%s: map file '%s' not initialized (->initialized == 0)\n", 
	      gv.args->progname, filename);
    }
    close(fd);
    return NULL;
  }
  /* for debugging sake, show inode (to make sure everybody
     is opening the same version) */
  struct stat sbuf;
  if (fstat(fd, &sbuf) == -1) die("fstat");
  if (gv.args->verbosity >= 2) {
    fprintf(stderr, 
	    "%s: resource file mapped on %p inode=%ld\n", 
	    gv.args->progname, res, sbuf.st_ino);
  }
  return res;
}

/* try to map resource file until it sees the file to
   have been fully initialized */
static respool_t map_respool_file(char * filename) {
  respool_t res = NULL;
  int initial_sleep_time = 100;
  int tolerance = 1000 * 1000 * 10;  /* 10 sec */
  int sleep_us = initial_sleep_time;		/* 100us */
  while (sleep_us == initial_sleep_time || 
	 sleep_us < tolerance / 2) { 
    res = try_map_respool_file(filename);
    if (res) break;
    /* not yet initialized. sleep a little and retry */
    usleep(sleep_us);
    /* exponentially increase sleep time up to 1 sec. */
    sleep_us *= 2;
  }
  /* if we wait for this much but the file is still not
     initialized, we bet it will be a bug (e.g., the initializer
     has gone) */
  if (res == NULL) {
    fprintf(stderr, 
	    "%s: the file %s has not been initialized after %d sec,"
	    "it's probably a bug in initialization.\n", 
	    gv.args->progname, filename, tolerance);
    exit(1);
  }
  __sync_synchronize();
  return res;
}

/* try to create a map file (exclusively).
   if it already exists, it assumes somebody has created
   it or is initializing it, so it maps the file */
static respool_t create_or_map_respool_file(char * filename, int mode, int n_resources) {
  /* try to be the creator of the file */
  int fd = open(filename, O_CREAT | O_EXCL | O_RDWR, mode);
  if (fd == -1) { 
    if (errno == EEXIST) {
      /* it existed. just map it */
      return map_respool_file(filename);
    } else die("open");
  }
  /* I took the responsibility of creating the file */
  struct stat sbuf;
  if (fchmod(fd, mode) == -1) die("fchmod");
  if (fstat(fd, &sbuf) == -1) die("fstat");
  if (gv.args->verbosity >= 2) {
    fprintf(stderr, 
	    "%s: creating file %s inode=%ld\n", 
	    gv.args->progname, filename, sbuf.st_ino);
  }
  /* set its file size */
  if (ftruncate(fd, sizeof(respool)) == -1) die("ftruncate");
  /* create a mapping and initialize it */
  respool_t res = mmap(NULL, sizeof(respool), 
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (res == MAP_FAILED) die("mmap");
  init_respool(res, n_resources);
  return res;
}

/* if filename is given (i.e., not NULL), 
   make a respool mapped on the file.
   if filename is NULL, simply instantiate one 
   in memory (this is just for debugging)
 */
static respool_t mk_respool(char * filename, int n_resources) {
  respool_t res;
  if (filename == NULL) {
    /* instantiate a non file-backed respool */
    res = my_alloc(sizeof(respool));
    init_respool(res, n_resources);
  } else {
    /* instantiate file-backed respool, possibly creating the file anew */
    res = create_or_map_respool_file(filename, 0666, n_resources);
  }
  return res;
}


/* return 1 if all members of this group G are free in RES.
   if they are, it also sets flags in the reply structure REP,
   as well as those in the respool structure RES */
int resource_group_available(intset_t g, respool_t res, reply_t rep) {
  int i;
  for (i = 0; i < g->n; i++) {
    int r = g->a[i];
    if (res->states[r].pid) return 0;
  }
  /* set flags for resources to be allocated */
  for (i = 0; i < g->n; i++) {
    int r = g->a[i];
    if (r < res->n_resources) {
      rep->acquired[r] = 1;
    } else {
      fprintf(stderr, "%s: warning: resource request for %d ignored\n", 
	      gv.args->progname, r);
    }
  }
  return 1;
}

/* given ra->mutex is held, check if we can obtain requested resources
   and obtain them if we can. no ffect otherwise */
static int try_acquire_resources(request_t req, respool_t res, reply_t rep) {
  int i;
  int n_allocatables = 0;
  if (gv.args->verbosity >= 2) {
    fprintf(stderr, "%s: try_acquire_resources\n", gv.args->progname);
  }
  if (req->n_required > req->n_groups) {
    fprintf(stderr, 
	    "%s: warning: required resources (%d) truncated to the "
	    "specified groups (%d)\n", 
	    gv.args->progname, req->n_required, req->n_groups);
    req->n_required = req->n_groups;
  }

  for (i = 0; i < res->n_resources; i++) {
    rep->acquired[i] = 0;
  }
  /* for each resource group, check if it is allocatable */
  for (i = 0; i < req->n_groups && n_allocatables < req->n_required; i++) {
    if (resource_group_available(req->groups[i], res, rep)) {
      n_allocatables++;
    }
  }
  if (n_allocatables < req->n_required) {
    if (gv.args->verbosity >= 2) {
      fprintf(stderr, "%s: -> NG\n", gv.args->progname);
    }
    return 0;			/* NG */
  } else {
    res->op_count++;
    pid_t pid = getpid();
    double ct = cur_time();
    for (i = 0; i < MAX_RESOURCES; i++) {
      if (rep->acquired[i]) {
	res->states[i].pid = pid;
	res->states[i].since = ct;
      }
    }
    if (gv.args->verbosity >= 2) {
      fprintf(stderr, "%s: -> OK\n", gv.args->progname);
    }
    return 1;
  }
}

/* lock requested resources (block until we lock them) */
static reply_t acquire_resources(request_t req, respool_t res) {
  reply_t rep = mk_reply();
  if (gv.args->verbosity >= 2) {
    fprintf(stderr, "%s: pthread_mutex_lock\n", gv.args->progname);
  }
  pthread_mutex_lock(&res->mutex);
  release_orphans_in_respool(res);
  while (try_acquire_resources(req, res, rep) == 0) {
    pthread_cond_wait(&res->cond, &res->mutex);
  }
  if (gv.args->verbosity >= 2) {
    fprintf(stderr, "%s: pthread_mutex_unlock\n", gv.args->progname);
  }
  pthread_mutex_unlock(&res->mutex);
  return rep;			/* OK */
}

/* release requested resources */
static void release_resources(reply_t rep, respool_t res) {
  int i;
  if (gv.args->verbosity >= 2) {
    fprintf(stderr, "%s: pthread_mutex_unlock\n", gv.args->progname);
  }
  pid_t pid = getpid();
  double ct = cur_time();
  int seen_bug = 0;
  pthread_mutex_lock(&res->mutex);
  for (i = 0; i < res->n_resources; i++) {
    if (rep->acquired[i]) {
      if (res->states[i].pid != pid) {
	seen_bug = 1;
	fprintf(stderr, 
		"%s: bug! res->states[%d].pid = %d != getpid() = %d (bailout)\n",
		gv.args->progname, i, res->states[i].pid, pid);
      }
      res->states[i].pid = 0;
      res->states[i].since = ct;
    }
  }
  pthread_cond_broadcast(&res->cond);
  if (gv.args->verbosity >= 2) {
    fprintf(stderr, "%s: pthread_mutex_unlock\n", gv.args->progname);
  }
  pthread_mutex_unlock(&res->mutex);
  if (seen_bug) exit(1);
}

void clean_die(char * msg, reply_t rep, respool_t res) {
  fprintf(stderr, "%s: release resources before die\n", gv.args->progname);
  release_resources(rep, res);
  die(msg);
}

/* signal handle that gets called for SIGINT or SIGTERM */

void sigint_term_handler(int signum) {
  
}

void install_sighandlers(reply_t rep, respool_t res) {
  struct sigaction sa;
  struct sigaction sa_old;
  bzero(&sa, sizeof(struct sigaction));
  sa.sa_handler = sigint_term_handler;
  if (sigaction(SIGINT, &sa, &sa_old) == -1) {
    clean_die("sigaction", rep, res);
  }
  if (sigaction(SIGTERM, &sa, &sa_old) == -1) {
    clean_die("sigaction", rep, res);
  }
}

int show_res_file(char * filename) {
  struct stat sbuf;
  if (stat(filename, &sbuf) == -1) {
    fprintf(stderr, "%s: no resource pool file %s (%s -R will create it)\n", 
	    gv.args->progname, filename, gv.args->progname);
    return 0;			/* NG */
  }
  respool_t res = map_respool_file(filename);
  if (res == NULL) return 0;	/* NG */
  show_respool(res, filename);
  return 1;
}

int reset_res_file(char * filename, int n_resources) {
  respool_t res = create_or_map_respool_file(filename, 0666, n_resources);
  if (res == NULL) return 0;	/* NG */
  init_respool(res, n_resources);
  return 1;
}

int release_orphans_in_res_file(char * filename) {
  struct stat sbuf;
  if (stat(filename, &sbuf) == -1) {
    fprintf(stderr, "%s: no resource pool file %s\n", 
	    gv.args->progname, filename);
    return 0;			/* NG */
  }
  respool_t res = map_respool_file(filename);
  if (res == NULL) return 0;	/* NG */
  release_orphans_in_respool(res);
  return 1;
}



cmdline_args_t mk_default_cmdline_args(char * argv0) {
  cmdline_args_t args = my_alloc(sizeof(cmdline_args));
  args->req = NULL;
  args->res_file = "/tmp/cpulock_resources";
  args->n_configured_resources = -1;
  args->show_resource = 0;
  args->var = "CPUS_LOCKED";
  args->bind = 1;
  args->progname = argv0;
  args->verbosity = 0;
  args->cmd = NULL;
  return args;
}

/* END of core functions */

/* below are code for random testing */
typedef struct thread_arg {
  int id;			/* serial id of the thraed 0,1,2, ... */
  pthread_t thread_id;		/* pthread id */
  double p;
  int n_locks_per_thread;
  int n_resources;
  char * res_file;
  respool_t res;
  void * ret_val;
} thread_arg, * thread_arg_t;

static void 
acquire_random_resources_and_release(respool_t res, 
				     unsigned short * xsub, 
				     int * requested, 
				     double p, int n) {
  int i, j;
  request_t req = mk_random_request(xsub, p, res->n_resources);
  for (i = 0; i < req->n_groups; i++) {
    intset_t g = req->groups[i];
    for (j = 0; j < g->n; j++) {
      requested[g->a[j]]++;
    }
  }
  if (gv.args->verbosity >= 2) {
    fprintf(stderr, "%s: %d-th acquire\n", gv.args->progname, n);
  }
  reply_t rep = acquire_resources(req, res);
  if (gv.args->verbosity >= 2) {
    fprintf(stderr, "%s: %d-th released\n", gv.args->progname, n);
  }
  for (i = 0; i < MAX_RESOURCES; i++) {
    if (rep->acquired[i]) {
      res->work_area[i]++;
    }
  }
  release_resources(rep, res);
}

static void * thread_fun(void * arg_) {
  thread_arg_t arg = arg_;
  char * res_file = arg->res_file;
  respool_t res 
    = (res_file ? mk_respool(res_file, arg->n_resources) : arg->res);
  int requested[MAX_RESOURCES];
  unsigned short xsub[3] = { arg->id, arg->id, arg->id };
  int i;
  for (i = 0; i < MAX_RESOURCES; i++) {
    requested[i] = 0;
  }
  for (i = 0; i < arg->n_locks_per_thread; i++) {
    acquire_random_resources_and_release(res, xsub, requested, arg->p, i);
  }
  if (gv.args->verbosity >= 2) {
    fprintf(stderr, "%s: done\n", gv.args->progname);
    show_respool(res, res_file);
  }
  for (i = 0; i < MAX_RESOURCES; i++) {
    printf("%d %d\n", i, requested[i]);
  }
  return NULL;
}

static void usage_test() {
  fprintf(stderr, 
	  "usage: %s nthreads [n_locks_per_thread]"
	  " [request_intensity] [res_file] [n_resources]\n", 
	  gv.args->progname);
}

int test_main(int argc, char ** argv) {
  gv.args = mk_default_cmdline_args(argv[0]);
  if (argc == 1) { usage_test(); return 1; }
  int nthreads = atoi(argv[1]);	/* number of threads */
  int n_locks_per_thread = (argc > 2 ? atoi(argv[2]) : 100);
  /* probability that each request acquires a particular resource */
  double p = (argc > 3 ? atof(argv[3]) : 0.5); 
  char * res_file = (argc > 4 ? argv[4] : NULL);
  int n_resources = (argc > 5 ? atoi(argv[5]) : -1); /* number of resources */
  /* if res_file is given, we don't instantiate it here and each thread
     tries to map it. otherwise instantiate it here on memory anonymously
     and share it among threads */
  respool_t res = (res_file ? NULL : mk_respool(NULL, n_resources));
  int i;

  thread_arg_t args = my_alloc(sizeof(thread_arg) * nthreads);
  for (i = 0; i < nthreads; i++) {
    args[i].id = i;
    args[i].p = p;
    args[i].n_locks_per_thread = n_locks_per_thread;
    args[i].res_file = res_file;
    args[i].res = res;
    args[i].n_resources = n_resources;
  }
#if CREATE_THREAD
  for (i = 0; i < nthreads; i++) {
    pthread_create(&args[i].thread_id, NULL, thread_fun, &args[i]);
  }
  for (i = 0; i < nthreads; i++) {
    pthread_join(args[i].thread_id, &args[i].ret_val);
  }
#else
  for (i = 0; i < nthreads; i++) {
    args[i].thread_id = i;
    args[i].ret_val = thread_fun(&args[i]);
  }
#endif

  /* if res_file was given, we map it here again and show
     its work_area of that file. otherwise res is on memory
     so just show its work_area */
  if (res_file) {
    res = mk_respool(res_file, n_resources);
  } 
  show_respool(res, res_file);
  return 0;
}

static void usage() {
  fprintf(stderr, 
#include "usage.h"
);
}

static cmdline_args_t parse_cmdline(int argc, char ** argv) {
  struct option long_options[] = {
    {"cpu-list",      required_argument, 0, 'c'},
    {"required",      required_argument, 0, 'n'},
    {"resource-file", required_argument, 0, 'f'},
    {"show-resource", required_argument, 0, 's'},
    {"cpus",          required_argument, 0, 'C'},
    {"bind-self",     required_argument, 0, 'b'},
    {"var",           required_argument, 0, 'x'},
    {"verbosity",     required_argument, 0, 'v'},
    {"no-bind",       no_argument,       0, 'B'},
    {"reset-resource", no_argument,       0, 'R'},
    {"release-orphan-resource", no_argument,       0, 'r'},
    {"help",          no_argument,       0, 'h'},
    {"version",       no_argument,       0, 'V'},
    {0, 0, 0, 0}
  };
  cmdline_args_t args = mk_default_cmdline_args(argv[0]);
  request_t req = NULL;
  request_t req_self = NULL;
  while (1) {
    char * invalid_char;
    int option_index = 0;
    int c = getopt_long(argc, argv, "C:c:f:n:v:BrRshV", 
			long_options, &option_index);
    if (c == -1) break;
    switch (c) {
    case 'C':
      args->n_configured_resources = strtol(optarg, &invalid_char, 10);
      if (invalid_char == optarg) {
	fprintf(stderr, "invalid arg to -C");
	return NULL; /* NG */
      }
      break;
    case 'b':
      {
	if (req_self == NULL) req_self = mk_request();
	if (parse_resource_list(optarg, req_self) == NULL) 
	  return NULL; /* NG */
	break;
      }
    case 'c':
      {
	if (req == NULL) req = mk_request();
	if (parse_resource_list(optarg, req) == NULL) return NULL; /* NG */
	break;
      }
    case 'f':
      args->res_file = strdup(optarg);
      if (args->res_file == NULL) {
	die("strdup");
	return NULL;
      }
      break;
    case 'n':
      req->n_required = strtol(optarg, &invalid_char, 10);
      if (invalid_char == optarg) {
	fprintf(stderr, "invalid arg to -n");
	return NULL; /* NG */
      }
      break;
    case 'v':
      args->verbosity = strtol(optarg, &invalid_char, 10);
      if (invalid_char == optarg) {
	fprintf(stderr, "invalid arg to -v");
	return NULL; /* NG */
      }
      break;
    case 'B':			/* opt NOT to bind to cores */
      args->bind = 0;
      break;
    case 'r':
      args->release_orphan = 1;
      break;
    case 'R':
      args->reset = 1;
      break;
    case 's':
      args->show_resource = 1;
      break;
    case 'h':
    case 'V':
      usage(); return args;	/* not error, but no request */
    default:
      usage(); return NULL;
    }
  }
  if (args->show_resource == 0 
      && args->release_orphan == 0
      && args->reset == 0) {
    if (req == NULL) {
      /* no -c given, the first arg must be it */
      if (optind < argc) {
	req = mk_request();
	if (parse_resource_list(argv[optind], req) == NULL)
	  return NULL; /* NG */
	optind++;
      } else {
	usage(); return NULL;
      }
    }
  }
  if (req && req->n_required < 0) {
    req->n_required = req->n_groups;
  }
  /* by default the number of resources = the number of processors */
  if (args->n_configured_resources == -1) {
    args->n_configured_resources = sysconf(_SC_NPROCESSORS_ONLN);
  } else if (args->n_configured_resources > MAX_RESOURCES) {
    fprintf(stderr, "n_configured_resources truncated to %d\n", MAX_RESOURCES);
    args->n_configured_resources = MAX_RESOURCES;
  }
  /* TODO: like taskset, argv[optind] may be mask or cpu-list */
  args->req = req;
  args->req_self = req_self;
  args->cmd = argv + optind;
  return args;
}

void set_environment(reply_t rep, respool_t res, char * var) {
  int i;
  /* count the number of bytes required */
  int n_cpus = 0;
  int len = 0;
  for (i = 0; i < res->n_resources; i++) {
    if (rep->acquired[i]) {
      char buf[100];
      if (n_cpus > 0) len += 1;
      len += sprintf(buf, "%d", i);
      n_cpus++;
    }
  }

  char * buf = my_alloc(len + 1);
  char * p = buf;
  for (i = 0; i < res->n_resources; i++) {
    if (rep->acquired[i]) {
      if (p > buf) p += sprintf(p, ",");
      p += sprintf(p, "%d", i);
    }
  }

  //printf("%s=%s\n", var, buf);
  if (setenv(var, buf, 1) == -1) die("setenv");
}

void do_setaffinity(reply_t rep, respool_t res) {
  cpu_set_t mask;
  int i;
  CPU_ZERO(&mask);
  for (i = 0; i < res->n_resources; i++) {
    if (rep->acquired[i]) {
      CPU_SET(i, &mask);
    }
  }
  if (sched_setaffinity(getpid(), sizeof(cpu_set_t), &mask) == -1) {
    die("sched_setaffinity");
  }
}



int exec_program(reply_t rep, respool_t res, char * var, 
		 int setaffinity, char ** cmd) {
  pid_t pid = fork();
  if (pid == -1) clean_die("fork", rep, res);
  if (pid == 0) {
    set_environment(rep, res, var);
    if (setaffinity) do_setaffinity(rep, res);
    execvp(cmd[0], cmd);
    die("execvp");
    return 1;
  } else {
    int status;
    install_sighandlers(rep, res);
    if (waitpid(pid, &status, 0) == -1) clean_die("waitpid", rep, res);
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    } else {
      return 1;
    }
  }
}

void set_affinity_self(request_t req) {
  int i, j;
  cpu_set_t mask;
  CPU_ZERO(&mask);
  for (i = 0; i < req->n_groups; i++) {
    intset_t g = req->groups[i];
    for (j = 0; j < g->n; j++) {
      CPU_SET(g->a[j], &mask);
    }
  }
  if (sched_setaffinity(getpid(), sizeof(cpu_set_t), &mask) == -1) {
    die("sched_setaffinity");
  }
}

int main(int argc, char ** argv) {
  gv.args = mk_default_cmdline_args(argv[0]);
  cmdline_args_t args = gv.args = parse_cmdline(argc, argv);
  if (args == NULL) return 1;      /* NG */
  if (args->req_self) {
    set_affinity_self(args->req_self);
  }
  /* some special short actions */
  if (args->show_resource) {	/* show respool and exit */
    show_res_file(args->res_file);
    return 0;
  } else if (args->reset) {	/* reset respool */
    reset_res_file(args->res_file, args->n_configured_resources);
    return 0;
  } else if (args->release_orphan) {
    release_orphans_in_res_file(args->res_file);
    return 0;
  }
  if (args->req == NULL) return 0; /* help or version */
  /* real work */
  respool_t res = mk_respool(args->res_file, args->n_configured_resources);
  reply_t rep = acquire_resources(args->req, res);
  int retval = exec_program(rep, res, args->var, args->bind, args->cmd);
  release_resources(rep, res);
  return retval;
}

