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
#include "resource.h"

#include <pipewire/pipewire.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <shlwapi.h>
#include <winuser.h>

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

#define MAX_NAME 32
#define MAX_PORTS 32
#define pwasio_err(code, msg, ...)                                             \
  do {                                                                         \
    sprintf(pwasio->err_msg, "%s: " msg "\n",                                  \
            __func__ __VA_OPT__(, ) __VA_ARGS__);                              \
    return code;                                                               \
  } while (false)

#define KEY_N_INPUTS "n_inputs"
#define KEY_N_OUTPUTS "n_outputs"
#define KEY_BUFSIZE "buffer_size"
#define KEY_SMPRATE "sample_rate"
#define KEY_AUTOCON "autoconnect"

#define DEF_N_INPUTS 2
#define DEF_N_OUTPUTS 2
#define DEF_BUFSIZE 256
#define DEF_SMPRATE 48000
#define DEF_AUTOCON 1

struct port {
  bool active;
  size_t offset[2];
};

struct thread {
  HANDLE handle;
  DWORD thread_id;
  pthread_t tid;

  void *(*start)(void *);
  void *arg, *ret;
};

struct pwasio {
  const struct asioVtbl *vtbl;
  LONG32 ref;
  HINSTANCE hinst;

  char err_msg[256];

  char name[MAX_NAME];
  size_t n_inputs, n_outputs, buffer_size, sample_rate;
  bool autoconnect;

  struct spa_thread_utils thread_utils;
  struct thread thread;
  struct pw_data_loop *loop;
  struct pw_stream *input, *output;
  struct pw_time time;
  struct pw_buffer *input_buf[2], *output_buf[2];
  struct port inputs[MAX_PORTS], outputs[MAX_PORTS];
  size_t idx;

  int fd;
  size_t fsize;
  float *buffer;

  bool running;

  struct asio_callbacks *callbacks;

  HANDLE panel;
  HWND dialog;

  bool move;
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
  if (InterlockedDecrement(&pwasio->ref))
    return pwasio->ref;

  if (pwasio->panel) {
    if (pwasio->dialog)
      PostMessageA(pwasio->dialog, WM_COMMAND, IDCANCEL, 0);
    WaitForSingleObject(pwasio->panel, 3000);
    CloseHandle(pwasio->panel);
  }

  pwasio->vtbl->DisposeBuffers(_data);
  if (pwasio->output)
    pw_stream_destroy(pwasio->output);
  if (pwasio->input)
    pw_stream_destroy(pwasio->input);
  if (pwasio->loop)
    pw_data_loop_destroy(pwasio->loop);
  pw_deinit();

  HeapFree(GetProcessHeap(), 0, pwasio);

  return 0;
}

/* Paranoid driver developers should assume that the application will access
 * buffer 0 as soon as bufferSwitch(0) is called, up until bufferSwitch(1)
 * returns (and vice-versa). */
static int _swap_buffers(struct spa_loop *, bool, uint32_t, const void *,
                         size_t, void *_data) {
  struct pwasio *pwasio = _data;
  if (pw_data_loop_in_thread(pwasio->loop))
    pwasio->callbacks->swap_buffers(pwasio->idx, true);
  pwasio->idx = !pwasio->idx;
  pw_stream_trigger_process(pwasio->output);
  return 0;
}
static void _input_process(void *_data) {
  struct pwasio *pwasio = _data;

  pw_stream_get_time_n(pwasio->input, &pwasio->time, sizeof pwasio->time);

  struct pw_buffer *buf;
  if ((buf = pw_stream_dequeue_buffer(pwasio->input)))
    pw_stream_queue_buffer(pwasio->input, buf);

  pw_data_loop_invoke(pwasio->loop, _swap_buffers, SPA_ID_INVALID, nullptr, 0,
                      false, pwasio);
}
static void _input_add_buffer(void *_data, struct pw_buffer *buf) {
  struct pwasio *pwasio = _data;
  size_t idx = !!pwasio->input_buf[0];
  pwasio->input_buf[idx] = buf;
  for (size_t i = 0; i < pwasio->n_inputs; i++)
    buf->buffer->datas[i] = (typeof(buf->buffer->datas[i])){
        .type = SPA_DATA_MemFd,
        .fd = pwasio->fd,
        .mapoffset = pwasio->inputs[i].offset[idx] * sizeof(float),
        .maxsize = pwasio->buffer_size * sizeof(float),
        .data = pwasio->buffer + pwasio->inputs[i].offset[idx],
        .chunk = buf->buffer->datas[i].chunk,
    };
};
static void _input_rem_buffer(void *_data, struct pw_buffer *buf) {
  struct pwasio *pwasio = _data;
  if (buf == pwasio->input_buf[0])
    pwasio->input_buf[0] = nullptr;
  if (buf == pwasio->input_buf[1])
    pwasio->input_buf[1] = nullptr;
}
static void _output_process(void *_data) {
  struct pwasio *pwasio = _data;
  struct pw_buffer *buf;
  if ((buf = pw_stream_dequeue_buffer(pwasio->output))) {
    for (size_t i = 0; i < pwasio->n_outputs; i++) {
      buf->buffer->datas[i].chunk->offset = 0;
      buf->buffer->datas[i].chunk->size = pwasio->buffer_size * sizeof(float);
      buf->buffer->datas[i].chunk->stride = sizeof(float);
      buf->buffer->datas[i].chunk->flags = 0;
    }
    pw_stream_queue_buffer(pwasio->output, buf);
  }
}
static void _output_add_buffer(void *_data, struct pw_buffer *buf) {
  struct pwasio *pwasio = _data;
  size_t idx = !!pwasio->output_buf[0];
  pwasio->output_buf[idx] = buf;
  for (size_t i = 0; i < pwasio->n_outputs; i++)
    buf->buffer->datas[i] = (typeof(buf->buffer->datas[i])){
        .type = SPA_DATA_MemFd,
        .fd = pwasio->fd,
        .mapoffset = pwasio->outputs[i].offset[idx] * sizeof(float),
        .maxsize = pwasio->buffer_size * sizeof(float),
        .data = pwasio->buffer + pwasio->outputs[i].offset[idx],
        .chunk = buf->buffer->datas[i].chunk,
    };
};
static void _output_rem_buffer(void *_data, struct pw_buffer *buf) {
  struct pwasio *pwasio = _data;
  if (buf == pwasio->output_buf[0])
    pwasio->output_buf[0] = nullptr;
  if (buf == pwasio->output_buf[1])
    pwasio->output_buf[1] = nullptr;
}
STDMETHODIMP_(LONG32) Init(struct asio *_data, void *) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  WCHAR path[MAX_PATH];
  GetModuleFileNameW(0, path, MAX_PATH);
  WideCharToMultiByte(CP_ACP, WC_SEPCHARS, StrRChrW(path, NULL, '\\') + 1, -1,
                      pwasio->name, sizeof pwasio->name, NULL, NULL);

  char rate_str[8], bufsize_str[8];
  sprintf(rate_str, "%lu", pwasio->sample_rate);
  sprintf(bufsize_str, "%lu", pwasio->buffer_size);
  static const struct pw_stream_events input_events = {
      PW_VERSION_STREAM_EVENTS,
      .add_buffer = _input_add_buffer,
      .remove_buffer = _input_rem_buffer,
      .process = _input_process,
  };
  if (!(pwasio->input = pw_stream_new_simple(
            pw_data_loop_get_loop(pwasio->loop), pwasio->name,
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                              "Capture", PW_KEY_MEDIA_ROLE, "Music",
                              PW_KEY_NODE_ALWAYS_PROCESS, "true",
                              PW_KEY_NODE_FORCE_RATE, rate_str,
                              PW_KEY_NODE_FORCE_QUANTUM, bufsize_str, nullptr),
            &input_events, pwasio)))
    pwasio_err(ASIO_ERROR_NO_MEMORY, "failed to create input stream");

  static const struct pw_stream_events output_events = {
      PW_VERSION_STREAM_EVENTS,
      .add_buffer = _output_add_buffer,
      .remove_buffer = _output_rem_buffer,
      .process = _output_process,
  };
  if (!(pwasio->output = pw_stream_new_simple(
            pw_data_loop_get_loop(pwasio->loop), pwasio->name,
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                              "Playback", PW_KEY_MEDIA_ROLE, "Music",
                              PW_KEY_NODE_ALWAYS_PROCESS, "true",
                              PW_KEY_NODE_FORCE_RATE, rate_str,
                              PW_KEY_NODE_FORCE_QUANTUM, bufsize_str, nullptr),
            &output_events, pwasio))) {
    pw_stream_destroy(pwasio->input);
    pwasio->input = nullptr;
    pwasio_err(ASIO_ERROR_NO_MEMORY, "failed to create output stream");
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

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if (pw_data_loop_start(pwasio->loop) < 0)
    pwasio_err(ASIO_ERROR_HW_MALFUNCTION, "failed to start PipeWire data loop");

  pwasio->running = true;
  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32) Stop(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if (pwasio->running) {
    pwasio->running = false;
    pw_data_loop_stop(pwasio->loop);
  }

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetChannels(struct asio *_data, LONG *n_inputs, LONG *n_outputs) {
  if (!n_inputs || !n_outputs)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *n_inputs = pwasio->n_inputs;
  *n_outputs = pwasio->n_outputs;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG) GetLatencies(struct asio *_data, LONG *in, LONG *out) {
  if (!in || !out)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *in = pwasio->buffer_size;
  *out = 0;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetBufferSize(struct asio *_data, LONG32 *min, LONG32 *max, LONG32 *pref,
              LONG32 *grn) {
  if (!min || !max || !pref || !grn)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *min = *max = *pref = pwasio->buffer_size;
  *grn = 0;
  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32) CanSampleRate(struct asio *_data, DOUBLE rate) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if ((size_t)rate != pwasio->sample_rate)
    pwasio_err(ASIO_ERROR_NO_CLOCK, "invalid sample rate");

  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) GetSampleRate(struct asio *_data, DOUBLE *rate) {
  if (!rate)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *rate = pwasio->sample_rate;
  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32) SetSampleRate(struct asio *_data, DOUBLE rate) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if ((size_t)rate != pwasio->sample_rate)
    pwasio_err(ASIO_ERROR_NO_CLOCK, "invalid sample rate");

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetClockSources(struct asio *_data, struct asio_clock_source *clocks,
                LONG32 *num) {
  if (!clocks || !num)
    return ASIO_ERROR_INVALID_PARAMETER;

  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
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
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
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

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  *pos = (typeof(*pos)){
      .lo = (uint64_t)pwasio->time.ticks,
      .hi = ((uint64_t)pwasio->time.ticks) >> 32,
  };
  *nsec = (typeof(*nsec)){
      .lo = (uint64_t)pwasio->time.now,
      .hi = ((uint64_t)pwasio->time.now) >> 32,
  };

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetChannelInfo(struct asio *_data, struct asio_channel_info *info) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if (info->index < 0)
    return ASIO_ERROR_INVALID_PARAMETER;

  if (info->input) {
    if (info->index >= (LONG32)pwasio->n_inputs)
      return ASIO_ERROR_INVALID_PARAMETER;
    info->active = pwasio->inputs[info->index].active;
    sprintf(info->name, "in_%d", info->index);
  } else {
    if (info->index >= (LONG32)pwasio->n_outputs)
      return ASIO_ERROR_INVALID_PARAMETER;
    info->active = pwasio->outputs[info->index].active;
    sprintf(info->name, "out_%d", info->index);
  }

  info->group = 0;
  info->type = ASIO_SAMPLE_TYPE_FLOAT32_LSB;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
CreateBuffers(struct asio *_data, struct asio_buffer_info *channels,
              LONG32 n_channels, LONG32 buffer_size,
              struct asio_callbacks *callbacks) {
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  if (!channels || !callbacks)
    return ASIO_ERROR_INVALID_PARAMETER;

  if ((size_t)buffer_size != pwasio->buffer_size)
    pwasio_err(ASIO_ERROR_INVALID_MODE, "invalid buffer size %d", buffer_size);

  pwasio->callbacks = callbacks;

  size_t offset = SPA_MAX(pwasio->buffer_size * sizeof *pwasio->buffer,
                          (size_t)getpagesize()) /
                  sizeof(float);
  pwasio->fsize = 4 * MAX_PORTS * offset * sizeof *pwasio->buffer;

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
  for (size_t i = 0; i < MAX_PORTS; i++)
    for (size_t b = 0; b < 2; b++) {
      pwasio->inputs[i].offset[b] = (2 * i + b) * offset;
      pwasio->outputs[i].offset[b] = (2 * (i + pwasio->n_inputs) + b) * offset;
    }

  for (size_t c = 0; c < (size_t)n_channels; c++) {
    struct asio_buffer_info *info = channels + c;
    struct port *p;
    if (info->input && info->channel < (LONG32)pwasio->n_inputs)
      p = pwasio->inputs + info->channel;
    else if (!info->input && info->channel < (LONG32)pwasio->n_outputs)
      p = pwasio->outputs + info->channel;
    else {
      sprintf(msg, "Invalid channel requested %s %d\n",
              info->input ? "input" : "output", info->channel);
      res = ASIO_ERROR_INVALID_MODE;
      goto cleanup;
    }
    p->active = true;
    for (size_t b = 0; b < 2; b++)
      info->buf[b] = pwasio->buffer + p->offset[b];
  }

  enum pw_stream_flags flags =
      PW_STREAM_FLAG_ALLOC_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
  if (pwasio->autoconnect)
    flags |= PW_STREAM_FLAG_AUTOCONNECT;
  char buf[1024];
  {
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[] = {
        spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_DSP_F32),
            SPA_FORMAT_AUDIO_rate, SPA_POD_Int(pwasio->sample_rate),
            SPA_FORMAT_AUDIO_channels, SPA_POD_Int(pwasio->n_inputs)),
        spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(2), SPA_PARAM_BUFFERS_size,
            SPA_POD_Int(buffer_size * sizeof(float)), SPA_PARAM_BUFFERS_stride,
            SPA_POD_Int(sizeof(float)), SPA_PARAM_BUFFERS_align,
            SPA_POD_Int(offset * sizeof(float))),
    };
    if (pw_stream_connect(pwasio->input, PW_DIRECTION_INPUT, PW_ID_ANY, flags,
                          params, SPA_N_ELEMENTS(params)) < 0) {
      sprintf(msg, "Failed to connect input stream\n");
      res = ASIO_ERROR_NO_MEMORY;
      goto cleanup;
    }
  }
  flags |= PW_STREAM_FLAG_DRIVER;
  {
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[] = {
        spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_DSP_F32),
            SPA_FORMAT_AUDIO_rate, SPA_POD_Int(pwasio->sample_rate),
            SPA_FORMAT_AUDIO_channels, SPA_POD_Int(pwasio->n_outputs)),
        spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(2), SPA_PARAM_BUFFERS_size,
            SPA_POD_Int(buffer_size * sizeof(float)), SPA_PARAM_BUFFERS_stride,
            SPA_POD_Int(sizeof(float)), SPA_PARAM_BUFFERS_align,
            SPA_POD_Int(offset * sizeof(float))),
    };
    if (pw_stream_connect(pwasio->output, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags,
                          params, SPA_N_ELEMENTS(params)) < 0) {
      sprintf(msg, "Failed to connect output stream\n");
      res = ASIO_ERROR_NO_MEMORY;
      goto cleanup;
    }
  }

  return ASIO_ERROR_OK;
cleanup:
  if (pw_stream_get_state(pwasio->output, nullptr) !=
      PW_STREAM_STATE_UNCONNECTED)
    pw_stream_disconnect(pwasio->output);
  if (pw_stream_get_state(pwasio->input, nullptr) !=
      PW_STREAM_STATE_UNCONNECTED)
    pw_stream_disconnect(pwasio->input);

  for (size_t i = 0; i < MAX_PORTS; i++)
    pwasio->inputs[i].active = pwasio->outputs[i].active = false;
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
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!pwasio->input || !pwasio->output)
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO");

  pwasio->vtbl->Stop(_data);

  if (pwasio->fd < 0)
    pwasio_err(ASIO_ERROR_INVALID_MODE, "no buffers");

  if (pw_stream_get_state(pwasio->output, nullptr) !=
      PW_STREAM_STATE_UNCONNECTED)
    pw_stream_disconnect(pwasio->output);
  if (pw_stream_get_state(pwasio->input, nullptr) !=
      PW_STREAM_STATE_UNCONNECTED)
    pw_stream_disconnect(pwasio->input);

  for (size_t i = 0; i < MAX_PORTS; i++)
    pwasio->inputs[i].active = pwasio->outputs[i].active = false;

  munmap(pwasio->buffer, pwasio->fsize);
  pwasio->buffer = nullptr;
  close(pwasio->fd);
  pwasio->fd = -1;

  return 0;
}

struct cfg {
  size_t n_inputs, n_outputs, buffer_size, sample_rate;
  bool autoconnect;
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
    break;
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDOK:
      BOOL conv;
      size_t val;

      val = GetDlgItemInt(hWnd, IDE_INPUTS, &conv, false);
      if (conv) {
        val = SPA_MIN(val, MAX_PORTS);
        cfg->reset = cfg->reset || val != cfg->n_inputs;
        cfg->n_inputs = val;
      }

      val = GetDlgItemInt(hWnd, IDE_OUTPUTS, &conv, false);
      if (conv) {
        val = SPA_MIN(val, MAX_PORTS);
        cfg->reset = cfg->reset || val != cfg->n_outputs;
        cfg->n_outputs = val;
      }

      val = GetDlgItemInt(hWnd, IDE_BUFSIZE, &conv, false);
      if (conv) {
        cfg->reset = cfg->reset || val != cfg->buffer_size;
        cfg->buffer_size = val;
      }

      val = GetDlgItemInt(hWnd, IDE_SMPRATE, &conv, false);
      if (conv) {
        cfg->reset = cfg->reset || val != cfg->sample_rate;
        cfg->sample_rate = val;
      }

      val = IsDlgButtonChecked(hWnd, IDC_AUTOCON) == BST_CHECKED;
      cfg->reset = cfg->reset || val != cfg->autoconnect;
      cfg->autoconnect = val;
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

#define CHK(call)                                                              \
  do {                                                                         \
    if ((call) != ERROR_SUCCESS)                                               \
      goto cleanup;                                                            \
  } while (false)
static DWORD WINAPI _panel_thread(LPVOID p) {
  struct pwasio *pwasio = p;

  struct cfg cfg = {
      .n_inputs = pwasio->n_inputs,
      .n_outputs = pwasio->n_outputs,
      .buffer_size = pwasio->buffer_size,
      .sample_rate = pwasio->sample_rate,
      .autoconnect = pwasio->autoconnect,
  };
  if (!(pwasio->dialog = CreateDialogParamA(pwasio->hinst,
                                            (LPCSTR)MAKEINTRESOURCE(IDD_PANEL),
                                            NULL, _panel_func, (LPARAM)&cfg)))
    return -1;

  ShowWindow(pwasio->dialog, SW_SHOW);

  MSG msg;
  while (GetMessageA(&msg, NULL, 0, 0) > 0) {
    if (!IsDialogMessageA(pwasio->dialog, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }

  if (cfg.reset) {
    HKEY driver = NULL;
    CHK(RegOpenKeyExA(HKEY_CURRENT_USER, DRIVER_REG, 0, KEY_WRITE, &driver));

    if (cfg.n_inputs != pwasio->n_inputs)
      CHK(RegSetValueExA(driver, KEY_N_INPUTS, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.n_inputs}, sizeof(DWORD)));
    if (cfg.n_outputs != pwasio->n_outputs)
      CHK(RegSetValueExA(driver, KEY_N_OUTPUTS, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.n_outputs}, sizeof(DWORD)));
    if (cfg.buffer_size != pwasio->buffer_size)
      CHK(RegSetValueExA(driver, KEY_BUFSIZE, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.buffer_size}, sizeof(DWORD)));
    if (cfg.sample_rate != pwasio->sample_rate)
      CHK(RegSetValueExA(driver, KEY_SMPRATE, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.sample_rate}, sizeof(DWORD)));
    if (cfg.autoconnect != pwasio->autoconnect)
      CHK(RegSetValueExA(driver, KEY_AUTOCON, 0, REG_DWORD,
                         (BYTE *)&(DWORD){cfg.autoconnect}, sizeof(DWORD)));

    if (pwasio->callbacks && pwasio->callbacks->message)
      pwasio->callbacks->message(ASIO_MESSAGE_RESET_REQUEST, 0, nullptr,
                                 nullptr);
  cleanup:
    if (driver)
      RegCloseKey(driver);
  }

  pwasio->dialog = nullptr;

  return 0;
}
#undef CHK
STDMETHODIMP_(LONG32) ControlPanel(struct asio *_data) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  if (pwasio->panel) {
    if (pwasio->dialog)
      return ASIO_ERROR_OK;
    WaitForSingleObject(pwasio->panel, INFINITE);
    CloseHandle(pwasio->panel);
    pwasio->panel = nullptr;
  }

  HANDLE t = CreateThread(NULL, 0, _panel_thread, pwasio, 0, nullptr);
  if (!t)
    return ASIO_ERROR_NOT_PRESENT;

  pwasio->panel = t;

  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) not_impl() { return ASIO_ERROR_NOT_PRESENT; }

static DWORD WINAPI _thread_func(LPVOID p) {
  struct thread *t = p;
  t->tid = pthread_self();
  t->ret = t->start(t->arg);
  return 0;
}
static struct spa_thread *_create(void *_data, const struct spa_dict *,
                                  void *(*start)(void *), void *arg) {
  struct pwasio *pwasio = _data;
  struct thread *t = &pwasio->thread;

  *t = (typeof(*t)){
      .start = start,
      .arg = arg,
  };
  t->handle = CreateThread(NULL, 0, _thread_func, t, 0, &t->thread_id);
  if (!t->handle)
    return nullptr;

  while (!t->tid)
    ;

  return (struct spa_thread *)t->tid;
}
static int _join(void *_data, struct spa_thread *, void **retval) {
  struct pwasio *pwasio = _data;
  struct thread *t = &pwasio->thread;
  if (!t->handle)
    return -1;

  DWORD result = WaitForSingleObject(t->handle, INFINITE);
  if (retval)
    *retval = t->ret;

  CloseHandle(t->handle);
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

#define get_dword(config, key, default)                                        \
  ({                                                                           \
    DWORD out;                                                                 \
    LONG err = RegQueryValueExA(config, key, 0, nullptr, (BYTE *)&out,         \
                                &(DWORD){sizeof out});                         \
    if (err == ERROR_FILE_NOT_FOUND)                                           \
      err = RegSetValueExA(config, KEY_N_INPUTS, 0, REG_DWORD,                 \
                           (BYTE *)&(DWORD){out = DEF_N_INPUTS}, sizeof out);  \
    if (err != ERROR_SUCCESS) {                                                \
      RegCloseKey(config);                                                     \
      goto error_registry;                                                     \
    }                                                                          \
    out;                                                                       \
  })
HRESULT WINAPI CreateInstance(LPCLASSFACTORY _data, LPUNKNOWN outer, REFIID,
                              LPVOID *ptr) {
  if (outer)
    return CLASS_E_NOAGGREGATION;

  if (!ptr)
    return E_INVALIDARG;

  HRESULT hr;

  struct pwasio *pwasio = *ptr = nullptr;
  pwasio = HeapAlloc(GetProcessHeap(), 0, sizeof(*pwasio));
  if (!pwasio) {
    ERR("Failed to allocate pwasio object\n");
    hr = E_OUTOFMEMORY;
    goto cleanup;
  }

  struct factory *factory = (struct factory *)_data;

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
  static const struct spa_thread_utils_methods thread_utils_methods = {
      .create = _create,
      .join = _join,
      .get_rt_range = _get_rt_range,
      .acquire_rt = _acquire_rt,
      .drop_rt = _drop_rt,
  };
  *pwasio = (typeof(*pwasio)){
      .vtbl = &vtbl,
      .ref = 1,
      .hinst = factory->hinst,

      .thread_utils =
          {
              .iface =
                  {
                      .type = SPA_TYPE_INTERFACE_ThreadUtils,
                      .version = SPA_VERSION_THREAD_UTILS,
                      .cb =
                          {
                              .funcs = &thread_utils_methods,
                              .data = pwasio,
                          },
                  },
          },

      .fd = -1,
      .buffer = MAP_FAILED,
  };

  HKEY config = NULL;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, DRIVER_REG, 0, NULL, 0,
                      KEY_WRITE | KEY_READ, NULL, &config,
                      nullptr) == ERROR_SUCCESS) {
    pwasio->n_inputs = get_dword(config, KEY_N_INPUTS, DEF_N_INPUTS);
    pwasio->n_outputs = get_dword(config, KEY_N_OUTPUTS, DEF_N_OUTPUTS);
    pwasio->buffer_size = get_dword(config, KEY_BUFSIZE, DEF_BUFSIZE);
    pwasio->sample_rate = get_dword(config, KEY_SMPRATE, DEF_SMPRATE);
    pwasio->autoconnect = get_dword(config, KEY_AUTOCON, DEF_AUTOCON);
    RegCloseKey(config);
  } else {
  error_registry:
    WARN("Unable to read configuration, using defaults\n");
    pwasio->n_inputs = DEF_N_INPUTS;
    pwasio->n_outputs = DEF_N_OUTPUTS;
    pwasio->buffer_size = DEF_BUFSIZE;
    pwasio->sample_rate = DEF_SMPRATE;
    pwasio->autoconnect = DEF_AUTOCON;
  }

  TRACE("Starting pwasio\n");
  SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
  struct rlimit rl;
  if (getrlimit(RLIMIT_RTPRIO, &rl) || rl.rlim_max < 1 || !(rl.rlim_cur = 1) ||
      setrlimit(RLIMIT_RTPRIO, &rl)) {
    ERR("Unable to get realtime privileges\n");
    hr = E_UNEXPECTED;
    goto cleanup;
  };

  pw_init(nullptr, nullptr);
  TRACE("Compiled with libpipewire-%s\n", pw_get_headers_version());
  TRACE("Linked with libpipewire-%s\n", pw_get_library_version());

  if (!(pwasio->loop = pw_data_loop_new(nullptr))) {
    ERR("Failed to create PipeWire loop\n");
    hr = E_UNEXPECTED;
    goto cleanup;
  }
  pw_data_loop_set_thread_utils(pwasio->loop, &pwasio->thread_utils);

  *ptr = pwasio;
  return S_OK;

cleanup:
  if (pwasio)
    pwasio->vtbl->Release((struct asio *)pwasio);

  return hr;
}
#undef CHK
