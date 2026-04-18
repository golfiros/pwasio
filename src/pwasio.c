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
#include "resource.h"

#include <pipewire/pipewire.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <commctrl.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/json.h>

WINE_DEFAULT_DEBUG_CHANNEL(pwasio);

#define MAX_STR 1024

#define pwasio_err(code, msg, ...)                                             \
  do {                                                                         \
    WINE_ERR(msg "\n" __VA_OPT__(, ) __VA_ARGS__);                             \
    snprintf(pwasio->err_msg, sizeof pwasio->err_msg, "%s: " msg "\n",         \
             __func__ __VA_OPT__(, ) __VA_ARGS__);                             \
    return code;                                                               \
  } while (false)

#define KEY_BUFSIZE "buffer_size"
#define KEY_SMPRATE "sample_rate"
#define KEY_PRIORITY "priority"
#define KEY_HOST_PRIORITY "host_priority"
#define KEY_INPUTS "inputs"
#define KEY_OUTPUTS "outputs"

#define DEFAULT_BUFSIZE 256
#define DEFAULT_SMPRATE 48000
#define DEFAULT_AUTOCON 1
#define DEFAULT_PRIORITY 0
static const char dummy_port[] = "dummy:port\0";

#define PWASIO_TARGET "ASIO:target:"

struct thread {
  HANDLE handle;
  DWORD thread_id;
  pthread_t tid;
  int priority;
  atomic_bool ready;

  void *(*start)(void *);
  void *arg, *ret;
};

// TODO: use a signal
static DWORD WINAPI _thread_func(LPVOID p) {
  struct thread *t = p;
  t->tid = pthread_self();
  atomic_store_explicit(&t->ready, true, memory_order_release);
  t->ret = t->start(t->arg);
  return 0;
}
static struct spa_thread *_create(void *_data, const struct spa_dict *,
                                  void *(*start)(void *), void *arg) {
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
static int _join(void *_data, struct spa_thread *, void **retval) {
  struct thread *t = _data;
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
static int _acquire_rt(void *_data, struct spa_thread *, int priority) {
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
static int _drop_rt(void *_data, struct spa_thread *) {
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
static const struct spa_thread_utils_methods thread_utils_methods = {
    .create = _create,
    .join = _join,
    .get_rt_range = _get_rt_range,
    .acquire_rt = _acquire_rt,
    .drop_rt = _drop_rt,
};

struct channel {
  size_t *port, idx;
  enum pw_direction dir;
  size_t offset[2];
  struct pw_buffer *buffer[2];
};
struct engine {
  size_t n_channels;
  struct channel *channels;

  size_t idx, pos, nsec;

  int fd;
  size_t maxsize;
  float *buffer;

  bool running;

  struct asio_callbacks *callbacks;
};
static void _add_buffer(void *_data, void *_port, struct pw_buffer *buf) {
  struct engine *engine = _data;
  struct channel *channel = &engine->channels[*(size_t *)_port];

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
  data->fd = engine->fd;
  data->maxsize = engine->maxsize * sizeof(float);
}
static void _remove_buffer(void *_data, void *_port, struct pw_buffer *buf) {
  struct engine *engine = _data;
  struct channel *channel = &engine->channels[*(size_t *)_port];

  if (buf == channel->buffer[0])
    channel->buffer[0] = nullptr;
  if (buf == channel->buffer[1])
    channel->buffer[1] = nullptr;
}
static void _process(void *_data, struct spa_io_position *pos) {
  struct engine *engine = _data;

  engine->pos = pos->clock.position;
  engine->nsec = pos->clock.nsec;

  for (size_t i = 0; i < engine->n_channels; i++) {
    struct channel channel = engine->channels[i];
    if (SPA_LIKELY(channel.port))
      pw_filter_dequeue_buffer(channel.port);
  }

  engine->callbacks->swap_buffers(engine->idx, false);

  struct pw_buffer *buf;
  for (size_t i = 0; i < engine->n_channels; i++) {
    struct channel channel = engine->channels[i];
    if (SPA_LIKELY(buf = channel.buffer[engine->idx])) {
      if (channel.dir != PW_DIRECTION_INPUT) {
        struct spa_data *d = &buf->buffer->datas[0];
        d->chunk->offset = 0;
        d->chunk->size = pos->clock.duration * sizeof(float);
        d->chunk->stride = sizeof(float);
        d->chunk->flags = 0;
      }
      pw_filter_queue_buffer(channel.port, buf);
    }
  }

  engine->idx = !engine->idx;
}
static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .add_buffer = _add_buffer,
    .remove_buffer = _remove_buffer,
    .process = _process,
};

struct node {
  uint32_t id;
  char name[MAX_STR], display[MAX_STR];
  struct port *ports[2];
  struct node *next;
};
struct port {
  size_t idx;
  uint32_t id;
  char name[MAX_STR];
  struct port *next;
};

struct context {
  struct pw_thread_loop *th_loop;
  struct pw_context *context;
  struct pw_data_loop *loop;

  struct pw_core *core;
  struct spa_hook core_listener;
  int last, pending, res;
  struct pw_registry *registry;
  struct spa_hook registry_listener;

  struct pw_filter *filter;

  struct path *paths;
  struct node *nodes, *unknown;

  struct pw_module *realtime;
  struct pw_metadata *settings, *defaults;
};

struct pwasio {
  const struct asioVtbl *vtbl;
  LONG32 ref;

  char name[ASIO_MAX_NAME];
  char err_msg[ASIO_MAX_ERR];

  struct context context;

  size_t buffer_size, sample_rate;
  char *ports[2];

  pthread_t host_tid, audio_tid;
  int host_priority;
  struct spa_thread_utils thread_utils;
  struct thread thread;

  struct engine engine;

  HINSTANCE hinst;
  HANDLE panel;
  _Atomic HWND dialog;
};

static void _done(void *_data, uint32_t id, int seq) {
  struct context *context = _data;
  if (id != PW_ID_CORE)
    return;
  context->last = seq;
  if (context->pending == seq)
    pw_thread_loop_signal(context->th_loop, false);
}
static void _error(void *_data, uint32_t id, int, int res, const char *) {
  struct context *context = _data;
  if (id == PW_ID_CORE) {
    if (res == -ENOENT)
      return;
    context->res = res;
  }
  pw_thread_loop_signal(context->th_loop, false);
}
static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = _done,
    .error = _error,
};
static inline int core_sync(struct context *context) {
  return context->pending =
             pw_proxy_sync((struct pw_proxy *)context->core, context->pending);
}
static inline int core_wait(const struct context *context) {
  while (true) {
    pw_thread_loop_wait(context->th_loop);
    if (context->res < 0)
      return context->res;
    if (context->last == context->pending)
      break;
  }
  return 0;
}

struct module {
  struct spa_hook listener;
  int priority;
};
static void _info(void *_data, const struct pw_module_info *info) {
  struct module *module = _data;
  const char *val;
  if ((val = spa_dict_lookup(info->props, "rt.prio")))
    module->priority = pw_properties_parse_int(val);
}
static const struct pw_module_events module_events = {
    PW_VERSION_MODULE_EVENTS,
    .info = _info,
};

struct metadata {
  struct spa_hook listener;
  enum {
    PWASIO_METADATA_SETTINGS,
    PWASIO_METADATA_DEFAULTS,
  } type;
  union {
    struct {
      size_t buffer_size, sample_rate;
    };
    char defaults[2][MAX_STR];
  };
};
int _property(void *_data, uint32_t, const char *key, const char *,
              const char *value) {
  struct metadata *metadata = _data;
  if (!key || !value)
    return 0;
  if (metadata->type == PWASIO_METADATA_SETTINGS) {
    if (spa_streq(key, "clock.rate"))
      metadata->sample_rate = pw_properties_parse_uint64(value);
    else if (spa_streq(key, "clock.quantum"))
      metadata->buffer_size = pw_properties_parse_uint64(value);
  } else {
    if (spa_streq(key, "default.audio.source"))
      spa_json_str_object_find(value, strlen(value), "name",
                               metadata->defaults[PW_DIRECTION_INPUT],
                               sizeof metadata->defaults[PW_DIRECTION_INPUT]);
    else if (spa_streq(key, "default.audio.sink"))
      spa_json_str_object_find(value, strlen(value), "name",
                               metadata->defaults[PW_DIRECTION_OUTPUT],
                               sizeof metadata->defaults[PW_DIRECTION_OUTPUT]);
  }
  return 0;
}
static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = _property,
};

static void _global(void *_data, uint32_t id, uint32_t, const char *type,
                    uint32_t version, const struct spa_dict *props) {
  struct context *context = _data;
  const char *val;
  if (spa_streq(type, PW_TYPE_INTERFACE_Module)) {
    if (!(val = spa_dict_lookup(props, PW_KEY_MODULE_NAME)) ||
        !spa_streq(val, "libpipewire-module-rt"))
      return;
    struct module *priority;
    if (!(context->realtime = pw_registry_bind(context->registry, id, type,
                                               version, sizeof *priority)))
      return;
    priority = pw_proxy_get_user_data((struct pw_proxy *)context->realtime);
    *priority = (typeof(*priority)){.priority = DEFAULT_PRIORITY};
    pw_module_add_listener(context->realtime, &priority->listener,
                           &module_events, priority);
    core_sync(context);
  } else if (spa_streq(type, PW_TYPE_INTERFACE_Metadata)) {
    if (!(val = spa_dict_lookup(props, PW_KEY_METADATA_NAME)))
      return;
    if (spa_streq(val, "settings")) {
      struct metadata *settings;
      if (!(context->settings = pw_registry_bind(context->registry, id, type,
                                                 version, sizeof *settings)))
        return;
      settings = pw_proxy_get_user_data((struct pw_proxy *)context->settings);
      *settings = (typeof(*settings)){
          .type = PWASIO_METADATA_SETTINGS,
          .buffer_size = DEFAULT_BUFSIZE,
          .sample_rate = DEFAULT_SMPRATE,
      };
      pw_metadata_add_listener(context->settings, &settings->listener,
                               &metadata_events, settings);
      core_sync(context);
    } else if (spa_streq(val, "default")) {
      struct metadata *defaults;
      if (!(context->defaults = pw_registry_bind(context->registry, id, type,
                                                 version, sizeof *defaults)))
        return;
      defaults = pw_proxy_get_user_data((struct pw_proxy *)context->defaults);
      *defaults = (typeof(*defaults)){
          .type = PWASIO_METADATA_DEFAULTS,
      };
      pw_metadata_add_listener(context->defaults, &defaults->listener,
                               &metadata_events, defaults);
      core_sync(context);
    }
  } else if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
    if (context->filter && id == pw_filter_get_node_id(context->filter))
      return;

    bool audio = false, internal = false;
    if ((val = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS))) {
      char str[256];
      strcpy(str, val);
      for (char *t = strtok(str, "/"); t; t = strtok(nullptr, "/")) {
        audio |= spa_streq(t, "Audio");
        internal |= spa_streq(t, "Internal");
      }
    } else if ((val = spa_dict_lookup(props, PW_KEY_MEDIA_TYPE)) &&
               spa_streq(val, "Audio"))
      audio = true;

    if (!audio || internal)
      return;

    struct node *node, **root = &context->nodes;

    if (!(node = malloc(sizeof *node)))
      return;
    *node = (typeof(*node)){.id = id};
    if (!(val = spa_dict_lookup(props, PW_KEY_NODE_NAME))) {
      free(node);
      return;
    }
    strncpy(node->name, val, sizeof node->name);
    if ((val = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION)))
      strncpy(node->display, val, sizeof node->display);
    else if ((val = spa_dict_lookup(props, PW_KEY_NODE_NICK)))
      strncpy(node->display, val, sizeof node->display);
    else
      strncpy(node->display, node->name, sizeof node->display);

    struct node **p = root;
    while (*p && (*p)->id < id)
      p = &(*p)->next;
    node->next = *p;
    *p = node;
  } else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
    if ((val = spa_dict_lookup(props, PW_KEY_PORT_MONITOR)) &&
        spa_streq(val, "true"))
      return;

    enum pw_direction dir;
    if (!(val = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)))
      return;
    if (spa_streq(val, "in")) {
      dir = PW_DIRECTION_INPUT;
    } else if (spa_streq(val, "out")) {
      dir = PW_DIRECTION_OUTPUT;
    } else
      return;

    if (!(val = spa_dict_lookup(props, PW_KEY_PORT_ID)))
      return;
    size_t idx = pw_properties_parse_uint64(val);

    if (!(val = spa_dict_lookup(props, PW_KEY_NODE_ID)))
      return;
    uint32_t node_id = pw_properties_parse_int(val);

    struct node *node = nullptr;
    if (context->filter && node_id == pw_filter_get_node_id(context->filter)) {
      if (!(val = spa_dict_lookup(props, PW_KEY_PORT_EXTRA)))
        return;
      val += strlen(PWASIO_TARGET);

      struct port *port = nullptr;
      for (node = context->nodes; node && !port; node = node->next)
        for (port = node->ports[!dir]; port; port = port->next) {
          char name[MAX_STR];
          snprintf(name, sizeof name, "%s:%s", node->name, port->name);
          if (spa_streq(val, name))
            break;
        }
      if (port) {
        struct pw_properties *props;
        if (!(props = pw_properties_new(PW_KEY_OBJECT_LINGER, "true", nullptr)))
          return;
        if (dir == PW_DIRECTION_INPUT) {
          pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", port->id);
          pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", id);
        } else {
          pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", id);
          pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", port->id);
        }
        pw_proxy_destroy(pw_core_create_object(
            context->core, "link-factory", PW_TYPE_INTERFACE_Link,
            PW_VERSION_LINK, &props->dict, 0));
        pw_properties_free(props);
        return;
      }
      dir = !dir;
      node = context->unknown;
    } else {
      if (!(val = spa_dict_lookup(props, PW_KEY_PORT_NAME)))
        return;
      for (node = context->nodes; node; node = node->next)
        if (node->id == node_id)
          break;
      if (!node)
        return;
    }

    struct port *port, **root = &node->ports[dir];
    if (!(port = malloc(sizeof *port)))
      return;
    *port = (typeof(*port)){
        .idx = idx,
        .id = id,
    };
    strncpy(port->name, val, sizeof port->name);

    struct port **p = root;
    while (*p && (*p)->idx < port->idx)
      p = &(*p)->next;
    port->next = *p;
    *p = port;
  }
}
static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = _global,
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
  LONG32 ref;
  if ((ref = InterlockedDecrement(&pwasio->ref)))
    return ref;

  if (pwasio->panel) {
    // TODO: tell the panel to close
    if (pwasio->dialog)
      PostMessage(pwasio->dialog, WM_COMMAND, IDCANCEL, 0);
    WaitForSingleObject(pwasio->panel, 3000);
    CloseHandle(pwasio->panel);
  }

  struct context *context = &pwasio->context;
  if (context->filter)
    pwasio->vtbl->DisposeBuffers(_data);

  for (size_t i = 0; i < 2; i++)
    if (pwasio->ports[i] != dummy_port)
      free(pwasio->ports[i]);

  if (context->th_loop) {
    pw_loop_invoke(pw_thread_loop_get_loop(context->th_loop), nullptr, 0,
                   nullptr, 0, false, nullptr);
    pw_thread_loop_stop(context->th_loop);
  }
  if (context->registry) {
    spa_hook_remove(&context->registry_listener);
    pw_proxy_destroy((struct pw_proxy *)context->registry);
  }
  for (struct node *node = context->nodes, *next; node; node = next) {
    for (size_t i = 0; i < 2; i++)
      for (struct port *port = node->ports[i], *next; port; port = next) {
        next = port->next;
        free(port);
      }
    next = node->next;
    free(node);
  }

  if (context->core) {
    spa_hook_remove(&context->core_listener);
    pw_core_disconnect(context->core);
  }
  if (context->context)
    pw_context_destroy(context->context);
  if (context->th_loop)
    pw_thread_loop_destroy(context->th_loop);

  if (pwasio->host_priority) {
    WINE_TRACE("setting host scheduler to SCHED_OTHER\n");
    pthread_setschedparam(pwasio->host_tid, SCHED_OTHER,
                          &(struct sched_param){.sched_priority = 0});
  }

  WINE_TRACE("stopping PipeWire\n");
  pw_deinit();

  HeapFree(GetProcessHeap(), 0, pwasio);

  return 0;
}

STDMETHODIMP_(LONG32) Init(struct asio *_data, void *) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;
  struct context *context = &pwasio->context;

  WINE_TRACE("starting pwasio\n");

  WCHAR path[MAX_PATH];
  GetModuleFileNameW(0, path, MAX_PATH);
  WideCharToMultiByte(CP_ACP, WC_SEPCHARS, StrRChrW(path, nullptr, '\\') + 1,
                      -1, pwasio->name, sizeof pwasio->name, nullptr, nullptr);

  int res;
  char msg[256];

  struct pw_properties *props;
  if (!(props = pw_properties_new(PW_KEY_CLIENT_NAME, pwasio->name,
                                  PW_KEY_CLIENT_API, "ASIO", nullptr))) {
    res = ASIO_ERROR_NO_MEMORY;
    snprintf(msg, sizeof msg, "failed to allocate PipeWire properties");
    goto cleanup;
  }
  if (!(context->th_loop = pw_thread_loop_new(pwasio->name, nullptr))) {
    res = ASIO_ERROR_NO_MEMORY;
    snprintf(msg, sizeof msg, "failed to create PipeWire loop");
    goto cleanup;
  }
  if (!(context->context =
            pw_context_new(pw_thread_loop_get_loop(context->th_loop),
                           pw_properties_copy(props), 0))) {
    res = ASIO_ERROR_NO_MEMORY;
    snprintf(msg, sizeof msg, "failed to create PipeWire context");
    goto cleanup;
  }
  pwasio->thread_utils.iface = SPA_INTERFACE_INIT(
      SPA_TYPE_INTERFACE_ThreadUtils, SPA_VERSION_THREAD_UTILS,
      &thread_utils_methods, &pwasio->thread);
  context->loop = pw_context_get_data_loop(context->context);
  pw_data_loop_set_thread_utils(context->loop, &pwasio->thread_utils);
  pw_data_loop_stop(context->loop);

  pw_thread_loop_start(context->th_loop);
  pw_thread_loop_lock(context->th_loop);

  if (!(context->core = pw_context_connect(context->context, props, 0))) {
    res = ASIO_ERROR_NO_MEMORY;
    snprintf(msg, sizeof msg, "failed to connect to PipeWire");
    goto cleanup;
  }

  props = nullptr;
  pw_core_add_listener(context->core, &context->core_listener, &core_events,
                       context);

  if (!(context->nodes = context->unknown = malloc(sizeof *context->unknown))) {
    res = ASIO_ERROR_NO_MEMORY;
    snprintf(msg, sizeof msg, "failed to allocate node tree");
    goto cleanup;
  }
  *context->nodes = (typeof(*context->nodes)){
      .id = SPA_ID_INVALID,
      .display = "Unknown",
  };

  if (!(context->registry =
            pw_core_get_registry(context->core, PW_VERSION_REGISTRY, 0))) {
    res = ASIO_ERROR_NO_MEMORY;
    snprintf(msg, sizeof msg, "failed to enumerate PipeWire objects");
    goto cleanup;
  }
  pw_registry_add_listener(context->registry, &context->registry_listener,
                           &registry_events, context);
  core_sync(context);
  if (core_wait(context) < 0) {
    res = ASIO_ERROR_HW_MALFUNCTION;
    snprintf(msg, sizeof msg, "PipeWire core error");
    goto cleanup;
  };

  HKEY key = nullptr;
  if (RegCreateKeyEx(HKEY_CURRENT_USER, DRIVER_REG, 0, nullptr, 0,
                     KEY_WRITE | KEY_READ, nullptr, &key, nullptr))
    key = nullptr;

  DWORD out;
  if (key && !RegQueryValueEx(key, KEY_BUFSIZE, 0, nullptr, (BYTE *)&out,
                              &(DWORD){sizeof out}))
    pwasio->buffer_size = out;
  else if (context->settings) {
    struct metadata *settings =
        pw_proxy_get_user_data((struct pw_proxy *)context->settings);
    pwasio->buffer_size = settings->buffer_size;
  } else
    pwasio->buffer_size = DEFAULT_BUFSIZE;
  if (key && !RegQueryValueEx(key, KEY_SMPRATE, 0, nullptr, (BYTE *)&out,
                              &(DWORD){sizeof out}))
    pwasio->sample_rate = out;
  else if (context->settings) {
    struct metadata *settings =
        pw_proxy_get_user_data((struct pw_proxy *)context->settings);
    pwasio->sample_rate = settings->sample_rate;
  } else
    pwasio->sample_rate = DEFAULT_SMPRATE;
  if (context->settings) {
    struct metadata *settings =
        pw_proxy_get_user_data((struct pw_proxy *)context->settings);
    spa_hook_remove(&settings->listener);
    pw_proxy_destroy((struct pw_proxy *)context->settings);
  }

  if (key && !RegQueryValueEx(key, KEY_PRIORITY, 0, nullptr, (BYTE *)&out,
                              &(DWORD){sizeof out})) {
    pwasio->thread.priority = out;
  } else if (context->realtime) {
    struct module *module =
        pw_proxy_get_user_data((struct pw_proxy *)context->realtime);
    pwasio->thread.priority = module->priority;
  } else
    pwasio->thread.priority = DEFAULT_PRIORITY;
  if (context->realtime) {
    struct module *module =
        pw_proxy_get_user_data((struct pw_proxy *)context->realtime);
    spa_hook_remove(&module->listener);
    pw_proxy_destroy((struct pw_proxy *)context->realtime);
  }

  if (key && !RegQueryValueEx(key, KEY_HOST_PRIORITY, 0, nullptr, (BYTE *)&out,
                              &(DWORD){sizeof out}))
    pwasio->host_priority = SPA_MIN((int)out, pwasio->thread.priority);
  else
    pwasio->host_priority = pwasio->thread.priority / 2;

  if (key &&
      !RegQueryValueEx(key, KEY_INPUTS, nullptr, nullptr, nullptr, &out)) {
    if ((pwasio->ports[PW_DIRECTION_INPUT] = malloc(out)))
      RegQueryValueEx(key, KEY_INPUTS, nullptr, nullptr,
                      (BYTE *)pwasio->ports[PW_DIRECTION_INPUT], &out);
  }
  if (key &&
      !RegQueryValueEx(key, KEY_OUTPUTS, nullptr, nullptr, nullptr, &out)) {
    if ((pwasio->ports[PW_DIRECTION_OUTPUT] = malloc(out)))
      RegQueryValueEx(key, KEY_OUTPUTS, nullptr, nullptr,
                      (BYTE *)pwasio->ports[PW_DIRECTION_OUTPUT], &out);
  }

  for (size_t i = 0; i < 2; i++) {
    if (!pwasio->ports[i]) {
      size_t len;
      if (context->defaults) {
        struct metadata *defaults =
            pw_proxy_get_user_data((struct pw_proxy *)context->defaults);
        for (const struct node *node = context->nodes;
             node && !pwasio->ports[i]; node = node->next)
          if (spa_streq(node->name, defaults->defaults[i])) {
            len = 1;
            for (const struct port *port = node->ports[!i]; port;
                 port = port->next)
              len += snprintf(nullptr, 0, "%s:%s", node->name, port->name) + 1;
            char *p;
            if (!(p = pwasio->ports[i] = malloc(len))) {
              pwasio->ports[i] = (char *)dummy_port;
              len = sizeof dummy_port;
              break;
            }
            pwasio->ports[i][len - 1] = '\0';
            for (const struct port *port = node->ports[!i]; port;
                 port = port->next)
              p += sprintf(p, "%s:%s", node->name, port->name) + 1;
          }
      } else {
        pwasio->ports[i] = (char *)dummy_port;
        len = sizeof dummy_port;
      }
    }
  }

  if (context->defaults) {
    struct metadata *defaults =
        pw_proxy_get_user_data((struct pw_proxy *)context->defaults);
    spa_hook_remove(&defaults->listener);
    pw_proxy_destroy((struct pw_proxy *)context->defaults);
  }

  if (key)
    RegCloseKey(key);

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT]) {
    res = ASIO_ERROR_NOT_PRESENT;
    snprintf(msg, sizeof msg, "no IO configured");
    goto cleanup;
  }

  if (pwasio->thread.priority) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_RTPRIO, &rl) || rl.rlim_max < 1 ||
        !(rl.rlim_cur =
              SPA_MAX(pwasio->thread.priority, pwasio->host_priority)) ||
        setrlimit(RLIMIT_RTPRIO, &rl)) {
      res = ASIO_ERROR_HW_MALFUNCTION;
      snprintf(msg, sizeof msg, "unable to get realtime privileges: %s",
               strerror(errno));
      goto cleanup;
    }
    if (pwasio->host_priority) {
      pwasio->host_tid = pthread_self();
      WINE_TRACE("setting host scheduler to SCHED_FIFO with priority %d\n",
                 pwasio->host_priority);
      if (pthread_setschedparam(
              pwasio->host_tid, SCHED_FIFO,
              &(struct sched_param){.sched_priority = pwasio->host_priority}))
        WINE_ERR("unable to set host realtime priority\n");
    }
  }

  pw_thread_loop_unlock(context->th_loop);

  return 1;

cleanup:
  for (size_t i = 0; i < 2; i++)
    if (pwasio->ports[i] != dummy_port)
      free(pwasio->ports[i]);
  if (context->registry) {
    spa_hook_remove(&context->registry_listener);
    pw_proxy_destroy((struct pw_proxy *)context->registry);
    context->registry = nullptr;
  }
  for (struct node *node = context->nodes, *next; node; node = next) {
    for (size_t i = 0; i < 2; i++)
      for (struct port *port = node->ports[i], *next; port; port = next) {
        next = port->next;
        free(port);
      }
    next = node->next;
    free(node);
  }
  context->nodes = nullptr;
  if (context->core) {
    spa_hook_remove(&context->core_listener);
    pw_core_disconnect(context->core);
    context->core = nullptr;
  }
  if (context->context) {
    pw_context_destroy(context->context);
    context->context = nullptr;
  }
  if (context->th_loop) {
    pw_thread_loop_destroy(context->th_loop);
    context->th_loop = nullptr;
  }
  if (props)
    pw_properties_free(props);

  pwasio_err(res, "%s", msg);
}

STDMETHODIMP_(VOID) GetDriverName(struct asio *, PSTR name) {
  WINE_TRACE("\n");
  snprintf(name, ASIO_MAX_NAME, "pwasio");
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
    snprintf(string, ASIO_MAX_ERR, "%s", pwasio->err_msg);
    *pwasio->err_msg = '\0';
  } else
    snprintf(string, ASIO_MAX_ERR, "undocumented error\n");
}

STDMETHODIMP_(LONG32) Start(struct asio *_data) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;
  const struct context *context = &pwasio->context;
  const struct engine *engine = &pwasio->engine;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  if (engine->running)
    return ASIO_ERROR_OK;

  pw_thread_loop_lock(context->th_loop);
  int res = pw_data_loop_start(context->loop);
  pw_thread_loop_unlock(context->th_loop);

  if (res < 0)
    pwasio_err(ASIO_ERROR_HW_MALFUNCTION, "failed to start PipeWire data loop");

  pwasio->engine.running = true;
  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32) Stop(struct asio *_data) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;
  const struct context *context = &pwasio->context;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  struct engine *engine = &pwasio->engine;

  if (!engine->running)
    return ASIO_ERROR_OK;

  engine->running = false;
  pw_thread_loop_lock(context->th_loop);
  int res = pw_data_loop_stop(context->loop);
  pw_thread_loop_unlock(context->th_loop);

  if (res < 0)
    pwasio_err(ASIO_ERROR_HW_MALFUNCTION, "failed to stop PipeWire data loop");

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetChannels(struct asio *_data, LONG *n_inputs, LONG *n_outputs) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  *n_inputs = 0;
  for (const char *p = (pwasio->ports[SPA_DIRECTION_INPUT]); *p;
       p += strlen(p) + 1)
    (*n_inputs)++;

  *n_outputs = 0;
  for (const char *p = (pwasio->ports[SPA_DIRECTION_OUTPUT]); *p;
       p += strlen(p) + 1)
    (*n_outputs)++;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG)
GetLatencies(struct asio *_data, LONG *in, LONG *out) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  *in = pwasio->buffer_size;
  *out = pwasio->buffer_size;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetBufferSize(struct asio *_data, LONG32 *min, LONG32 *max, LONG32 *pref,
              LONG32 *grn) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  *min = *max = *pref = pwasio->buffer_size;
  *grn = 0;
  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32) CanSampleRate(struct asio *_data, DOUBLE rate) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  if (fabs(rate - pwasio->sample_rate) > 0.5)
    pwasio_err(ASIO_ERROR_NO_CLOCK, "invalid sample rate");

  return ASIO_ERROR_OK;
}
STDMETHODIMP_(LONG32) GetSampleRate(struct asio *_data, DOUBLE *rate) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  *rate = pwasio->sample_rate;
  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32) SetSampleRate(struct asio *_data, DOUBLE rate) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  if (fabs(rate - pwasio->sample_rate) > 0.5)
    pwasio_err(ASIO_ERROR_NO_CLOCK, "invalid sample rate");

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetClockSources(struct asio *_data, struct asio_clock_source *clocks,
                LONG32 *num) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  if (!*num)
    return ASIO_ERROR_OK;

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

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  if (idx)
    return ASIO_ERROR_INVALID_MODE;

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetSamplePosition(struct asio *_data, struct asio_samples *pos,
                  struct asio_timestamp *nsec) {
  struct pwasio *pwasio = (struct pwasio *)_data;
  const struct engine *engine = &pwasio->engine;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  if (!engine->running)
    pwasio_err(ASIO_ERROR_SP_NOT_ADVANCING, "clock not running");

  pthread_t tid;
  if ((tid = pthread_self()) != pwasio->audio_tid) {
    pwasio->audio_tid = tid;
    if (pthread_setschedparam(
            pwasio->audio_tid, SCHED_FIFO,
            &(struct sched_param){.sched_priority = pwasio->thread.priority}))
      WINE_ERR("unable to set host realtime priority\n");
  }

  *pos = (typeof(*pos)){
      .lo = engine->pos,
      .hi = engine->pos >> 32,
  };
  *nsec = (typeof(*nsec)){
      .lo = engine->nsec,
      .hi = engine->nsec >> 32,
  };

  return ASIO_ERROR_OK;
}

STDMETHODIMP_(LONG32)
GetChannelInfo(struct asio *_data, struct asio_channel_info *info) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  struct engine *engine = &pwasio->engine;

  if (info->input) {
    if (engine->channels)
      for (size_t i = 0; i < engine->n_channels; i++)
        if (engine->channels[i].dir == PW_DIRECTION_INPUT &&
            engine->channels[i].idx == (size_t)info->index) {
          info->active = true;
          break;
        }
    snprintf(info->name, sizeof info->name, "in_%d", info->index);
  } else {
    if (engine->channels)
      for (size_t i = 0; i < engine->n_channels; i++)
        if (engine->channels[i].dir == PW_DIRECTION_OUTPUT &&
            engine->channels[i].idx == (size_t)info->index) {
          info->active = true;
          break;
        }
    snprintf(info->name, sizeof info->name, "out_%d", info->index);
  }

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
  struct context *context = &pwasio->context;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  if (buffer_size != (LONG32)pwasio->buffer_size)
    pwasio_err(ASIO_ERROR_INVALID_MODE, "invalid buffer size %d", buffer_size);

  struct engine *engine = &pwasio->engine;
  *engine = (typeof(*engine)){
      .n_channels = n_channels,

      .fd = -1,
      .maxsize = SPA_MAX(buffer_size * sizeof(float), (size_t)getpagesize()) /
                 sizeof(float),
      .buffer = MAP_FAILED,

      .callbacks = callbacks,
  };

  size_t fsize = 2 * n_channels * engine->maxsize * sizeof(float);

  char msg[sizeof pwasio->err_msg];
  LONG32 res;
  if ((engine->fd = memfd_create("pwasio-buf", MFD_CLOEXEC)) < 0 ||
      ftruncate(engine->fd, fsize) < 0 ||
      (engine->buffer = mmap(nullptr, fsize, PROT_READ | PROT_WRITE, MAP_SHARED,
                             engine->fd, 0)) == MAP_FAILED ||
      !(engine->channels = malloc(n_channels * sizeof *engine->channels))) {
    res = ASIO_ERROR_NO_MEMORY;
    snprintf(msg, sizeof msg, "buffer allocations failed");
    goto cleanup;
  }

  struct pw_properties *props;
  if (!(props = pw_properties_copy(pw_core_get_properties(context->core)))) {
    res = ASIO_ERROR_NO_MEMORY;
    snprintf(msg, sizeof msg, "failed to create PipeWire filter");
    goto cleanup;
  }

  pw_properties_set(props, PW_KEY_NODE_NAME, pwasio->name);
  pw_properties_set(props, PW_KEY_NODE_GROUP, "group.dsp.0");
  pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, pwasio->name);
  pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
  pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Duplex");
  pw_properties_set(props, PW_KEY_MEDIA_ROLE, "DSP");
  pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");
  pw_properties_setf(props, PW_KEY_NODE_FORCE_RATE, "%lu", pwasio->sample_rate);
  pw_properties_setf(props, PW_KEY_NODE_FORCE_QUANTUM, "%d", buffer_size);

  pw_thread_loop_lock(context->th_loop);
  if (!(context->filter = pw_filter_new_simple(
            pw_data_loop_get_loop(context->loop), pwasio->name, props,
            &filter_events, engine))) {
    res = ASIO_ERROR_NO_MEMORY;
    snprintf(msg, sizeof msg, "failed to create PipeWire filter\n");
    pw_properties_free(props);
    pw_thread_loop_unlock(context->th_loop);
    goto cleanup;
  }
  size_t offset = 0;
  for (size_t c = 0; c < (size_t)n_channels; c++) {
    struct asio_buffer_info *info = &channels[c];
    struct channel *channel = &engine->channels[c];
    *channel = (typeof(*channel)){
        .idx = info->index,
    };

    struct pw_properties *props;
    if (!(props = pw_properties_new(nullptr, nullptr))) {
      res = ASIO_ERROR_NO_MEMORY;
      snprintf(msg, sizeof msg, "unable to allocate port for %s %d",
               info->input ? "input" : "output", info->index);
      pw_thread_loop_unlock(context->th_loop);
      goto cleanup;
    }
    if (info->input) {
      pw_properties_setf(props, PW_KEY_PORT_NAME, "in_%d", info->index);
      channel->dir = PW_DIRECTION_INPUT;
    } else {
      pw_properties_setf(props, PW_KEY_PORT_NAME, "out_%d", info->index);
      channel->dir = PW_DIRECTION_OUTPUT;
    }

    size_t idx = info->index;
    const char *port;
    for (port = (pwasio->ports[channel->dir]); *port;
         port += strlen(port) + 1) {
      if (!idx--)
        break;
    }
    pw_properties_setf(props, PW_KEY_PORT_EXTRA, PWASIO_TARGET "%s", port);
    char buf[MAX_STR];
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
            SPA_POD_Int(engine->maxsize * sizeof(float)),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(sizeof(float)),
            SPA_PARAM_BUFFERS_align,
            SPA_POD_Int(engine->maxsize * sizeof(float)),
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)),
    };
    if (!(channel->port = pw_filter_add_port(
              context->filter, channel->dir, PW_FILTER_PORT_FLAG_ALLOC_BUFFERS,
              sizeof *channel->port, props, params, SPA_N_ELEMENTS(params)))) {
      res = ASIO_ERROR_NO_MEMORY;
      snprintf(msg, sizeof msg, "unable to allocate port for %s %d",
               info->input ? "input" : "output", info->index);
      pw_properties_free(props);
      pw_thread_loop_unlock(context->th_loop);
      goto cleanup;
    }
    pw_filter_update_properties(
        context->filter, channel->port,
        &SPA_DICT_ITEMS(
            SPA_DICT_ITEM(PW_KEY_FORMAT_DSP, "32 bit float mono audio")));
    *channel->port = c;
    for (size_t b = 0; b < 2; b++) {
      channel->offset[b] = offset * sizeof(float);
      info->buf[b] = engine->buffer + offset;
      offset += engine->maxsize;
    }
  }
  if (pw_filter_connect(context->filter, PW_FILTER_FLAG_NONE, nullptr, 0) < 0) {
    snprintf(msg, sizeof msg, "Failed to connect filter");
    res = ASIO_ERROR_NO_MEMORY;
    pw_thread_loop_unlock(context->th_loop);
    goto cleanup;
  }
  core_sync(context);
  if (core_wait(context) < 0) {
    snprintf(msg, sizeof msg, "PipeWire core error");
    pw_thread_loop_unlock(context->th_loop);
    res = ASIO_ERROR_HW_MALFUNCTION;
    goto cleanup;
  }
  pw_thread_loop_unlock(context->th_loop);

  return ASIO_ERROR_OK;

cleanup:
  if (context->filter) {
    pw_thread_loop_lock(context->th_loop);
    pw_filter_destroy(context->filter);
    pw_thread_loop_unlock(context->th_loop);
    context->filter = nullptr;
  }
  if (engine->channels)
    free(engine->channels);
  if (engine->buffer != MAP_FAILED)
    munmap(engine->buffer, fsize);
  if (engine->fd >= 0)
    close(engine->fd);

  pwasio_err(res, "%s", msg);
}
STDMETHODIMP_(LONG32) DisposeBuffers(struct asio *_data) {
  WINE_TRACE("\n");
  struct pwasio *pwasio = (struct pwasio *)_data;
  struct context *context = &pwasio->context;

  if (!*pwasio->ports[PW_DIRECTION_INPUT] &&
      !*pwasio->ports[PW_DIRECTION_OUTPUT])
    pwasio_err(ASIO_ERROR_NOT_PRESENT, "no IO configured");

  if (!context->filter)
    pwasio_err(ASIO_ERROR_INVALID_MODE, "no buffers");

  struct engine *engine = &pwasio->engine;

  if (engine->running)
    pwasio->vtbl->Stop(_data);

  pw_thread_loop_lock(context->th_loop);
  pw_filter_destroy(context->filter);
  pw_thread_loop_unlock(context->th_loop);

  context->filter = nullptr;
  free(engine->channels);
  size_t fsize = 2 * engine->n_channels * engine->maxsize * sizeof(float);
  munmap(engine->buffer, fsize);
  close(engine->fd);

  return ASIO_ERROR_OK;
}

struct panel {
  struct context *context;
  HWND tree[2], list[2];
  size_t buffer_size, sample_rate;
  int priority, host_priority;
  size_t len[2];
  char *ports[2];
};
#define TVM_INIT (WM_APP + 1)
#define TVM_DESTROY (WM_APP + 2)
#define TVM_MOVEUP (WM_APP + 3)
#define TVM_MOVEDOWN (WM_APP + 4)
#define TVM_REMOVE (WM_APP + 5)
#define TVM_PARSE (WM_APP + 6)
static LRESULT CALLBACK _checkbox_func(HWND tree, UINT uMsg, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR uIdSubClass,
                                       DWORD_PTR) {
  struct panel *panel = (typeof(panel))GetWindowLongPtr(tree, GWLP_USERDATA);
  HWND list;
  int sel;
  if (panel) {
    list = panel->list[uIdSubClass];
    sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
  }
  switch (uMsg) {
  case TVM_INIT: {
    struct panel *panel = (typeof(panel))lParam;
    SetWindowLongPtr(tree, GWLP_USERDATA, lParam);

    SetWindowLong(tree, GWL_STYLE,
                  GetWindowLong(tree, GWL_STYLE) | TVS_CHECKBOXES |
                      TVS_HASLINES | TVS_NOHSCROLL);

    list = panel->list[uIdSubClass];
    SetWindowLong(list, GWL_STYLE,
                  GetWindowLong(tree, GWL_STYLE) | LVS_REPORT |
                      LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS);

    RECT rc;
    GetClientRect(list, &rc);
    ListView_InsertColumn(list, 0,
                          &((LVCOLUMN){
                              .mask = LVCF_WIDTH,
                              .cx = rc.right - rc.left,
                          }));
    for (const char *p = panel->ports[uIdSubClass]; *p; p += strlen(p) + 1)
      ListView_InsertItem(list, &((LVITEM){
                                    .mask = LVIF_PARAM,
                                }));

    for (const struct node *node = panel->context->nodes; node;
         node = node->next) {
      HTREEITEM hnode = nullptr;
      for (const struct port *port = node->ports[!uIdSubClass]; port;
           port = port->next) {
        if (!hnode) {
          hnode = TreeView_InsertItem(
              tree,
              &((TVINSERTSTRUCT){
                  .hParent = TVI_ROOT,
                  .hInsertAfter = TVI_LAST,
                  .item =
                      {
                          .mask = TVIF_TEXT | TVIF_STATE,
                          .pszText = (LPSTR)node->display,
                          .state = TVIS_STATEIMAGEMASK | TVIS_EXPANDED,
                          .stateMask = INDEXTOSTATEIMAGEMASK(3) | TVIS_EXPANDED,
                      },
              }));
        }
        char *name;
        if (node == panel->context->unknown) {
          if (!(name = strdup(port->name))) {
            PostMessage(tree, TVM_DESTROY, 0, 0);
            return TRUE;
          }
        } else {
          if (!(name = malloc(
                    snprintf(nullptr, 0, "%s:%s", node->name, port->name) +
                    1))) {
            PostMessage(tree, TVM_DESTROY, 0, 0);
            return TRUE;
          }
          sprintf(name, "%s:%s", node->name, port->name);
        }
        HTREEITEM hport = TreeView_InsertItem(
            tree, &((TVINSERTSTRUCT){
                      .hParent = hnode,
                      .hInsertAfter = TVI_LAST,
                      .item =
                          {
                              .mask = TVIF_TEXT | TVIF_PARAM,
                              .pszText = (LPSTR)port->name,
                              .lParam = (LPARAM)name,
                          },
                  }));
        sel = 0;
        for (const char *p = panel->ports[uIdSubClass]; *p;
             p += strlen(p) + 1) {
          if (spa_streq(p, name)) {
            TreeView_SetCheckState(tree, hport, true);
            ListView_SetItem(list, &((LVITEM){
                                       .iItem = sel,
                                       .mask = LVIF_PARAM,
                                       .lParam = (LPARAM)hport,
                                   }));
          }
          sel++;
        }
      }
    }
  } break;
  case TVM_DESTROY:
    for (HTREEITEM hnode = TreeView_GetRoot(tree); hnode;
         hnode = TreeView_GetNextSibling(tree, hnode)) {
      TVITEM port = {
          .mask = LVIF_PARAM,
      };
      for (port.hItem = TreeView_GetChild(tree, hnode); port.hItem;
           port.hItem = TreeView_GetNextSibling(tree, port.hItem)) {
        TreeView_GetItem(tree, &port);
        if (port.lParam)
          free((char *)port.lParam);
      }
    }
    break;
  case TVM_MOVEUP:
  case TVM_MOVEDOWN: {
    LVITEM src = {
        .iItem = sel,
        .mask = LVIF_PARAM,
    };
    ListView_GetItem(list, &src);

    LVITEM dst = {
        .mask = LVIF_PARAM,
    };
    if (sel > 0 && uMsg == TVM_MOVEUP)
      dst.iItem = sel - 1;
    else if (sel >= 0 && sel < ListView_GetItemCount(list) - 1 &&
             uMsg == TVM_MOVEDOWN)
      dst.iItem = sel + 1;
    else
      return DefSubclassProc(tree, uMsg, wParam, lParam);
    ListView_GetItem(list, &dst);

    src.iItem = dst.iItem;
    src.mask |= LVIF_STATE;
    src.state = src.stateMask = LVIS_SELECTED;
    ListView_SetItem(list, &src);

    dst.iItem = sel;
    ListView_SetItem(list, &dst);
  } break;
  case TVM_REMOVE:
    if (sel < 0)
      return DefSubclassProc(tree, uMsg, wParam, lParam);
    LVITEM item = {
        .iItem = sel,
        .mask = TVIF_PARAM,
    };
    ListView_GetItem(list, &item);
    ListView_DeleteItem(list, sel);
    if (item.lParam)
      TreeView_SetCheckState(tree, (HTREEITEM)item.lParam, false);
    break;
  case TVM_PARSE: {
    panel->len[uIdSubClass] = 1;
    for (size_t i = 0; i < (size_t)ListView_GetItemCount(list); i++) {
      LVITEM item = {
          .iItem = i,
          .mask = LVIF_PARAM,
      };
      ListView_GetItem(list, &item);
      TVITEM port = {
          .hItem = (typeof(port.hItem))item.lParam,
          .mask = TVIF_PARAM,
      };
      TreeView_GetItem(tree, &port);
      panel->len[uIdSubClass] += strlen((const char *)port.lParam) + 1;
    }
    char *p;
    if (!(p = panel->ports[uIdSubClass] = malloc(panel->len[uIdSubClass]))) {
      panel->len[uIdSubClass] = 0;
      break;
    }
    panel->ports[uIdSubClass][panel->len[uIdSubClass] - 1] = '\0';
    for (size_t i = 0; i < (size_t)ListView_GetItemCount(list); i++) {
      LVITEM item = {
          .iItem = i,
          .mask = LVIF_PARAM,
      };
      ListView_GetItem(list, &item);
      TVITEM port = {
          .hItem = (typeof(port.hItem))item.lParam,
          .mask = TVIF_PARAM,
      };
      TreeView_GetItem(tree, &port);
      size_t len = strlen((const char *)port.lParam) + 1;
      memcpy(p, (const char *)port.lParam, len);
      p += len;
    }
    break;
  }
  case WM_LBUTTONDOWN:
  case WM_LBUTTONDBLCLK:
  case WM_KEYDOWN: {
    TVITEM port = {
        .mask = TVIF_STATE | TVIF_PARAM,
        .stateMask = TVIS_STATEIMAGEMASK,
    };
    if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONDBLCLK) {
      TVHITTESTINFO ht = {
          .pt =
              {
                  GET_X_LPARAM(lParam),
                  GET_Y_LPARAM(lParam),
              },
      };
      TreeView_HitTest(tree, &ht);
      if (ht.flags & TVHT_ONITEMSTATEICON)
        port.hItem = ht.hItem;
    } else if (uMsg == WM_KEYDOWN && wParam == VK_SPACE)
      port.hItem = TreeView_GetSelection(tree);

    if (!port.hItem)
      return DefSubclassProc(tree, uMsg, wParam, lParam);
    TreeView_GetItem(tree, &port);

    port.mask &= ~TVIF_PARAM;
    if ((port.state & TVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(1)) {
      port.state = INDEXTOSTATEIMAGEMASK(2);
      ListView_InsertItem(list, &((LVITEM){
                                    .iItem = ListView_GetItemCount(list),
                                    .mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE,
                                    .lParam = (LPARAM)port.hItem,
                                    .stateMask = LVIS_SELECTED,
                                    .state = LVIS_SELECTED,
                                }));
    } else if ((port.state & TVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2)) {
      port.state = INDEXTOSTATEIMAGEMASK(1);
      for (size_t i = 0; i < (size_t)ListView_GetItemCount(list); i++) {
        LVITEM item = {
            .iItem = i,
            .mask = LVIF_PARAM,
        };
        ListView_GetItem(list, &item);
        if (port.hItem == (typeof(port.hItem))item.lParam) {
          ListView_DeleteItem(list, i);
          break;
        }
      }
    } else
      return TRUE;
    TreeView_SetItem(tree, &port);
  } break;
  default:
    return DefSubclassProc(tree, uMsg, wParam, lParam);
  }
  for (sel = 0; sel < ListView_GetItemCount(list); sel++) {
    LVITEM item = {
        .iItem = sel,
        .mask = LVIF_PARAM,
    };
    ListView_GetItem(list, &item);
    char buf[MAX_STR];
    if (item.lParam) {
      char port_name[MAX_STR], node_name[MAX_STR];
      TVITEM port = {
          .hItem = (typeof(port.hItem))item.lParam,
          .mask = TVIF_TEXT,
          .pszText = port_name,
          .cchTextMax = sizeof port_name,
      };
      TreeView_GetItem(tree, &port);
      TVITEM node = {
          .hItem = TreeView_GetParent(tree, port.hItem),
          .mask = TVIF_TEXT | TVIF_PARAM,
          .pszText = node_name,
          .cchTextMax = sizeof node_name,
      };
      TreeView_GetItem(tree, &node);
      snprintf(buf, sizeof buf, "%d - %s:%s", sel + 1, node_name, port_name);
    } else
      snprintf(buf, sizeof buf, "%d - port error", sel + 1);
    item.mask = LVIF_TEXT;
    item.pszText = buf;
    ListView_SetItem(list, &item);
  }
  return TRUE;
}
static INT_PTR CALLBACK _panel_func(HWND hWnd, UINT uMsg, WPARAM wParam,
                                    LPARAM lParam) {
  struct panel *panel = (typeof(panel))GetWindowLongPtr(hWnd, GWLP_USERDATA);

  switch (uMsg) {
  case WM_INITDIALOG: {
    panel = (typeof(panel))lParam;
    panel->tree[PW_DIRECTION_INPUT] = GetDlgItem(hWnd, IDC_INPUT_TREE);
    panel->tree[PW_DIRECTION_OUTPUT] = GetDlgItem(hWnd, IDC_OUTPUT_TREE);
    panel->list[PW_DIRECTION_INPUT] = GetDlgItem(hWnd, IDC_INPUT_LIST);
    panel->list[PW_DIRECTION_OUTPUT] = GetDlgItem(hWnd, IDC_OUTPUT_LIST);

    SetWindowLongPtr(hWnd, GWLP_USERDATA, lParam);

    pw_thread_loop_lock(panel->context->th_loop);
    for (size_t i = 0; i < 2; i++) {
      SetWindowSubclass(panel->tree[i], _checkbox_func, i,
                        (DWORD_PTR)panel->list[i]);
      SendMessage(panel->tree[i], TVM_INIT, 0, lParam);
    }
    pw_thread_loop_unlock(panel->context->th_loop);

    SetDlgItemInt(hWnd, IDE_BUFSIZE, panel->buffer_size, false);
    SetDlgItemInt(hWnd, IDE_SMPRATE, panel->sample_rate, false);
    SetDlgItemInt(hWnd, IDE_PRIORITY, panel->priority, false);
    SetDlgItemInt(hWnd, IDE_HOST_PRIORITY, panel->host_priority, false);
  } break;
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDC_INPUT_UP:
      SendMessage(panel->tree[PW_DIRECTION_INPUT], TVM_MOVEUP, 0, 0);
      break;
    case IDC_INPUT_DOWN:
      SendMessage(panel->tree[PW_DIRECTION_INPUT], TVM_MOVEDOWN, 0, 0);
      break;
    case IDC_INPUT_REMOVE:
      SendMessage(panel->tree[PW_DIRECTION_INPUT], TVM_REMOVE, 0, 0);
      break;
    case IDC_OUTPUT_UP:
      SendMessage(panel->tree[PW_DIRECTION_OUTPUT], TVM_MOVEUP, 0, 0);
      break;
    case IDC_OUTPUT_DOWN:
      SendMessage(panel->tree[PW_DIRECTION_OUTPUT], TVM_MOVEDOWN, 0, 0);
      break;
    case IDC_OUTPUT_REMOVE:
      SendMessage(panel->tree[PW_DIRECTION_OUTPUT], TVM_REMOVE, 0, 0);
      break;
    case IDOK:
      BOOL conv;
      INT val;
      val = GetDlgItemInt(hWnd, IDE_BUFSIZE, &conv, true);
      if (conv && val > 0)
        panel->buffer_size = val;
      val = GetDlgItemInt(hWnd, IDE_SMPRATE, &conv, true);
      if (conv && val > 0)
        panel->sample_rate = val;
      val = GetDlgItemInt(hWnd, IDE_PRIORITY, &conv, true);
      if (conv && val >= 0)
        panel->priority = val;
      val = GetDlgItemInt(hWnd, IDE_HOST_PRIORITY, &conv, true);
      if (conv && val >= 0)
        panel->host_priority = val;
      for (size_t i = 0; i < 2; i++)
        SendMessage(panel->tree[i], TVM_PARSE, 0, 0);
    case IDCANCEL:
      DestroyWindow(hWnd);
      break;
    }
    break;
  case WM_DESTROY:
    for (size_t i = 0; i < 2; i++)
      SendMessage(panel->tree[i], TVM_DESTROY, 0, 0);
    PostQuitMessage(0);
    break;
  default:
    return FALSE;
  }
  return TRUE;
}

static DWORD WINAPI _panel_thread(LPVOID p) {
  struct pwasio *pwasio = p;

  struct panel panel = {
      .context = &pwasio->context,
      .buffer_size = pwasio->buffer_size,
      .sample_rate = pwasio->sample_rate,
      .priority = pwasio->thread.priority,
      .host_priority = pwasio->host_priority,
      .ports = {pwasio->ports[0], pwasio->ports[1]},
  };

  InitCommonControlsEx(&(INITCOMMONCONTROLSEX){
      .dwSize = sizeof(INITCOMMONCONTROLSEX),
      .dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES,
  });
  if (!(pwasio->dialog =
            CreateDialogParam(pwasio->hinst, (LPCSTR)MAKEINTRESOURCE(IDD_PANEL),
                              nullptr, _panel_func, (LPARAM)&panel)))
    return -1;

  ShowWindow(pwasio->dialog, SW_SHOW);

  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessage(pwasio->dialog, &msg)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  HKEY key = nullptr;
  if (RegCreateKeyEx(HKEY_CURRENT_USER, DRIVER_REG, 0, nullptr, 0,
                     KEY_WRITE | KEY_READ, nullptr, &key, nullptr))
    key = nullptr;

  bool reset = false;
  if (key && panel.buffer_size != pwasio->buffer_size) {
    if (RegSetValueEx(key, KEY_BUFSIZE, 0, REG_DWORD,
                      (BYTE *)&(DWORD){panel.buffer_size}, sizeof(DWORD)))
      WINE_WARN("failed to write buffer size configuration\n");
    reset = true;
  }
  if (key && panel.sample_rate != pwasio->sample_rate) {
    if (RegSetValueEx(key, KEY_SMPRATE, 0, REG_DWORD,
                      (BYTE *)&(DWORD){panel.sample_rate}, sizeof(DWORD)))
      WINE_WARN("failed to write sample rate configuration\n");
    reset = true;
  }
  if (key && panel.priority != pwasio->thread.priority) {
    if (RegSetValueEx(key, KEY_PRIORITY, 0, REG_DWORD,
                      (BYTE *)&(DWORD){panel.priority}, sizeof(DWORD)))
      WINE_WARN("failed to write driver priority configuration\n");
    reset = true;
  }
  if (key && panel.host_priority != pwasio->host_priority) {
    if (RegSetValueEx(key, KEY_HOST_PRIORITY, 0, REG_DWORD,
                      (BYTE *)&(DWORD){panel.host_priority}, sizeof(DWORD)))
      WINE_WARN("failed to write host priority configuration\n");
    reset = true;
  }
  for (size_t i = 0; i < 2; i++) {
    if (panel.ports[i] && panel.ports[i] != pwasio->ports[i]) {
      for (const char *p = panel.ports[i], *q = pwasio->ports[i];
           !reset && (*p || *q); p += strlen(p) + 1, q += strlen(q) + 1)
        if (key && !spa_streq(p, q)) {
          if (RegSetValueEx(
                  key, i == PW_DIRECTION_INPUT ? KEY_INPUTS : KEY_OUTPUTS, 0,
                  REG_MULTI_SZ, (BYTE *)panel.ports[i], panel.len[i]))
            WINE_WARN("unable to write io configuration\n");
          reset = true;
        }
      free(panel.ports[i]);
    }
  }
  if (key)
    RegCloseKey(key);

  if (reset && pwasio->engine.callbacks && pwasio->engine.callbacks->message)
    pwasio->engine.callbacks->message(ASIO_MESSAGE_RESET_REQUEST, 0, nullptr,
                                      nullptr);

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

  struct pwasio *pwasio = *ptr = HeapAlloc(GetProcessHeap(), 0, sizeof *pwasio);
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
  };

  WINE_TRACE("starting PipeWire\n");
  pw_init(nullptr, nullptr);
  WINE_TRACE("compiled with libpipewire-%s\n", pw_get_headers_version());
  WINE_TRACE("linked with libpipewire-%s\n", pw_get_library_version());

  return S_OK;
}
