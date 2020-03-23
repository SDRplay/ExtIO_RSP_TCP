/*
 * ExtIO wrapper for rsp_tcp
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Heavily modified version of ExtIO_RTL_TCP by hayati ayguen <h_ayguen@web.de>
 */

#include <stdint.h>
#include <ActiveSocket.h>

#include <Windows.h>
#include <WindowsX.h>
#include <commctrl.h>
#include <process.h>
#include <tchar.h>
#include <stdio.h>

#include "resource.h"
#include "ExtIO_RSP.h"
#include "rsp_tcp_api.h"
#include "LC_ExtIO_Types.h"

#ifdef _MSC_VER
	#pragma warning(disable : 4996)
	#define snprintf  _snprintf
#endif

static int buffer_sizes[] = 
{ 
	1,2, 4, 8, 16, 32, 64, 128, 256
};

static sr_t samplerates[] = {
	{ 250000.0,	 TEXT("0.250 MSPS"),  250000 },
	{ 1000000.0, TEXT("1.00 MSPS"),  1000000 },
	{ 1500000.0, TEXT("1.50 MSPS"), 1500000 },
	{ 1536000.0, TEXT("1.536 MSPS"), 1536000 },
	{ 1800000.0, TEXT("1.8 MSPS"), 1800000 },
	{ 1920000.0, TEXT("1.92 MSPS"), 1920000 },
	{ 2048000.0, TEXT("2.048 MSPS"), 2048000 },
	{ 2000000.0, TEXT("2.00 MSPS"), 2000000 },
	{ 2400000.0, TEXT("2.4 MSPS"),  2400000 },
	{ 5000000.0, TEXT("5 MSPS"), 5000000 },
	{ 6000000.0, TEXT("6 MSPS"), 6000000 },
	{ 7000000.0, TEXT("7 MSPS"), 7000000 },
	{ 8000000.0, TEXT("8 MSPS"), 8000000 },
	{ 9000000.0, TEXT("9 MSPS"), 9000000 },
	{ 10000000.0, TEXT("10 MSPS"), 10000000 }
};

static const int samplerate_count = sizeof(samplerates) / sizeof(samplerates[0]);
static volatile rsp_extended_capabilities_t rsp_cap;
static volatile bool agc_state = true;
static volatile bool new_agc_state = true;
static volatile bool lna_state = false;
static volatile bool new_lna_state = false;
static volatile bool biast_state = false;
static volatile bool new_biast_state = false;
static volatile uint32_t notch_state = 0;
static volatile uint32_t new_notch_state = 0;
static volatile uint8_t gain_index = 28;
static volatile uint8_t new_gain_index = 28;
static volatile uint8_t antenna_input = 0;
static volatile uint8_t new_antenna_input = 0;
static volatile bool reapply_all_settings = false;
static volatile long last_frequency = 100000000;
static volatile long new_frequency = 100000000;
static volatile int last_samplerate_index = 7;
static volatile int new_samplerate_index = 7;
static volatile int last_ppm_correction = 0;
static volatile int new_ppm_correction = 0;

static volatile int buffer_size_index = 5;
static volatile int buffer_len = buffer_sizes[buffer_size_index];

static float* float_buffer = NULL;
static uint8_t* samples_buffer = NULL;

static char ip_address[32] = "127.0.0.1";
static int port_number = 1234;
static volatile bool auto_reconnect = true;
static volatile bool persist_connection = true;
static bool async_connection = true;
static int socket_delay_ms = 0;

volatile bool worker_stopped = false;
volatile bool streaming_enabled = false;
static volatile HANDLE worker_handle = INVALID_HANDLE_VALUE;

void ThreadProc(void * param);
int StartThread();
int StopThread();

void (* WinradCallBack)(int, int, float, void *) = NULL;

static INT_PTR CALLBACK MainDlgProc(HWND, UINT, WPARAM, LPARAM);
static HWND dialog_handle = NULL;

extern "C"
bool  LIBRSPTCP_API __stdcall InitHW(char *name, char *model, int& type)
{
	strcpy_s(name, 63, "RSP-TCP");
	strcpy_s(model, 15, "RSP-TCP");
	name[63] = 0;
	model[15] = 0;

	type = exthwUSBfloat32;
	return true;
}

extern "C"
int LIBRSPTCP_API __stdcall GetStatus()
{	
    return 0;
}

extern "C"
bool  LIBRSPTCP_API __stdcall OpenHW()
{
	dialog_handle = CreateDialog(hInst, MAKEINTRESOURCE(IDD_RSPTCP_SETTINGS), NULL, (DLGPROC)MainDlgProc);	
	if (dialog_handle)
	{
		ShowWindow(dialog_handle, SW_HIDE);
	}
	
	if (persist_connection)
	{
		streaming_enabled = false;
		if (StartThread() < 0)
		{
			return false;
		}
	}

	return true;
}

extern "C"
long LIBRSPTCP_API __stdcall SetHWLO(long freq)
{
	new_frequency = freq;
	return 0;
}

extern "C"
int LIBRSPTCP_API __stdcall StartHW(long freq)
{
	streaming_enabled = true;
	reapply_all_settings = true;
	if (StartThread() < 0)
	{
		return -1;
	}
    
	SetHWLO(freq);

	if (dialog_handle)
	{
		EnableWindow(GetDlgItem(dialog_handle, IDC_IP_PORT), FALSE);
		EnableWindow(GetDlgItem(dialog_handle, IDC_BUFFER), FALSE);
	}
		
	return buffer_len / 2;
}

extern "C"
long LIBRSPTCP_API __stdcall GetHWLO()
{
	return new_frequency;
}


extern "C"
long LIBRSPTCP_API __stdcall GetHWSR()
{	
	return (long)(samplerates[new_samplerate_index].valueInt);
}

extern "C"
int LIBRSPTCP_API __stdcall ExtIoGetSrates( int srate_idx, double * samplerate )
{
	if (srate_idx < samplerate_count)
	{
		*samplerate = samplerates[srate_idx].value;
		return 0;
	}
	
	return 1;
}

extern "C"
int  LIBRSPTCP_API __stdcall ExtIoGetActualSrateIdx(void)
{
	return new_samplerate_index;
}

extern "C"
int  LIBRSPTCP_API __stdcall ExtIoSetSrate( int samplerate_index )
{
	if (samplerate_index >= 0 && samplerate_index < samplerate_count)
	{
		new_samplerate_index = samplerate_index;		
		if (dialog_handle)
		{
			ComboBox_SetCurSel(GetDlgItem(dialog_handle, IDC_SAMPLERATE), samplerate_index);
		}
		
		WinradCallBack(-1, extHw_Changed_SampleRate, 0, NULL);
		return 0;
	}
	
	return 1;
}

extern "C"
int   LIBRSPTCP_API __stdcall ExtIoGetSetting( int idx, char * description, char * value )
{
	switch (idx)
	{
	case SETTING_IPADDRESS:
		snprintf(description, 1024, "%s", "IP Address");
		snprintf(value, 1024, "%s", ip_address);
		return 0;
	case SETTING_PORTNUMBER:
		snprintf(description, 1024, "%s", "Port Number");
		snprintf(value, 1024, "%d", port_number);
		return 0;
	case SETTING_AUTOCONNECT:
		snprintf(description, 1024, "%s", "Auto Connect");
		snprintf(value, 1024, "%d", auto_reconnect);
		return 0;
	case SETTING_PERSISTCONNECT:
		snprintf(description, 1024, "%s", "Persistent Connection");
		snprintf(value, 1024, "%d", persist_connection);
		return 0;
	case SETTING_SAMPLERATE_INDEX:
		snprintf( description, 1024, "%s", "Sample Rate Index" );
		snprintf(value, 1024, "%d", new_samplerate_index);
		return 0;
	case SETTING_PPM_CORRECTION:
		snprintf( description, 1024, "%s", "PPM Correction" );
		snprintf(value, 1024, "%d", new_ppm_correction);
		return 0;
	case SETTING_BUFFERSIZE:
		snprintf( description, 1024, "%s", "Buffer Size" );
		snprintf(value, 1024, "%d", buffer_size_index);
		return 0;
	case SETTING_ASYNC_CONNECTION:
		snprintf(description, 1024, "%s", "Asynchronous Socket");
		snprintf(value, 1024, "%d", async_connection);
		return 0;
	
	default:
		break;
	}
	
	return -1;
}

extern "C"
void  LIBRSPTCP_API __stdcall ExtIoSetSetting( int idx, const char * value )
{
	int val;

	switch ( idx )
	{
	case SETTING_IPADDRESS:
		snprintf(ip_address, 31, "%s", value);
		break;
	
	case SETTING_PORTNUMBER:
		val = atoi(value);
		if (val >= 0 && val < 65536)
		{
			port_number = val;
		}
		break;
	
	case SETTING_AUTOCONNECT:
		auto_reconnect = atoi(value) ? true : false;
		break;
	
	case SETTING_PERSISTCONNECT:
		persist_connection = atoi(value) ? true : false;
		break;
	
	case SETTING_SAMPLERATE_INDEX:
		val = atoi( value );
		if (val >= 0 && val < samplerate_count)
		{
			new_samplerate_index = val;
		}
		break;
	
	case SETTING_PPM_CORRECTION:
		val = atoi( value );
		if(val >= MIN_PPM && val < MAX_PPM)
		{
			new_ppm_correction = val;
		}
		break;	
	
	case SETTING_BUFFERSIZE:
		val = atoi( value );
		if(val >= 0 && val < (sizeof(buffer_sizes) / sizeof(buffer_sizes[0])))
		{
			buffer_size_index = val;
			buffer_len = buffer_sizes[buffer_size_index] * 1024;
		}
		break;	
	
	case SETTING_ASYNC_CONNECTION:
		async_connection = atoi(value) ? true : false;
		break;	
	default:
		break;
	}
}

extern "C"
void LIBRSPTCP_API __stdcall StopHW()
{	
	streaming_enabled = false;
	if (!persist_connection)
	{
		StopThread();
	}
	
	if (dialog_handle)
	{
		EnableWindow(GetDlgItem(dialog_handle, IDC_IP_PORT), TRUE);
		EnableWindow(GetDlgItem(dialog_handle, IDC_BUFFER), TRUE);
	}
}

extern "C"
void LIBRSPTCP_API __stdcall CloseHW()
{
	streaming_enabled = false;
	StopThread();

	if (dialog_handle)
	{
		DestroyWindow(dialog_handle);
	}
}

extern "C"
void LIBRSPTCP_API __stdcall ShowGUI()
{
	if (dialog_handle)
	{
		ShowWindow(dialog_handle, SW_SHOW);
		SetForegroundWindow(dialog_handle);
	}
}

extern "C"
void LIBRSPTCP_API  __stdcall HideGUI()
{
	if (dialog_handle)
	{
		ShowWindow(dialog_handle, SW_HIDE);
	}
}

extern "C"
void LIBRSPTCP_API  __stdcall SwitchGUI()
{
	if (dialog_handle)
	{
		if (IsWindowVisible(dialog_handle))
		{
			ShowWindow(dialog_handle, SW_HIDE);
		}
		else
		{
			ShowWindow(dialog_handle, SW_SHOW);
		}
	}
}

extern "C"
void LIBRSPTCP_API __stdcall SetCallback(void (* myCallBack)(int, int, float, void *))
{
	WinradCallBack = myCallBack;
}

int StartThread()
{	
	if (worker_handle != INVALID_HANDLE_VALUE)
	{
		return 0;
	}
	
	worker_stopped = false;
	
	float_buffer = (float*)malloc(BUFFER_LENGTH * sizeof(float));
	if (!float_buffer)
	{
		return -1;
	}

	samples_buffer = (uint8_t*)malloc(BUFFER_LENGTH * sizeof(uint16_t));
	if (!samples_buffer)
	{
		free(float_buffer);
		float_buffer = NULL;
		return -1;
	}

	worker_handle = (HANDLE) _beginthread( ThreadProc, 0, NULL );
	if (worker_handle == INVALID_HANDLE_VALUE)
	{
		free(float_buffer);
		free(samples_buffer);
		samples_buffer = NULL;
		float_buffer = NULL;
		return -1;
	}
	
	return 0;
}

int StopThread()
{
	if (worker_handle == INVALID_HANDLE_VALUE)
	{
		return 0;
	}
	
	worker_stopped = true;	
	WaitForSingleObject(worker_handle,INFINITE);	

	if (samples_buffer)
	{
		free(samples_buffer);
		samples_buffer = NULL;
	}

	if (float_buffer)
	{
		free(float_buffer);
		float_buffer = NULL;
	}

	return 0;
}

static bool SendCommand(CActiveSocket &conn, uint8_t cmdId, uint32_t value)
{
	rtl_tcp_cmd.ac[3] = cmdId;
	rtl_tcp_cmd.ui[1] = htonl(value);
	
	int sent = conn.Send(&rtl_tcp_cmd.ac[3], 5);
	return (5 == sent);
}

void ProcessSettings(CActiveSocket& conn)
{
	// Frequency Correction
	if (last_ppm_correction != new_ppm_correction || reapply_all_settings)
	{
		SendCommand(conn, 0x05, new_ppm_correction);		
		last_ppm_correction = new_ppm_correction;
	}

	// Antenna Input
	if (new_antenna_input != antenna_input || reapply_all_settings)
	{
		uint32_t value = new_antenna_input;
		SendCommand(conn, RSP_TCP_COMMAND_SET_ANTENNA, value);
		antenna_input = new_antenna_input;
	}

	// AGC
	if (new_agc_state != agc_state || reapply_all_settings)
	{
		uint32_t value = new_agc_state ? RSP_TCP_AGC_ENABLE : RSP_TCP_AGC_DISABLE;
		SendCommand(conn, RSP_TCP_COMMAND_SET_AGC, value);
		agc_state = new_agc_state;
	}

	// Bias-T
	if (new_biast_state != biast_state || reapply_all_settings)
	{
		uint32_t value = new_biast_state ? RSP_TCP_BIAST_ENABLE : RSP_TCP_BIAST_DISABLE;
		SendCommand(conn, RSP_TCP_COMMAND_SET_BIAST, value);
		biast_state = new_biast_state;
	}

	// Gain Index
	if (new_gain_index != gain_index || reapply_all_settings)
	{
		uint32_t value = new_gain_index;
		SendCommand(conn, 0xd, value);
		gain_index = new_gain_index;
	}

	// Notch filters
	if (new_notch_state != notch_state || reapply_all_settings)
	{
		uint32_t value = new_notch_state;
		SendCommand(conn, RSP_TCP_COMMAND_SET_NOTCH, value);
		notch_state = new_notch_state;
	}

	// Antenna Input
	if (new_antenna_input != antenna_input || reapply_all_settings)
	{
		uint32_t value = new_antenna_input;
		SendCommand(conn, RSP_TCP_COMMAND_SET_ANTENNA, value);
		antenna_input = new_antenna_input;
	}

	// Frequency
	if (last_frequency != new_frequency || reapply_all_settings)
	{
		SendCommand(conn, 0x01, new_frequency);
		last_frequency = new_frequency;
	}

	// Sample rate
	if (last_samplerate_index != new_samplerate_index || reapply_all_settings)
	{
		SendCommand(conn, 0x02, samplerates[new_samplerate_index].valueInt);
		last_samplerate_index = new_samplerate_index;
	}

	reapply_all_settings = false;
}

void ThreadProc(void *p)
{	
	reapply_all_settings = true;

	memset((void*)&rsp_cap, 0, sizeof(rsp_extended_capabilities_t));
	
	while (!worker_stopped)
	{	
		int header_pos = 0;
		bool header_valid = true;
		CActiveSocket conn;

		if (dialog_handle)
		{
			PostMessage(dialog_handle, WM_PRINT, (WPARAM)0, (LPARAM)PRF_CLIENT);
		}

		conn.Initialize();		
		if (!conn.Open(ip_address, (uint16_t)port_number))
		{
			goto reconnect;
		}

		conn.SetBlocking();
		conn.SetReceiveTimeoutMillis(1000);
		while (!worker_stopped)
		{							
			int to_read = (sizeof(rtl_tcp_dongle_info) + sizeof(rsp_extended_capabilities_t)) - header_pos;
			int read = conn.Receive(to_read);
			if (read > 0)
			{
				uint8_t *socket_buffer = conn.GetData();
				if (header_pos < 16)
				{
					memcpy((void*)(&rtl_tcp_dongle_info.ac[header_pos]), socket_buffer, read);
				}
				else
				{
					memcpy((void*)((uint8_t*)(&rsp_cap) + (header_pos - 16)), socket_buffer, read);
				}
				header_pos += read;
				
				if (header_pos >= sizeof(rtl_tcp_dongle_info) + sizeof(rsp_extended_capabilities_t))
				{
					if (rsp_cap.magic[0] != 'R'
						|| rsp_cap.magic[1] != 'S'
						|| rsp_cap.magic[2] != 'P'
						|| rsp_cap.magic[3] != '0')
					{
						header_valid = false;
						break;
					}
					else
					{
						break;
					}					
				}
			}			
			else
			{
				CSimpleSocket::CSocketError err = conn.GetSocketError();
				if (CSimpleSocket::SocketSuccess != err && 
					CSimpleSocket::SocketEwouldblock != err)
				{
					header_valid = false;
					break;
				}
				else 
				if (CSimpleSocket::SocketEwouldblock == err &&
					socket_delay_ms >= 0)
				{
					::Sleep(socket_delay_ms);
				}
			}
		}
		
		if (!header_valid)
		{
			goto reconnect;
		}
	
		rsp_cap.capabilities = ntohl(rsp_cap.capabilities);
		rsp_cap.version = ntohl(rsp_cap.version);
		rsp_cap.hardware_version = ntohl(rsp_cap.hardware_version);
		rsp_cap.sample_format = ntohl(rsp_cap.sample_format);

		// Check capabilities struct version and ensure sample format is one we support
		if (rsp_cap.version != RSP_CAPABILITIES_VERSION ||
			(rsp_cap.sample_format != RSP_TCP_SAMPLE_FORMAT_UINT8 &&
				rsp_cap.sample_format != RSP_TCP_SAMPLE_FORMAT_INT16))
		{
			goto reconnect;
		}

		reapply_all_settings = true;
		
		if (dialog_handle)
		{
			PostMessage(dialog_handle, WM_PRINT, (WPARAM)0, (LPARAM)PRF_CLIENT);
		}

		int read_offset = 0;
		int threshold = rsp_cap.sample_format == RSP_TCP_SAMPLE_FORMAT_INT16 ? buffer_len * 2 : buffer_len;

		while (!worker_stopped)
		{			
			if (streaming_enabled)
			{
				ProcessSettings(conn);
			}
			
			int to_read = threshold - read_offset;
			int length = conn.Receive(to_read, (char*)&samples_buffer[read_offset]);
			if (length <= 0)
			{
				CSimpleSocket::CSocketError err = conn.GetSocketError();
				if (CSimpleSocket::SocketSuccess != err &&
					CSimpleSocket::SocketEwouldblock != err)
				{
					goto reconnect;
				}

				if (CSimpleSocket::SocketEwouldblock == err &&
					socket_delay_ms >= 0)
				{
					::Sleep(socket_delay_ms);
				}
				continue;
			}

			read_offset += length;			
			if (read_offset >= threshold)
			{				
				read_offset = 0;

				if (rsp_cap.sample_format == RSP_TCP_SAMPLE_FORMAT_UINT8)
				{
					const float s = 1.0f / 255.0f;
					unsigned char* char_ptr = (unsigned char*)samples_buffer;
					float* float_ptr = float_buffer;

					for (int i = 0; i < threshold / 2; i++)
					{
						*float_ptr++ = ((float)(*char_ptr++ - 127.5f)) * s;
						*float_ptr++ = ((float)(*char_ptr++ - 127.5f)) * s;
					}
					WinradCallBack(threshold / 2, 0, 0, float_buffer);
				}
				else
				if (rsp_cap.sample_format == RSP_TCP_SAMPLE_FORMAT_INT16)
				{
					const float s = 1.0f / 32768.0f;

					short* short_ptr = (short*)samples_buffer;
					float* float_ptr = float_buffer;

					for (int i = 0; i < threshold / 4; i++)
					{
						*float_ptr++ = (float)(*short_ptr++ * s);
						*float_ptr++ = (float)(*short_ptr++ * s);
					}
					WinradCallBack(threshold / 4, 0, 0, float_buffer);
				}
			}	 
		}

reconnect:
		
		conn.Close();
		
		memset((void*)&rsp_cap, 0, sizeof(rsp_extended_capabilities_t));		
		if (!auto_reconnect)
		{
			break;
		}
	}

	worker_handle = INVALID_HANDLE_VALUE;
	_endthread();
}

static void UpdateGainReduction(HWND hwndDlg)
{
	TCHAR str[255];
	HWND hDlgItmGain = GetDlgItem(hwndDlg, IDC_GAIN);
	
	SendMessage(hDlgItmGain, TBM_SETRANGEMIN, (WPARAM)TRUE, (LPARAM)0);
	SendMessage(hDlgItmGain, TBM_SETRANGEMAX, (WPARAM)TRUE, (LPARAM)GAIN_STEPS);

	EnableWindow(hDlgItmGain, !new_agc_state? TRUE : FALSE);

	if (!new_agc_state)
	{
		_stprintf_s(str, 255, TEXT("%d"), new_gain_index);

		gain_index = -1;
	}
	else
	{
		_stprintf_s(str, 255, TEXT("AGC"));
	}
	Static_SetText(GetDlgItem(hwndDlg, IDC_GRVALUE), str);
}

static void UpdateAgc(HWND hwndDlg)
{
	HWND hDlgItmAgc = GetDlgItem(hwndDlg, IDC_AGC);

	Button_SetCheck(hDlgItmAgc, new_agc_state? BST_CHECKED : BST_UNCHECKED);

	EnableWindow(hDlgItmAgc, true);	
}

static void UpdateNotchFilters(HWND hwndDlg)
{
	HWND hDlgItmAmNotch = GetDlgItem(hwndDlg, IDC_AMNOTCH);
	HWND hDlgItmDabNotch = GetDlgItem(hwndDlg, IDC_DABNOTCH);
	HWND hDlgItmFmNotch = GetDlgItem(hwndDlg, IDC_FMNOTCH);
	HWND hDlgItmRfNotch = GetDlgItem(hwndDlg, IDC_RFNOTCH);
	
	BOOL am_enabled = (rsp_cap.capabilities & RSP_CAPABILITY_AM_NOTCH) ? TRUE : FALSE;
	BOOL dab_enabled = (rsp_cap.capabilities & RSP_CAPABILITY_DAB_NOTCH) ? TRUE : FALSE;
	BOOL fm_enabled = (rsp_cap.capabilities & RSP_CAPABILITY_BROADCAST_NOTCH) ? TRUE : FALSE;
	BOOL rf_enabled = (rsp_cap.capabilities & RSP_CAPABILITY_RF_NOTCH) ? TRUE : FALSE;

	BOOL am_checked = (new_notch_state & RSP_TCP_NOTCH_AM) ? TRUE : FALSE;
	BOOL dab_checked = (new_notch_state & RSP_TCP_NOTCH_DAB) ? TRUE : FALSE;
	BOOL fm_checked = (new_notch_state & RSP_TCP_NOTCH_BROADCAST) ? TRUE : FALSE;
	BOOL rf_checked = (new_notch_state & RSP_TCP_NOTCH_RF) ? TRUE : FALSE;

	Button_SetCheck(hDlgItmAmNotch, am_checked? BST_CHECKED : BST_UNCHECKED);
	Button_SetCheck(hDlgItmDabNotch, dab_checked ? BST_CHECKED : BST_UNCHECKED);
	Button_SetCheck(hDlgItmFmNotch, fm_checked ? BST_CHECKED : BST_UNCHECKED);
	Button_SetCheck(hDlgItmRfNotch, rf_checked ? BST_CHECKED : BST_UNCHECKED);

	EnableWindow(hDlgItmAmNotch, am_enabled);
	EnableWindow(hDlgItmFmNotch, fm_enabled);
	EnableWindow(hDlgItmDabNotch, dab_enabled);
	EnableWindow(hDlgItmRfNotch, rf_enabled);
}

static void UpdateBiast(HWND hwndDlg)
{
	HWND hDlgItmBiast = GetDlgItem(hwndDlg, IDC_BIAST);

	BOOL enable = (rsp_cap.capabilities & RSP_CAPABILITY_BIAS_T) ? TRUE : FALSE;
	
	EnableWindow(hDlgItmBiast, enable);	
	Button_SetCheck(hDlgItmBiast, new_biast_state? BST_CHECKED : BST_UNCHECKED);
}

static void UpdateAntennaInputs(HWND hwndDlg)
{
	TCHAR str[256];
	HWND hDlgItmAntInput = GetDlgItem(hwndDlg, IDC_ANTINPUT);

	ComboBox_ResetContent(hDlgItmAntInput);
	
	_stprintf_s(str, 255, TEXT("Antenna A"));
	ComboBox_AddString(hDlgItmAntInput, str);

	if (rsp_cap.antenna_input_count >= 2)
	{
		_stprintf_s(str, 255, TEXT("Antenna B"));
		ComboBox_AddString(hDlgItmAntInput, str);
	}

	if (rsp_cap.antenna_input_count >= 3)
	{
		ComboBox_AddString(hDlgItmAntInput, rsp_cap.third_antenna_name);
	}

	ComboBox_SetCurSel(hDlgItmAntInput, new_antenna_input);
	if (rsp_cap.antenna_input_count == 1)
	{
		EnableWindow(hDlgItmAntInput, FALSE);
	}
}

static void UpdateVisualState(HWND hwndDlg)
{
	UpdateGainReduction(hwndDlg);
	UpdateAntennaInputs(hwndDlg);
	UpdateAgc(hwndDlg);
	UpdateBiast(hwndDlg);
	UpdateNotchFilters(hwndDlg);
}

static INT_PTR CALLBACK MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
   	switch (uMsg)
    {
        case WM_INITDIALOG:
		{
			Button_SetCheck(GetDlgItem(hwndDlg, IDC_AUTORECONNECT), auto_reconnect ? BST_CHECKED : BST_UNCHECKED);
			Button_SetCheck(GetDlgItem(hwndDlg, IDC_PERSISTCONNECTION), persist_connection ? BST_CHECKED : BST_UNCHECKED);
			SendMessage(GetDlgItem(hwndDlg, IDC_PPM_S), UDM_SETRANGE, (WPARAM)TRUE, (LPARAM)MAKELONG(MAX_PPM, MIN_PPM));

			// Set Max Gain to start with
			SendMessage(GetDlgItem(hwndDlg, IDC_GAIN), TBM_SETRANGEMIN, (WPARAM)TRUE, (LPARAM)0);
			SendMessage(GetDlgItem(hwndDlg, IDC_GAIN), TBM_SETRANGEMAX, (WPARAM)TRUE, (LPARAM)GAIN_STEPS);
			SendMessage(GetDlgItem(hwndDlg, IDC_GAIN), TBM_SETPOS, (WPARAM)TRUE, (LPARAM)0);

			TCHAR tempStr[255];
			_stprintf_s(tempStr, 255, TEXT("%d"), new_ppm_correction);
			Edit_SetText(GetDlgItem(hwndDlg,IDC_PPM), tempStr);
			
			{
				TCHAR tempStr[255];
				_stprintf_s(tempStr, 255, TEXT("%s:%d"), ip_address, port_number);
				Edit_SetText(GetDlgItem(hwndDlg, IDC_IP_PORT), tempStr);
			}

			for (int i = 0; i < samplerate_count; i++) 
			{
				ComboBox_AddString(GetDlgItem(hwndDlg, IDC_SAMPLERATE), samplerates[i].name);
			}			
			ComboBox_SetCurSel(GetDlgItem(hwndDlg, IDC_SAMPLERATE), new_samplerate_index);		
			
			for (int i = 0; i < (sizeof(buffer_sizes) / sizeof(buffer_sizes[0])); i++)
			{
				TCHAR str[255];
				_stprintf_s(str, 255, TEXT("%d kB"), buffer_sizes[i]);
				ComboBox_AddString(GetDlgItem(hwndDlg, IDC_BUFFER), str);
			}
			ComboBox_SetCurSel(GetDlgItem(hwndDlg, IDC_BUFFER), buffer_size_index);			
			buffer_len = buffer_sizes[buffer_size_index] * 1024;
					
			UpdateVisualState(hwndDlg);

			return TRUE;
		}

		case WM_PRINT:
		{
			if (lParam == (LPARAM)PRF_CLIENT)
			{
				UpdateVisualState(hwndDlg);
			}
			return TRUE;
		}

        case WM_COMMAND:            
			switch (GET_WM_COMMAND_ID(wParam, lParam))
            {
				case IDC_PPM:
				{
					if (GET_WM_COMMAND_CMD(wParam, lParam) == EN_CHANGE)
					{
						TCHAR ppm[255];
						Edit_GetText((HWND)lParam, ppm, 255);
						new_ppm_correction = _ttoi(ppm);
						WinradCallBack(-1, extHw_Changed_LO, 0, NULL);
					}
					return TRUE;
				}
				
				case IDC_AUTORECONNECT:
				{
					auto_reconnect = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? 1 : 0;
					return TRUE;
				}

				case IDC_ANTINPUT:
				{
					if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
					{
						uint8_t input = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));

						if (input == 2 && new_frequency > (long)rsp_cap.third_antenna_freq_limit)
						{
							char mess[255];
							sprintf(mess, "%s input can only be selected below %d MHz", rsp_cap.third_antenna_name, (rsp_cap.third_antenna_freq_limit / 1000000));
							MessageBox(hwndDlg, mess, "RSP-TCP", MB_OK);
							UpdateAntennaInputs(hwndDlg);
						}
						else
						{
							new_antenna_input = input;
						}
						WinradCallBack(-1, extHw_Changed_SampleRate, 0, NULL);
					}
					
					return TRUE;
				}

				case IDC_AGC:
				{
					bool checked = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? true : false;

					new_agc_state = checked;
					UpdateVisualState(hwndDlg);
					
					return TRUE;
				}

				case IDC_LNA:
				{
					bool checked = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? true : false;

					new_lna_state = checked;
					UpdateVisualState(hwndDlg);
					
					return TRUE;
				}

				case IDC_BIAST:
				{
					bool checked = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? true : false;

					new_biast_state = checked;
					UpdateVisualState(hwndDlg);
					
					return TRUE;
				}
				
				case IDC_AMNOTCH:
				{
					bool checked = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? true : false;
					
					if (checked)
					{
						new_notch_state |= RSP_TCP_NOTCH_AM;
					}
					else
					{
						new_notch_state &= ~RSP_TCP_NOTCH_AM;
					}

					return TRUE;
				}

				case IDC_FMNOTCH:
				{
					bool checked = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? true : false;
					
					if (checked)
					{
						new_notch_state |= RSP_TCP_NOTCH_BROADCAST;
					}
					else
					{
						new_notch_state &= ~RSP_TCP_NOTCH_BROADCAST;
					}
					
					return TRUE;
				}

				case IDC_DABNOTCH:
				{
					bool checked = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? true : false;
					
					if (checked)
					{
						new_notch_state |= RSP_TCP_NOTCH_DAB;
					}
					else
					{
						new_notch_state &= ~RSP_TCP_NOTCH_DAB;
					}
					
					return TRUE;
				}

				case IDC_RFNOTCH:
				{
					bool checked = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? true : false;
					
					if (checked)
					{
						new_notch_state |= RSP_TCP_NOTCH_RF;
					}
					else
					{
						new_notch_state &= ~RSP_TCP_NOTCH_RF;
					}
					
					return TRUE;
				}

				case IDC_PERSISTCONNECTION:
				{
					persist_connection = (Button_GetCheck(GET_WM_COMMAND_HWND(wParam, lParam)) == BST_CHECKED) ? 1 : 0;
					if (!persist_connection && !streaming_enabled)
					{
						StopThread();
					}
					return TRUE;
				}

				case IDC_SAMPLERATE:
				{
					if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
					{
						new_samplerate_index = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));
						WinradCallBack(-1, extHw_Changed_SampleRate, 0, NULL);
					}
					if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_EDITUPDATE)
					{
						TCHAR  ListItem[256];
						ComboBox_GetText((HWND)lParam, ListItem, 256);
						TCHAR *endptr;
						double coeff = _tcstod(ListItem, &endptr);

						while (_istspace(*endptr)) ++endptr;

						int exp = 1;
						switch (_totupper(*endptr)) {
						case 'K': exp = 1000; break;
						case 'M': exp = 1000 * 1000; break;
						}

						uint32_t newrate = uint32_t(coeff * exp);
						if (newrate >= MINRATE && newrate <= MAXRATE) {
							WinradCallBack(-1, extHw_Changed_SampleRate, 0, NULL);
						}

					}
					return TRUE;
				}
				
				case IDC_BUFFER:
				{
					if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
					{
						buffer_size_index = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));
						buffer_len = buffer_sizes[buffer_size_index] * 1024;
						WinradCallBack(-1, extHw_Changed_SampleRate, 0, NULL);
					}
					return TRUE;
				}

				case IDC_IP_PORT:
				{
					if (GET_WM_COMMAND_CMD(wParam, lParam) == EN_CHANGE)
					{
						TCHAR tempStr[255];
						Edit_GetText((HWND)lParam, tempStr, 255);
						char * IP = strtok(tempStr, ":");
						if (IP)
							snprintf(ip_address, 31, "%s", IP);
						char * PortStr = strtok(NULL, ":");
						if (PortStr)
						{
							int PortNo = atoi(PortStr);
							if (PortNo > 0 && PortNo < 65536)
								port_number = PortNo;
						}
					}
					return TRUE;
				}
			}
            break;

		case WM_VSCROLL:
			{
				HWND hGain = GetDlgItem(hwndDlg, IDC_GAIN);
				if ((HWND)lParam == hGain)
				{
					int value = SendMessage(hGain, TBM_GETPOS, (WPARAM)0, (LPARAM)0);
					
					SendMessage(hGain, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)value);
										
					new_gain_index = GAIN_STEPS - value;
					UpdateVisualState(hwndDlg);
					return TRUE;
				}
				if ((HWND)lParam == GetDlgItem(hwndDlg, IDC_PPM_S))
				{
					return TRUE;
				}
			}
			break;

        case WM_CLOSE:
		{
			ShowWindow(hwndDlg, SW_HIDE);
			return TRUE;		
		}

		case WM_DESTROY:
		{
			dialog_handle = NULL;
			return TRUE;
		}		
	}

	return FALSE;
}