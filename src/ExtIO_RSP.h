#pragma once

#include <tchar.h>

#ifdef LIBRSPTCP_EXPORTS
#define LIBRSPTCP_API __declspec(dllexport)
#else
#define LIBRSPTCP_API __declspec(dllimport)
#endif

extern HMODULE hInst;

#define BUFFER_LENGTH	(256*1024)
#define MAX_PPM	(1000)
#define MIN_PPM	(-1000)
#define MAXRATE	(10000000)
#define MINRATE	30000
#define GAIN_STEPS (28)

typedef struct sample_rate_t
{
	double value;
	TCHAR *name;
	int    valueInt;
} sr_t;

typedef enum
{
	SETTING_IPADDRESS = 0,
	SETTING_PORTNUMBER = 1,
	SETTING_AUTOCONNECT = 2,
	SETTING_PERSISTCONNECT = 3,
	SETTING_SAMPLERATE_INDEX = 4,
	SETTING_BUFFERSIZE = 5,
	SETTING_PPM_CORRECTION = 6,
	SETTING_ASYNC_CONNECTION = 7
} settings_t;

static union
{
	uint8_t ac[8];
	uint32_t ui[2];
} rtl_tcp_cmd;

static volatile union
{
	int8_t		ac[12];
	uint32_t	ui[3];
} rtl_tcp_dongle_info;
