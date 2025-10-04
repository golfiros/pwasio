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

#define _GNU_SOURCE
#include "pwasio.h"
#include "asio.h"

#include <pipewire/pipewire.h>
#include <sched.h>
#include <sys/mman.h>

#include <shlwapi.h>

#undef strncpy
#define strncpy lstrcpynA
#include <spa/param/audio/format-utils.h>

#ifdef DEBUG
#include <assert.h>
#include <wine/debug.h>
WINE_DEFAULT_DEBUG_CHANNEL(asio);
/*
#define assert_eq(x, y, fx, fy)                                                \
  do {                                                                         \
    typeof(x) _x = (x);                                                        \
    typeof(y) _y = (y);                                                        \
    if (_x != _y) {                                                            \
      ERR(#x " = " #fx ", " #y " = " #fy "\n", _x, _y);                        \
      assert(_x == _y && #x "==" #y);                                          \
    }                                                                          \
  } while (false)
*/
#else
#define assert(...)
#define assert_eq(...)
#define TRACE(...)
#define WARN(...)
#define ERR(...)
#endif

#define MAX_NAME_LENGTH 32

#define MIN_BUFFER_SIZE 32
#define MAX_BUFFER_SIZE 8192
#define PREF_BUFFER_SIZE 128

#define DEFAULT_SAMPLE_RATE 48000

#define MAX_PORTS 32

struct port {
  struct pw_buffer *buf[2];
  size_t offset[2];
  float *ptr[2];
  bool ready[2];
};

struct pwasio {
  const struct asioVtbl *vtbl;
  LONG32 ref;
  char err_msg[256];

  char name[MAX_NAME_LENGTH];
  size_t n_inputs, n_outputs;

  size_t buffer_size;
  float sample_rate;

  struct pw_data_loop *loop;
  struct pw_filter *filter;

  int fd;
  struct port *ports[MAX_PORTS];

  bool running;
  bool idx;

  struct asio_callbacks *callbacks;
  struct asio_samples pos;
  struct asio_timestamp nsec;
};

STDMETHODIMP QueryInterface(struct asio *_data, REFIID riid, PVOID *out) {
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
STDMETHODIMP_(ULONG32) AddRef(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  return InterlockedIncrement(&pwasio->ref);
}
STDMETHODIMP_(ULONG32) Release(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  TRACE("Releasing pwasio\n");
  if (InterlockedDecrement(&pwasio->ref))
    return pwasio->ref;

  if (pwasio->running)
    pwasio->vtbl->Stop(_data);
  if (pwasio->fd > 0)
    pwasio->vtbl->DisposeBuffers(_data);
  if (pwasio->filter)
    pw_filter_destroy(pwasio->filter);
  if (pwasio->loop)
    pw_data_loop_destroy(pwasio->loop);
  pw_deinit();
  HeapFree(GetProcessHeap(), 0, pwasio);
  TRACE("pwasio terminated\n");

  return 0;
}
static void _add_buffer(void *_data, void *_port, struct pw_buffer *buf) {
  struct pwasio *pwasio = _data;
  struct port *port = _port;

  TRACE("\n");

  if (!(buf->buffer->datas->type & (1 << SPA_DATA_MemFd))) {
    pw_log_error("unsupported data type %08x", buf->buffer->datas->type);
    return;
  }

  size_t idx = port->buf[pwasio->idx] ? !pwasio->idx : pwasio->idx;

  *buf->buffer->datas = (typeof(*buf->buffer->datas)){
      .type = SPA_DATA_MemFd,
      .flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE,
      .fd = pwasio->fd,
      .mapoffset = port->offset[idx],
      .maxsize = sizeof(float) * pwasio->buffer_size,
      .data = port->ptr[idx],
      .chunk = buf->buffer->datas->chunk,
  };
  port->buf[idx] = buf;
}
static void _rem_buffer(void *_data, void *_port, struct pw_buffer *buf) {
  struct pwasio *pwasio = _data;
  struct port *port = _port;
  TRACE("\n");
  if (buf == port->buf[0]) {
    for (size_t i = 0; i < pwasio->buffer_size; i++)
      port->ptr[0][i] = 0.0f;
    port->buf[0] = nullptr;
  }
  if (buf == port->buf[1]) {
    for (size_t i = 0; i < pwasio->buffer_size; i++)
      port->ptr[1][i] = 0.0f;
    port->buf[1] = nullptr;
  }
}
/* Paranoid driver developers should assume that the application will access
 * buffer 0 as soon as bufferSwitch(0) is called, up until bufferSwitch(1)
 * returns (and vice-versa). */
struct _swap_buffers_args {
  size_t idx;
  struct spa_io_position pos;
};
static int _swap_buffers(struct spa_loop *, bool, uint32_t, const void *_arg,
                         size_t, void *_data) {
  struct pwasio *pwasio = _data;
  const struct _swap_buffers_args *args = _arg;

  pwasio->nsec = (typeof(pwasio->nsec)){
      .lo = args->pos.clock.nsec,
      .hi = args->pos.clock.nsec >> 32,
  };
  if (pwasio->pos.lo > (ULONG32)(-1) - args->pos.clock.duration)
    pwasio->pos.hi++;
  pwasio->pos.lo += args->pos.clock.duration;

  pwasio->callbacks->swap_buffers(args->idx, true);

  for (size_t i = 0; i < MAX_PORTS; i++)
    if (pwasio->ports[i])
      pwasio->ports[i]->ready[!args->idx] = true;

  return 0;
}
static void _process(void *_data, struct spa_io_position *pos) {
  struct pwasio *pwasio = _data;

  for (size_t i = 0; i < MAX_PORTS; i++)
    if (pwasio->ports[i])
      pw_filter_dequeue_buffer(pwasio->ports[i]);

  pw_data_loop_invoke(pwasio->loop, _swap_buffers, SPA_ID_INVALID,
                      &(struct _swap_buffers_args){
                          .idx = pwasio->idx,
                          .pos = *pos,
                      },
                      sizeof(struct _swap_buffers_args), false, pwasio);
  pwasio->idx = !pwasio->idx;

  struct pw_buffer *buf;
  for (size_t i = 0; i < MAX_PORTS; i++)
    if (pwasio->ports[i] && pwasio->ports[i]->ready[pwasio->idx] &&
        (buf = pwasio->ports[i]->buf[pwasio->idx])) {
      if (i >= pwasio->n_inputs) {
        buf->buffer->datas->chunk->offset = 0;
        buf->buffer->datas->chunk->size = pwasio->buffer_size * sizeof(float);
        buf->buffer->datas->chunk->stride = sizeof(float);
        buf->buffer->datas->chunk->flags = 0;
      }
      pwasio->ports[i]->ready[pwasio->idx] = false;
      pw_filter_queue_buffer(pwasio->ports[i], buf);
    }
}
static void _drained(void *) { TRACE("\n"); }
STDMETHODIMP_(LONG32) Init(struct asio *_data, void *) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  WCHAR path[MAX_PATH];
  GetModuleFileNameW(0, path, MAX_PATH);
  WideCharToMultiByte(CP_ACP, WC_SEPCHARS, StrRChrW(path, NULL, '\\') + 1, -1,
                      pwasio->name, MAX_NAME_LENGTH, NULL, NULL);

  pwasio->n_inputs = 2;
  pwasio->n_outputs = 2;

  char rate_str[32];
  sprintf(rate_str, "%lu", (size_t)pwasio->sample_rate);
  static const struct pw_filter_events filter_events = {
      PW_VERSION_FILTER_EVENTS,     .add_buffer = _add_buffer,
      .remove_buffer = _rem_buffer, .process = _process,
      .drained = _drained,
  };

  if (!(pwasio->filter = pw_filter_new_simple(
            pw_data_loop_get_loop(pwasio->loop), pwasio->name,
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                              "Filter", PW_KEY_MEDIA_ROLE, "DSP",
                              PW_KEY_NODE_ALWAYS_PROCESS, "true",
                              PW_KEY_NODE_FORCE_RATE, rate_str, nullptr),
            &filter_events, pwasio))) {
    sprintf(pwasio->err_msg, "Failed to create PipeWire filter\n");
    return ASIO_ERROR_NO_MEMORY;
  }

  return 1;
}

STDMETHODIMP_(VOID) GetDriverName(struct asio *, PSTR name) {
  strcpy(name, "pwasio");
}

STDMETHODIMP_(LONG32) GetDriverVersion(struct asio *) {
  return (PWASIO_VERSION_MAJOR << 20) + (PWASIO_VERSION_MINOR << 10) +
         (PWASIO_VERSION_PATCH);
}
STDMETHODIMP_(VOID) GetErrorMessage(struct asio *_data, PSTR string) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (*pwasio->err_msg) {
    strcpy(string, pwasio->err_msg);
    *pwasio->err_msg = '\0';
  } else
    strcpy(string, "Undocumented error\n");
}
STDMETHODIMP_(LONG32) Start(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (pwasio->fd < 0) {
    sprintf(pwasio->err_msg, "Tried to start loop without buffers\n");
    return ASIO_ERROR_NOT_PRESENT;
  }

  pw_data_loop_start(pwasio->loop);

  TRACE("pwasio started\n");
  pwasio->running = true;
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) Stop(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (!pwasio->running) {
    sprintf(pwasio->err_msg, "Tried to stop loop when not running\n");
    return ASIO_ERROR_NOT_PRESENT;
  }
  pwasio->running = false;
  pw_data_loop_stop(pwasio->loop);
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32)
GetChannels(struct asio *_data, LONG *n_inputs, LONG *n_outputs) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!n_inputs || !n_outputs)
    return ASIO_ERROR_INVALID_PARAMETER;

  *n_inputs = pwasio->n_inputs;
  *n_outputs = pwasio->n_outputs;
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG) GetLatencies(struct asio *_data, LONG *in, LONG *out) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!in || !out)
    return ASIO_ERROR_INVALID_PARAMETER;

  if (!pwasio->filter) {
    sprintf(pwasio->err_msg, "Tried to get latencies without filter\n");
    return ASIO_ERROR_NOT_PRESENT;
  }

  *in = *out = 0;

  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32)
GetBufferSize(struct asio *, LONG32 *min, LONG32 *max, LONG32 *pref,
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
STDMETHODIMP_(LONG32) CanSampleRate(struct asio *, DOUBLE) {
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) GetSampleRate(struct asio *_data, DOUBLE *rate) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  TRACE("Sample rate info requested\n");

  if (!rate)
    return ASIO_ERROR_INVALID_PARAMETER;

  *rate = pwasio->sample_rate;
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) SetSampleRate(struct asio *_data, DOUBLE rate) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (!pwasio->filter) {
    sprintf(pwasio->err_msg,
            "Tried to set sample rate before creating filter\n");
    return ASIO_ERROR_NOT_PRESENT;
  }
  TRACE("Setting sample rate to %f\n", rate);
  char rate_str[32];
  sprintf(rate_str, "%lu", (size_t)pwasio->sample_rate);
  if (pw_filter_update_properties(pwasio->filter, nullptr,
                                  &SPA_DICT_ITEMS(SPA_DICT_ITEM(
                                      PW_KEY_NODE_FORCE_RATE, rate_str))) < 0) {
    sprintf(pwasio->err_msg, "Failed to set sample rate to %s\n", rate_str);
    return ASIO_ERROR_HW_MALFUNCTION;
  }
  pwasio->sample_rate = rate;
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32)
GetClockSources(struct asio *, struct asio_clock_source *clocks, LONG32 *num) {
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
STDMETHODIMP_(LONG32) SetClockSource(struct asio *_data, LONG32 idx) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (!idx) {
    sprintf(pwasio->err_msg, "Invalid clock source\n");
    return ASIO_ERROR_NOT_PRESENT;
  }
  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32)
GetSamplePosition(struct asio *_data, struct asio_samples *pos,
                  struct asio_timestamp *nsec) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (!nsec || !pos)
    return ASIO_ERROR_INVALID_PARAMETER;

  *pos = pwasio->pos;
  *nsec = pwasio->nsec;

  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32)
GetChannelInfo(struct asio *_data, struct asio_channel_info *info) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (info->index < 0 ||
      (info->input ? info->index >= (LONG32)pwasio->n_inputs
                   : info->index >= (LONG32)pwasio->n_outputs))
    return ASIO_ERROR_INVALID_PARAMETER;

  info->active = !!pwasio->ports[info->input * pwasio->n_inputs + info->index];
  info->group = 0;
  info->type = ASIO_SAMPLE_TYPE_FLOAT32_LSB;

  if (info->input)
    sprintf(info->name, "in_%d", info->index);
  else
    sprintf(info->name, "out_%d", info->index);

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
CreateBuffers(struct asio *_data, struct asio_buffer_info *channels,
              LONG32 n_channels, LONG32 buffer_size,
              struct asio_callbacks *callbacks) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->filter) {
    sprintf(pwasio->err_msg, "Tried to create buffers without filter\n");
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

  char bufsize_str[32];
  sprintf(bufsize_str, "%u", buffer_size);
  if (pw_filter_update_properties(
          pwasio->filter, nullptr,
          &SPA_DICT_ITEMS(
              SPA_DICT_ITEM(PW_KEY_NODE_FORCE_QUANTUM, bufsize_str))) < 0) {
    sprintf(pwasio->err_msg, "Failed to set sample rate to %s\n", bufsize_str);
    return ASIO_ERROR_HW_MALFUNCTION;
  }

  pwasio->buffer_size = buffer_size;
  pwasio->callbacks = callbacks;

  size_t offset =
      SPA_MAX(pwasio->buffer_size * sizeof(float), (size_t)getpagesize());
  if ((pwasio->fd = memfd_create("pwasio-buf", MFD_CLOEXEC)) < 0) {
    sprintf(pwasio->err_msg, "Failed to create buffer file descriptor\n");
    goto cleanup;
  }
  if (ftruncate(pwasio->fd, 2 * MAX_PORTS * offset) < 0) {
    sprintf(pwasio->err_msg, "Failed to truncate buffer file descriptor\n");
    goto cleanup;
  }

  struct spa_pod_builder b = {};
  char buf[1024];
  spa_pod_builder_init(&b, buf, sizeof buf);
  const struct spa_pod *params[] = {
      spa_pod_builder_add_object(
          &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
          SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
          SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
          SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_DSP_F32)),
      spa_pod_builder_add_object(
          &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
          SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(2), SPA_PARAM_BUFFERS_blocks,
          SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
          SPA_POD_Int(buffer_size * sizeof(float)), SPA_PARAM_BUFFERS_stride,
          SPA_POD_Int(sizeof(float))),
  };

  for (size_t channel = 0; channel < (size_t)n_channels; channel++) {
    struct asio_buffer_info *buffer_info = channels + channel;
    char name[16];
    if (buffer_info->input && buffer_info->channel < (LONG32)pwasio->n_inputs)
      sprintf(name, "in_%d", buffer_info->channel);
    else if (!buffer_info->input &&
             buffer_info->channel < (LONG32)pwasio->n_outputs)
      sprintf(name, "out_%d", buffer_info->channel);
    else {
      sprintf(pwasio->err_msg, "Invalid channel requested\n");
      goto cleanup;
    }

    size_t i = !buffer_info->input * pwasio->n_inputs + buffer_info->channel;
    if (!(pwasio->ports[i] = pw_filter_add_port(
              pwasio->filter,
              buffer_info->input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
              PW_FILTER_PORT_FLAG_ALLOC_BUFFERS, sizeof *pwasio->ports[i],
              pw_properties_new(PW_KEY_PORT_NAME, name, nullptr), params,
              SPA_N_ELEMENTS(params)))) {
      sprintf(pwasio->err_msg, "Filter port creation failed\n");
      goto cleanup;
    }
    pw_filter_update_properties(
        pwasio->filter, pwasio->ports[i],
        &SPA_DICT_ITEMS(
            SPA_DICT_ITEM(PW_KEY_FORMAT_DSP, "32 bit float mono audio")));
    *pwasio->ports[i] = (typeof(*pwasio->ports[i])){
        .ptr = {MAP_FAILED, MAP_FAILED},
    };
    for (size_t b = 0; b < 2; b++) {
      if ((pwasio->ports[i]->ptr[b] = buffer_info->buf[b] =
               mmap(nullptr, pwasio->buffer_size * sizeof(float),
                    PROT_READ | PROT_WRITE, MAP_SHARED, pwasio->fd,
                    pwasio->ports[i]->offset[b] = (2 * i + b) * offset)) ==
          MAP_FAILED) {
        sprintf(pwasio->err_msg, "Failed to mmap buffer\n");
        goto cleanup;
      }
      TRACE("port %lu buffer %lu mapped to %p\n", i, b, buffer_info->buf[b]);
    }
  }

  if (pw_filter_connect(pwasio->filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) <
      0) {
    sprintf(pwasio->err_msg, "Failed to connect to PipeWire\n");
    goto cleanup;
  }

  return ASIO_ERROR_OK;
cleanup:
  for (size_t i = 0; i < MAX_PORTS; i++)
    if (pwasio->ports[i]) {
      for (size_t b = 0; b < 2; b++)
        if (pwasio->ports[i]->ptr[b])
          munmap(pwasio->ports[i]->ptr[b], pwasio->buffer_size * sizeof(float));
      pw_filter_remove_port(pwasio->ports[i]);
      pwasio->ports[i] = nullptr;
    }
  if (pwasio->fd > 0)
    close(pwasio->fd);
  return ASIO_ERROR_NO_MEMORY;
}
STDMETHODIMP_(LONG32) DisposeBuffers(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  TRACE("Disposing all buffers\n");

  if (pwasio->running)
    pwasio->vtbl->Stop(_data);

  if (!pwasio->filter) {
    sprintf(pwasio->err_msg, "Tried to dispose of buffers without filter\n");
    return ASIO_ERROR_NOT_PRESENT;
  }

  if (pwasio->fd < 0) {
    sprintf(pwasio->err_msg,
            "Tried to dispose of buffers when none are allocated\n");
    return ASIO_ERROR_NOT_PRESENT;
  }

  pw_filter_disconnect(pwasio->filter);

  for (size_t i = 0; i < MAX_PORTS; i++)
    if (pwasio->ports[i]) {
      for (size_t b = 0; b < 2; b++)
        if (pwasio->ports[i]->ptr[b])
          munmap(pwasio->ports[i]->ptr[b], pwasio->buffer_size * sizeof(float));
      pw_filter_remove_port(pwasio->ports[i]);
      pwasio->ports[i] = nullptr;
    }

  close(pwasio->fd);

  return 0;
}
STDMETHODIMP_(LONG32) not_impl() { return ASIO_ERROR_NOT_PRESENT; }

struct thread {
  HANDLE handle;
  DWORD thread_id;
  void *(*start)(void *);
  void *arg, *ret;
};
static DWORD WINAPI thread_func(LPVOID p) {
  struct thread *t = p;
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
  if (priority == -1) {
    priority = THREAD_PRIORITY_TIME_CRITICAL;
    sched_setscheduler(0, SCHED_FIFO,
                       &(struct sched_param){.sched_priority = 1});
  }
  return -!SetThreadPriority(t->handle, priority);
}
static int _drop_rt(void *, struct spa_thread *p) {
  struct thread *t = (struct thread *)p;
  sched_setscheduler(0, SCHED_OTHER,
                     &(struct sched_param){.sched_priority = 0});
  return -!SetThreadPriority(t->handle, THREAD_PRIORITY_NORMAL);
}

HRESULT WINAPI CreateInstance(LPCLASSFACTORY, LPUNKNOWN outer, REFIID,
                              LPVOID *ptr) {
  if (outer)
    return CLASS_E_NOAGGREGATION;

  if (!ptr)
    return E_INVALIDARG;

  HRESULT hr = E_UNEXPECTED;
  struct pwasio *pwasio = *ptr = nullptr;
  pwasio = HeapAlloc(GetProcessHeap(), 0, sizeof(*pwasio));
  if (!pwasio) {
    ERR("Failed to allocate pwasio object\n");
    hr = E_OUTOFMEMORY;
    goto cleanup;
  }

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
      .ControlPanel = (void *)not_impl,
      .Future = (void *)not_impl,
      .OutputReady = (void *)not_impl,
  };
  *pwasio = (typeof(*pwasio)){
      .vtbl = &vtbl,
      .ref = 1,
      .sample_rate = DEFAULT_SAMPLE_RATE,
      .fd = -1,
  };

  TRACE("Starting pwasio\n");
  SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

  pw_init(nullptr, nullptr);
  TRACE("Compiled with libpipewire-%s\n", pw_get_headers_version());
  TRACE("Linked with libpipewire-%s\n", pw_get_library_version());

  if (!(pwasio->loop = pw_data_loop_new(nullptr))) {
    ERR("Failed to create PipeWire loop\n");
    goto cleanup;
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
  pw_data_loop_set_thread_utils(pwasio->loop, &thread_utils);

  *ptr = pwasio;
  return S_OK;

cleanup:
  if (pwasio)
    pwasio->vtbl->Release((struct asio *)pwasio);
  return hr;
}
