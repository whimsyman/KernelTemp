#include <iostream>
#include <Windows.h>
#include <TlHelp32.h>

#include "client_dll.hpp"
#include "offsets.hpp"
#include "buttons.hpp"

// key to hold for bhop.
constexpr int bhop_key = VK_SPACE;

// jump button states, read from the game (Nov 2026).
constexpr DWORD jump_pressed = 0x10001;
constexpr DWORD jump_released = 0x1000100;

// FL_ONGROUND bit in m_fFlags.
constexpr std::uint32_t fl_onground = 1 << 0;

static DWORD get_process_id(const wchar_t* process_name) {
	DWORD process_id = 0;

	HANDLE snap_shot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	if (snap_shot == INVALID_HANDLE_VALUE)
		return process_id;

	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(decltype(entry));

	if (Process32FirstW(snap_shot, &entry) == TRUE) {
		do {
			if (_wcsicmp(process_name, entry.szExeFile) == 0) {
				process_id = entry.th32ProcessID;
				break;
			}
		} while (Process32NextW(snap_shot, &entry) == TRUE);
	}

	CloseHandle(snap_shot);

	return process_id;
}

static std::uintptr_t get_module_base(const DWORD pid, const wchar_t* module_name) {
	std::uintptr_t module_base = 0;

	// snap_shot of process modules (dlls)
	HANDLE snap_shot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
	if (snap_shot == INVALID_HANDLE_VALUE)
		return module_base;

	MODULEENTRY32W entry = {};
	entry.dwSize = sizeof(decltype(entry));

	if (Module32FirstW(snap_shot, &entry) == TRUE) {
		do {
			if (_wcsicmp(module_name, entry.szModule) == 0) {
				module_base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
				break;
			}
		} while (Module32NextW(snap_shot, &entry) == TRUE);
	}

	CloseHandle(snap_shot);

	return module_base;
}

namespace driver {
	namespace codes {
		// use to setup the driver.
		constexpr ULONG attach = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x696, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		// read process memory.
		constexpr ULONG read = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x697, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		// write process memory.
		constexpr ULONG write = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x698, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
	} // namespace codes

	// shared between usermode and kernelmode.
	struct Request {
		HANDLE process_id;

		PVOID target;
		PVOID buffer;

		SIZE_T size;
		SIZE_T return_size;
	};

	bool attach_to_process(HANDLE driver_handle, const DWORD pid) {
		Request r = {};
		r.process_id = reinterpret_cast<HANDLE>(pid);

		return DeviceIoControl(driver_handle, codes::attach, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);
	}

	template <class T>
	T read_memory(HANDLE driver_handle, const std::uintptr_t addr) {
		T temp = {};

		Request r = {};
		r.target = reinterpret_cast<PVOID>(addr);
		r.buffer = &temp;
		r.size = sizeof(T);

		DeviceIoControl(driver_handle, codes::read, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);

		return temp;
	}

	template <class T>
	void write_memory(HANDLE driver_handle, const std::uintptr_t addr, const T& value) {
		Request r = {};
		r.target = reinterpret_cast<PVOID>(addr);
		r.buffer = (PVOID)&value;
		r.size = sizeof(T);

		DeviceIoControl(driver_handle, codes::write, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);
	}
} // namespace driver

int main() {
	const DWORD pid = get_process_id(L"cs2.exe");

	if (pid == 0) {
		std::cout << "Failed to find cs2.\n";
		std::cin.get();
		return 1;
	}

	const HANDLE driver = CreateFile(L"\\\\.\\WDK", GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (driver == INVALID_HANDLE_VALUE) {
		std::cout << "Failed to create the driver handle.\n";
		std::cin.get();
		return 1;
	}

	if (driver::attach_to_process(driver, pid)) {
		std::cout << "Attachment successful.\n";

		if (const std::uintptr_t client = get_module_base(pid, L"client.dll"); client != 0) {
			std::cout << "Client found.\n";

			while (true) {
				if (GetAsyncKeyState(VK_END) & 0x8000)
					break;

				const auto local_player_pawn = driver::read_memory<std::uintptr_t>(driver, client + client_dll::dwLocalPlayerPawn);
				if (local_player_pawn == 0) {
					Sleep(1);
					continue;
				}

				const auto flags = driver::read_memory<std::uint32_t>(driver, local_player_pawn + client_dll::C_BaseEntity::m_fFlags);
				const bool on_ground = flags & fl_onground;
				const bool key_held = GetAsyncKeyState(bhop_key) & 0x8000;

				const auto jump_state = driver::read_memory<DWORD>(driver, client + buttons::jump);

				// press jump on the ground, release it in the air so the next landing can jump again.
				if (key_held && on_ground) {
					driver::write_memory(driver, client + buttons::jump, jump_pressed);
				}
				else if (jump_state == jump_pressed) {
					driver::write_memory(driver, client + buttons::jump, jump_released);
				}

				Sleep(1);
			}
		}
		else {
			std::cout << "Failed to find client.dll.\n";
		}
	}
	else {
		std::cout << "Failed to attach to cs2.\n";
	}

	CloseHandle(driver);
	std::cin.get();

	return 0;
}