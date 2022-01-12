﻿#include "pch.h"
#include "Apps/MainApp/Internal/GameResourceOverrider.h"

#include <XivAlexanderCommon/Sqex/SeString.h>
#include <XivAlexanderCommon/Utils/Win32/Process.h>

#include "Apps/MainApp/Internal/VirtualSqPacks.h"
#include "Config.h"
#include "Misc/DebuggerDetectionDisabler.h"
#include "Misc/Hooks.h"
#include "Misc/Logger.h"
#include "XivAlexander.h"

class ReEnterPreventer {
	std::mutex m_lock;
	std::set<DWORD> m_tids;

public:
	class Lock {
		ReEnterPreventer& p;
		bool re = false;

	public:
		explicit Lock(ReEnterPreventer& p)
			: p(p) {
			const auto tid = GetCurrentThreadId();

			std::lock_guard lock(p.m_lock);
			re = p.m_tids.find(tid) != p.m_tids.end();
			if (!re)
				p.m_tids.insert(tid);
		}

		Lock(const Lock&) = delete;
		Lock& operator=(const Lock&) = delete;
		Lock(Lock&& r) = delete;
		Lock& operator=(Lock&&) = delete;

		~Lock() {
			if (!re) {
				std::lock_guard lock(p.m_lock);
				p.m_tids.erase(GetCurrentThreadId());
			}
		}

		operator bool() const {
			// True if new enter
			return !re;
		}
	};
};

struct XivAlexander::Apps::MainApp::Internal::GameResourceOverrider::Implementation {
	Apps::MainApp::App& App;
	const std::shared_ptr<Config> m_config;
	const std::shared_ptr<Misc::Logger> m_logger;
	const std::shared_ptr<Misc::DebuggerDetectionDisabler> m_debugger;
	const std::filesystem::path m_sqpackPath;
	std::optional<Internal::VirtualSqPacks> m_sqpacks;
	bool m_bSqpackFailed = false;

	std::vector<std::unique_ptr<Misc::Hooks::PointerFunction<size_t, uint32_t, const char*, size_t>>> fns{};
	std::vector<std::string> m_lastLoggedPaths;
	std::mutex m_lastLoggedPathMtx;
	Utils::CallOnDestruction::Multiple m_cleanup;

	Misc::Hooks::ImportedFunction<HANDLE, LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE> CreateFileW{"kernel32::CreateFileW", "kernel32.dll", "CreateFileW"};
	Misc::Hooks::ImportedFunction<BOOL, HANDLE> CloseHandle{"kernel32::CloseHandle", "kernel32.dll", "CloseHandle"};
	Misc::Hooks::ImportedFunction<BOOL, HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED> ReadFile{"kernel32::ReadFile", "kernel32.dll", "ReadFile"};
	Misc::Hooks::ImportedFunction<BOOL, HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD> SetFilePointerEx{"kernel32::SetFilePointerEx", "kernel32.dll", "SetFilePointerEx"};

	ReEnterPreventer m_repCreateFileW, m_repReadFile;
	
	Utils::Win32::Thread VirtualSqPackInitThread;
	Utils::ListenerManager<Implementation, void> OnVirtualSqPacksInitialized;

	Implementation(Apps::MainApp::App& app)
		: App(app)
		, m_config(Config::Acquire())
		, m_logger(Misc::Logger::Acquire())
		, m_debugger(Misc::DebuggerDetectionDisabler::Acquire())
		, m_sqpackPath(Utils::Win32::Process::Current().PathOf().remove_filename() / L"sqpack") {

		VirtualSqPackInitThread = Utils::Win32::Thread(L"VirtualSqPackInitThread", [&]() {
			if (!Dll::IsLoadedAsDependency() && !Dll::IsLoadedFromEntryPoint())
				return;

			try {
				m_sqpacks.emplace(App, Utils::Win32::Process::Current().PathOf().remove_filename() / L"sqpack");
			} catch (const std::exception& e) {
				m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"Failed to load VirtualSqPacks: {}", e.what());
				return;
			}

			OnVirtualSqPacksInitialized();

			});

		m_cleanup += CreateFileW.SetHook([this](
			_In_ LPCWSTR lpFileName,
			_In_ DWORD dwDesiredAccess,
			_In_ DWORD dwShareMode,
			_In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
			_In_ DWORD dwCreationDisposition,
			_In_ DWORD dwFlagsAndAttributes,
			_In_opt_ HANDLE hTemplateFile
			) {
				if (const auto lock = ReEnterPreventer::Lock(m_repCreateFileW); lock &&
					!(dwDesiredAccess & GENERIC_WRITE) &&
					dwCreationDisposition == OPEN_EXISTING &&
					!hTemplateFile) {

					VirtualSqPackInitThread.Wait();
					if (const auto res = m_sqpacks ? m_sqpacks->Open(lpFileName) : nullptr)
						return res;
				}

				return CreateFileW.bridge(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
			});

		m_cleanup += CloseHandle.SetHook([this](
			HANDLE handle
		) {
				if (m_sqpacks && m_sqpacks->Close(handle))
					return 0;

				return CloseHandle.bridge(handle);
			});

		m_cleanup += ReadFile.SetHook([this](
			_In_ HANDLE hFile,
			_Out_writes_bytes_to_opt_(nNumberOfBytesToRead, *lpNumberOfBytesRead) __out_data_source(FILE) LPVOID lpBuffer,
			_In_ DWORD nNumberOfBytesToRead,
			_Out_opt_ LPDWORD lpNumberOfBytesRead,
			_Inout_opt_ LPOVERLAPPED lpOverlapped
			) {
				if (const auto pvpath = m_sqpacks ? m_sqpacks->Get(hFile) : nullptr) {
					auto& vpath = *pvpath;
					try {
						m_sqpacks->MarkIoRequest();
						const auto fp = lpOverlapped ? ((static_cast<uint64_t>(lpOverlapped->OffsetHigh) << 32) | lpOverlapped->Offset) : vpath.FilePointer.QuadPart;
						const auto read = vpath.Stream->ReadStreamPartial(fp, lpBuffer, nNumberOfBytesToRead);

						if (lpNumberOfBytesRead)
							*lpNumberOfBytesRead = static_cast<DWORD>(read);

						if (read != nNumberOfBytesToRead) {
							m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"ReadFile: {}, requested {} bytes, read {} bytes; state: {}",
								vpath.Path.filename(), nNumberOfBytesToRead, read, vpath.Stream->DescribeState());
						} else {
							if (m_config->Runtime.LogAllDataFileRead) {
								m_logger->Format<LogLevel::Info>(LogCategory::GameResourceOverrider, L"ReadFile: {}, requested {} bytes; state: {}",
									vpath.Path.filename(), nNumberOfBytesToRead, vpath.Stream->DescribeState());
							}
						}

						if (lpOverlapped) {
							if (lpOverlapped->hEvent)
								SetEvent(lpOverlapped->hEvent);
							lpOverlapped->Internal = 0;
							lpOverlapped->InternalHigh = static_cast<DWORD>(read);
						} else
							vpath.FilePointer.QuadPart = fp + read;

						return TRUE;

					} catch (const Utils::Win32::Error& e) {
						if (e.Code() != ERROR_IO_PENDING)
							m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"ReadFile: {}, Message: {}",
								vpath.Path.filename(), e.what());
						SetLastError(e.Code());
						return FALSE;

					} catch (const std::exception& e) {
						m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"ReadFile: {}, Message: {}",
							vpath.Path.filename(), e.what());
						SetLastError(ERROR_READ_FAULT);
						return FALSE;
					}
				}
				return ReadFile.bridge(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
			});

		m_cleanup += SetFilePointerEx.SetHook([this](
			_In_ HANDLE hFile,
			_In_ LARGE_INTEGER liDistanceToMove,
			_Out_opt_ PLARGE_INTEGER lpNewFilePointer,
			_In_ DWORD dwMoveMethod) {
				if (const auto pvpath = m_sqpacks ? m_sqpacks->Get(hFile) : nullptr) {
					if (lpNewFilePointer)
						*lpNewFilePointer = {};

					auto& vpath = *pvpath;
					try {
						m_sqpacks->MarkIoRequest();
						const auto len = vpath.Stream->StreamSize();

						if (dwMoveMethod == FILE_BEGIN)
							vpath.FilePointer.QuadPart = liDistanceToMove.QuadPart;
						else if (dwMoveMethod == FILE_CURRENT)
							vpath.FilePointer.QuadPart += liDistanceToMove.QuadPart;
						else if (dwMoveMethod == FILE_END)
							vpath.FilePointer.QuadPart = len - liDistanceToMove.QuadPart;
						else {
							SetLastError(ERROR_INVALID_PARAMETER);
							return FALSE;
						}

						if (vpath.FilePointer.QuadPart > static_cast<int64_t>(len))
							vpath.FilePointer.QuadPart = static_cast<int64_t>(len);

						if (lpNewFilePointer)
							*lpNewFilePointer = vpath.FilePointer;

					} catch (const Utils::Win32::Error& e) {
						m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"SetFilePointerEx: {}, Message: {}",
							vpath.Path.filename(), e.what());
						SetLastError(e.Code());
						return FALSE;

					} catch (const std::exception& e) {
						m_logger->Format<LogLevel::Warning>(LogCategory::GameResourceOverrider, L"ReadFile: {}, Message: {}",
							vpath.Path.filename(), e.what());
						SetLastError(ERROR_READ_FAULT);
						return FALSE;
					}

					return TRUE;
				}
				return SetFilePointerEx.bridge(hFile, liDistanceToMove, lpNewFilePointer, dwMoveMethod);
			});


		for (auto ptr : Misc::Signatures::LookupForData([](const IMAGE_SECTION_HEADER& p) {
					return strncmp(reinterpret_cast<const char*>(p.Name), ".text", 5) == 0;
				},
				"\x40\x57\x48\x8d\x3d\x00\x00\x00\x00\x00\x8b\xd8\x4c\x8b\xd2\xf7\xd1\x00\x85\xc0\x74\x25\x41\xf6\xc2\x03\x74\x1f\x41\x0f\xb6\x12\x8b\xc1",
				"\xFF\xFF\xFF\xFF\xFF\x00\x00\x00\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF",
				34,
				{}
			)) {
			fns.emplace_back(std::make_unique<Misc::Hooks::PointerFunction<size_t, uint32_t, const char*, size_t>>(
				"FFXIV::GeneralHashCalcFn",
				reinterpret_cast<size_t(__stdcall*)(uint32_t, const char*, size_t)>(ptr)
			));
			m_cleanup += fns.back()->SetHook([this, ptr, self = fns.back().get()](uint32_t initVal, const char* str, size_t len) {
				if (!str || !*str || len >= 512 || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str, static_cast<int>(len), nullptr, 0))
					return self->bridge(initVal, str, len);

				auto name = std::string(str);
				std::string ext, rest;
				if (const auto i1 = name.find_first_of('.'); i1 != std::string::npos) {
					ext = name.substr(i1);
					name.resize(i1);
					if (const auto i2 = ext.find_first_of('.', 1); i2 != std::string::npos) {
						rest = ext.substr(i2);
						ext.resize(i2);
					}
				}

				const auto nameLower = [&name]() {
					auto val = Utils::FromUtf8(name);
					CharLowerW(&val[0]);
					return Utils::ToUtf8(val);
				}();

				const auto extLower = [&ext]() {
					auto val = Utils::FromUtf8(ext);
					CharLowerW(&val[0]);
					return Utils::ToUtf8(val);
				}();

				auto overrideLanguage = Sqex::Language::Unspecified;
				if (extLower == ".scd") {
					if (nameLower.starts_with("cut/") || nameLower.starts_with("sound/voice/vo_line"))
						overrideLanguage = m_config->Runtime.VoiceResourceLanguageOverride;
				} else {
					overrideLanguage = m_config->Runtime.ResourceLanguageOverride;
				}

				std::string description;
				if (overrideLanguage != Sqex::Language::Unspecified) {
					const char* languageCodes[] = {"ja", "en", "de", "fr", "chs", "cht", "ko"};
					const auto targetLanguageCode = languageCodes[static_cast<int>(overrideLanguage) - 1];

					std::string newName;
					if (nameLower.starts_with("ui/uld/logo")) {
						// do nothing, as overriding this often freezes the game
					} else {
						for (const auto languageCode : languageCodes) {
							char t[16];
							sprintf_s(t, "_%s", languageCode);
							if (nameLower.ends_with(t)) {
								newName = name.substr(0, name.size() - strlen(languageCode)) + targetLanguageCode;
								break;
							}
							sprintf_s(t, "/%s/", languageCode);
							if (const auto pos = nameLower.find(t); pos != std::string::npos) {
								newName = std::format("{}/{}/{}", name.substr(0, pos), targetLanguageCode, name.substr(pos + strlen(t)));
								break;
							}
							sprintf_s(t, "_%s_", languageCode);
							if (const auto pos = nameLower.find(t); pos != std::string::npos) {
								newName = std::format("{}_{}_{}", name.substr(0, pos), targetLanguageCode, name.substr(pos + strlen(t)));
								break;
							}
						}
					}
					if (!newName.empty() && name != newName && m_sqpacks && m_sqpacks->EntryExists(std::format("{}{}", newName, ext))) {
						const auto newStr = std::format("{}{}{}", newName, ext, rest);
						description = std::format("{} => {}", std::string_view(str, len), newStr);
						Utils::Win32::Process::Current().WriteMemory(const_cast<char*>(str), newStr.c_str(), newStr.size() + 1, true);
						len = newStr.size();
					}
				}
				const auto res = self->bridge(initVal, str, len);

				if (m_config->Runtime.UseHashTrackerKeyLogging || !description.empty()) {
					auto current = std::string(str, len);

					const auto lock = std::lock_guard(m_lastLoggedPathMtx);
					if (const auto it = std::ranges::find(m_lastLoggedPaths, current); it != m_lastLoggedPaths.end()) {
						m_lastLoggedPaths.erase(it);
					} else {
						const auto pathSpec = Sqex::Sqpack::EntryPathSpec(current);
						m_logger->Format(LogCategory::GameResourceOverrider,
							"{} (~{:08x}/~{:08x}, ~{:08x}) => ~{:08x} (f={:x}, iv={:x})",
							description.empty() ? current : description,
							pathSpec.PathHash, pathSpec.NameHash, pathSpec.FullPathHash,
							res, reinterpret_cast<size_t>(ptr), initVal);
					}
					m_lastLoggedPaths.push_back(std::move(current));
					while (m_lastLoggedPaths.size() > 16)
						m_lastLoggedPaths.erase(m_lastLoggedPaths.begin());
				}

				return res;
			});
		}
	}

	~Implementation() {
		m_cleanup.Clear();
	}
};

XivAlexander::Apps::MainApp::Internal::GameResourceOverrider::GameResourceOverrider(Apps::MainApp::App& app)
	: m_pImpl(std::make_unique<Implementation>(app)) {
}

XivAlexander::Apps::MainApp::Internal::GameResourceOverrider::~GameResourceOverrider() = default;

std::optional<XivAlexander::Apps::MainApp::Internal::VirtualSqPacks>& XivAlexander::Apps::MainApp::Internal::GameResourceOverrider::GetVirtualSqPacks() {
	return m_pImpl->m_sqpacks;
}

Utils::CallOnDestruction XivAlexander::Apps::MainApp::Internal::GameResourceOverrider::OnVirtualSqPacksInitialized(std::function<void()> f) {
	return m_pImpl->OnVirtualSqPacksInitialized(std::move(f));
}