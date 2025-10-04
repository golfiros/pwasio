#ifndef __PWASIO_ASIO_H__
#define __PWASIO_ASIO_H__

#include <unknwn.h>

struct asio_samples {
  ULONG32 hi;
  ULONG32 lo;
};
struct asio_timestamp {
  ULONG32 hi;
  ULONG32 lo;
};

enum {
  ASIO_ERROR_OK = 0l,
  ASIO_ERROR_SUCCESS = 0x3f4847a0l,
  ASIO_ERROR_NOT_PRESENT = -1000l,
  ASIO_ERROR_HW_MALFUNCTION,
  ASIO_ERROR_INVALID_PARAMETER,
  ASIO_ERROR_INVALID_MODE,
  ASIO_ERROR_SP_NOT_ADVANCING,
  ASIO_ERROR_NO_CLOCK,
  ASIO_ERROR_NO_MEMORY,
};

struct asio_time_code {
  DOUBLE speed;
  LONG64 time_code;
  LONG32 flags;
  CHAR future[64];
};

enum asio_time_info_flags {
  ASIO_TIME_INFO_SYSTEM_TIME_VALID = 0X1,
  ASIO_TIME_INFO_SAMPLE_POSITION_VALID = 0X2,
  ASIO_TIME_INFO_SAMPLE_RATE_VALID = 0X4,
  ASIO_TIME_INFO_SPEED_VALID = 0X8,
  ASIO_TIME_INFO_SAMPLE_RATE_CHANGED = 0X10,
  ASIO_TIME_INFO_CLOCK_SOURCE_CHANGED = 0X20,
};

struct asio_time_info {
  DOUBLE speed;
  LONG64 sys_time;
  LONG64 sample_pos;
  DOUBLE sample_rate;
  LONG32 flags;
  CHAR _[12];
};

struct asio_time {
  LONG32 _[4];
  struct asio_time_info info;
  struct asio_time_code code;
};

struct asio_clock_source {
  LONG32 index;
  LONG32 channel;
  LONG32 group;
  LONG32 current;
  CHAR name[32];
};

enum asio_message {
  ASIO_MESSAGE_SUPPORTED = 1L,
  ASIO_MESSAGE_ENGINE_VERSION,
  ASIO_MESSAGE_RESET_REQUEST,
  ASIO_MESSAGE_BUFFER_SIZE_CHANGE,
  ASIO_MESSAGE_RESYNC_REQUEST,
  ASIO_MESSAGE_LATENCIES_CHANGED,
  ASIO_MESSAGE_SUPPORTS_TIME_INFO,
  ASIO_MESSAGE_SUPPORTS_TIME_CODE,
  ASIO_MESSAGE_MMC_COMMAND,
  ASIO_MESSAGE_SUPPORTS_INPUT_MONITOR,
  ASIO_MESSAGE_SUPPORTS_INPUT_GAIN,
  ASIO_MESSAGE_SUPPORTS_INPUT_METER,
  ASIO_MESSAGE_SUPPORTS_OUTPUT_GAIN,
  ASIO_MESSAGE_SUPPORTS_OUTPUT_METER,
  ASIO_MESSAGE_OVERLOAD,
  ASIO_MESSAGE_COUNT,
};

struct asio_callbacks {
  VOID(CALLBACK *swap_buffers)(LONG32 idx, LONG32 direct);
  VOID(CALLBACK *sample_rate_change)(DOUBLE rate);
  LONG32(CALLBACK *message)(LONG32 sel, LONG32 val, PVOID msg, DOUBLE *opt);
  struct asio_time *(CALLBACK *swap_buffers_time_info)(struct asio_time *time,
                                                       LONG32 idx,
                                                       LONG32 direct);
};

struct asio_buffer_info {
  const LONG32 input;
  const LONG32 channel;
  PVOID buf[2];
};

enum asio_sample_type {
  ASIO_SAMPLE_TYPE_INT16_MSB = 0l,
  ASIO_SAMPLE_TYPE_INT24_MSB = 1,
  ASIO_SAMPLE_TYPE_INT32_MSB = 2,
  ASIO_SAMPLE_TYPE_FLOAT32_MSB = 3,
  ASIO_SAMPLE_TYPE_FLOAT64_MSB = 4,

  ASIO_SAMPLE_TYPE_INT32_MSB16 = 8,
  ASIO_SAMPLE_TYPE_INT32_MSB18 = 9,
  ASIO_SAMPLE_TYPE_INT32_MSB20 = 10,
  ASIO_SAMPLE_TYPE_INT32_MSB24 = 11,

  ASIO_SAMPLE_TYPE_INT16_LSB = 16,
  ASIO_SAMPLE_TYPE_INT24_LSB = 17,
  ASIO_SAMPLE_TYPE_INT32_LSB = 18,
  ASIO_SAMPLE_TYPE_FLOAT32_LSB = 19,
  ASIO_SAMPLE_TYPE_FLOAT64_LSB = 20,

  ASIO_SAMPLE_TYPE_INT32_LSB16 = 24,
  ASIO_SAMPLE_TYPE_INT32_LSB18 = 25,
  ASIO_SAMPLE_TYPE_INT32_LSB20 = 26,
  ASIO_SAMPLE_TYPE_INT32_LSB24 = 27,

  ASIO_SAMPLE_TYPE_DSD_INT8_LSB1 = 32,
  ASIO_SAMPLE_TYPE_DSD_INT8_MSB1 = 33,
  ASIO_SAMPLE_TYPE_DSD_INT8_NER8 = 40,

  ASIO_SAMPLE_TYPE_COUNT,
};

struct asio_channel_info {
  LONG32 index;
  LONG32 input;
  LONG32 active;
  LONG32 group;
  LONG32 type;
  CHAR name[32];
};

#define INTERFACE asio
DECLARE_INTERFACE_(INTERFACE, ) {
  STDMETHOD(QueryInterface)(THIS, REFIID, PVOID *);
  STDMETHOD_(ULONG32, AddRef)(THIS);
  STDMETHOD_(ULONG32, Release)(THIS);

  STDMETHOD_(LONG32, Init)(THIS, PVOID);
  STDMETHOD_(VOID, GetDriverName)(THIS, PSTR);
  STDMETHOD_(LONG32, GetDriverVersion)(THIS);
  STDMETHOD_(VOID, GetErrorMessage)(THIS, char *);
  STDMETHOD_(LONG32, Start)(THIS);
  STDMETHOD_(LONG32, Stop)(THIS);
  STDMETHOD_(LONG32, GetChannels)(THIS, LONG32 *, LONG32 *);
  STDMETHOD_(LONG32, GetLatencies)(THIS, LONG32 *, LONG32 *);
  STDMETHOD_(LONG32, GetBufferSize)(THIS, LONG32 *, LONG32 *, LONG32 *,
                                    LONG32 *);
  STDMETHOD_(LONG32, CanSampleRate)(THIS, DOUBLE);
  STDMETHOD_(LONG32, GetSampleRate)(THIS, DOUBLE *);
  STDMETHOD_(LONG32, SetSampleRate)(THIS, DOUBLE);
  STDMETHOD_(LONG32, GetClockSources)(THIS, struct asio_clock_source *,
                                      LONG32 *);
  STDMETHOD_(LONG32, SetClockSource)(THIS, LONG32);
  STDMETHOD_(LONG32, GetSamplePosition)(THIS, struct asio_samples *,
                                        struct asio_timestamp *);
  STDMETHOD_(LONG32, GetChannelInfo)(THIS, struct asio_channel_info *);
  STDMETHOD_(LONG32, CreateBuffers)(THIS, struct asio_buffer_info *, LONG32,
                                    LONG32, struct asio_callbacks *);
  STDMETHOD_(LONG32, DisposeBuffers)(THIS);
  STDMETHOD_(LONG32, ControlPanel)(THIS);
  STDMETHOD_(LONG32, Future)(THIS, LONG32, PVOID);
  STDMETHOD_(LONG32, OutputReady)(THIS);
};
#undef INTERFACE

#endif // !__PWASIO_ASIO_H__
