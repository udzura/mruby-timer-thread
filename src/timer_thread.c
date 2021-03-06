#define _GNU_SOURCE 1

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/variable.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define DONE mrb_gc_arena_restore(mrb, 0);

/* OSX does not support POSIX Timer... */
#ifndef __APPLE__

/* thanks: https://github.com/ksss/mruby-signal/blob/master/src/signal.c */
static const struct signals {
  const char *signm;
  int signo;
} siglist[] = {{"EXIT", 0},
#ifdef SIGHUP
               {"HUP", SIGHUP},
#endif
               {"INT", SIGINT},
#ifdef SIGQUIT
               {"QUIT", SIGQUIT},
#endif
#ifdef SIGILL
               {"ILL", SIGILL},
#endif
#ifdef SIGTRAP
               {"TRAP", SIGTRAP},
#endif
#ifdef SIGABRT
               {"ABRT", SIGABRT},
#endif
#ifdef SIGIOT
               {"IOT", SIGIOT},
#endif
#ifdef SIGEMT
               {"EMT", SIGEMT},
#endif
#ifdef SIGFPE
               {"FPE", SIGFPE},
#endif
#ifdef SIGKILL
               {"KILL", SIGKILL},
#endif
#ifdef SIGBUS
               {"BUS", SIGBUS},
#endif
#ifdef SIGSEGV
               {"SEGV", SIGSEGV},
#endif
#ifdef SIGSYS
               {"SYS", SIGSYS},
#endif
#ifdef SIGPIPE
               {"PIPE", SIGPIPE},
#endif
#ifdef SIGALRM
               {"ALRM", SIGALRM},
#endif
#ifdef SIGTERM
               {"TERM", SIGTERM},
#endif
#ifdef SIGURG
               {"URG", SIGURG},
#endif
#ifdef SIGSTOP
               {"STOP", SIGSTOP},
#endif
#ifdef SIGTSTP
               {"TSTP", SIGTSTP},
#endif
#ifdef SIGCONT
               {"CONT", SIGCONT},
#endif
#ifdef SIGCHLD
               {"CHLD", SIGCHLD},
#endif
#ifdef SIGCLD
               {"CLD", SIGCLD},
#else
#ifdef SIGCHLD
               {"CLD", SIGCHLD},
#endif
#endif
#ifdef SIGTTIN
               {"TTIN", SIGTTIN},
#endif
#ifdef SIGTTOU
               {"TTOU", SIGTTOU},
#endif
#ifdef SIGIO
               {"IO", SIGIO},
#endif
#ifdef SIGXCPU
               {"XCPU", SIGXCPU},
#endif
#ifdef SIGXFSZ
               {"XFSZ", SIGXFSZ},
#endif
#ifdef SIGVTALRM
               {"VTALRM", SIGVTALRM},
#endif
#ifdef SIGPROF
               {"PROF", SIGPROF},
#endif
#ifdef SIGWINCH
               {"WINCH", SIGWINCH},
#endif
#ifdef SIGUSR1
               {"USR1", SIGUSR1},
#endif
#ifdef SIGUSR2
               {"USR2", SIGUSR2},
#endif
#ifdef SIGLOST
               {"LOST", SIGLOST},
#endif
#ifdef SIGMSG
               {"MSG", SIGMSG},
#endif
#ifdef SIGPWR
               {"PWR", SIGPWR},
#endif
#ifdef SIGPOLL
               {"POLL", SIGPOLL},
#endif
#ifdef SIGDANGER
               {"DANGER", SIGDANGER},
#endif
#ifdef SIGMIGRATE
               {"MIGRATE", SIGMIGRATE},
#endif
#ifdef SIGPRE
               {"PRE", SIGPRE},
#endif
#ifdef SIGGRANT
               {"GRANT", SIGGRANT},
#endif
#ifdef SIGRETRACT
               {"RETRACT", SIGRETRACT},
#endif
#ifdef SIGSOUND
               {"SOUND", SIGSOUND},
#endif
#ifdef SIGINFO
               {"INFO", SIGINFO},
#endif
               {NULL, 0}};

static int signm2signo(const char *nm)
{
  const struct signals *sigs;

  for (sigs = siglist; sigs->signm; sigs++) {
    if (strcmp(sigs->signm, nm) == 0) {
      return sigs->signo;
    }
  }

  /* Handle RT Signal#0 as special for strtol's err spec */
  if (strcmp("RT0", nm) == 0) {
    return SIGRTMIN;
  }

  if (strncmp("RT", nm, 2) == 0) {
    int ret = (int)strtol(nm + 2, NULL, 0);
    if (!ret || (SIGRTMIN + ret > SIGRTMAX)) {
      return 0;
    }
    return SIGRTMIN + ret;
  }
  return 0;
}

static int mrb_to_signo(mrb_state *mrb, mrb_value vsig)
{
  int sig = -1;
  const char *s;

  switch (mrb_type(vsig)) {
  case MRB_TT_FIXNUM:
    sig = mrb_fixnum(vsig);
    if (sig < 0 || sig >= SIGRTMAX) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "invalid signal number (%S)", vsig);
    }
    break;
  case MRB_TT_SYMBOL:
    s = mrb_sym2name(mrb, mrb_symbol(vsig));
    if (!s) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "bad signal");
    }
    goto str_signal;
  default:
    vsig = mrb_string_type(mrb, vsig);
    s = RSTRING_PTR(vsig);

  str_signal:
    if (memcmp("SIG", s, 3) == 0) {
      s += 3;
    }
    sig = signm2signo(s);
    if (sig == 0 && strcmp(s, "EXIT") != 0) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "unsupported signal");
    }
    break;
  }
  return sig;
}

struct mrb_timer_posix_thread_param {
  int signo;
  pthread_t thread_id;
};

typedef struct {
  timer_t *timer_ptr;
  int timer_signo;
  clock_t clockid;
  struct mrb_timer_posix_thread_param *thread_param_ptr;
} mrb_timer_posix_data;

static void mrb_timer_posix_free(mrb_state *mrb, void *p)
{
  mrb_timer_posix_data *data = (mrb_timer_posix_data *)p;
  timer_delete(*data->timer_ptr);
  mrb_free(mrb, data->timer_ptr);
  if (data->thread_param_ptr) {
    mrb_free(mrb, data->thread_param_ptr);
  }
  mrb_free(mrb, data);
}

static const struct mrb_data_type mrb_timer_posix_data_type = {"mrb_timer_posix_data", mrb_timer_posix_free};

static mrb_value mrb_rtsignal_get(mrb_state *mrb, mrb_value self)
{
  mrb_int idx;
  if (mrb_get_args(mrb, "i", &idx) == -1) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot get arguments");
  }

  if (SIGRTMIN + (int)idx > SIGRTMAX) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "RTSignal indx too large");
  }

  return mrb_fixnum_value(SIGRTMIN + (int)idx);
}

static void mrb_timer_posix_thread_func(union sigval sv)
{
  struct mrb_timer_posix_thread_param *param = (struct mrb_timer_posix_thread_param *)(sv.sival_ptr);
  pthread_kill(param->thread_id, param->signo);
}

#define MRB_TIMER_POSIX_KEY_SIGNO mrb_intern_lit(mrb, "signal")
#define MRB_TIMER_POSIX_KEY_CLOCK_ID mrb_intern_lit(mrb, "clock_id")
#define MRB_TIMER_POSIX_KEY_THREAD_ID mrb_intern_lit(mrb, "thread_id")

/* initialize */
static mrb_value mrb_timer_posix_init(mrb_state *mrb, mrb_value self)
{
  mrb_timer_posix_data *data;
  timer_t *timer_ptr;
  mrb_value options = mrb_nil_value();
  mrb_value has_signo_key, signo, clock_arg, thread_id_arg;
  clockid_t clockid = CLOCK_REALTIME;
  pthread_t thread_id;
  struct mrb_timer_posix_thread_param *param = NULL;

  struct sigevent sev;
  memset(&sev, 0, sizeof(struct sigevent));
  sev.sigev_signo = -1;

  if (mrb_get_args(mrb, "|o", &options) == -1) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot get arguments");
  }

  /* Parse options as hash */
  if (mrb_hash_p(options)) {
    has_signo_key = mrb_funcall(mrb, options, "has_key?", 1, mrb_symbol_value(MRB_TIMER_POSIX_KEY_SIGNO));
    if (mrb_bool(has_signo_key)) {
      signo = mrb_hash_get(mrb, options, mrb_symbol_value(MRB_TIMER_POSIX_KEY_SIGNO));
      /* handles nil as special... */
      if (mrb_nil_p(signo)) {
        sev.sigev_notify = SIGEV_NONE;
        sev.sigev_signo = 0; /* mark as active */
      } else {
        int sno = mrb_to_signo(mrb, signo);
        if (sno <= 0) {
          mrb_raise(mrb, E_ARGUMENT_ERROR, "Invalid value for signal");
        }
        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo = sno;
      }
    }

    clock_arg = mrb_hash_get(mrb, options, mrb_symbol_value(MRB_TIMER_POSIX_KEY_CLOCK_ID));
    /* has key and is not nil */
    if (mrb_fixnum_p(clock_arg)) {
      clockid = (clockid_t)mrb_fixnum(clock_arg);
    }

#ifdef SIGEV_THREAD
    thread_id_arg = mrb_hash_get(mrb, options, mrb_symbol_value(MRB_TIMER_POSIX_KEY_THREAD_ID));
    /* has key and is not nil */
    if (mrb_float_p(thread_id_arg)) {
      thread_id = (pthread_t)mrb_float(thread_id_arg);
      param = mrb_malloc(mrb, sizeof(struct mrb_timer_posix_thread_param));
      param->thread_id = thread_id;
      param->signo = sev.sigev_signo;
      if (param->signo == -1) { /* By default handling */
        param->signo = SIGALRM;
      }

      sev.sigev_notify = SIGEV_THREAD;
      sev.sigev_value.sival_ptr = (void *)param;
      sev.sigev_notify_function = mrb_timer_posix_thread_func;
    }
#endif
  }

  data = (mrb_timer_posix_data *)DATA_PTR(self);
  if (data) {
    mrb_timer_posix_free(mrb, data);
  }

  DATA_TYPE(self) = &mrb_timer_posix_data_type;
  DATA_PTR(self) = NULL;

  data = (mrb_timer_posix_data *)mrb_malloc(mrb, sizeof(mrb_timer_posix_data));
  timer_ptr = (timer_t *)mrb_malloc(mrb, sizeof(timer_t));

  if (sev.sigev_signo < 0) {
    if (timer_create(clockid, NULL, timer_ptr) == -1) {
      mrb_sys_fail(mrb, "timer_create failed");
    }
    data->timer_ptr = timer_ptr;
    data->timer_signo = SIGALRM; /* default */
  } else {
    if (timer_create(clockid, &sev, timer_ptr) == -1) {
      mrb_sys_fail(mrb, "timer_create failed");
    }
    data->timer_ptr = timer_ptr;
    data->timer_signo = sev.sigev_signo;
  }
  data->clockid = clockid;
  if (param) {
    data->thread_param_ptr = param;
  } else {
    data->thread_param_ptr = NULL;
  }

  DATA_PTR(self) = data;
  return self;
}

static int mrb_set_itimerspec(mrb_int start, mrb_int start_nsec, mrb_int interval, mrb_int interval_nsec,
                              struct itimerspec *ts)
{
  if (start < 0 || interval < 0 || ts == NULL) {
    errno = EINVAL;
    return -1;
  }

  ts->it_value.tv_sec = (time_t)start;
  ts->it_value.tv_nsec = (long)start_nsec;

  ts->it_interval.tv_sec = (time_t)interval;
  ts->it_interval.tv_nsec = (long)interval_nsec;

  return 0;
}

static mrb_value mrb_timer_posix_start(mrb_state *mrb, mrb_value self)
{
  mrb_timer_posix_data *data = DATA_PTR(self);
  mrb_int start, interval = -1;
  mrb_int s_sec, s_nsec, i_sec = 0, i_nsec = 0;
  struct itimerspec ts;

  /* start and interval should be msec */
  if (mrb_get_args(mrb, "i|i", &start, &interval) == -1) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "Cannot get arguments");
  }

  s_sec = (mrb_int)(start / 1000);
  s_nsec = (start % 1000) * 1000000;

  if (interval >= 0) {
    i_sec = (mrb_int)(interval / 1000);
    i_nsec = (interval % 1000) * 1000000;
  }

  if (mrb_set_itimerspec(s_sec, s_nsec, i_sec, i_nsec, &ts) == -1) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Values must be 0 or positive");
  }

  if (timer_settime(*(data->timer_ptr), 0, &ts, NULL) == -1) {
    mrb_sys_fail(mrb, "timer_settime");
  }

  return self;
}

static mrb_value mrb_timer_posix_stop(mrb_state *mrb, mrb_value self)
{
  mrb_timer_posix_data *data = DATA_PTR(self);
  struct itimerspec ts;
  if (mrb_set_itimerspec(0, 0, 0, 0, &ts) == -1) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Invalid value for stop");
  }

  if (timer_settime(*(data->timer_ptr), 0, &ts, NULL) == -1) {
    mrb_sys_fail(mrb, "timer_settime");
  }

  return self;
}

static mrb_value mrb_timer_posix_status_raw(mrb_state *mrb, mrb_value self)
{
  mrb_timer_posix_data *data = DATA_PTR(self);
  struct itimerspec ts;
  mrb_value ret;

  if (timer_gettime(*(data->timer_ptr), &ts) == -1) {
    mrb_sys_fail(mrb, "timer_gettime");
  }

  ret = mrb_hash_new_capa(mrb, 2);
  mrb_hash_set(mrb, ret, mrb_str_new_lit(mrb, "value.sec"), mrb_fixnum_value((mrb_int)ts.it_value.tv_sec));
  mrb_hash_set(mrb, ret, mrb_str_new_lit(mrb, "value.nsec"), mrb_fixnum_value((mrb_int)ts.it_value.tv_nsec));
  mrb_hash_set(mrb, ret, mrb_str_new_lit(mrb, "interval.sec"), mrb_fixnum_value((mrb_int)ts.it_interval.tv_sec));
  mrb_hash_set(mrb, ret, mrb_str_new_lit(mrb, "interval.nsec"), mrb_fixnum_value((mrb_int)ts.it_interval.tv_nsec));

  return ret;
}

static mrb_value mrb_timer_posix_is_running(mrb_state *mrb, mrb_value self)
{
  mrb_timer_posix_data *data = DATA_PTR(self);
  struct itimerspec ts;

  if (timer_gettime(*(data->timer_ptr), &ts) == -1) {
    mrb_sys_fail(mrb, "timer_gettime");
  }
  return mrb_bool_value(ts.it_value.tv_sec || ts.it_value.tv_nsec);
}

static mrb_value mrb_timer_posix_signo(mrb_state *mrb, mrb_value self)
{
  mrb_timer_posix_data *data = DATA_PTR(self);
  int signo = data->timer_signo;
  if (signo > 0) {
    return mrb_fixnum_value(data->timer_signo);
  } else {
    return mrb_nil_value();
  }
}

static mrb_value mrb_timer_posix_clockid(mrb_state *mrb, mrb_value self)
{
  mrb_timer_posix_data *data = DATA_PTR(self);
  return mrb_fixnum_value((int)data->clockid);
}

#define EXPORT_CLOCK_CONST(name) mrb_define_const(mrb, timer, #name, mrb_fixnum_value(name))

void mrb_mruby_timer_thread_gem_init(mrb_state *mrb)
{
  struct RClass *rtsignal, *timer, *posix;
  rtsignal = mrb_define_module(mrb, "RTSignal");
  mrb_define_module_function(mrb, rtsignal, "get", mrb_rtsignal_get, MRB_ARGS_REQ(1));

  timer = mrb_define_module(mrb, "Timer");

  posix = mrb_define_class_under(mrb, timer, "POSIX", mrb->object_class);
  MRB_SET_INSTANCE_TT(posix, MRB_TT_DATA);
  mrb_define_method(mrb, posix, "initialize", mrb_timer_posix_init, MRB_ARGS_ARG(0, 1));
  mrb_define_method(mrb, posix, "start", mrb_timer_posix_start, MRB_ARGS_ARG(1, 1));
  mrb_define_method(mrb, posix, "stop", mrb_timer_posix_stop, MRB_ARGS_NONE());
  mrb_define_method(mrb, posix, "__status_raw", mrb_timer_posix_status_raw, MRB_ARGS_NONE());
  mrb_define_method(mrb, posix, "running?", mrb_timer_posix_is_running, MRB_ARGS_NONE());

  mrb_define_method(mrb, posix, "signo", mrb_timer_posix_signo, MRB_ARGS_NONE());
  mrb_define_method(mrb, posix, "clock_id", mrb_timer_posix_clockid, MRB_ARGS_NONE());

  EXPORT_CLOCK_CONST(CLOCK_REALTIME);
  EXPORT_CLOCK_CONST(CLOCK_MONOTONIC);
  EXPORT_CLOCK_CONST(CLOCK_PROCESS_CPUTIME_ID);
  EXPORT_CLOCK_CONST(CLOCK_THREAD_CPUTIME_ID);
#ifdef CLOCK_BOOTTIME
  EXPORT_CLOCK_CONST(CLOCK_BOOTTIME);
#endif
#ifdef CLOCK_REALTIME_ALARM
  EXPORT_CLOCK_CONST(CLOCK_REALTIME_ALARM);
#endif
#ifdef CLOCK_BOOTTIME_ALARM
  EXPORT_CLOCK_CONST(CLOCK_BOOTTIME_ALARM);
#endif

  DONE;
}

#else

static mrb_value mrb_timer_posix_dummy_start(mrb_state *mrb, mrb_value self)
{
  mrb_raise(mrb, E_NOTIMP_ERROR, "Unsupported platform");
  return mrb_nil_value();
}

void mrb_mruby_timer_thread_gem_init(mrb_state *mrb)
{
  struct RClass *timer, *posix;
  timer = mrb_define_module(mrb, "Timer");
  posix = mrb_define_class_under(mrb, timer, "POSIX", mrb->object_class);
  mrb_define_method(mrb, posix, "start", mrb_timer_posix_dummy_start, MRB_ARGS_ANY());
  DONE;
}

#endif

void mrb_mruby_timer_thread_gem_final(mrb_state *mrb)
{
}
