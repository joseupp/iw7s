#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <utils/thread.hpp>
#include <utils/compression.hpp>

#include <exception/minidump.hpp>

#include <version.hpp>

namespace exception
{
	namespace
	{
		const char* crash_name;

		thread_local struct
		{
			DWORD code = 0;
			PVOID address = nullptr;
		} exception_data;

		// Error fatal "exceeded maximum number of script strings" al salir del juego (comando quit, 0x140BADE9E llama
		// al apagado 0x140342C20): las zonas se descargan cuando ya no quedan cadenas de script libres. El juego se
		// esta cerrando igualmente: se sale sin ventana de error.
		utils::hook::detour fatal_error_hook;

		void fatal_error_stub(const char* fmt, ...)
		{
			char message[1024]{};
			va_list ap;
			va_start(ap, fmt);
			vsnprintf_s(message, _TRUNCATE, fmt ? fmt : "", ap);
			va_end(ap);

			if (std::strstr(message, "script strings"))
			{
				void* frames[48]{};
				const auto count = CaptureStackBackTrace(0, 48, frames, nullptr);
				for (USHORT i = 0; i < count; ++i)
				{
					if (frames[i] == reinterpret_cast<void*>(0x140BADEA3))
					{
						TerminateProcess(GetCurrentProcess(), 0);
					}
				}
			}

			fatal_error_hook.invoke<void>("%s", message);
		}

		void show_mouse_cursor()
		{
			while (ShowCursor(TRUE) < 0);
		}

		void display_error_dialog()
		{
			std::string error_str = utils::string::va("Fatal error (0x%08X) at 0x%p.\n"
				"A minidump has been written at: %s\n\n",
				exception_data.code, exception_data.address, crash_name);

			utils::thread::suspend_other_threads();
			show_mouse_cursor();

			MessageBoxA(nullptr, error_str.data(), "ERROR", MB_ICONERROR);
			TerminateProcess(GetCurrentProcess(), exception_data.code);
		}

		void reset_state()
		{
			display_error_dialog();
		}

		size_t get_reset_state_stub()
		{
			static auto* stub = utils::hook::assemble([](utils::hook::assembler& a)
			{
				a.sub(rsp, 0x10);
				a.or_(rsp, 0x8);
				a.jmp(reset_state);
			});

			return reinterpret_cast<size_t>(stub);
		}

		std::string get_timestamp()
		{
			tm ltime{};
			char timestamp[MAX_PATH] = { 0 };
			const auto time = _time64(nullptr);

			_localtime64_s(&ltime, &time);
			strftime(timestamp, sizeof(timestamp) - 1, "%Y-%m-%d-%H-%M-%S", &ltime);

			return timestamp;
		}

		std::string generate_crash_info(const LPEXCEPTION_POINTERS exceptioninfo)
		{
			std::string info{};
			const auto line = [&info](const std::string& text)
			{
				info.append(text);
				info.append("\r\n");
			};

			line("IW7S Crash Dump");
			line("");
			line("Version: "s + VERSION);
			line("Environment: "s + (game::environment::is_dedi() ? "Dedicated" : "Base"));
			line("Timestamp: "s + get_timestamp());
			line(utils::string::va("Exception: 0x%08X", exceptioninfo->ExceptionRecord->ExceptionCode));
			line(utils::string::va("Address: 0x%llX", exceptioninfo->ExceptionRecord->ExceptionAddress));

#pragma warning(push)
#pragma warning(disable: 4996)
			OSVERSIONINFOEXA version_info;
			ZeroMemory(&version_info, sizeof(version_info));
			version_info.dwOSVersionInfoSize = sizeof(version_info);
			GetVersionExA(reinterpret_cast<LPOSVERSIONINFOA>(&version_info));
#pragma warning(pop)

			line(utils::string::va("OS Version: %u.%u", version_info.dwMajorVersion, version_info.dwMinorVersion));

			return info;
		}

		void write_minidump(const LPEXCEPTION_POINTERS exceptioninfo)
		{
			crash_name = utils::string::va("minidumps/iw7s-crash-%s.zip", get_timestamp().data());

			utils::compression::zip::archive zip_file{};
			zip_file.add("crash.dmp", create_minidump(exceptioninfo));
			zip_file.add("info.txt", generate_crash_info(exceptioninfo));
			zip_file.write(crash_name, "IW7S Crash Dump");
		}

		bool is_harmless_error(const LPEXCEPTION_POINTERS exceptioninfo)
		{
			const auto code = exceptioninfo->ExceptionRecord->ExceptionCode;
			return code == STATUS_INTEGER_OVERFLOW || code == STATUS_FLOAT_OVERFLOW || code == STATUS_SINGLE_STEP;
		}

		LONG WINAPI exception_filter(const LPEXCEPTION_POINTERS exceptioninfo)
		{
			if (is_harmless_error(exceptioninfo))
			{
				return EXCEPTION_CONTINUE_EXECUTION;
			}

			write_minidump(exceptioninfo);

			exception_data.code = exceptioninfo->ExceptionRecord->ExceptionCode;
			exception_data.address = exceptioninfo->ExceptionRecord->ExceptionAddress;
			exceptioninfo->ContextRecord->Rip = get_reset_state_stub();

			return EXCEPTION_CONTINUE_EXECUTION;
		}

		void WINAPI set_unhandled_exception_filter_stub(LPTOP_LEVEL_EXCEPTION_FILTER)
		{
			// Don't register anything here...
		}
	}

	class component final : public component_interface
	{
	public:
		component()
		{
			SetUnhandledExceptionFilter(exception_filter);
		}

		void post_load() override
		{
			const utils::nt::library ntdll("ntdll.dll");
			auto* set_filter = ntdll.get_proc<void(*)(LPTOP_LEVEL_EXCEPTION_FILTER)>("RtlSetUnhandledExceptionFilter");

			set_filter(exception_filter);
			utils::hook::jump(set_filter, set_unhandled_exception_filter_stub, true);
		}

		void post_unpack() override
		{
			fatal_error_hook.create(0x140D34180, fatal_error_stub);
		}
	};
}

REGISTER_COMPONENT(exception::component)