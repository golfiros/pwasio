/*
Copyright (C) 2006 Robert Reif
Portions copyright (C) 2007 Ralf Beck
Portions copyright (C) 2007 Johnny Petrantoni
Portions copyright (C) 2007 Stephane Letz
Portions copyright (C) 2008 William Steidtmann
Portions copyright (C) 2010 Peter L Jones
Portions copyright (C) 2010 Torben Hohn
Portions copyright (C) 2010 Nedko Arnaudov
Portions copyright (C) 2013 Joakim Hernberg
Portions copyright (C) 2020-2023 Filipe Coelho
Portions copyright (C) 2025 Gabriel Golfetti

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "pwasio.h"
#include "asio.h"
#include "pipewire/filter.h"
#include "pipewire/keys.h"
#include "pipewire/properties.h"
#include "resource.h"
#include "spa/utils/dict.h"

#include <pipewire/pipewire.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <shlwapi.h>
#include <winuser.h>

#undef strncpy
#define strncpy lstrcpynA
#include <spa/param/audio/format-utils.h>

WINE_DEFAULT_DEBUG_CHANNEL(pwasio);

#define MAX_NAME 32
#define MAX_PORTS 32
#define pwasio_err(code, msg, ...)                                             \
  do {                                                                         \
    WINE_ERR(msg "\n" __VA_OPT__(, ) __VA_ARGS__);                             \
    sprintf(pwasio->err_msg, "%s: " msg "\n",                                  \
            __func__ __VA_OPT__(, ) __VA_ARGS__);                              \
    return code;                                                               \
  } while (false)

#define KEY_N_INPUTS "n_inputs"
#define KEY_N_OUTPUTS "n_outputs"
#define KEY_BUFSIZE "buffer_size"
#define KEY_SMPRATE "sample_rate"
#define KEY_AUTOCON "autoconnect"
#define KEY_PRIORITY "priority"
#define KEY_HOST_PRIORITY "host_priority"

#define DEFAULT_N_INPUTS 2
#define DEFAULT_N_OUTPUTS 2
#define DEFAULT_BUFSIZE 256
#define DEFAULT_SMPRATE 48000
#define DEFAULT_AUTOCON 1
#define DEFAULT_PRIORITY 0

struct channel {
  struct asio_channel_info info;
  struct channel **port;
  size_t offset[2];
  struct pw_buffer *buffer[2];
};

struct thread {
  HANDLE handle;
  DWORD thread_id;
  pthread_t tid;
  int priority;
  atomic_bool ready;

  void *(*start)(void *);
  void *arg, *ret;
};

static DWORD WINAPI _thread_func(LPVOID p) {
  struct thread *t = p;
  t->tid = pthread_self();
  atomic_store_explicit(&t->ready, true, memory_order_release);
  t->ret = t->start(t->arg);
  return 0;
}
static struct spa_thread *_thread_utils_create(void *_data,
                                               const struct spa_dict *,
                                               void *(*start)(void *),
                                               void *arg) {
  struct thread *t = _data;

  t->start = start;
  t->arg = arg;
  t->handle = CreateThread(nullptr, 0, _thread_func, t, 0, &t->thread_id);
  if (!t->handle)
    return nullptr;

  while (!atomic_load_explicit(&t->ready, memory_order_acquire))
    sched_yield();

  return (struct spa_thread *)t->tid;
}
static int _thread_utils_join(void *_data, struct spa_thread *, void **retval) {
  struct thread *t = _data;
  if (!t->handle)
    return -1;

  DWORD result = WaitForSingleObject(t->handle, INFINITE);
  if (retval)
    *retval = t->ret;

  CloseHandle(t->handle);
  return (result == WAIT_OBJECT_0) ? 0 : -1;
}
static int _thread_utils_get_rt_range(void *, const struct spa_dict *, int *min,
                                      int *max) {
  *min = THREAD_PRIORITY_NORMAL;
  *max = THREAD_PRIORITY_TIME_CRITICAL;
  return 0;
}
static int _thread_utils_acquire_rt(void *_data, struct spa_thread *,
                                    int priority) {
  struct thread *t = _data;
  int err = 0;
  if (t->priority && priority == -1) {
    WINE_TRACE("setting driver scheduler to SCHED_FIFO with priority %d\n",
               t->priority);
    if ((err = pthread_setschedparam(
             t->tid, SCHED_FIFO,
             &(struct sched_param){.sched_priority = t->priority})))
      WINE_ERR("%s\n", strerror(err));
  }
  return err;
}
static int _thread_utils_drop_rt(void *_data, struct spa_thread *) {
  struct thread *t = _data;
  if (t->priority) {
    WINE_TRACE("setting driver scheduler to SCHED_OTHER\n");
    int err;
    if ((err = pthread_setschedparam(
             t->tid, SCHED_OTHER, &(struct sched_param){.sched_priority = 0})))
      WINE_ERR("%s\n", strerror(err));
  }
  return 0;
}

struct pwasio {
  const struct asioVtbl *vtbl;
  LONG32 ref;
  HINSTANCE hinst;

  char err_msg[256];
  char name[MAX_NAME];

  size_t n_in, n_out;
  struct channel channels[MAX_PORTS];
  size_t buffer_size, sample_rate;

  bool autoconnect;

  struct {
    struct pw_thread_loop *loop;
    struct pw_context *context;

    pthread_t host_tid;
    int host_priority;
    struct spa_thread_utils thread_utils;
    struct thread thread;
  } context;

  struct pw_data_loop *loop;
  struct pw_filter *filter;

  size_t idx, pos, nsec;

  int fd;
  size_t fsize;
  float *buffer;

  bool running;

  struct asio_callbacks *callbacks;

  HANDLE panel;
  HWND dialog;
};

STDMETHODIMP QueryInterface(struct asio *_data, REFIID riid, PVOID *out) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (out == nullptr)
    return E_INVALIDARG;

  if (IsEqualIID(&class_id, riid)) {
    InterlockedIncrement(&pwasio->ref);
    *out = pwasio;
    return S_OK;
  }

  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG32) AddRef(struct asio *_data) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;
  return InterlockedIncrement(&pwasio->ref);
}

STDMETHODIMP_(ULONG32) Release(struct asio *_data) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (InterlockedDecrement(&pwasio->ref))
    return pwasio->ref;

  if (pwasio->panel) {
    if (pwasio->dialog)
      PostMessageA(pwasio->dialog, WM_COMMAND, IDCANCEL, 0);
    WaitForSingleObject(pwasio->panel, 3000);
    CloseHandle(pwasio->panel);
  }

  if (pwasio->fd >= 0)
    pwasio->vtbl->DisposeBuffers(_data);
  if (pwasio->context.loop) {
    pw_loop_invoke(pw_thread_loop_get_loop(pwasio->context.loop), nullptr, 0,
                   nullptr, 0, false, nullptr);
    pw_thread_loop_stop(pwasio->context.loop);
  }
  if (pwasio->filter)
    pw_filter_destroy(pwasio->filter);
  if (pwasio->context.context)
    pw_context_destroy(pwasio->context.context);
  if (pwasio->context.loop)
    pw_thread_loop_destroy(pwasio->context.loop);

  if (pwasio->context.host_priority) {
    WINE_TRACE("setting host scheduler to SCHED_OTHER\n");
    pthread_setschedparam(pwasio->context.host_tid, SCHED_OTHER,
                          &(struct sched_param){.sched_priority = 0});
  }

  WINE_TRACE("stopping PipeWire\n");
  pw_deinit();

  HeapFree(GetProcessHeap(), 0, pwasio);

  return 0;
}

void _add_buffer(void *_data, void *_port, struct pw_buffer *buf) {
  struct pwasio *pwasio = _data;
  struct channel *channel = *(struct channel **)_port;

  struct spa_data *data = &buf->buffer->datas[0];
  if (!channel->buffer[0]) {
    channel->buffer[0] = buf;
    data->mapoffset = channel->offset[0];
  } else if (!channel->buffer[1]) {
    channel->buffer[1] = buf;
    data->mapoffset = channel->offset[1];
  } else {
    WINE_WARN("extra buffer\n");
    return;
  }
  data->type = SPA_DATA_MemFd;
  data->flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;
  data->fd = pwasio->fd;
  data->maxsize = pwasio->buffer_size * sizeof *pwasio->buffer;
}
void _remove_buffer(void *, void *_port, struct pw_buffer *buf) {
  struct channel *channel = *(struct channel **)_port;
  if (buf == channel->buffer[0])
    channel->buffer[0] = nullptr;
  if (buf == channel->buffer[1])
    channel->buffer[1] = nullptr;
}

/* Paranoid driver developers should assume that the application will access
 * buffer 0 as soon as bufferSwitch(0) is called, up until bufferSwitch(1)
 * returns (and vice-versa). */
void _process(void *_data, struct spa_io_position * /* position */) {
  struct pwasio *pwasio = _data;

  pwasio->pos += pwasio->buffer_size;
  struct timespec now;
#ifdef CLOCK_MONOTONIC_RAW
  clock_gettime(CLOCK_MONOTONIC_RAW, &now);
#else
  clock_gettime(CLOCK_MONOTONIC, &now);
#endif
  pwasio->nsec = 1000000000 * now.tv_sec + now.tv_nsec;

  if (pw_data_loop_in_thread(pwasio->loop)) {
    for (size_t i = 0; i < pwasio->n_in + pwasio->n_out; i++)
      if (pwasio->channels[i].port)
        pw_filter_dequeue_buffer(pwasio->channels[i].port);

    pwasio->callbacks->swap_buffers(pwasio->idx, false);

    struct pw_buffer *buf;
    for (size_t i = 0; i < pwasio->n_in + pwasio->n_out; i++)
      if ((buf = pwasio->channels[i].buffer[pwasio->idx])) {
        if (i >= pwasio->n_in) {
          struct spa_data *d = &buf->buffer->datas[0];
          d->chunk->offset = 0;
          d->chunk->size = pwasio->buffer_size * sizeof(float);
          d->chunk->stride = sizeof(float);
          d->chunk->flags = 0;
        }
        pw_filter_queue_buffer(pwasio->channels[i].port, buf);
      }

    pwasio->idx = !pwasio->idx;
  }
}

STDMETHODIMP_(LONG32) Init(struct asio *_data, void *) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  HKEY config = nullptr;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, DRIVER_REG, 0, nullptr, 0,
                      KEY_WRITE | KEY_READ, nullptr, &config,
                      nullptr) == ERROR_SUCCESS) {
#define get_dword(config, key, default)                                        \
  ({                                                                           \
    DWORD out;                                                                 \
    LONG err = RegQueryValueExA(config, key, 0, nullptr, (BYTE *)&out,         \
                                &(DWORD){sizeof out});                         \
    if (err == ERROR_FILE_NOT_FOUND)                                           \
      err = RegSetValueExA(config, key, 0, REG_DWORD,                          \
                           (BYTE *)&(DWORD){out = default}, sizeof out);       \
    if (err != ERROR_SUCCESS) {                                                \
      RegCloseKey(config);                                                     \
      WINE_WARN("unable to read configuration for " key ", using defaults\n"); \
      goto error_registry;                                                     \
    }                                                                          \
    out;                                                                       \
  })
    pwasio->n_in = get_dword(config, KEY_N_INPUTS, DEFAULT_N_INPUTS);
    pwasio->n_out = get_dword(config, KEY_N_OUTPUTS, DEFAULT_N_OUTPUTS);
    pwasio->buffer_size = get_dword(config, KEY_BUFSIZE, DEFAULT_BUFSIZE);
    pwasio->sample_rate = get_dword(config, KEY_SMPRATE, DEFAULT_SMPRATE);
    pwasio->autoconnect = get_dword(config, KEY_AUTOCON, DEFAULT_AUTOCON);
    pwasio->context.thread.priority =
        get_dword(config, KEY_PRIORITY, DEFAULT_PRIORITY);
    pwasio->context.host_priority =
        get_dword(config, KEY_HOST_PRIORITY, DEFAULT_PRIORITY);
#undef get_dword
    RegCloseKey(config);
  } else {
  error_registry:
    pwasio->n_in = DEFAULT_N_INPUTS;
    pwasio->n_out = DEFAULT_N_OUTPUTS;
    pwasio->buffer_size = DEFAULT_BUFSIZE;
    pwasio->sample_rate = DEFAULT_SMPRATE;
    pwasio->autoconnect = DEFAULT_AUTOCON;
    pwasio->context.thread.priority = DEFAULT_PRIORITY;
    pwasio->context.host_priority = DEFAULT_PRIORITY;
  }
  for (size_t i = 0; i < pwasio->n_in; i++) {
    struct asio_channel_info *info = &pwasio->channels[i].info;
    info->index = i;
    info->input = true;
    info->group = 0;
    info->type = ASIO_SAMPLE_TYPE_FLOAT32_LSB;
    sprintf(info->name, "in_%lu", i);
  }
  for (size_t i = 0; i < pwasio->n_in; i++) {
    struct asio_channel_info *info = &pwasio->channels[pwasio->n_in + i].info;
    info->index = i;
    info->input = false;
    info->group = 0;
    info->type = ASIO_SAMPLE_TYPE_FLOAT32_LSB;
    sprintf(info->name, "out_%lu", i);
  }

  WINE_TRACE("starting pwasio\n");
  pwasio->context.host_tid = pthread_self();
  if (pwasio->context.thread.priority || pwasio->context.host_priority) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_RTPRIO, &rl) || rl.rlim_max < 1 ||
        !(rl.rlim_cur = SPA_MAX(pwasio->context.thread.priority,
                                pwasio->context.host_priority)) ||
        setrlimit(RLIMIT_RTPRIO, &rl))
      pwasio_err(ASIO_ERROR_INVALID_MODE,
                 "unable to get realtime privileges: %s\n", strerror(errno));
    if (pwasio->context.host_priority) {
      WINE_TRACE("setting host scheduler to SCHED_FIFO with priority %d\n",
                 pwasio->context.host_priority);
      if (pthread_setschedparam(
              pwasio->context.host_tid, SCHED_FIFO,
              &(struct sched_param){.sched_priority =
                                        pwasio->context.host_priority}))
        WINE_ERR("unable to set host realtime priority\n");
    }
  }

  WCHAR path[MAX_PATH];
  GetModuleFileNameW(0, path, MAX_PATH);
  WideCharToMultiByte(CP_ACP, WC_SEPCHARS, StrRChrW(path, nullptr, '\\') + 1,
                      -1, pwasio->name, sizeof pwasio->name, nullptr, nullptr);

  int err;
  struct pw_properties *props;
  if (!(props = pw_properties_new(PW_KEY_CLIENT_NAME, pwasio->name,
                                  PW_KEY_CLIENT_API, "ASIO", nullptr))) {
    err = ASIO_ERROR_NO_MEMORY;
    sprintf(pwasio->err_msg, "failed to allocate PipeWire properties\n");
    goto err_props;
  }
  if (!(pwasio->context.loop = pw_thread_loop_new(pwasio->name, nullptr))) {
    err = ASIO_ERROR_NO_MEMORY;
    sprintf(pwasio->err_msg, "failed to create PipeWire loop\n");
    goto err_loop;
  }
  if (!(pwasio->context.context =
            pw_context_new(pw_thread_loop_get_loop(pwasio->context.loop),
                           pw_properties_copy(props), 0))) {
    err = ASIO_ERROR_NO_MEMORY;
    sprintf(pwasio->err_msg, "failed to create PipeWire context\n");
    goto err_ctx;
  }

  static const struct spa_thread_utils_methods thread_utils_methods = {
      .create = _thread_utils_create,
      .join = _thread_utils_join,
      .get_rt_range = _thread_utils_get_rt_range,
      .acquire_rt = _thread_utils_acquire_rt,
      .drop_rt = _thread_utils_drop_rt,
  };
  pwasio->context.thread_utils.iface = SPA_INTERFACE_INIT(
      SPA_TYPE_INTERFACE_ThreadUtils, SPA_VERSION_THREAD_UTILS,
      &thread_utils_methods, &pwasio->context.thread);

  pwasio->loop = pw_context_get_data_loop(pwasio->context.context);
  pw_data_loop_stop(pwasio->loop);

  pw_context_set_object(pwasio->context.context, SPA_TYPE_INTERFACE_ThreadUtils,
                        &pwasio->context.thread_utils);

  pw_thread_loop_start(pwasio->context.loop);
  pw_thread_loop_lock(pwasio->context.loop);

  pw_properties_set(props, PW_KEY_NODE_NAME, pwasio->name);
  pw_properties_set(props, PW_KEY_NODE_GROUP, "group.dsp.0");
  pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, pwasio->name);
  pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
  pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Duplex");
  pw_properties_set(props, PW_KEY_MEDIA_ROLE, "DSP");
  pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");
  pw_properties_set(props, PW_KEY_NODE_LOCK_QUANTUM, "true");
  pw_properties_setf(props, PW_KEY_NODE_FORCE_RATE, "%lu", pwasio->sample_rate);
  pw_properties_setf(props, PW_KEY_NODE_FORCE_QUANTUM, "%lu",
                     pwasio->buffer_size);
  static const struct pw_filter_events filter_events = {
      PW_VERSION_FILTER_EVENTS,
      .add_buffer = _add_buffer,
      .remove_buffer = _remove_buffer,
      .process = _process,
  };
  if (!(pwasio->filter = pw_filter_new_simple(
            pw_data_loop_get_loop(pwasio->loop), pwasio->name, props,
            &filter_events, pwasio))) {
    err = ASIO_ERROR_NO_MEMORY;
    sprintf(pwasio->err_msg, "failed to create PipeWire filter\n");
    goto err_filter;
  }

  pw_thread_loop_unlock(pwasio->context.loop);

  return 1;

err_filter:
  pw_context_destroy(pwasio->context.context);
  pwasio->context.context = nullptr;
err_ctx:
  pw_thread_loop_destroy(pwasio->context.loop);
  pwasio->context.loop = nullptr;
err_loop:
  pw_properties_free(props);
err_props:
  WINE_TRACE("%s", pwasio->err_msg);
  return err;
}

STDMETHODIMP_(VOID) GetDriverName(struct asio *, PSTR name) {
  WINE_TRACE("%p\n", name);
  strcpy(name, "pwasio");
}

STDMETHODIMP_(LONG32) GetDriverVersion(struct asio *) {
  WINE_TRACE("\n");
  return (PWASIO_VERSION_MAJOR << 20) + (PWASIO_VERSION_MINOR << 10) +
         (PWASIO_VERSION_PATCH);
}

STDMETHODIMP_(VOID) GetErrorMessage(struct asio *_data, PSTR string) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (*pwasio->err_msg) {
    strcpy(string, pwasio->err_msg);
    *pwasio->err_msg = '\0';
  } else
    strcpy(string, "undocumented error\n");
}

STDMETHODIMP_(LONG32) Start(struct asio *_data) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  pw_thread_loop_lock(pwasio->context.loop);
  if (pw_data_loop_start(pwasio->loop) < 0)
    pwasio_err(ASIO_ERROR_HW_MALFUNCTION, "failed to start PipeWire data loop");
  pw_thread_loop_unlock(pwasio->context.loop);

  pwasio->running = true;
  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32) Stop(struct asio *_data) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  WINE_TRACE("locking loop\n");
  pw_thread_loop_lock(pwasio->context.loop);
  if (pwasio->running) {
    pwasio->running = false;
    WINE_TRACE("stopping loop\n");
    pw_data_loop_stop(pwasio->loop);
  }
  WINE_TRACE("unlocking loop\n");
  pw_thread_loop_unlock(pwasio->context.loop);

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetChannels(struct asio *_data, LONG *n_inputs, LONG *n_outputs) {
  WINE_TRACE("\n");
  if (!n_inputs || !n_outputs)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *n_inputs = pwasio->n_in;
  *n_outputs = pwasio->n_out;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG) GetLatencies(struct asio *_data, LONG *in, LONG *out) {
  WINE_TRACE("\n");
  if (!in || !out)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *in = pwasio->buffer_size;
  *out = pwasio->buffer_size;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetBufferSize(struct asio *_data, LONG32 *min, LONG32 *max, LONG32 *pref,
              LONG32 *grn) {
  WINE_TRACE("\n");
  if (!min || !max || !pref || !grn)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *min = *max = *pref = pwasio->buffer_size;
  *grn = 0;
  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32) CanSampleRate(struct asio *_data, DOUBLE rate) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if ((size_t)rate != pwasio->sample_rate)
    pwasio_err(ASIO_ERROR_NO_CLOCK, "invalid sample rate");

  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) GetSampleRate(struct asio *_data, DOUBLE *rate) {
  if (!rate)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *rate = pwasio->sample_rate;
  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32) SetSampleRate(struct asio *_data, DOUBLE rate) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if ((size_t)rate != pwasio->sample_rate)
    pwasio_err(ASIO_ERROR_NO_CLOCK, "invalid sample rate");

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetClockSources(struct asio *_data, struct asio_clock_source *clocks,
                LONG32 *num) {
  WINE_TRACE("\n");
  if (!clocks || !num)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *clocks = (typeof(*clocks)){
      .index = 0,
      .channel = -1,
      .group = -1,
      .current = true,
      .name = "PipeWire",
  };
  *num = 1;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32) SetClockSource(struct asio *_data, LONG32 idx) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if (!idx)
    return ASIO_ERROR_INVALID_PARAMETER;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetSamplePosition(struct asio *_data, struct asio_samples *pos,
                  struct asio_timestamp *nsec) {
  if (!nsec || !pos)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *pos = (typeof(*pos)){
      .lo = pwasio->pos,
      .hi = pwasio->pos >> 32,
  };
  *nsec = (typeof(*nsec)){
      .lo = pwasio->nsec,
      .hi = pwasio->nsec >> 32,
  };

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetChannelInfo(struct asio *_data, struct asio_channel_info *info) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if (info->index < 0)
    return ASIO_ERROR_INVALID_PARAMETER;

  int idx;
  if (info->input && info->index < (LONG32)pwasio->n_in)
    idx = info->index;
  else if (!info->input && info->index < (LONG32)pwasio->n_out)
    idx = info->index + pwasio->n_in;
  else
    return ASIO_ERROR_INVALID_PARAMETER;

  *info = pwasio->channels[idx].info;
  info->group = 0;
  info->type = ASIO_SAMPLE_TYPE_FLOAT32_LSB;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
CreateBuffers(struct asio *_data, struct asio_buffer_info *channels,
              LONG32 n_channels, LONG32 buffer_size,
              struct asio_callbacks *callbacks) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if (!channels || !callbacks)
    return ASIO_ERROR_INVALID_PARAMETER;

  if ((size_t)buffer_size != pwasio->buffer_size)
    pwasio_err(ASIO_ERROR_INVALID_MODE, "invalid buffer size %d", buffer_size);

  pwasio->callbacks = callbacks;

  size_t step = SPA_MAX(pwasio->buffer_size * sizeof *pwasio->buffer,
                        (size_t)getpagesize()) /
                sizeof *pwasio->buffer;
  pwasio->fsize = 2 * n_channels * step * sizeof *pwasio->buffer;

  char msg[256];
  LONG32 res;
  if ((pwasio->fd = memfd_create("pwasio-buf", MFD_CLOEXEC)) < 0) {
    sprintf(msg, "Failed to create buffer file descriptor\n");
    res = ASIO_ERROR_NO_MEMORY;
    goto cleanup;
  }
  if (ftruncate(pwasio->fd, pwasio->fsize) < 0) {
    sprintf(msg, "Failed to truncate buffer file\n");
    res = ASIO_ERROR_NO_MEMORY;
    goto cleanup;
  }
  if ((pwasio->buffer = mmap(nullptr, pwasio->fsize, PROT_READ | PROT_WRITE,
                             MAP_SHARED, pwasio->fd, 0)) == MAP_FAILED) {
    sprintf(msg, "Failed to mmap buffer\n");
    res = ASIO_ERROR_NO_MEMORY;
    goto cleanup;
  }

  pw_thread_loop_lock(pwasio->context.loop);
  size_t offset = 0;
  for (size_t c = 0; c < (size_t)n_channels; c++) {
    struct asio_buffer_info *info = channels + c;
    size_t idx;
    enum pw_direction dir;
    if (channels[c].input && channels[c].index < (LONG32)pwasio->n_in) {
      idx = channels[c].index;
      dir = PW_DIRECTION_INPUT;
    } else if (!channels[c].input &&
               channels[c].index < (LONG32)pwasio->n_out) {
      idx = channels[c].index + pwasio->n_in;
      dir = PW_DIRECTION_OUTPUT;
    } else {
      sprintf(msg, "Invalid channel requested %s %d\n",
              info->input ? "input" : "output", info->index);
      res = ASIO_ERROR_INVALID_MODE;
      goto cleanup;
    }
    char buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[] = {
        spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
            SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_DSP_F32)),
        spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(2), SPA_PARAM_BUFFERS_size,
            SPA_POD_Int(pwasio->buffer_size * sizeof(float)),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(sizeof(float)),
            SPA_PARAM_BUFFERS_align, SPA_POD_Int(step * sizeof(float)),
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)),
    };
    if (!(pwasio->channels[idx].port = pw_filter_add_port(
              pwasio->filter, dir, PW_FILTER_PORT_FLAG_ALLOC_BUFFERS,
              sizeof *pwasio->channels[idx].port,
              pw_properties_new(PW_KEY_PORT_NAME,
                                pwasio->channels[idx].info.name, nullptr),
              params, SPA_N_ELEMENTS(params)))) {
      sprintf(msg, "unable to allocate port for %s %d\n",
              info->input ? "input" : "output", info->index);
      res = ASIO_ERROR_NO_MEMORY;
      pw_thread_loop_unlock(pwasio->context.loop);
      goto cleanup;
    }
    pw_filter_update_properties(
        pwasio->filter, pwasio->channels[idx].port,
        &SPA_DICT_ITEMS(
            SPA_DICT_ITEM(PW_KEY_FORMAT_DSP, "32 bit float mono audio")));
    *pwasio->channels[idx].port = &pwasio->channels[idx];
    pwasio->channels[idx].info.active = true;
    for (size_t b = 0; b < 2; b++) {
      pwasio->channels[idx].offset[b] = offset * sizeof *pwasio->buffer;
      channels[c].buf[b] = pwasio->buffer + offset;
      offset += step;
    }
  }
  if (pw_filter_connect(pwasio->filter, PW_FILTER_FLAG_NONE, nullptr, 0) < 0) {
    sprintf(msg, "Failed to connect filter\n");
    res = ASIO_ERROR_NO_MEMORY;
    pw_thread_loop_unlock(pwasio->context.loop);
    goto cleanup;
  }
  pw_thread_loop_unlock(pwasio->context.loop);

  return ASIO_ERROR_OK;

cleanup:
  pw_thread_loop_lock(pwasio->context.loop);
  for (size_t i = 0; i < MAX_PORTS; i++) {
    if (pwasio->channels[i].port) {
      pw_filter_remove_port(pwasio->channels[i].port);
      pwasio->channels[i].port = nullptr;
    }
    pwasio->channels[i].info.active = false;
  }
  pw_thread_loop_lock(pwasio->context.loop);
  if (pwasio->buffer != MAP_FAILED) {
    munmap(pwasio->buffer, pwasio->fsize);
    pwasio->buffer = nullptr;
  }
  if (pwasio->fd > 0) {
    close(pwasio->fd);
    pwasio->fd = -1;
  }

  pwasio_err(res, "%s", msg);
}
STDMETHODIMP_(LONG32) DisposeBuffers(struct asio *_data) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if (pwasio->running)
    pwasio->vtbl->Stop(_data);

  if (pwasio->fd < 0)
    pwasio_err(ASIO_ERROR_INVALID_MODE, "no buffers");

  pw_thread_loop_lock(pwasio->context.loop);
  if (pw_filter_get_state(pwasio->filter, nullptr) !=
      PW_FILTER_STATE_UNCONNECTED)
    pw_filter_disconnect(pwasio->filter);
  for (size_t i = 0; i < MAX_PORTS; i++) {
    if (pwasio->channels[i].port) {
      pw_filter_remove_port(pwasio->channels[i].port);
      pwasio->channels[i].port = nullptr;
    }
    pwasio->channels[i].info.active = false;
  }
  pw_thread_loop_unlock(pwasio->context.loop);

  munmap(pwasio->buffer, pwasio->fsize);
  pwasio->buffer = MAP_FAILED;
  close(pwasio->fd);
  pwasio->fd = -1;

  return 0;
}

struct cfg {
  size_t n_inputs, n_outputs, buffer_size, sample_rate;
  bool autoconnect;
  int priority, host_priority;
  bool reset;
};
static INT_PTR CALLBACK _panel_func(HWND hWnd, UINT uMsg, WPARAM wParam,
                                    LPARAM lParam) {
  struct cfg *cfg = (struct cfg *)GetWindowLongPtrA(hWnd, GWLP_USERDATA);

  switch (uMsg) {
  case WM_INITDIALOG:
    SetWindowLongPtrA(hWnd, GWLP_USERDATA, lParam);
    cfg = (struct cfg *)lParam;
    SetDlgItemInt(hWnd, IDE_INPUTS, cfg->n_inputs, false);
    SetDlgItemInt(hWnd, IDE_OUTPUTS, cfg->n_outputs, false);
    SetDlgItemInt(hWnd, IDE_BUFSIZE, cfg->buffer_size, false);
    SetDlgItemInt(hWnd, IDE_SMPRATE, cfg->sample_rate, false);
    CheckDlgButton(hWnd, IDC_AUTOCON,
                   cfg->autoconnect ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(hWnd, IDE_PRIORITY, cfg->priority, false);
    SetDlgItemInt(hWnd, IDE_HOST_PRIORITY, cfg->host_priority, false);
    break;
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDOK:
      BOOL conv;
      INT val;

      val = GetDlgItemInt(hWnd, IDE_INPUTS, &conv, true);
      if (conv && val >= 0) {
        val = SPA_MIN(val, MAX_PORTS);
        cfg->reset = cfg->reset || (UINT)val != cfg->n_inputs;
        cfg->n_inputs = val;
      }

      val = GetDlgItemInt(hWnd, IDE_OUTPUTS, &conv, true);
      if (conv && val >= 0) {
        val = SPA_MIN(val, MAX_PORTS);
        cfg->reset = cfg->reset || (UINT)val != cfg->n_outputs;
        cfg->n_outputs = val;
      }

      val = GetDlgItemInt(hWnd, IDE_BUFSIZE, &conv, true);
      if (conv && val > 0) {
        cfg->reset = cfg->reset || (UINT)val != cfg->buffer_size;
        cfg->buffer_size = val;
      }

      val = GetDlgItemInt(hWnd, IDE_SMPRATE, &conv, true);
      if (conv && val > 0) {
        cfg->reset = cfg->reset || (UINT)val != cfg->sample_rate;
        cfg->sample_rate = val;
      }

      val = IsDlgButtonChecked(hWnd, IDC_AUTOCON) == BST_CHECKED;
      cfg->reset = cfg->reset || val != cfg->autoconnect;
      cfg->autoconnect = val;

      val = GetDlgItemInt(hWnd, IDE_PRIORITY, &conv, true);
      if (conv && val >= 0) {
        cfg->reset = cfg->reset || val != cfg->priority;
        cfg->priority = val;
      }

      val = GetDlgItemInt(hWnd, IDE_HOST_PRIORITY, &conv, true);
      if (conv && val >= 0) {
        cfg->reset = cfg->reset || val != cfg->host_priority;
        cfg->host_priority = val;
      }
    case IDCANCEL:
      DestroyWindow(hWnd);
      break;
    }
    break;
  case WM_DESTROY:
    PostQuitMessage(0);
    break;
  default:
    return FALSE;
  }
  return TRUE;
}

static DWORD WINAPI _panel_thread(LPVOID p) {
  struct pwasio *pwasio = p;

  struct cfg cfg = {
      .n_inputs = pwasio->n_in,
      .n_outputs = pwasio->n_out,
      .buffer_size = pwasio->buffer_size,
      .sample_rate = pwasio->sample_rate,
      .autoconnect = pwasio->autoconnect,
      .priority = pwasio->context.thread.priority,
      .host_priority = pwasio->context.host_priority,
  };
  if (!(pwasio->dialog = CreateDialogParamA(
            pwasio->hinst, (LPCSTR)MAKEINTRESOURCE(IDD_PANEL), nullptr,
            _panel_func, (LPARAM)&cfg)))
    return -1;

  ShowWindow(pwasio->dialog, SW_SHOW);

  MSG msg;
  while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageA(pwasio->dialog, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }

  if (cfg.reset) {
    HKEY config = nullptr;
#define CHK(call)                                                              \
  do {                                                                         \
    if ((call) != ERROR_SUCCESS) {                                             \
      WINE_WARN("failed to write configuration\n");                            \
      goto cleanup;                                                            \
    }                                                                          \
  } while (false)
    CHK(RegOpenKeyExA(HKEY_CURRENT_USER, DRIVER_REG, 0, KEY_WRITE, &config));

    if (cfg.n_inputs != pwasio->n_in)
      CHK(RegSetValueExA(config, KEY_N_INPUTS, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.n_inputs}, sizeof(DWORD)));
    if (cfg.n_outputs != pwasio->n_out)
      CHK(RegSetValueExA(config, KEY_N_OUTPUTS, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.n_outputs}, sizeof(DWORD)));
    if (cfg.buffer_size != pwasio->buffer_size)
      CHK(RegSetValueExA(config, KEY_BUFSIZE, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.buffer_size}, sizeof(DWORD)));
    if (cfg.sample_rate != pwasio->sample_rate)
      CHK(RegSetValueExA(config, KEY_SMPRATE, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.sample_rate}, sizeof(DWORD)));
    if (cfg.autoconnect != pwasio->autoconnect)
      CHK(RegSetValueExA(config, KEY_AUTOCON, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.autoconnect}, sizeof(DWORD)));
    if (cfg.priority != pwasio->context.thread.priority)
      CHK(RegSetValueExA(config, KEY_PRIORITY, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.priority}, sizeof(DWORD)));
    if (cfg.host_priority != pwasio->context.host_priority)
      CHK(RegSetValueExA(config, KEY_HOST_PRIORITY, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.host_priority}, sizeof(DWORD)));

    if (pwasio->callbacks && pwasio->callbacks->message)
      pwasio->callbacks->message(ASIO_MESSAGE_RESET_REQUEST, 0, nullptr,
                                 nullptr);
#undef CHK
  cleanup:
    if (config)
      RegCloseKey(config);
  }

  pwasio->dialog = nullptr;

  return 0;
}
STDMETHODIMP_(LONG32) ControlPanel(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (pwasio->panel) {
    if (pwasio->dialog)
      return ASIO_ERROR_OK;
    WaitForSingleObject(pwasio->panel, INFINITE);
    CloseHandle(pwasio->panel);
    pwasio->panel = nullptr;
  }

  HANDLE t = CreateThread(nullptr, 0, _panel_thread, pwasio, 0, nullptr);
  if (!t)
    return ASIO_ERROR_NOT_PRESENT;

  pwasio->panel = t;

  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) not_impl() { return ASIO_ERROR_NOT_PRESENT; }

HRESULT WINAPI CreateInstance(LPCLASSFACTORY _data, LPUNKNOWN outer, REFIID,
                              LPVOID *ptr) {
  WINE_TRACE("\n");
  if (outer)
    return CLASS_E_NOAGGREGATION;

  if (!ptr)
    return E_INVALIDARG;

  struct pwasio *pwasio = *ptr =
      HeapAlloc(GetProcessHeap(), 0, sizeof(*pwasio));
  if (!pwasio)
    return E_OUTOFMEMORY;

  static const struct asioVtbl vtbl = {
      .QueryInterface = QueryInterface,
      .AddRef = AddRef,
      .Release = Release,

      .Init = Init,
      .GetDriverName = GetDriverName,
      .GetDriverVersion = GetDriverVersion,
      .GetErrorMessage = GetErrorMessage,
      .Start = Start,
      .Stop = Stop,
      .GetChannels = GetChannels,
      .GetLatencies = GetLatencies,
      .GetBufferSize = GetBufferSize,
      .CanSampleRate = CanSampleRate,
      .GetSampleRate = GetSampleRate,
      .SetSampleRate = SetSampleRate,
      .GetClockSources = GetClockSources,
      .SetClockSource = SetClockSource,
      .GetSamplePosition = GetSamplePosition,
      .GetChannelInfo = GetChannelInfo,
      .CreateBuffers = CreateBuffers,
      .DisposeBuffers = DisposeBuffers,
      .ControlPanel = ControlPanel,
      .Future = (void *)not_impl,
      .OutputReady = (void *)not_impl,
  };
  *pwasio = (typeof(*pwasio)){
      .vtbl = &vtbl,
      .ref = 1,
      .hinst = ((struct factory *)_data)->hinst,

      .fd = -1,
      .buffer = MAP_FAILED,
  };

  WINE_TRACE("starting PipeWire\n");
  pw_init(nullptr, nullptr);
  WINE_TRACE("compiled with libpipewire-%s\n", pw_get_headers_version());
  WINE_TRACE("linked with libpipewire-%s\n", pw_get_library_version());

  *ptr = pwasio;
  return S_OK;
}
#undef CHK
