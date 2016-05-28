//////////////////////////////////////////////////////////////////////////////////
// MParallel - Parallel Batch Processor
// Copyright (c) 2016 LoRd_MuldeR <mulder2@gmx.de>. Some rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
// http://www.gnu.org/licenses/gpl-2.0.txt
//////////////////////////////////////////////////////////////////////////////////


//Const
const unsigned int MPARALLEL_VERSION_MAJOR = 1;
const unsigned int MPARALLEL_VERSION_MINOR = 0;

//CRT
#include <string>
#include <sstream>
#include <cstring>
#include <queue>
#include <algorithm>
#include <io.h>
#include <fcntl.h>

//Win32
#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1
#include <Windows.h>
#include <Shellapi.h>

//Utils
#define PRINT(...) fwprintf(stderr, __VA_ARGS__);
#define BOUND(MIN,VAL,MAX) std::min(std::max((MIN), (VAL)), (MAX));
#define MATCH(X,Y) (_wcsicmp((X), (Y)) == 0)

//Options
static DWORD        g_option_max_instances;
static DWORD        g_option_process_timeout;
static bool         g_option_read_stdin_lines;
static bool         g_option_auto_quote_vars;
static bool         g_option_force_use_shell;
static bool         g_option_abort_on_failure;
static std::wstring g_option_separator;
static std::wstring g_option_command_pattern;
static std::wstring g_option_input_file_name;

//Types
typedef std::queue<std::wstring> queue_t;

//Globals
static queue_t g_queue;
static bool    g_logo_printed;
static HANDLE  g_processes[MAXIMUM_WAIT_OBJECTS];
static bool    g_isrunning[MAXIMUM_WAIT_OBJECTS];
static DWORD   g_process_count;

// ==========================================================================
// TEXT OUTPUT
// ==========================================================================

#define my_print(...) do \
{ \
	if(!g_logo_printed) \
	{ \
		print_logo(); \
		g_logo_printed = true; \
	} \
	PRINT(__VA_ARGS__); \
} \
while(0)

static void print_logo(void)
{
	PRINT(L"===============================================================================\n");
	PRINT(L"MParallel - Parallel Batch Processor, Version %u.%02u [%S]\n", MPARALLEL_VERSION_MAJOR, MPARALLEL_VERSION_MINOR, __DATE__);
	PRINT(L"Copyright (c) 2016 LoRd_MuldeR <mulder2@gmx.de>. Some rights reserved.\n\n");
	PRINT(L"This program is free software: you can redistribute it and/or modify\n");
	PRINT(L"it under the terms of the GNU General Public License <http://www.gnu.org/>.\n");
	PRINT(L"Note that this program is distributed with ABSOLUTELY NO WARRANTY.\n");
	PRINT(L"=============================================================================== \n\n");
}

// ==========================================================================
// ERROR HANDLING
// ==========================================================================

static void fatal_exit(const wchar_t *const error_message)
{
	const HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
	if (hStdErr != INVALID_HANDLE_VALUE)
	{
		DWORD written;
		WriteFile(hStdErr, error_message, lstrlenW(error_message), &written, NULL);
		FlushFileBuffers(hStdErr);
	}
	TerminateProcess(GetCurrentProcess(), 666);
}

static void my_invalid_parameter_handler(wchar_t const*, wchar_t const*, wchar_t const*, unsigned int, uintptr_t)
{
	fatal_exit(L"\n\nFATAL: Invalid parameter handler invoked!\n\n");
}

// ==========================================================================
// SYSTEM INFO
// ==========================================================================

static DWORD my_popcount(DWORD number)
{
	number = number - ((number >> 1) & 0x55555555);
	number = (number & 0x33333333) + ((number >> 2) & 0x33333333);
	return (((number + (number >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

static DWORD processor_count(void)
{
	DWORD procMask, sysMask;
	if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask))
	{
		const DWORD count = my_popcount(procMask);
		return BOUND(DWORD(1), count, DWORD(MAXIMUM_WAIT_OBJECTS));
	}
	return 1;
}

// ==========================================================================
// STRING SUPPORT ROUTINES
// ==========================================================================

//Parse unsigned integer
static bool parse_uint32(const wchar_t *str, DWORD &value)
{
	if (swscanf_s(str, L"%lu", &value) != 1)
	{
		my_print(L"ERROR: Argument \"%s\" doesn't look like a valid integer!\n\n", str);
		return false;
	}
	return true;
}

//Replace sub-strings
static bool replace_str(std::wstring& str, const std::wstring& needle, const std::wstring& replacement)
{
	bool okay = false;
	for (;;)
	{
		const size_t start_pos = str.find(needle);
		if (start_pos == std::string::npos)
		{
			break;
		}
		str.replace(start_pos, needle.length(), replacement);
		okay = true;
	}
	return okay;
}

//Check for space chars
static bool contains_whitespace(const wchar_t *str)
{
	while (*str)
	{
		if (iswspace(*(str++)))
		{
			return true;
		}
	}
	return false;
}

//Trim trailing EOL chars
static wchar_t *trim_str(wchar_t *str)
{
	while ((*str) && iswspace(*str) || iswcntrl(*str) || iswblank(*str))
	{
		str++;
	}
	size_t pos = wcslen(str);
	while (pos > 0)
	{
		--pos;
		if (iswspace(str[pos]) || iswcntrl(str[pos]) || iswblank(str[pos]))
		{
			str[pos] = L'\0';
			continue;
		}
		break;
	}
	return str;
}

// ==========================================================================
// COMMAND-LINE HANDLING
// ==========================================================================

#define REQUIRE_VALUE() do \
{ \
	if ((!value) || (!value[0])) \
	{ \
		my_print(L"ERROR: Argumet for option \"--%s\" is missing!\n\n", option); \
		return false; \
	} \
} \
while(0)

#define REQUIRE_NO_VALUE() do \
{ \
	if (value && value[0]) \
	{ \
		my_print(L"ERROR: Excess argumet for option \"--%s\" encountred!\n\n", option); \
		return false; \
	} \
} \
while(0)

//Parse commands (simple)
static void parse_commands_simple(const int argc, const wchar_t *const argv[], const int offset, const wchar_t *const separator)
{
	int i = offset;
	std::wstringstream command_buffer;
	while (i < argc)
	{
		const wchar_t *const current = argv[i++];
		if ((!separator) || wcscmp(current, separator))
		{
			if (command_buffer.tellp())
			{
				command_buffer << L' ';
			}
			if ((!current[0]) || contains_whitespace(current))
			{
				command_buffer << L'"' << current << L'"';
			}
			else
			{
				command_buffer << current;
			}
		}
		else
		{
			if (command_buffer.tellp())
			{
				g_queue.push(std::move(command_buffer.str()));
			}
			command_buffer.str(std::wstring());
			command_buffer.clear();
		}
	}
	if (command_buffer.tellp())
	{
		g_queue.push(command_buffer.str());
	}
}

//Parse commands with pattern
static void parse_commands_pattern(const std::wstring &pattern, int argc, const wchar_t *const argv[], const int offset, const wchar_t *const separator)
{
	int i = offset, k = 0;
	std::wstring command_buffer = pattern;
	while (i < argc)
	{
		const wchar_t *const current = argv[i++];
		if ((!separator) || wcscmp(current, separator))
		{
			
			std::wstringstream placeholder;
			placeholder << L"{{" << (k++) << L"}}";
			if (g_option_auto_quote_vars && contains_whitespace(current))
			{
				std::wstringstream replacement;
				replacement << L'"' << current << L'"';
				replace_str(command_buffer, placeholder.str(), replacement.str());
			}
			else
			{
				replace_str(command_buffer, placeholder.str(), current);
			}
		}
		else
		{
			if (!command_buffer.empty())
			{
				g_queue.push(command_buffer);
				k = 0;
				command_buffer = pattern;
			}
		}
	}
	if ((!command_buffer.empty()) && (k > 0))
	{
		g_queue.push(command_buffer);
	}
}

//Parse commands
static void parse_commands(int argc, const wchar_t *const argv[], const int offset, const wchar_t *const separator)
{
	if (!g_option_command_pattern.empty())
	{
		parse_commands_pattern(g_option_command_pattern, argc, argv, offset, separator);
	}
	else
	{
		parse_commands_simple(argc, argv, offset, separator);
	}
}

//Parse option
static bool parse_option_string(const wchar_t *const option, const wchar_t *const value)
{
	DWORD temp;

	if (MATCH(option, L"pattern"))
	{
		REQUIRE_VALUE();
		g_option_command_pattern = value;
		return true;
	}
	else if (MATCH(option, L"count"))
	{
		REQUIRE_VALUE();
		if (parse_uint32(value, temp))
		{
			g_option_max_instances = BOUND(DWORD(1), temp, DWORD(MAXIMUM_WAIT_OBJECTS));
			return true;
		}
		return false;
	}
	else if (MATCH(option, L"separator"))
	{
		REQUIRE_VALUE();
		g_option_separator = value;
		return true;
	}
	else if (MATCH(option, L"stdin"))
	{
		REQUIRE_NO_VALUE();
		g_option_read_stdin_lines = true;
		return true;
	}
	else if (MATCH(option, L"input"))
	{
		REQUIRE_VALUE();
		g_option_input_file_name = value;
		return true;
	}
	else if (MATCH(option, L"auto-quote"))
	{
		REQUIRE_NO_VALUE();
		g_option_auto_quote_vars = true;
		return true;
	}
	else if (MATCH(option, L"shell"))
	{
		REQUIRE_NO_VALUE();
		g_option_force_use_shell = true;
		return true;
	}
	else if (MATCH(option, L"timeout"))
	{
		REQUIRE_VALUE();
		if (parse_uint32(value, temp))
		{
			g_option_process_timeout = temp;
			return true;
		}
		return false;
	}
	else if (MATCH(option, L"abort"))
	{
		REQUIRE_NO_VALUE();
		g_option_abort_on_failure = true;
		return true;
	}

	my_print(L"ERROR: Unknown option \"--%s\" encountred!\n\n", option);
	return false;
}

//Parse option
static bool parse_option_string(const wchar_t *const option_str)
{
	wchar_t opt_buffer[32];
	const wchar_t *const pos = wcschr(option_str, L'=');
	if (pos && (pos != option_str))
	{
		wcsncpy_s(opt_buffer, 32, option_str, (pos - option_str));
		return parse_option_string(opt_buffer, (*(pos + 1)) ? (pos + 1) : NULL);
	}
	else
	{
		return parse_option_string(option_str, NULL);
	}
}

//Parse arguments
static bool parse_arguments(const int argc, const wchar_t *const argv[])
{
	int i = 1;
	while(i < argc)
	{
		const wchar_t *const current = argv[i++];
		if ((current[0] == L'-') && (current[1] == L'-'))
		{
			if (current[2])
			{
				if (!parse_option_string(&current[2]))
				{
					return false;
				}
			}
			else
			{
				parse_commands(argc, argv, i, g_option_separator.c_str());
				break;
			}
		}
		else
		{
			parse_commands(argc, argv, --i, g_option_separator.c_str());
			break;
		}
	}
	return true;
}

//Read from file stream
static void parse_commands_file(FILE *const input)
{
	wchar_t line_buffer[32768];
	while (fgetws(line_buffer, 32768, input))
	{
		int argc;
		const wchar_t *const trimmed = trim_str(line_buffer);
		if (trimmed && trimmed[0])
		{
			wchar_t *const *const argv = CommandLineToArgvW(trimmed, &argc);
			if (!argv)
			{
				fatal_exit(L"Exit: CommandLineToArgvW() has failed!\n\n");
			}
			parse_commands(argc, argv, 0, NULL);
			LocalFree((HLOCAL)argv);
		}
	}
}

//Read from file
static bool parse_commands_file(const wchar_t *const file_name)
{
	FILE *file = NULL;
	if (_wfopen_s(&file, file_name, L"r") == 0)
	{
		_setmode(_fileno(file), _O_U8TEXT);
		parse_commands_file(file);
		fclose(file);
		return true;
	}
	my_print(L"ERROR: Unbale to open file \"%s\" for reading!\n\n", file_name);
	return false;
}

// ==========================================================================
// PROCESS FUNCTIONS
// ==========================================================================

//Print Win32 error message
static void print_win32_error(const wchar_t *const format, const DWORD error)
{
	wchar_t buffer[1024];
	if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer, 1024, NULL) > 0)
	{
		my_print(format, trim_str(buffer));
	}
}

//Terminate all running processes
static void terminate_processes(void)
{
	for (DWORD i = 0; i < g_option_max_instances; i++)
	{
		if (g_isrunning[i])
		{
			g_process_count--;
			TerminateProcess(g_processes[i], 666);
			CloseHandle(g_processes[i]);
			g_isrunning[i] = false;
			g_processes[i] = NULL;
		}
	}
}

//Start the next process
static HANDLE start_next_process(void)
{
	std::wstringstream command;
	if (g_option_force_use_shell)
	{
		command << L"cmd.exe /c ";
	}
	command << g_queue.front();
	g_queue.pop();

	STARTUPINFOW startup_info;
	memset(&startup_info, 0, sizeof(STARTUPINFOW));

	PROCESS_INFORMATION process_info;
	memset(&process_info, 0, sizeof(PROCESS_INFORMATION));

	if (CreateProcessW(NULL, (LPWSTR)command.str().c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &startup_info, &process_info))
	{
		CloseHandle(process_info.hThread);
		return process_info.hProcess;
	}

	const DWORD error = GetLastError();
	print_win32_error(L"\nProcess creation has failed: %s\n", error);
	my_print(L"ERROR: Process ``%s��could not be created!\n\n", command.str().c_str());
	return NULL;
}

//Wait for *any* running process to terminate
static DWORD wait_for_process(bool &timeout)
{
	DWORD index[MAXIMUM_WAIT_OBJECTS];
	HANDLE handles[MAXIMUM_WAIT_OBJECTS];
	DWORD count = 0;
	for (DWORD i = 0; i < g_option_max_instances; i++)
	{
		if (g_isrunning[i])
		{
			index[count] = i;
			handles[count++] = g_processes[i];
		}
	}
	if (count > 0)
	{
		const DWORD ret = WaitForMultipleObjects(count, &handles[0], FALSE, (g_option_process_timeout > 0) ? g_option_process_timeout : INFINITE);
		if ((ret >= WAIT_OBJECT_0) && (ret < WAIT_OBJECT_0 + count))
		{
			return index[ret - WAIT_OBJECT_0];
		}
		if ((ret == WAIT_TIMEOUT) && (g_option_process_timeout > 0))
		{
			timeout = true;
		}
	}
	return MAXDWORD;
}

//Run processes
static int run_processes(void)
{
	DWORD slot = 0, exit_code = 0;
	bool aborted = false;

	//MAIN PROCESSING LOOP
	while (!((g_queue.empty() && (g_process_count < 1)) || aborted))
	{
		//Launch the next process(es)
		while ((!g_queue.empty()) && (g_process_count < g_option_max_instances))
		{
			if (const HANDLE process = start_next_process())
			{
				g_process_count++;
				while (g_isrunning[slot])
				{
					slot = (slot + 1) % g_option_max_instances;
				}
				g_processes[slot] = process;
				g_isrunning[slot] = true;
			}
			else
			{
				if (g_option_abort_on_failure)
				{
					exit_code = std::max(exit_code, DWORD(1));
					aborted = true;
					my_print(L"\nERROR: Process creation failed, aborting!\n\n");
					break;
				}
			}
		}

		//Wait for one process to terminate
		if ((!aborted) && (g_process_count > 0) && ((g_process_count >= g_option_max_instances) || g_queue.empty()))
		{
			bool timeout = false;
			const DWORD index = wait_for_process(timeout);
			if (index != MAXDWORD)
			{
				g_process_count--;
				DWORD temp;
				if (GetExitCodeProcess(g_processes[index], &temp))
				{
					exit_code = std::max(exit_code, temp);
					if ((exit_code > 0) && g_option_abort_on_failure)
					{
						aborted = true;
						my_print(L"\nERROR: Command failed, aborting! (ExitCode: %u)\n\n", exit_code);
						break;
					}
				}
				CloseHandle(g_processes[index]);
				g_processes[index] = NULL;
				g_isrunning[index] = false;
			}
			else
			{
				if (timeout)
				{
					my_print(L"\nERROR: Timeout encountered, terminating process!\n\n");
					terminate_processes();
				}
				else
				{
					exit_code = std::max(exit_code, DWORD(1));
					aborted = true;
					my_print(L"\nERROR: Failed to wait for running process!\n\n");
					break;
				}
			}
		}
	}

	//Wait for the pending processes
	while (g_process_count > 0)
	{
		bool timeout = false;
		const DWORD index = wait_for_process(timeout);
		if (index != MAXDWORD)
		{
			g_process_count--;
			CloseHandle(g_processes[index]);
			g_processes[index] = NULL;
			g_isrunning[index] = false;
		}
		else
		{
			my_print(L"ERROR: Failed to wait for running process!\n");
			terminate_processes();
			break;
		}
	}

	//Terminate all processes still running at this point
	terminate_processes();
	return exit_code;
}

// ==========================================================================
// MAIN FUNCTION
// ==========================================================================

static int mparallel_main(const int argc, const wchar_t *const argv[])
{
	//Initialize globals and options
	g_logo_printed = false;
	g_option_force_use_shell = false;
	g_option_read_stdin_lines = false;
	g_option_auto_quote_vars = false;
	g_option_abort_on_failure = false;
	g_option_separator = L":";
	g_option_max_instances = processor_count();
	g_process_count = 0;
	g_option_process_timeout = 0;

	//Clear
	memset(g_processes, 0, sizeof(HANDLE) * MAXIMUM_WAIT_OBJECTS);
	memset(g_isrunning, 0, sizeof(bool)   * MAXIMUM_WAIT_OBJECTS);
	
	//Parse CLI arguments
	if (!parse_arguments(argc, argv))
	{
		my_print(L"Failed to parse command-line arguments. Run with option \"--help\" for guidance!\n\n");
		return EXIT_FAILURE;
	}

	//Parse jobs from file
	if (!g_option_input_file_name.empty())
	{
		if (!parse_commands_file(g_option_input_file_name.c_str()))
		{
			my_print(L"Failed to read commands from specified input file!\n\n");
			return EXIT_FAILURE;
		}
	}

	//Parse jobs from STDIN
	if (g_option_read_stdin_lines)
	{
		parse_commands_file(stdin);
	}

	//Valid queue?
	if (g_queue.size() < 1)
	{
		my_print(L"Nothing to do. Run with option \"--help\" for guidance!\n\n");
		return EXIT_FAILURE;
	}

	g_logo_printed = true;
	return run_processes();
}

int wmain(const int argc, const wchar_t *const argv[])
{
	SetErrorMode(SetErrorMode(0x3) | 0x3);
	__try
	{
		_set_invalid_parameter_handler(my_invalid_parameter_handler);
		int filenos[] = { _fileno(stderr), _fileno(stdout), _fileno(stdin), -1 };
		for (int i = 0; filenos[i] >= 0; i++) _setmode(filenos[i], _O_U8TEXT);
		return mparallel_main(argc, argv);
	}
	__except (1)
	{
		fatal_exit(L"\n\nFATAL: Unhandeled exception error!\n\n");
	}
}

