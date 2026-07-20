#include "subprocess.h"

// Runs one external program and captures its output on a non-game thread or in
// the command-line baker. Platform handles are always closed, and timeout or
// cancellation terminates the child and returns an ordinary result/error.

#include <algorithm>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace cs2fow
{
namespace
{

void append_output_tail(std::string &tail, const char *data, size_t size)
{
	if (size >= k_process_output_tail_bytes)
	{
		tail.assign(data + size - k_process_output_tail_bytes, k_process_output_tail_bytes);
		return;
	}
	if (tail.size() + size > k_process_output_tail_bytes)
	{
		tail.erase(0, tail.size() + size - k_process_output_tail_bytes);
	}
	tail.append(data, size);
}

#if defined(_WIN32)
std::wstring quote_argument(const std::wstring &value)
{
	if (value.empty()) return L"\"\"";
	if (value.find_first_of(L" \t\"") == std::wstring::npos)
	{
		return value;
	}
	std::wstring result = L"\"";
	size_t backslashes = 0;
	for (const wchar_t character : value)
	{
		if (character == L'\\')
		{
			++backslashes;
			continue;
		}
		if (character == L'\"')
		{
			result.append(backslashes * 2u + 1u, L'\\');
			result.push_back(character);
		}
		else
		{
			result.append(backslashes, L'\\');
			result.push_back(character);
		}
		backslashes = 0;
	}
	result.append(backslashes * 2u, L'\\');
	result.push_back(L'\"');
	return result;
}
#endif

} // namespace

bool lower_process_priority(std::string &error)
{
#if defined(_WIN32)
	if (!SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS))
	{
		error = "could not lower process priority";
		return false;
	}
#else
	if (setpriority(PRIO_PROCESS, 0, 10) != 0)
	{
		error = std::string("could not lower process priority: ") + std::strerror(errno);
		return false;
	}
#endif
	return true;
}

bool run_process(const std::filesystem::path &executable, const std::vector<std::filesystem::path> &arguments,
	std::chrono::milliseconds timeout, const std::atomic_bool *cancel, bool low_priority,
	posix_process_group process_group, process_result &result, std::string &error)
{
	result = {};
	(void)process_group;
	if (!std::filesystem::exists(executable))
	{
		error = "executable not found: " + executable.string();
		return false;
	}
	const auto started = std::chrono::steady_clock::now();
#if defined(_WIN32)
	const std::wstring executable_wide = executable.wstring();
	std::wstring command = quote_argument(executable_wide);
	for (const std::filesystem::path &argument : arguments)
	{
		command.push_back(L' ');
		command += quote_argument(argument.native());
	}
	std::vector<wchar_t> mutable_command(command.begin(), command.end());
	mutable_command.push_back(L'\0');
	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	if (job == nullptr)
	{
		error = "could not create process job";
		return false;
	}
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
	{
		CloseHandle(job);
		error = "could not configure process job";
		return false;
	}
	SECURITY_ATTRIBUTES pipe_security {sizeof(pipe_security), nullptr, TRUE};
	HANDLE output_read = nullptr;
	HANDLE output_write = nullptr;
	if (!CreatePipe(&output_read, &output_write, &pipe_security, 0)
		|| !SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0))
	{
		if (output_read != nullptr) CloseHandle(output_read);
		if (output_write != nullptr) CloseHandle(output_write);
		CloseHandle(job);
		error = "could not create process output pipe";
		return false;
	}
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = nullptr;
	startup.hStdOutput = output_write;
	startup.hStdError = output_write;
	const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | (low_priority ? BELOW_NORMAL_PRIORITY_CLASS : 0);
	if (!CreateProcessW(executable_wide.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &startup, &process))
	{
		CloseHandle(output_read);
		CloseHandle(output_write);
		CloseHandle(job);
		error = "could not start process";
		return false;
	}
	CloseHandle(output_write);
	if (!AssignProcessToJobObject(job, process.hProcess))
	{
		TerminateProcess(process.hProcess, 1);
		CloseHandle(output_read);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		CloseHandle(job);
		error = "could not assign process job";
		return false;
	}
	ResumeThread(process.hThread);
	const auto drain_output = [&]
	{
		char buffer[4096];
		for (;;)
		{
			DWORD available = 0;
			if (!PeekNamedPipe(output_read, nullptr, 0, nullptr, &available, nullptr))
			{
				return GetLastError() == ERROR_BROKEN_PIPE;
			}
			if (available == 0)
			{
				return true;
			}
			DWORD read = 0;
			if (!ReadFile(output_read, buffer, std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer))), &read, nullptr))
			{
				return GetLastError() == ERROR_BROKEN_PIPE;
			}
			append_output_tail(result.output_tail, buffer, read);
		}
	};
	bool output_ok = true;
	for (;;)
	{
		output_ok = drain_output() && output_ok;
		if (WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0)
		{
			output_ok = drain_output() && output_ok;
			break;
		}
		if (cancel != nullptr && cancel->load())
		{
			result.cancelled = true;
			TerminateJobObject(job, 1);
			WaitForSingleObject(process.hProcess, INFINITE);
			output_ok = drain_output() && output_ok;
			break;
		}
		if (timeout.count() > 0 && std::chrono::steady_clock::now() - started >= timeout)
		{
			result.timed_out = true;
			TerminateJobObject(job, 1);
			WaitForSingleObject(process.hProcess, INFINITE);
			output_ok = drain_output() && output_ok;
			break;
		}
	}
	DWORD exit_code = 1;
	GetExitCodeProcess(process.hProcess, &exit_code);
	result.exit_code = static_cast<int>(exit_code);
	CloseHandle(output_read);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	CloseHandle(job);
	if (!output_ok)
	{
		error = "could not read process output";
		return false;
	}
#else
	std::string executable_string = executable.string();
	std::vector<std::string> values;
	values.reserve(arguments.size() + 1u);
	values.push_back(executable_string);
	for (const std::filesystem::path &argument : arguments) values.push_back(argument.native());
	std::vector<char *> argv;
	argv.reserve(values.size() + 1u);
	for (std::string &value : values)
	{
		argv.push_back(value.data());
	}
	argv.push_back(nullptr);
	int output_pipe[2] {-1, -1};
	if (pipe(output_pipe) != 0 || fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) != 0)
	{
		if (output_pipe[0] >= 0) close(output_pipe[0]);
		if (output_pipe[1] >= 0) close(output_pipe[1]);
		error = std::string("could not create process output pipe: ") + std::strerror(errno);
		return false;
	}
	posix_spawn_file_actions_t file_actions;
	int setup_error = posix_spawn_file_actions_init(&file_actions);
	const bool file_actions_initialized = setup_error == 0;
	if (setup_error == 0) setup_error = posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDOUT_FILENO);
	if (setup_error == 0) setup_error = posix_spawn_file_actions_adddup2(&file_actions, output_pipe[1], STDERR_FILENO);
	if (setup_error == 0) setup_error = posix_spawn_file_actions_addclose(&file_actions, output_pipe[0]);
	if (setup_error == 0) setup_error = posix_spawn_file_actions_addclose(&file_actions, output_pipe[1]);
	if (setup_error != 0)
	{
		if (file_actions_initialized) posix_spawn_file_actions_destroy(&file_actions);
		close(output_pipe[0]);
		close(output_pipe[1]);
		error = std::string("could not configure process output: ") + std::strerror(setup_error);
		return false;
	}
	posix_spawnattr_t attributes;
	setup_error = posix_spawnattr_init(&attributes);
	const bool attributes_initialized = setup_error == 0;
	if (process_group == posix_process_group::isolated)
	{
		if (setup_error == 0) setup_error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
		if (setup_error == 0) setup_error = posix_spawnattr_setpgroup(&attributes, 0);
	}
	if (setup_error != 0)
	{
		posix_spawn_file_actions_destroy(&file_actions);
		if (attributes_initialized) posix_spawnattr_destroy(&attributes);
		close(output_pipe[0]);
		close(output_pipe[1]);
		error = std::string("could not configure process attributes: ") + std::strerror(setup_error);
		return false;
	}
	pid_t pid = 0;
	const int spawn_error = posix_spawn(&pid, executable_string.c_str(), &file_actions, &attributes, argv.data(), environ);
	posix_spawn_file_actions_destroy(&file_actions);
	posix_spawnattr_destroy(&attributes);
	close(output_pipe[1]);
	if (spawn_error != 0)
	{
		close(output_pipe[0]);
		if (spawn_error == EACCES)
		{
			error = "could not start process: missing execute permission or host blocking executables (EACCES)";
		}
		else
		{
			error = std::string("could not start process: ") + std::strerror(spawn_error);
		}
		return false;
	}
	const auto drain_output = [&]
	{
		char buffer[4096];
		for (;;)
		{
			const ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
			if (count > 0)
			{
				append_output_tail(result.output_tail, buffer, static_cast<size_t>(count));
				continue;
			}
			if (count == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return true;
			}
			if (errno != EINTR)
			{
				return false;
			}
		}
	};
	int status = 0;
	bool output_ok = true;
	const auto terminate = [&]
	{
		kill(process_group == posix_process_group::isolated ? -pid : pid, SIGKILL);
	};
	for (;;)
	{
		output_ok = drain_output() && output_ok;
		const pid_t waited = waitpid(pid, &status, WNOHANG);
		if (waited == pid)
		{
			output_ok = drain_output() && output_ok;
			break;
		}
		if (waited < 0 && errno != EINTR)
		{
			const int wait_error = errno;
			terminate();
			waitpid(pid, &status, 0);
			close(output_pipe[0]);
			error = std::string("could not wait for process: ") + std::strerror(wait_error);
			return false;
		}
		if (cancel != nullptr && cancel->load())
		{
			result.cancelled = true;
			terminate();
			waitpid(pid, &status, 0);
			output_ok = drain_output() && output_ok;
			break;
		}
		if (timeout.count() > 0 && std::chrono::steady_clock::now() - started >= timeout)
		{
			result.timed_out = true;
			terminate();
			waitpid(pid, &status, 0);
			output_ok = drain_output() && output_ok;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	close(output_pipe[0]);
	result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	(void)low_priority;
	if (!output_ok)
	{
		error = "could not read process output";
		return false;
	}
#endif
	return true;
}

} // namespace cs2fow
