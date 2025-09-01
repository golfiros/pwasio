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

#include <assert.h>
#include <pipewire/pipewire.h>
#include <sys/syscall.h>

#include <mmsystem.h>
#include <shlwapi.h>

#ifdef DEBUG
#include <wine/debug.h>
WINE_DEFAULT_DEBUG_CHANNEL(asio);
#define assert_eq(x, y, fx, fy)                                                \
  do {                                                                         \
    typeof(x) _x = (x);                                                        \
    typeof(y) _y = (y);                                                        \
    if (_x != _y) {                                                            \
      ERR(#x " = " #fx ", " #y " = " #fy "\n", _x, _y);                        \
      assert(_x == _y && #x "==" #y);                                          \
    }                                                                          \
  } while (false)
;
#else
#define assert_eq(...)
#define TRACE(...)
#define WARN(...)
#define ERR(...)
#endif

#define MAX_NAME_LENGTH 32
#define MAX_PORTS 32

#define MIN_BUFFER_SIZE 32
#define MAX_BUFFER_SIZE 8192
#define PREF_BUFFER_SIZE 256

#define DEFAULT_SAMPLE_RATE 48000

bool pwasio_init() {
  pw_init(nullptr, nullptr);

  TRACE("Starting pwasio\n");
  TRACE("Compiled with libpipewire-%s\n", pw_get_headers_version());
  TRACE("Linked with libpipewire-%s\n", pw_get_library_version());

  return true;
};

struct pwasio {
  const struct asioVtbl *vtbl;
  LONG32 ref;
  char err_msg[256];

  char name[32];
  size_t n_inputs, n_outputs;

  size_t buffer_size;
  float sample_rate;

  struct pw_data_loop *loop;
  struct pw_filter *filter;
  struct asio_callbacks *callbacks;
  void *ports[MAX_PORTS];
  float *buffer;
  bool idx;

  bool running;
  struct asio_samples pos;
  struct asio_timestamp nsec;
  size_t wine_clock, unix_clock;
};

STDMETHODIMP query_interface(struct asio *_data, REFIID riid, PVOID *out) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (out == NULL)
    return E_INVALIDARG;

  if (IsEqualIID(&class_id, riid)) {
    InterlockedIncrement(&pwasio->ref);
    *out = pwasio;
    return S_OK;
  }

  return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG32) add_ref(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  return InterlockedIncrement(&(pwasio->ref));
}
STDMETHODIMP_(ULONG32) release(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  TRACE("Releasing pwasio\n");
  if (InterlockedDecrement(&pwasio->ref))
    return pwasio->ref;

  if (pwasio->running)
    pwasio->vtbl->Stop(_data);
  if (pwasio->filter)
    pwasio->vtbl->DisposeBuffers(_data);

  pw_data_loop_destroy(pwasio->loop);
  TRACE("pwasio terminated\n");
  HeapFree(GetProcessHeap(), 0, pwasio);

  return 0;
}

struct thread {
  HANDLE handle;
  DWORD thread_id;
  void *(*start)(void *);
  void *arg, *ret;
};
static DWORD WINAPI thread_func(LPVOID p) {
  struct thread *t = p;
  TRACE("Spawned worker thread %ld\n", syscall(SYS_gettid));
  t->ret = t->start(t->arg);
  return 0;
}
static struct spa_thread *_create(void *, const struct spa_dict *,
                                  void *(*start)(void *), void *arg) {
  struct thread *t = HeapAlloc(GetProcessHeap(), 0, sizeof *t);
  if (!t)
    return nullptr;

  *t = (typeof(*t)){
      .start = start,
      .arg = arg,
  };
  t->handle = CreateThread(NULL, 0, thread_func, t, 0, &t->thread_id);
  if (!t->handle) {
    HeapFree(GetProcessHeap(), 0, t);
    return nullptr;
  }

  return (struct spa_thread *)t;
}
static int _join(void *, struct spa_thread *p, void **retval) {
  struct thread *t = (struct thread *)p;
  if (!t || !t->handle)
    return -1;

  DWORD result = WaitForSingleObject(t->handle, INFINITE);
  if (retval)
    *retval = t->ret;

  CloseHandle(t->handle);
  HeapFree(GetProcessHeap(), 0, t);
  return (result == WAIT_OBJECT_0) ? 0 : -1;
}
static int _get_rt_range(void *, const struct spa_dict *, int *min, int *max) {
  *min = THREAD_PRIORITY_NORMAL;
  *max = THREAD_PRIORITY_TIME_CRITICAL;
  return 0;
}
static int _acquire_rt(void *, struct spa_thread *p, int priority) {
  struct thread *t = (struct thread *)p;
  return SetThreadPriority(t->handle, priority);
}
static int _drop_rt(void *, struct spa_thread *p) {
  struct thread *t = (struct thread *)p;
  return SetThreadPriority(t->handle, THREAD_PRIORITY_NORMAL);
}
static const struct spa_thread_utils_methods thread_utils_methods = {
    .create = _create,
    .join = _join,
    .get_rt_range = _get_rt_range,
    .acquire_rt = _acquire_rt,
    .drop_rt = _drop_rt,
};
static struct spa_thread_utils thread_utils = {
    .iface =
        {
            .type = SPA_TYPE_INTERFACE_ThreadUtils,
            .version = SPA_VERSION_THREAD_UTILS,
            .cb =
                {
                    .funcs = &thread_utils_methods,
                },
        },
};
STDMETHODIMP_(LONG32) init(struct asio *_data, void *) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  WCHAR path[MAX_PATH];
  GetModuleFileNameW(0, path, MAX_PATH);
  WideCharToMultiByte(CP_ACP, WC_SEPCHARS, StrRChrW(path, NULL, '\\') + 1, -1,
                      pwasio->name, MAX_NAME_LENGTH, NULL, NULL);

  if (!(pwasio->loop = pw_data_loop_new(nullptr))) {
    sprintf(pwasio->err_msg, "Failed to create PipeWire loop\n");
    return false;
  }
  pw_data_loop_set_thread_utils(pwasio->loop, &thread_utils);

  pwasio->n_inputs = 2;
  pwasio->n_outputs = 2;

  TRACE("pwasio initialized on PID %ld\n", syscall(SYS_gettid));
  return true;
}
STDMETHODIMP_(VOID) get_driver_name(struct asio *, PSTR name) {
  strcpy(name, "PipeWire ASIO");
}
STDMETHODIMP_(LONG32) get_driver_version(struct asio *) {
  return (PWASIO_VERSION_MAJOR << 20) + (PWASIO_VERSION_MINOR << 10) +
         (PWASIO_VERSION_PATCH);
}
STDMETHODIMP_(VOID) get_error_message(struct asio *_data, PSTR string) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (*pwasio->err_msg) {
    strcpy(string, pwasio->err_msg);
    *pwasio->err_msg = '\0';
  } else
    strcpy(string, "Undocumented error\n");
}

STDMETHODIMP_(LONG32) start(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter) {
    sprintf(pwasio->err_msg, "Tried to start loop when not ready\n");
    return ASIO_ERROR_NOT_PRESENT;
  }

  pwasio->idx = false;
  pwasio->wine_clock = 1000000L * timeGetTime();
  TRACE("Wine clock started at %lu\n", pwasio->wine_clock);
  pw_data_loop_start(pwasio->loop);

  TRACE("pwasio started\n");
  pwasio->running = true;
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) stop(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (!pwasio->running) {
    sprintf(pwasio->err_msg, "Tried to stop loop when not running\n");
    return ASIO_ERROR_NOT_PRESENT;
  }
  pw_data_loop_stop(pwasio->loop);
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32)
get_channels(struct asio *_data, LONG *n_inputs, LONG *n_outputs) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!n_inputs || !n_outputs)
    return ASIO_ERROR_INVALID_PARAMETER;

  *n_inputs = pwasio->n_inputs;
  *n_outputs = pwasio->n_outputs;
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG) get_latencies(struct asio *_data, LONG *in, LONG *out) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!in || !out)
    return ASIO_ERROR_INVALID_PARAMETER;

  if (!pwasio->filter) {
    sprintf(pwasio->err_msg, "Tried to get latencies without PipeWire\n");
    return ASIO_ERROR_NOT_PRESENT;
  }

  *in = *out = 0;

  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32)
get_buffer_size(struct asio *, LONG32 *min, LONG32 *max, LONG32 *pref,
                LONG32 *grn) {
  TRACE("Buffer size info requested\n");

  if (!min || !max || !pref || !grn)
    return ASIO_ERROR_INVALID_PARAMETER;

  *min = MIN_BUFFER_SIZE;
  *max = MAX_BUFFER_SIZE;
  *pref = PREF_BUFFER_SIZE;
  *grn = -1;
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) can_sample_rate(struct asio *, DOUBLE) {
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) get_sample_rate(struct asio *_data, DOUBLE *rate) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  TRACE("Sample rate info requested\n");

  if (!rate)
    return ASIO_ERROR_INVALID_PARAMETER;

  *rate = pwasio->sample_rate;
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) set_sample_rate(struct asio *_data, DOUBLE rate) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  TRACE("Setting sample rate to %f\n", rate);
  pwasio->sample_rate = rate;
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32)
get_clock_sources(struct asio *, struct asio_clock_source *clocks,
                  LONG32 *num) {
  if (!clocks || !num)
    return ASIO_ERROR_INVALID_PARAMETER;

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
STDMETHODIMP_(LONG32) set_clock_source(struct asio *_data, LONG32 idx) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (!idx) {
    sprintf(pwasio->err_msg, "Invalid clock source\n");
    return ASIO_ERROR_NOT_PRESENT;
  }
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32)
get_sample_position(struct asio *_data, struct asio_samples *pos,
                    struct asio_timestamp *nsec) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (!nsec || !pos)
    return ASIO_ERROR_INVALID_PARAMETER;

  *pos = pwasio->pos;
  *nsec = pwasio->nsec;

  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32)
get_channel_info(struct asio *_data, struct asio_channel_info *info) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (info->index < 0 ||
      (info->input ? info->index >= (LONG32)pwasio->n_inputs
                   : info->index >= (LONG32)pwasio->n_outputs))
    return ASIO_ERROR_INVALID_PARAMETER;

  info->active = !!pwasio->ports[info->input * pwasio->n_inputs + info->index];
  info->group = 0;
  info->type = ASIO_SAMPLE_TYPE_FLOAT32_LSB;

  if (info->input)
    sprintf(info->name, "input_%d", info->index);
  else
    sprintf(info->name, "output_%d", info->index);

  return ASIO_ERROR_OK;
}
struct message {
  LONG32 selector, value;
  PVOID message;
  DOUBLE *opt;
};
/*
static int _message(struct spa_loop *, bool, uint32_t, const void *_args,
                    size_t, void *_data) {
  struct pwasio *pwasio = _data;
  const struct message *args = _args;
  if (pwasio->callbacks->message(ASIO_MESSAGE_SUPPORTED, args->selector, 0,
                                   nullptr))
    return pwasio->callbacks->message(args->selector, args->value,
                                        args->message, args->opt);
  else
    return 0;
}
*/
static int _swap_buffers(struct spa_loop *, bool, uint32_t, const void *,
                         size_t, void *_data) {
  struct pwasio *pwasio = _data;
  pwasio->callbacks->swap_buffers(pwasio->idx, 1);
  pwasio->idx = !pwasio->idx;
  return 0;
}
void _process(void *_data, struct spa_io_position *pos) {
  struct pwasio *pwasio = _data;

  size_t n_frames = pos->clock.duration;

  if (!pwasio->unix_clock)
    pwasio->unix_clock = pos->clock.nsec;
  size_t nsec = (pos->clock.nsec - pwasio->unix_clock) + pwasio->wine_clock;
  pwasio->nsec = (typeof(pwasio->nsec)){
      .lo = nsec,
      .hi = nsec >> 32,
  };
  if (pwasio->pos.lo > (ULONG32)(-1) - n_frames)
    pwasio->pos.hi++;
  pwasio->pos.lo += n_frames;

  assert_eq(n_frames, pwasio->buffer_size, "%lu", "%lu");
  assert_eq((size_t)((double)pos->clock.rate.denom / pos->clock.rate.num),
            (size_t)pwasio->sample_rate, "%lu", "%lu");

  float *buf;
  void **ports = pwasio->ports;
  size_t n_inputs = pwasio->n_inputs, n_outputs = pwasio->n_outputs;

  for (size_t i = 0; i < n_inputs; i++)
    if (ports[i] && (buf = pw_filter_get_dsp_buffer(ports[i], n_frames)))
      for (size_t t = 0; t < n_frames; t++)
        pwasio->buffer[(2 * i + pwasio->idx) * n_frames + t] = buf[t];
    else
      for (size_t t = 0; t < n_frames; t++)
        pwasio->buffer[(2 * i + pwasio->idx) * n_frames + t] = 0.0f;

  pw_data_loop_invoke(pwasio->loop, _swap_buffers, SPA_ID_INVALID, nullptr, 0,
                      true, pwasio);

  for (size_t i = n_inputs; i < n_inputs + n_outputs; i++)
    if (ports[i] && (buf = pw_filter_get_dsp_buffer(ports[i], n_frames)))
      for (size_t t = 0; t < n_frames; t++)
        buf[t] = pwasio->buffer[(2 * i + pwasio->idx) * n_frames + t];
}
static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = _process,
};
STDMETHODIMP_(LONG32)
create_buffers(struct asio *_data, struct asio_buffer_info *channels,
               LONG32 n_channels, LONG32 buffer_size,
               struct asio_callbacks *callbacks) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->loop) {
    sprintf(pwasio->err_msg, "Tried to create buffers without PipeWire\n");
    return ASIO_ERROR_NOT_PRESENT;
  }

  if (!channels || !callbacks)
    return ASIO_ERROR_INVALID_MODE;

  if (buffer_size > MAX_BUFFER_SIZE || buffer_size < MIN_BUFFER_SIZE ||
      (buffer_size & (buffer_size - 1))) {
    sprintf(pwasio->err_msg,
            "Tried to create buffers with invalid buffer size %d\n",
            buffer_size);
    return ASIO_ERROR_INVALID_MODE;
  }

  char buffer_size_str[32], sample_rate_str[32];
  sprintf(buffer_size_str, "%lu", (size_t)buffer_size);
  sprintf(sample_rate_str, "%lu", (size_t)pwasio->sample_rate);
  if (!(pwasio->filter = pw_filter_new_simple(
            pw_data_loop_get_loop(pwasio->loop), pwasio->name,
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                              "Filter", PW_KEY_MEDIA_ROLE, "DSP",
                              PW_KEY_NODE_FORCE_QUANTUM, buffer_size_str,
                              PW_KEY_NODE_FORCE_RATE, sample_rate_str,
                              PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr),
            &filter_events, pwasio))) {
    sprintf(pwasio->err_msg, "Failed to create PipeWire filter\n");
    return ASIO_ERROR_NO_MEMORY;
  }

  pwasio->buffer = HeapAlloc(GetProcessHeap(), 0,
                             2 * (pwasio->n_inputs + pwasio->n_outputs) *
                                 buffer_size * sizeof(*pwasio->buffer));
  for (size_t i = 0; i < (size_t)n_channels; i++) {
    struct asio_buffer_info *buf = &channels[i];
    TRACE("Allocating filter port for %s %d\n", buf->input ? "input" : "output",
          buf->channel);
    char name[16];
    if (buf->input && buf->channel < (LONG32)pwasio->n_inputs)
      sprintf(name, "input_%d", buf->channel);
    else if (!buf->input && buf->channel < (LONG32)pwasio->n_outputs)
      sprintf(name, "output_%d", buf->channel);
    else {
      HeapFree(GetProcessHeap(), 0, pwasio->buffer);
      pw_filter_destroy(pwasio->filter);
      return ASIO_ERROR_INVALID_MODE;
    }

    size_t idx = !buf->input * pwasio->n_inputs + buf->channel;
    if (!(pwasio->ports[idx] = pw_filter_add_port(
              pwasio->filter,
              buf->input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
              PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
              pw_properties_new(PW_KEY_PORT_NAME, name, PW_KEY_FORMAT_DSP,
                                "32 bit float mono audio", nullptr),
              nullptr, 0)))
      return ASIO_ERROR_NO_MEMORY;

    buf->buf[0] = pwasio->buffer + 2 * idx * buffer_size;
    buf->buf[1] = pwasio->buffer + (2 * idx + 1) * buffer_size;
  }

  if (pw_filter_connect(pwasio->filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) <
      0) {
    sprintf(pwasio->err_msg, "Failed to connect to PipeWire\n");
    pw_filter_destroy(pwasio->filter);
    return ASIO_ERROR_INVALID_MODE;
  }

  pwasio->buffer_size = buffer_size;
  pwasio->callbacks = callbacks;

  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) dispose_buffers(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  TRACE("Disposing all buffers\n");

  if (pwasio->running)
    pwasio->vtbl->Stop(_data);
  if (!pwasio->filter) {
    sprintf(pwasio->err_msg,
            "Tried to dispose of buffers when none are allocated\n");
    return ASIO_ERROR_NOT_PRESENT;
  }

  for (size_t i = 0; i < pwasio->n_inputs + pwasio->n_outputs; i++)
    if (pwasio->ports[i]) {
      pw_filter_remove_port(pwasio->ports[i]);
      pwasio->ports[i] = nullptr;
    }

  HeapFree(GetProcessHeap(), 0, pwasio->buffer);

  pw_filter_destroy(pwasio->filter);
  pwasio->filter = nullptr;

  return 0;
}

STDMETHODIMP_(LONG32) not_impl() { return ASIO_ERROR_NOT_PRESENT; }
static const struct asioVtbl vtbl = {
    .QueryInterface = query_interface,
    .AddRef = add_ref,
    .Release = release,

    .Init = init,
    .GetDriverName = get_driver_name,
    .GetDriverVersion = get_driver_version,
    .GetErrorMessage = get_error_message,
    .Start = start,
    .Stop = stop,
    .GetChannels = get_channels,
    .GetLatencies = get_latencies,
    .GetBufferSize = get_buffer_size,
    .CanSampleRate = can_sample_rate,
    .GetSampleRate = get_sample_rate,
    .SetSampleRate = set_sample_rate,
    .GetClockSources = get_clock_sources,
    .SetClockSource = set_clock_source,
    .GetSamplePosition = get_sample_position,
    .GetChannelInfo = get_channel_info,
    .CreateBuffers = create_buffers,
    .DisposeBuffers = dispose_buffers,
    .ControlPanel = (void *)not_impl,
    .Future = (void *)not_impl,
    .OutputReady = (void *)not_impl,
};

void *pwasio_create() {
  struct pwasio *pwasio = HeapAlloc(GetProcessHeap(), 0, sizeof(*pwasio));
  if (!pwasio)
    return nullptr;

  *pwasio = (typeof(*pwasio)){
      .vtbl = &vtbl,
      .ref = 1,
      .sample_rate = DEFAULT_SAMPLE_RATE,
  };

  return pwasio;
}
