﻿#include "pch.h"

#include <XivAlexander/XivAlexander.h>
#include <XivAlexanderCommon/Utils_Win32_InjectedModule.h>
#include <XivAlexanderCommon/Utils_Win32_Resource.h>
#include <XivAlexanderCommon/XivAlex.h>
#include <XivAlexanderCommon/Utils_Win32_TaskDialogBuilder.h>

#include "App_ConfigRepository.h"
#include "DllMain.h"
#include "resource.h"

static XivAlexDll::LoaderAction ParseLoaderAction(std::string val) {
	if (val.empty())
		return XivAlexDll::LoaderAction::Interactive;
	auto valw = Utils::FromUtf8(val);
	CharLowerW(&valw[0]);
	val = Utils::ToUtf8(valw);
	for (size_t i = 0; i < static_cast<size_t>(XivAlexDll::LoaderAction::Count_); ++i) {
		const auto compare = std::string(LoaderActionToString(static_cast<XivAlexDll::LoaderAction>(i)));
		auto equal = true;
		for (size_t j = 0; equal && j < val.length() && j < compare.length(); ++j) {
			equal = val[j] == compare[j];
		}
		if (equal)
			return static_cast<XivAlexDll::LoaderAction>(i);
	}
	throw std::runtime_error("invalid LoaderAction");
}

template<>
std::string argparse::details::repr(XivAlexDll::LoaderAction const& val) {
	return LoaderActionToString(val);
}

enum class LauncherType : int {
	Auto,
	Select,
	International,
	Korean,
	Chinese,
	Count_,  // for internal use only
};

template<>
std::string argparse::details::repr(LauncherType const& val) {
	switch (val) {
		case LauncherType::Auto: return "auto";
		case LauncherType::Select: return "select";
		case LauncherType::International: return "international";
		case LauncherType::Korean: return "korean";
		case LauncherType::Chinese: return "chinese";
	}
	return std::format("({})", static_cast<int>(val));
}

template<>
std::string argparse::details::repr(XivAlex::GameRegion const& val) {
	switch (val) {
		case XivAlex::GameRegion::International: return Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_CLIENT_INTERNATIONAL) + 1);
		case XivAlex::GameRegion::Korean: return Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_CLIENT_KOREAN) + 1);
		case XivAlex::GameRegion::Chinese: return Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_CLIENT_CHINESE) + 1);
	}
	return std::format("({})", static_cast<int>(val));
}

LauncherType ParseLauncherType(const std::string& val_) {
	auto val = Utils::FromUtf8(val_);
	CharLowerW(&val[0]);
	for (size_t i = 0; i < static_cast<size_t>(LauncherType::Count_); ++i) {
		auto compare = Utils::FromUtf8(argparse::details::repr(static_cast<LauncherType>(i)));
		CharLowerW(&compare[0]);

		auto equal = true;
		for (size_t j = 0; equal && j < val.length() && j < compare.length(); ++j)
			equal = val[j] == compare[j];
		if (equal)
			return static_cast<LauncherType>(i);
	}
	if (val[0] == L'e' || val[0] == L'd' || val[0] == L'g' || val[0] == L'f' || val[0] == L'j')
		return LauncherType::International;

	throw std::runtime_error(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_ERROR_INVALID_LAUNCHER_TYPE) + 1));
}

class XivAlexanderLoaderParameter {
public:
	argparse::ArgumentParser argp;

	XivAlexDll::LoaderAction m_action = XivAlexDll::LoaderAction::Interactive;
	LauncherType m_launcherType = LauncherType::Auto;
	bool m_quiet = false;
	bool m_help = false;
	bool m_disableAutoRunAs = true;
	std::set<DWORD> m_targetPids{};
	std::vector<Utils::Win32::Process> m_targetProcessHandles{};
	std::set<std::wstring> m_targetSuffix{};
	std::wstring m_runProgram;
	std::wstring m_runProgramArgs;
	bool m_debugUpdate = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);

	XivAlexanderLoaderParameter()
		: argp("XivAlexanderLoader") {
		// SetThreadUILanguage(MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN));
		// SetThreadUILanguage(MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN));

		argp.add_argument("-a", "--action")
			.help(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_HELP_ACTION) + 1))
			.required()
			.nargs(1)
			.default_value(XivAlexDll::LoaderAction::Interactive)
			.action([](const std::string& val) { return ParseLoaderAction(val); });
		argp.add_argument("-l", "--launcher")
			.help(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_HELP_LAUNCHER) + 1))
			.required()
			.nargs(1)
			.default_value(LauncherType::Auto)
			.action([](const std::string& val) { return ParseLauncherType(val); });
		argp.add_argument("-q", "--quiet")
			.help(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_HELP_QUIET) + 1))
			.default_value(false)
			.implicit_value(true);
		argp.add_argument("-d", "--disable-runas")
			.help(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_HELP_DISABLE_RUNAS) + 1))
			.default_value(false)
			.implicit_value(true);

		// internal use
		argp.add_argument("--handle-instead-of-pid")
			.help(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_HELP_INTERNAL_USE_ONLY) + 1))
			.required()
			.default_value(false)
			.implicit_value(true);
		argp.add_argument("--wait-process")
			.help(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_HELP_INTERNAL_USE_ONLY) + 1))
			.required()
			.nargs(1)
			.default_value(Utils::Win32::Process())
			.action([](const std::string& val) {
				return Utils::Win32::Process(reinterpret_cast<HANDLE>(std::stoull(val, nullptr, 0)), true);
			});

		argp.add_argument("targets")
			.help(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_HELP_TARGETS) + 1))
			.remaining();
	}

	void Parse() {
		std::vector<std::string> args;

		if (int nArgs; LPWSTR* szArgList = CommandLineToArgvW(Dll::GetOriginalCommandLine().data(), &nArgs)) {
			for (int i = 0; i < nArgs; i++)
				args.emplace_back(Utils::ToUtf8(szArgList[i]));
			LocalFree(szArgList);
		}

		// prevent argparse from taking over --help
		if (std::find(args.begin() + 1, args.end(), std::string("-h")) != args.end()
			|| std::find(args.begin() + 1, args.end(), std::string("-help")) != args.end()) {
			m_help = true;
			return;
		}

		argp.parse_args(args);

		m_action = argp.get<XivAlexDll::LoaderAction>("-a");
		m_launcherType = argp.get<LauncherType>("-l");
		m_quiet = argp.get<bool>("-q");
		m_disableAutoRunAs = argp.get<bool>("-d");
		if (const auto waitHandle = argp.get<Utils::Win32::Process>("--wait-process"))
			waitHandle.Wait();

		if (argp.present("targets")) {
			const auto targets = argp.get<std::vector<std::string>>("targets");
			if (argp.get<bool>("--handle-instead-of-pid")) {
				for (const auto& target : targets) {
					m_targetProcessHandles.emplace_back(reinterpret_cast<HANDLE>(std::stoull(target, nullptr, 0)), true);
				}

			} else {
				if (!targets.empty()) {
					m_runProgram = Utils::FromUtf8(targets[0]);
					m_runProgramArgs = Utils::FromUtf8(Utils::Win32::ReverseCommandLineToArgv(std::span(targets.begin() + 1, targets.end())));
				}

				for (const auto& target : targets) {
					size_t idx = 0;
					DWORD pid = 0;

					try {
						pid = std::stoi(target, &idx);
					} catch (const std::invalid_argument&) {
						// empty
					} catch (const std::out_of_range&) {
						// empty
					}
					if (idx != target.length()) {
						auto buf = Utils::FromUtf8(target);
						CharLowerW(&buf[0]);
						m_targetSuffix.emplace(std::move(buf));
					} else {
						m_targetPids.insert(pid);
					}
				}
			}
		}
	}

	[[nodiscard]] std::wstring GetHelpMessage() const {
		return std::format(FindStringResourceEx(Dll::Module(), IDS_APP_DESCRIPTION) + 1, argp.help().str());
	}
};

static std::unique_ptr<XivAlexanderLoaderParameter> g_parameters;

static std::set<DWORD> GetTargetPidList() {
	std::set<DWORD> pids;
	if (!g_parameters->m_targetPids.empty()) {
		auto list = Utils::Win32::GetProcessList();
		std::ranges::sort(list);
		std::ranges::set_intersection(list, g_parameters->m_targetPids, std::inserter(pids, pids.end()));
		pids.insert(g_parameters->m_targetPids.begin(), g_parameters->m_targetPids.end());
	} else if (g_parameters->m_targetSuffix.empty()) {
		g_parameters->m_targetSuffix.emplace(XivAlex::GameExecutable32NameW);
		g_parameters->m_targetSuffix.emplace(XivAlex::GameExecutable64NameW);
	}
	if (!g_parameters->m_targetSuffix.empty()) {
		for (const auto pid : Utils::Win32::GetProcessList()) {
			Utils::Win32::Process hProcess;
			try {
				hProcess = Utils::Win32::Process(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
			} catch (const std::exception&) {
				// some processes only allow PROCESS_QUERY_INFORMATION,
				// while denying PROCESS_QUERY_LIMITED_INFORMATION.
				try {
					hProcess = Utils::Win32::Process(PROCESS_QUERY_INFORMATION, false, pid);
				} catch (const std::exception&) {
					continue;
				}
			}
			try {
				auto pathbuf = hProcess.PathOf().wstring();
				auto suffixFound = false;
				CharLowerW(&pathbuf[0]);
				for (const auto& suffix : g_parameters->m_targetSuffix) {
					if ((suffixFound = pathbuf.ends_with(suffix)))
						break;
				}
				if (suffixFound)
					pids.insert(pid);
			} catch (const std::exception& e) {
				OutputDebugStringW(std::format(L"Error for PID {}: {}\n", pid, e.what()).c_str());
			}
		}
	}
	return pids;
}

static auto OpenProcessForInjection(DWORD pid) {
	return Utils::Win32::Process(PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, false, pid);
}

static bool RequiresAdminAccess(const std::set<DWORD>& pids) {
	try {
		for (const auto pid : pids)
			OpenProcessForInjection(pid);
	} catch (const Utils::Win32::Error& e) {
		if (e.Code() == ERROR_ACCESS_DENIED)
			return true;
	}
	return false;
}

static void DoPidTask(DWORD pid, const std::filesystem::path& dllDir, const std::filesystem::path& dllPath) {
	auto process = OpenProcessForInjection(pid);

	if (process.IsProcess64Bits() != Utils::Win32::Process::Current().IsProcess64Bits()) {
		Utils::Win32::RunProgram({
			.path = Utils::Win32::Process::Current().PathOf().parent_path() / (process.IsProcess64Bits() ? XivAlex::XivAlexLoader64NameW : XivAlex::XivAlexLoader32NameW),
			.args = std::format(L"{}-a {} {}",
				g_parameters->m_quiet ? L"-q " : L"",
				LoaderActionToString(g_parameters->m_action),
				process.GetId()),
			.wait = true
		});
		return;
	}

	void* rpModule = process.AddressOf(dllPath, Utils::Win32::Process::ModuleNameCompareMode::FullPath, false);
	const auto path = process.PathOf();

	auto loaderAction = g_parameters->m_action;
	if (loaderAction == XivAlexDll::LoaderAction::Ask || loaderAction == XivAlexDll::LoaderAction::Interactive) {
		if (rpModule) {
			switch (Dll::MessageBoxF(nullptr, MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1,
				IDS_CONFIRM_INJECT_AGAIN,
				pid, path,
				Utils::Win32::MB_GetString(IDYES - 1),
				Utils::Win32::MB_GetString(IDNO - 1),
				Utils::Win32::MB_GetString(IDCANCEL - 1)
			)) {
				case IDYES:
					loaderAction = XivAlexDll::LoaderAction::Load;
					break;
				case IDNO:
					loaderAction = XivAlexDll::LoaderAction::Unload;
					break;
				case IDCANCEL:
					loaderAction = XivAlexDll::LoaderAction::Count_;
			}
		} else {
			const auto regionAndVersion = XivAlex::ResolveGameReleaseRegion(path);
			const auto gameConfigFilename = std::format(L"game.{}.{}.json",
				std::get<0>(regionAndVersion),
				std::get<1>(regionAndVersion));
			const auto gameConfigPath = dllDir / gameConfigFilename;

			switch (Dll::MessageBoxF(nullptr, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1,
				IDS_CONFIRM_INJECT,
				pid, path, gameConfigPath)) {
				case IDYES:
					loaderAction = XivAlexDll::LoaderAction::Load;
					break;
				case IDNO:
					loaderAction = XivAlexDll::LoaderAction::Count_;
			}
		}
	}

	if (loaderAction == XivAlexDll::LoaderAction::Count_)
		return;

	if (loaderAction == XivAlexDll::LoaderAction::Unload && !rpModule)
		return;

	const auto injectedModule = Utils::Win32::InjectedModule(std::move(process), dllPath);
	auto unloadRequired = false;
	const auto cleanup = Utils::CallOnDestruction([&injectedModule, &unloadRequired]() {
		if (unloadRequired)
			injectedModule.Call("EnableXivAlexander", nullptr, "EnableXivAlexander(0)");
	});

	if (loaderAction == XivAlexDll::LoaderAction::Load) {
		unloadRequired = true;
		if (const auto loadResult = injectedModule.Call("EnableXivAlexander", reinterpret_cast<void*>(1), "EnableXivAlexander(1)"); loadResult != 0)
			throw std::runtime_error(std::format(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_ERROR_LOAD) + 1), loadResult));
		else
			unloadRequired = false;
	} else if (loaderAction == XivAlexDll::LoaderAction::Unload) {
		unloadRequired = true;
	}
}

static int RunProgramRetryAfterElevatingSelfAsNecessary(const std::filesystem::path& path, const std::wstring& args = L"") {
	if (Utils::Win32::RunProgram({
		.path = path,
		.dir = path.parent_path().c_str(),
		.args = args,
		.elevateMode = Utils::Win32::RunProgramParams::CancelIfRequired,
	}))
		return 0;
	return Utils::Win32::RunProgram({
			.dir = path.parent_path().c_str(),
			.args = Utils::FromUtf8(Utils::Win32::ReverseCommandLineToArgv({
				"--disable-runas",
				"-a", LoaderActionToString(XivAlexDll::LoaderAction::Launcher),
				"-l", "select",
				path.string()
			})) + (args.empty() ? L"" : L" " + args),
			.elevateMode = Utils::Win32::RunProgramParams::Force,
		})
		? 0
		: 1;
}

static int SelectAndRunLauncher() {
	if (!g_parameters->m_runProgram.empty()) {
		return RunProgramRetryAfterElevatingSelfAsNecessary(g_parameters->m_runProgram, g_parameters->m_runProgramArgs);
	}

	try {
		IFileOpenDialogPtr pDialog;
		DWORD dwFlags;
		static const COMDLG_FILTERSPEC fileTypes[] = {
			{L"FFXIV Boot Files (ffxivboot.exe; ffxivboot64.exe; ffxiv_boot.exe)", L"ffxivboot.exe; ffxivboot64.exe; ffxiv_boot.exe"},
			{L"Executable Files (*.exe)", L"*.exe"},
		};
		Utils::Win32::Error::ThrowIfFailed(pDialog.CreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER));
		Utils::Win32::Error::ThrowIfFailed(pDialog->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes));
		Utils::Win32::Error::ThrowIfFailed(pDialog->SetFileTypeIndex(0));
		Utils::Win32::Error::ThrowIfFailed(pDialog->SetDefaultExtension(L"exe"));
		Utils::Win32::Error::ThrowIfFailed(pDialog->SetTitle(FindStringResourceEx(Dll::Module(), IDS_TITLE_SELECT_BOOT) + 1));
		Utils::Win32::Error::ThrowIfFailed(pDialog->GetOptions(&dwFlags));
		Utils::Win32::Error::ThrowIfFailed(pDialog->SetOptions(dwFlags | FOS_FORCEFILESYSTEM));
		Utils::Win32::Error::ThrowIfFailed(pDialog->Show(nullptr), true);

		std::wstring fileName;
		{
			IShellItemPtr pResult;
			PWSTR pszFileName;
			Utils::Win32::Error::ThrowIfFailed(pDialog->GetResult(&pResult));
			Utils::Win32::Error::ThrowIfFailed(pResult->GetDisplayName(SIGDN_FILESYSPATH, &pszFileName));
			if (!pszFileName)
				throw std::runtime_error("DEBUG: The selected file does not have a filesystem path.");
			fileName = pszFileName;
			CoTaskMemFree(pszFileName);
		}

		return RunProgramRetryAfterElevatingSelfAsNecessary(fileName);

	} catch (const Utils::Win32::CancelledError&) {
		return 0;

	} catch (const std::exception& e) {
		Dll::MessageBoxF(nullptr, MB_ICONERROR, IDS_ERROR_UNEXPECTED, e.what() ? e.what() : "Unknown");
	}
	return 0;
}

static int RunLauncher() {
	try {
		XivAlexDll::EnableInjectOnCreateProcess(XivAlexDll::InjectOnCreateProcessAppFlags::Use | XivAlexDll::InjectOnCreateProcessAppFlags::InjectAll);

		const auto launchers = XivAlex::FindGameLaunchers();
		switch (g_parameters->m_launcherType) {
			case LauncherType::Auto: {
				if (launchers.empty() || !g_parameters->m_runProgram.empty())
					return SelectAndRunLauncher();
				else if (launchers.size() == 1)
					return RunProgramRetryAfterElevatingSelfAsNecessary(launchers.front().second.BootApp);
				else {
					for (const auto& it : launchers) {
						if (Dll::MessageBoxF(nullptr, MB_YESNO | MB_ICONQUESTION,
							IDS_CONFIRM_LAUNCH,
							argparse::details::repr(it.first), it.second.RootPath) == IDYES)
							RunProgramRetryAfterElevatingSelfAsNecessary(it.second.BootApp, L"");
					}
				}
				return 0;
			}

			case LauncherType::Select:
				return SelectAndRunLauncher();

			case LauncherType::International: {
				for (const auto& [region, info] : launchers)
					if (region == XivAlex::GameRegion::International)
						return RunProgramRetryAfterElevatingSelfAsNecessary(info.BootApp);
				throw std::out_of_range(nullptr);
			}

			case LauncherType::Korean: {
				for (const auto& [region, info] : launchers)
					if (region == XivAlex::GameRegion::Korean)
						return RunProgramRetryAfterElevatingSelfAsNecessary(info.BootApp);
				throw std::out_of_range(nullptr);
			}

			case LauncherType::Chinese: {
				for (const auto& [region, info] : launchers)
					if (region == XivAlex::GameRegion::Chinese)
						return RunProgramRetryAfterElevatingSelfAsNecessary(info.BootApp);
				throw std::out_of_range(nullptr);
			}
		}
	} catch (const std::out_of_range&) {
		if (g_parameters->m_action == XivAlexDll::LoaderAction::Interactive) {
			if (!g_parameters->m_quiet)
				SelectAndRunLauncher();

		} else if (!g_parameters->m_quiet) {
			Dll::MessageBoxF(nullptr, MB_OK | MB_ICONINFORMATION, IDS_ERROR_NOT_FOUND);
		}
		return -1;

	} catch (const std::exception& e) {
		if (!g_parameters->m_quiet)
			Dll::MessageBoxF(nullptr, MB_OK | MB_ICONERROR, IDS_ERROR_UNEXPECTED, e.what());
		return -1;
	}
	return 0;
}

static bool RequiresElevationForUpdate(std::vector<DWORD> excludedPid) {
	// check if other ffxiv processes else than excludedPid exists, and if they're inaccessible.
	for (const auto pid : GetTargetPidList()) {
		try {
			if (std::ranges::find(excludedPid, pid) == excludedPid.end())
				OpenProcessForInjection(pid);
		} catch (...) {
			return true;
		}
	}
	return false;
}

#pragma optimize("", off)
template<typename...Args>
Utils::CallOnDestruction ShowLazyProgress(bool explicitCancel, const UINT nFormatResId, Args ...args) {
	const auto format = FindStringResourceEx(Dll::Module(), nFormatResId) + 1;
	auto cont = std::make_shared<bool>(true);
	auto t = Utils::Win32::Thread(L"UpdateStatus", [explicitCancel, msg = std::format(format, std::forward<Args>(args)...), cont]() {
		while (*cont) {
			if (Dll::MessageBoxF(nullptr, (explicitCancel ? MB_OKCANCEL : MB_OK) | MB_ICONINFORMATION, msg) == (explicitCancel ? IDCANCEL : IDOK)) {
				if (*cont)
					Utils::Win32::Process::Current().Terminate(0);
				else
					return;
			}
		}
	});
	return {
		[t = std::move(t), cont = std::move(cont)]() {
			*cont = false;
			const auto tid = t.GetId();
			while (t.Wait(100) == WAIT_TIMEOUT) {
				HWND hwnd = nullptr;
				while ((hwnd = FindWindowExW(nullptr, hwnd, nullptr, nullptr))) {
					const auto hwndThreadId = GetWindowThreadProcessId(hwnd, nullptr);
					if (tid == hwndThreadId) {
						SendMessageW(hwnd, WM_CLOSE, 0, 0);
					}
				}
			}
		}
	};
}
#pragma optimize("", on)

static void PerformUpdateAndExitIfSuccessful(std::vector<Utils::Win32::Process> gameProcesses, const std::string& url, const std::filesystem::path& updateZip) {
	std::vector<DWORD> prevProcessIds;
	std::ranges::transform(gameProcesses, std::back_inserter(prevProcessIds), [](const Utils::Win32::Process& k) { return k.GetId(); });
	const auto& currentProcess = Utils::Win32::Process::Current();
	const auto launcherDir = currentProcess.PathOf().parent_path();
	const auto tempExtractionDir = launcherDir / L"__UPDATE__";
	const auto loaderPath32 = launcherDir / XivAlex::XivAlexLoader32NameW;
	const auto loaderPath64 = launcherDir / XivAlex::XivAlexLoader64NameW;

	{
		const auto progress = ShowLazyProgress(true, IDS_UPDATE_PROGRESS_DOWNLOADING_FILES);

		try {
			for (int i = 0; i < 2; ++i) {
				try {
					if (g_parameters->m_debugUpdate) {
						const auto tempPath = launcherDir / L"updatesourcetest.zip";
						libzippp::ZipArchive arc(tempPath.string());
						arc.open(libzippp::ZipArchive::Write);
						arc.addFile(Utils::ToUtf8(XivAlex::XivAlexDll32NameW), Utils::ToUtf8((launcherDir / XivAlex::XivAlexDll32NameW).wstring()));
						arc.addFile(Utils::ToUtf8(XivAlex::XivAlexDll64NameW), Utils::ToUtf8((launcherDir / XivAlex::XivAlexDll64NameW).wstring()));
						arc.addFile(Utils::ToUtf8(XivAlex::XivAlexLoader32NameW), Utils::ToUtf8((launcherDir / XivAlex::XivAlexLoader32NameW).wstring()));
						arc.addFile(Utils::ToUtf8(XivAlex::XivAlexLoader64NameW), Utils::ToUtf8((launcherDir / XivAlex::XivAlexLoader64NameW).wstring()));
						arc.close();
						copy(tempPath, updateZip);
					} else {
						if (!exists(updateZip) || i == 1) {
							std::ofstream f(updateZip, std::ios::binary);
							curlpp::Easy req;
							req.setOpt(curlpp::options::Url(url));
							req.setOpt(curlpp::options::UserAgent("Mozilla/5.0"));
							req.setOpt(curlpp::options::FollowLocation(true));
							f << req;
						}
					}

					remove_all(tempExtractionDir);

					const auto hFile = Utils::Win32::Handle(CreateFileW(updateZip.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr),
						INVALID_HANDLE_VALUE, "CreateFileW({}, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)", updateZip);
					LARGE_INTEGER fileSize;
					if (!GetFileSizeEx(hFile, &fileSize))
						throw Utils::Win32::Error("GetFileSizeEx");
					if (fileSize.QuadPart > 128 * 1024 * 1024)
						throw Utils::Win32::Error(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_UPDATE_FILE_TOO_BIG) + 1));
					const auto hMapFile = Utils::Win32::Handle(CreateFileMappingW(hFile, nullptr, PAGE_READONLY, fileSize.HighPart, fileSize.LowPart, nullptr), nullptr, "CreateFileMappingW");
					const auto pMapped = Utils::Win32::Closeable<void*, UnmapViewOfFile>(MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, fileSize.LowPart));

					const auto pArc = libzippp::ZipArchive::fromBuffer(pMapped, fileSize.LowPart);
					const auto freeArc = Utils::CallOnDestruction([pArc]() {
						pArc->close();
						delete pArc;
					});
					for (const auto& entry : pArc->getEntries()) {
						const auto pData = static_cast<char*>(entry.readAsBinary());
						const auto freeData = Utils::CallOnDestruction([pData]() { delete[] pData; });
						const auto targetPath = launcherDir / tempExtractionDir / entry.getName();
						create_directories(targetPath.parent_path());
						std::ofstream f(targetPath, std::ios::binary);
						f.write(pData, static_cast<std::streamsize>(entry.getSize()));
					}
					break;
				} catch (...) {
					if (url.empty())
						return;

					if (i == 1)
						throw;
				}
			}
		} catch (const std::exception&) {
			remove_all(tempExtractionDir);
			remove(updateZip);
			throw;
		}
	}
	// TODO: ask user to quit all FFXIV related processes, except the game itself.

	if (RequiresElevationForUpdate(prevProcessIds)) {
		Utils::Win32::RunProgram({
			.args = std::format(L"--action {}", LoaderActionToString(XivAlexDll::LoaderAction::UpdateCheck)),
			.elevateMode = Utils::Win32::RunProgramParams::Force,
		});
		return;
	}

	{
		const auto progress = ShowLazyProgress(true, IDS_UPDATE_PROGRESS_PREPARING_FILES);

		std::vector unloadTargets = gameProcesses;
		for (const auto pid : Utils::Win32::GetProcessList()) {
			if (pid == GetCurrentProcessId() || std::ranges::find(prevProcessIds, pid) != prevProcessIds.end())
				continue;

			std::filesystem::path processPath;

			try {
				processPath = Utils::Win32::Process(PROCESS_QUERY_LIMITED_INFORMATION, false, pid).PathOf();
			} catch (const std::exception&) {
				try {
					processPath = Utils::Win32::Process(PROCESS_QUERY_INFORMATION, false, pid).PathOf();
				} catch (const std::exception&) {
					// ¯\_(ツ)_/¯
					// unlikely that we can't access information of process that has anything to do with xiv,
					// unless it's antivirus, in which case we can't do anything, which will result in failure anyway.
					continue;
				}
			}
			if (lstrcmpiW(processPath.c_str(), loaderPath32.c_str()) == 0 || lstrcmpiW(processPath.c_str(), loaderPath64.c_str()) == 0) {
				while (true) {
					try {
						const auto hProcess = Utils::Win32::Process(PROCESS_TERMINATE | SYNCHRONIZE, false, pid);
						if (hProcess.Wait(0) != WAIT_TIMEOUT)
							break;
						hProcess.Terminate(0);
					} catch (const Utils::Win32::Error& e) {
						if (e.Code() == ERROR_INVALID_PARAMETER)  // this process already gone
							break;
						if (Dll::MessageBoxF(nullptr, MB_OKCANCEL, IDS_UPDATE_PROCESS_KILL_FAILURE, pid, e.what()) == IDCANCEL)
							return;
					}
				}

			} else if (lstrcmpiW(processPath.filename().c_str(), XivAlex::GameExecutable64NameW) == 0 || lstrcmpiW(processPath.filename().c_str(), XivAlex::GameExecutable32NameW) == 0) {
				auto process = OpenProcessForInjection(pid);
				gameProcesses.emplace_back(process);
				unloadTargets.emplace_back(std::move(process));
			} else {
				try {
					auto process = OpenProcessForInjection(pid);
					const auto is64 = process.IsProcess64Bits();
					const auto modulePath = launcherDir / (is64 ? XivAlex::XivAlexDll64NameW : XivAlex::XivAlexDll32NameW);
					if (process.AddressOf(modulePath, Utils::Win32::Process::ModuleNameCompareMode::FullPath, false))
						unloadTargets.emplace_back(std::move(process));
				} catch (...) {
				}
			}
		}

		LaunchXivAlexLoaderWithTargetHandles(unloadTargets, XivAlexDll::LoaderAction::Internal_Cleanup_Handle, true);
		LaunchXivAlexLoaderWithTargetHandles(gameProcesses, XivAlexDll::LoaderAction::Internal_Update_Step2_ReplaceFiles, false, currentProcess, XivAlexDll::Current, tempExtractionDir);

		currentProcess.Terminate(0);
	}
}

static void CheckForUpdates(std::vector<Utils::Win32::Process> prevProcesses, bool offerAutomaticUpdate) {
	const auto updateZip = Utils::Win32::Process::Current().PathOf().parent_path() / "update.zip";
	if (exists(updateZip))
		PerformUpdateAndExitIfSuccessful(prevProcesses, "", updateZip);

	try {
		std::vector<int> remote, local;
		XivAlex::VersionInformation up;
		if (g_parameters->m_debugUpdate) {
			local = {0, 0, 0, 0};
			remote = {0, 0, 0, 1};
		} else {
			const auto checking = ShowLazyProgress(true, IDS_UPDATE_PROGRESS_CHECKING);
			const auto [selfFileVersion, selfProductVersion] = Utils::Win32::FormatModuleVersionString(GetModuleHandleW(nullptr));
			up = XivAlex::CheckUpdates();
			const auto remoteS = Utils::StringSplit<std::string>(up.Name.substr(1), ".");
			const auto localS = Utils::StringSplit<std::string>(selfProductVersion, ".");
			for (const auto& s : remoteS)
				remote.emplace_back(std::stoi(s));
			for (const auto& s : localS)
				local.emplace_back(std::stoi(s));
		}
		if (local.size() != 4 || remote.size() != 4)
			throw std::runtime_error(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_UPDATE_FILE_TOO_BIG) + 1));
		if (local >= remote) {
			Dll::MessageBoxF(nullptr, MB_OK | MB_ICONINFORMATION,
				IDS_UPDATE_UNAVAILABLE,
				local[0], local[1], local[2], local[3], remote[0], remote[1], remote[2], remote[3], up.PublishDate);
			return;
		}
		if (offerAutomaticUpdate) {
			while (true) {
				switch (Dll::MessageBoxF(nullptr, MB_YESNOCANCEL,
					IDS_UPDATE_CONFIRM,
					remote[0], remote[1], remote[2], remote[3], up.PublishDate, local[0], local[1], local[2], local[3],
					Utils::Win32::MB_GetString(IDYES - 1),
					Utils::Win32::MB_GetString(IDNO - 1),
					Utils::Win32::MB_GetString(IDCANCEL - 1)
				)) {
					case IDYES:
						ShellExecuteW(nullptr, L"open", FindStringResourceEx(Dll::Module(), IDS_URL_RELEASES) + 1, nullptr, nullptr, SW_SHOW);
						break;

					case IDNO:
						PerformUpdateAndExitIfSuccessful(std::move(prevProcesses), up.DownloadLink, updateZip);
						return;

					case IDCANCEL:
						return;
				}
			}
		} else {
			// TODO: create string resource: New update is available, cannot autoupdate because being called as dll, etc etc.
			ShellExecuteW(nullptr, L"open", FindStringResourceEx(Dll::Module(), IDS_URL_RELEASES) + 1, nullptr, nullptr, SW_SHOW);
		}
	} catch (const std::exception& e) {
		Dll::MessageBoxF(nullptr, MB_OK | MB_ICONERROR, IDS_ERROR_UNEXPECTED, e.what());
	}
}

static void EnsureBestBits() {
	BOOL w = FALSE;
	if (!IsWow64Process(GetCurrentProcess(), &w) || !w)
		return;
	ExitProcess(Utils::Win32::RunProgram({
		.path = Utils::Win32::Process::Current().PathOf().parent_path() / XivAlex::XivAlexLoader64NameW,
		.args = Utils::Win32::GetCommandLineWithoutProgramName(Dll::GetOriginalCommandLine()),
		.wait = true
	}).WaitAndGetExitCode());
}

static void Install(const std::filesystem::path& gamePath) {
	EnsureBestBits();

	const auto cdir = Utils::Win32::Process::Current().PathOf().parent_path();
	const auto dataPath = Utils::Win32::EnsureKnownFolderPath(FOLDERID_RoamingAppData) / L"XivAlexander";
	const auto exePath = Utils::Win32::EnsureKnownFolderPath(FOLDERID_LocalAppData) / L"XivAlexander";
	create_directories(dataPath);
	create_directories(exePath);

	const auto configPath = dataPath / "config.runtime.json";
	auto config64 = App::Config::RuntimeRepository(nullptr, {}, Utils::ToUtf8((gamePath / "ffxiv_dx11.exe").wstring()));
	auto config32 = App::Config::RuntimeRepository(nullptr, {}, Utils::ToUtf8((gamePath / "ffxiv.exe").wstring()));
	config64.Reload(configPath);
	config32.Reload(configPath);

	auto success = false;
	Utils::CallOnDestruction::Multiple revert;

	const auto d3d11 = gamePath / "d3d11.dll";
	const auto d3d9 = gamePath / "d3d9.dll";
	const auto dxgi = gamePath / "dxgi.dll";
	const auto dinput8 = gamePath / "dinput8.dll";
	// * Don't use dxgi for now, as d3d11 overriders may choose to call system DLL directly,
	//   and XIVQuickLauncher warns something about broken GShade install.
	// * dinput8.dll is for manual troubleshooting for now.

	for (const auto& f : {d3d9, d3d11, dxgi, dinput8}) {
		if (!exists(f) || !XivAlex::IsXivAlexanderDll(f))
			continue;

		std::filesystem::path temp;
		for (size_t i = 0; ; i++)
			if (!exists(temp = std::filesystem::path(f).replace_filename(std::format(L"_temp.{}.dll", i))))
				break;
		rename(f, temp);
		revert += [f, temp, &success]() {
			if (success)
				remove(temp);
			else
				rename(temp, f);
		};
	}

	if (exists(d3d9)) {
		std::filesystem::path fn;
		for (size_t i = 0; ; i++) {
			fn = (d3d9.parent_path() / std::format("xivalex.chaindll.{}", i)) / "d3d9.dll";
			if (!exists(fn))
				break;
		}
		create_directories(fn.parent_path());
		rename(d3d9, fn);
		revert += [d3d9, fn, &success]() { if (!success) rename(fn, d3d9); };
		auto list = config32.ChainLoadPath_d3d9.Value();
		list.emplace_back(std::move(fn));
		config32.ChainLoadPath_d3d9 = list;
	}
	if (exists(d3d11)) {
		std::filesystem::path fn;
		for (size_t i = 0; ; i++) {
			fn = (d3d11.parent_path() / std::format("xivalex.chaindll.{}", i)) / "d3d11.dll";
			if (!exists(fn))
				break;
		}
		create_directories(fn.parent_path());
		rename(d3d11, fn);
		revert += [d3d11, fn, &success]() { if (!success) rename(fn, d3d11); };
		auto list = config64.ChainLoadPath_d3d11.Value();
		list.emplace_back(std::move(fn));
		config64.ChainLoadPath_d3d11 = list;
	}

	remove(gamePath / "config.xivalexinit.json");
	copy_file(cdir / XivAlex::XivAlexDll64NameW, d3d11);
	revert += [d3d11, &success]() { if (!success) remove(d3d11); };
	copy_file(cdir / XivAlex::XivAlexDll32NameW, d3d9);
	revert += [d3d9, &success]() { if (!success) remove(d3d9); };

	for (const auto& f : std::filesystem::directory_iterator(cdir)) {
		if (f.is_directory())
			continue;

		if (f.path().filename().wstring().starts_with(L"game.")
			&& f.path().filename().wstring().ends_with(L".json"))
			copy_file(f, dataPath / f.path().filename(), std::filesystem::copy_options::overwrite_existing);

		if (f.path().filename().wstring().ends_with(L".exe")
			|| f.path().filename().wstring().ends_with(L".dll"))
			copy_file(f, exePath / f.path().filename(), std::filesystem::copy_options::overwrite_existing);
	}

	config64.Save(configPath);
	config32.Save(configPath);

	success = true;
	revert.Clear();

	Dll::MessageBoxF(nullptr, MB_OK, L"Installed");
}

static void Uninstall(const std::filesystem::path& gamePath) {
	EnsureBestBits();

	const auto cdir = Utils::Win32::Process::Current().PathOf().parent_path();
	const auto dataPath = Utils::Win32::EnsureKnownFolderPath(FOLDERID_RoamingAppData) / L"XivAlexander";
	const auto exePath = Utils::Win32::EnsureKnownFolderPath(FOLDERID_LocalAppData) / L"XivAlexander";

	const auto configPath = dataPath / "config.runtime.json";
	auto config64 = App::Config::RuntimeRepository(nullptr, {}, Utils::ToUtf8((gamePath / "ffxiv_dx11.exe").wstring()));
	auto config32 = App::Config::RuntimeRepository(nullptr, {}, Utils::ToUtf8((gamePath / "ffxiv.exe").wstring()));
	config64.Reload(configPath);
	config32.Reload(configPath);

	auto success = false;
	Utils::CallOnDestruction::Multiple revert;

	const auto d3d11 = gamePath / "d3d11.dll";
	const auto d3d9 = gamePath / "d3d9.dll";
	const auto dxgi = gamePath / "dxgi.dll";
	const auto dinput8 = gamePath / "dinput8.dll";

	for (const auto& f : {d3d9, d3d11, dxgi, dinput8}) {
		if (!exists(f) || !XivAlex::IsXivAlexanderDll(f))
			continue;

		std::filesystem::path temp;
		for (size_t i = 0; ; i++)
			if (!exists(temp = std::filesystem::path(f).replace_filename(std::format(L"_temp.{}.dll", i))))
				break;
		rename(f, temp);
		revert += [f, temp, &success]() {
			if (success)
				remove(temp);
			else
				rename(temp, f);
		};
	}

	if (!exists(d3d9) && config32.ChainLoadPath_d3d9.Value().size() == 1) {
		std::filesystem::path fn = config32.ChainLoadPath_d3d9.Value().back();
		rename(fn, d3d9);
		revert += [d3d9, fn, &success]() { if (!success) rename(d3d9, fn); };
		config32.ChainLoadPath_d3d9 = std::vector<std::filesystem::path>();
	}
	if (!exists(d3d11) && config64.ChainLoadPath_d3d11.Value().size() == 1) {
		std::filesystem::path fn = config64.ChainLoadPath_d3d11.Value().back();
		rename(fn, d3d11);
		revert += [d3d11, fn, &success]() { if (!success) rename(d3d11, fn); };
		config64.ChainLoadPath_d3d11 = std::vector<std::filesystem::path>();
	}

	for (const auto& item : std::filesystem::directory_iterator(gamePath)) {
		try {
			if (item.is_directory() && item.path().filename().wstring().starts_with(L"xivalex.chaindll.")) {
				auto anyFile = false;
				for (const auto& _ : std::filesystem::directory_iterator(item)) {
					anyFile = true;
					break;
				}
				if (!anyFile)
					remove(item);
			}
		} catch (...) {
			// pass
		}
	}

	config64.Save(configPath);
	config32.Save(configPath);

	success = true;
	revert.Clear();

	if (Dll::MessageBoxF(nullptr, MB_YESNO | MB_ICONQUESTION, L"Uninstalled; to completely install, remove the configuration files. Do you want to open the configuration folder?") == IDYES) {
		SHELLEXECUTEINFOW shex{
			.cbSize = sizeof shex,
			.hwnd = nullptr,
			.lpFile = dataPath.c_str(),
			.nShow = SW_SHOW,
		};
		if (!ShellExecuteExW(&shex))
			throw Utils::Win32::Error("ShellExecuteW");
	}
}

int __stdcall XivAlexDll::XA_LoaderApp(LPWSTR lpCmdLine) {
	SetEnvironmentVariableW(L"XIVALEXANDER_DISABLE", nullptr);

	if (!SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
		std::abort();

	try {
		g_parameters = std::make_unique<XivAlexanderLoaderParameter>();
		g_parameters->Parse();
	} catch (const std::exception& err) {
		Dll::MessageBoxF(nullptr, MB_ICONWARNING, IDS_ERROR_COMMAND_LINE, err.what(), g_parameters->GetHelpMessage());
		return -1;
	}
	if (g_parameters->m_help) {
		Dll::MessageBoxF(nullptr, MB_OK, g_parameters->GetHelpMessage().c_str());
		return 0;
	}
	if (g_parameters->m_action == LoaderAction::Web) {
		ShellExecuteW(nullptr, L"open", FindStringResourceEx(Dll::Module(), IDS_URL_MAIN) + 1, nullptr, nullptr, SW_SHOW);
		return 0;
	}

	std::string debugPrivilegeError = "OK.";
	try {
		Utils::Win32::AddDebugPrivilege();
	} catch (const std::exception& err) {
		debugPrivilegeError = Utils::ToUtf8(std::format(FindStringResourceEx(Dll::Module(), IDS_ERROR_SEDEBUGPRIVILEGE) + 1, err.what()));
	}

	const auto& currentProcess = Utils::Win32::Process::Current();
	const auto dllDir = currentProcess.PathOf().parent_path();
	const auto dllPath = dllDir / XivAlex::XivAlexDllNameW;

	try {
		switch (CheckPackageVersion()) {
			case CheckPackageVersionResult::OK:
				break;

			case CheckPackageVersionResult::MissingFiles:
				throw std::runtime_error(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_ERROR_MISSING_FILES) + 1));

			case CheckPackageVersionResult::VersionMismatch:
				throw std::runtime_error(Utils::ToUtf8(FindStringResourceEx(Dll::Module(), IDS_ERROR_INCONSISTENT_FILES) + 1));
		}
	} catch (const std::exception& e) {
		if (Dll::MessageBoxF(nullptr, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON1, IDS_ERROR_COMPONENTS, e.what()) == IDYES) {
			ShellExecuteW(nullptr, L"open", FindStringResourceEx(Dll::Module(), IDS_URL_RELEASES) + 1, nullptr, nullptr, SW_SHOW);
		}
		return -1;
	}

	try {
#ifdef _DEBUG
		Dll::MessageBoxF(nullptr, MB_OK, L"Action: {}", argparse::details::repr(g_parameters->m_action));
#endif
		switch (g_parameters->m_action) {
			case LoaderAction::Install: {
				Install(g_parameters->m_runProgram);
				return 0;
			}

			case LoaderAction::Uninstall: {
				Uninstall(g_parameters->m_runProgram);
				return 0;
			}

			case LoaderAction::Internal_Inject_HookEntryPoint: {
				std::vector<Utils::Win32::Process> mine, defer;
				for (auto& process : g_parameters->m_targetProcessHandles) {
					if (process.IsProcess64Bits() == (INT64_MAX == INTPTR_MAX))
						mine.emplace_back(std::move(process));
					else
						defer.emplace_back(std::move(process));
				}
				for (auto& process : mine)
					PatchEntryPointForInjection(process);

				if (!defer.empty())
					LaunchXivAlexLoaderWithTargetHandles(defer, g_parameters->m_action, true, {}, Opposite);
				return 0;
			}

			case LoaderAction::Internal_Inject_LoadXivAlexanderImmediately:
			case LoaderAction::Internal_Cleanup_Handle: {
				std::vector<Utils::Win32::Process> mine, defer;
				for (auto& process : g_parameters->m_targetProcessHandles) {
					if (process.IsProcess64Bits() == (INT64_MAX == INTPTR_MAX))
						mine.emplace_back(std::move(process));
					else
						defer.emplace_back(std::move(process));
				}
				for (auto& process : mine) {
					try {
						const auto m = Utils::Win32::InjectedModule(std::move(process), dllPath);
						if (g_parameters->m_action == LoaderAction::Internal_Inject_LoadXivAlexanderImmediately)
							m.Call("EnableXivAlexander", reinterpret_cast<void*>(1), "EnableXivAlexander(1)");
						else
							m.Call("DisableAllApps", nullptr, "DisableAllApps(0)");
					} catch (...) {
						// pass
					}
				}

				if (!defer.empty()) {
					LaunchXivAlexLoaderWithTargetHandles(defer, g_parameters->m_action, true, {}, Opposite);
				}
				return 0;
			}

			case LoaderAction::UpdateCheck:
			case LoaderAction::Internal_Update_DependencyDllMode: {
				if (!Utils::Win32::Process::Current().IsProcess64Bits()) {
					SYSTEM_INFO si;
					GetNativeSystemInfo(&si);
					if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
						std::vector<HANDLE> handles;
						LaunchXivAlexLoaderWithTargetHandles(g_parameters->m_targetProcessHandles, g_parameters->m_action, false, {}, Force64);
						return 0;
					}
				}

				CheckForUpdates(std::move(g_parameters->m_targetProcessHandles),
					g_parameters->m_action == LoaderAction::UpdateCheck);
				return 0;
			}

			case LoaderAction::Internal_Update_Step2_ReplaceFiles: {
				const auto checking = ShowLazyProgress(true, IDS_UPDATE_PROGRESS_UPDATING_FILES);
				const auto temporaryUpdatePath = Utils::Win32::Process::Current().PathOf().parent_path();
				const auto targetUpdatePath = temporaryUpdatePath.parent_path();
				if (temporaryUpdatePath.filename() != L"__UPDATE__")
					throw std::runtime_error("cannot update outside of update process");
				copy(
					temporaryUpdatePath,
					targetUpdatePath,
					std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing
				);
				LaunchXivAlexLoaderWithTargetHandles(g_parameters->m_targetProcessHandles, LoaderAction::Internal_Update_Step3_CleanupFiles, false, currentProcess, Current, targetUpdatePath);
				return 0;
			}

			case LoaderAction::Internal_Update_Step3_CleanupFiles: {
				{
					const auto checking = ShowLazyProgress(true, IDS_UPDATE_CLEANUP_UPDATE);
					remove_all(Utils::Win32::Process::Current().PathOf().parent_path() / L"__UPDATE__");
					remove(Utils::Win32::Process::Current().PathOf().parent_path() / L"update.zip");
				}

				if (g_parameters->m_targetProcessHandles.empty()) {
					g_parameters->m_action = LoaderAction::Interactive;
					break;

				} else {
					const auto checking = ShowLazyProgress(true, IDS_UPDATE_RELOADING_XIVALEXANDER);
					auto pids = GetTargetPidList();
					for (const auto& process : g_parameters->m_targetProcessHandles)
						pids.erase(process.GetId());

					if (!g_parameters->m_targetProcessHandles.empty())
						LaunchXivAlexLoaderWithTargetHandles(g_parameters->m_targetProcessHandles, LoaderAction::Internal_Inject_LoadXivAlexanderImmediately, true);

					if (!pids.empty()) {
						for (const auto& pid : pids) {
							g_parameters->m_action = LoaderAction::Load;
							g_parameters->m_quiet = true;
							try {
								DoPidTask(pid, dllDir, dllPath);
							} catch (...) {
							}
						}
					}

					{
						const auto checking = ShowLazyProgress(false, IDS_UPDATE_COMPLETE);
						Sleep(3000);
					}
					return 0;
				}
			}
		}
	} catch (const std::exception& e) {
		Dll::MessageBoxF(nullptr, MB_OK | MB_ICONERROR, IDS_ERROR_UNEXPECTED,
			e.what());
		return -1;
	}

	if (g_parameters->m_action == LoaderAction::Launcher)
		return RunLauncher();

	auto pids = GetTargetPidList();

	if (!g_parameters->m_disableAutoRunAs && !Utils::Win32::IsUserAnAdmin() && RequiresAdminAccess(pids)) {
		try {
			if (Utils::Win32::RunProgram({
				.args = std::format(L"--disable-runas {}", lpCmdLine),
				.wait = true,
				.elevateMode = Utils::Win32::RunProgramParams::Force,
			}))
				return 0;
		} catch (const std::exception&) {
			// pass
		}
	}

	if (g_parameters->m_action == LoaderAction::Interactive) {
		enum class Mode {
			Install,
			RunOnce,
			Break,
		} mode = Mode::Install;
		while (mode != Mode::Break) {
			pids = GetTargetPidList();

			if (pids.empty())
				mode = Mode::Install;

			std::filesystem::path gamePath, bootPath;
			auto bootInjectable = false;
			bool needChooser = true;
			const auto installationChooser = [&gamePath, &bootPath, &bootInjectable](HWND hwnd) {
				IFileOpenDialogPtr pDialog;
				DWORD dwFlags;
				static const COMDLG_FILTERSPEC fileTypes[] = {
					{L"FFXIV executable files (ffxivboot.exe; ffxivboot64.exe; ffxiv_boot.exe; ffxiv_dx11.exe; ffxiv.exe)", L"ffxivboot.exe; ffxivboot64.exe; ffxiv_boot.exe; ffxiv_dx11.exe; ffxiv.exe"},
					{L"Executable Files (*.exe)", L"*.exe"},
					{L"All files (*.*)", L"*"},
				};
				Utils::Win32::Error::ThrowIfFailed(pDialog.CreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER));
				Utils::Win32::Error::ThrowIfFailed(pDialog->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes));
				Utils::Win32::Error::ThrowIfFailed(pDialog->SetFileTypeIndex(0));
				Utils::Win32::Error::ThrowIfFailed(pDialog->SetDefaultExtension(L"exe"));
				Utils::Win32::Error::ThrowIfFailed(pDialog->SetTitle(FindStringResourceEx(Dll::Module(), IDS_TITLE_SELECT_BOOT) + 1));
				Utils::Win32::Error::ThrowIfFailed(pDialog->GetOptions(&dwFlags));
				Utils::Win32::Error::ThrowIfFailed(pDialog->SetOptions(dwFlags | FOS_FORCEFILESYSTEM));
				try {
					Utils::Win32::Error::ThrowIfFailed(pDialog->Show(nullptr), true);
				} catch (const Utils::Win32::CancelledError&) {
					return false;
				}

				std::wstring fileName;
				{
					IShellItemPtr pResult;
					PWSTR pszFileName;
					Utils::Win32::Error::ThrowIfFailed(pDialog->GetResult(&pResult));
					Utils::Win32::Error::ThrowIfFailed(pResult->GetDisplayName(SIGDN_FILESYSPATH, &pszFileName));
					if (!pszFileName)
						throw std::runtime_error("DEBUG: The selected file does not have a filesystem path.");
					fileName = pszFileName;
					CoTaskMemFree(pszFileName);
				}

				gamePath = fileName;
				bootInjectable = true;
				if (lstrcmpiW(gamePath.filename().wstring().c_str(), XivAlex::GameExecutable32NameW) == 0
					|| lstrcmpiW(gamePath.filename().wstring().c_str(), XivAlex::GameExecutable64NameW) == 0) {
					gamePath = gamePath.parent_path();
					if (!exists(bootPath = gamePath.parent_path() / "FFXIVBoot.exe"))
						if (!exists(bootPath = gamePath.parent_path() / "boot" / "ffxivboot.exe"))
							if (!exists(bootPath = gamePath.parent_path() / "boot" / "ffxiv_boot.exe"))
								bootPath.clear();
				} else {
					bootPath = gamePath;
					if (!exists((gamePath = bootPath / "game") / "ffxivgame.ver"))
						if (!exists((gamePath = bootPath.parent_path() / "game") / "ffxivgame.ver"))
							if (!exists((gamePath = bootPath.parent_path().parent_path() / "game") / "ffxivgame.ver"))
								gamePath.clear();
				}
				return true;
			};

			auto selectedPid = pids.empty() ? 0 : *pids.begin();

			const auto loadOrUnload = [&selectedPid, &dllPath](bool load) {
				Utils::Win32::Process process;
				try {
					process = Utils::Win32::Process(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, selectedPid);
				} catch (...) {
					process = Utils::Win32::Process(PROCESS_QUERY_INFORMATION, FALSE, selectedPid);
				}
				if (process.IsProcess64Bits() != Utils::Win32::Process::Current().IsProcess64Bits()
					|| (!Utils::Win32::IsUserAnAdmin() && RequiresAdminAccess({process.GetId()}))) {
					Utils::Win32::RunProgram({
						.path = Utils::Win32::Process::Current().PathOf().parent_path() / (process.IsProcess64Bits() ? XivAlex::XivAlexLoader64NameW : XivAlex::XivAlexLoader32NameW),
						.args = std::format(L"-a {} {}",
							LoaderActionToString(load ? LoaderAction::Load : LoaderAction::Unload),
							process.GetId()),
						.wait = true
					});
				} else {
					process = OpenProcessForInjection(selectedPid);
					const auto injectedModule = Utils::Win32::InjectedModule(std::move(process), dllPath);
					auto unloadRequired = false;
					const auto cleanup = Utils::CallOnDestruction([&injectedModule, &unloadRequired]() {
						if (unloadRequired)
							injectedModule.Call("EnableXivAlexander", nullptr, "EnableXivAlexander(0)");
					});

					if (load) {
						unloadRequired = true;
						if (const auto loadResult = injectedModule.Call("EnableXivAlexander", reinterpret_cast<void*>(1), "EnableXivAlexander(1)"); loadResult == 0)
							unloadRequired = false;
					} else
						unloadRequired = true;
				}
			};

			auto builder = Utils::Win32::TaskDialog::Builder();
			if (mode == Mode::Install) {
				builder
					.WithMainInstruction(L"Install XivAlexander")
					.WithContent(L"You can install XivAlexander to launch XivAlexander with the game.\n\n* Requires Administrator permissions.\n* Compatible with Reshade.\n* No game data files are touched.");
				builder.WithButton({
					.Text = L"&Install XivAlexander\nInstall XivAlexander for the selected game installation.",
					.Callback = [&](auto& dialog) {
						try {
							auto withHider = dialog.WithHiddenDialog();
							if (needChooser && !installationChooser(dialog.GetHwnd()))
								return Utils::Win32::TaskDialog::Handled;

							Utils::Win32::RunProgram({
								.args = std::format(L"-a {} {}",
									LoaderActionToString(LoaderAction::Install),
									Utils::Win32::ReverseCommandLineToArgv(gamePath.wstring())),
								.wait = true,
								.elevateMode = Utils::Win32::RunProgramParams::Force,
							});
							withHider.Cancel();
							return Utils::Win32::TaskDialog::NotHandled;
						} catch (const std::exception& e) {
							Dll::MessageBoxF(nullptr, MB_ICONERROR, IDS_ERROR_UNEXPECTED, e.what());
						}
						return Utils::Win32::TaskDialog::Handled;
					},
				});
				builder.WithButton({
					.Text = L"&Uninstall XivAlexander\nUninstall XivAlexander from the selected game installation.",
					.Callback = [&](auto& dialog) {
						try {
							auto withHider = dialog.WithHiddenDialog();
							if (needChooser && !installationChooser(dialog.GetHwnd()))
								return Utils::Win32::TaskDialog::Handled;

							Utils::Win32::RunProgram({
								.args = std::format(L"-a {} {}",
									LoaderActionToString(LoaderAction::Uninstall),
									Utils::Win32::ReverseCommandLineToArgv(gamePath.wstring())),
								.wait = true,
								.elevateMode = Utils::Win32::RunProgramParams::Force,
							});
							withHider.Cancel();
							return Utils::Win32::TaskDialog::NotHandled;
						} catch (const std::exception& e) {
							Dll::MessageBoxF(nullptr, MB_ICONERROR, IDS_ERROR_UNEXPECTED, e.what());
						}
						return Utils::Win32::TaskDialog::Handled;
					},
				});
				builder.WithButton({
					.Text = L"&Launch Game\nRun game launcher and load XivAlexander when game is run.",
					.Callback = [&](auto& dialog) {
						try {
							const auto withHider = dialog.WithHiddenDialog();
							if (needChooser && !installationChooser(dialog.GetHwnd()))
								return Utils::Win32::TaskDialog::Handled;

							if (bootPath.empty())
								throw std::runtime_error("Unable to detect boot path");

							if (bootInjectable || needChooser) {
								const auto isIntl = (lstrcmpiW(bootPath.filename().c_str(), L"ffxivboot.exe") == 0 || lstrcmpiW(bootPath.filename().c_str(), L"ffxivboot64.exe") == 0)
									&& lstrcmpiW(bootPath.parent_path().filename().wstring().c_str(), L"boot") == 0;
								Utils::Win32::RunProgram({
									.args = std::format(L"-a {} -l select {}",
										LoaderActionToString(LoaderAction::Launcher),
										Utils::Win32::ReverseCommandLineToArgv(bootPath.wstring())),
									.wait = true,
									.elevateMode = isIntl ? Utils::Win32::RunProgramParams::NeverUnlessShellIsElevated : Utils::Win32::RunProgramParams::NoElevationIfDenied,
								});
							} else {
								SHELLEXECUTEINFOW shex{
									.cbSize = sizeof shex,
									.hwnd = dialog.GetHwnd(),
									.lpFile = bootPath.c_str(),
									.nShow = SW_SHOW,
								};
								if (!ShellExecuteExW(&shex))
									throw Utils::Win32::Error("ShellExecuteExW");
								Dll::MessageBoxF(nullptr, MB_ICONWARNING, L"Cannot launch game with XivAlexander enabled. You will have to install XivAlexander, or load XivAlexander after you have started the game and pressed the Refresh link below.");
							}
						} catch (const std::exception& e) {
							Dll::MessageBoxF(nullptr, MB_ICONERROR, IDS_ERROR_UNEXPECTED, e.what());
						}
						return Utils::Win32::TaskDialog::Handled;
					},
				});
				if (!pids.empty()) {
					builder.WithButton({
						.Text = L"&Run once...\nLoad XivAlexander into running game once without installation.",
						.Callback = [&](auto&) {
							mode = Mode::RunOnce;
							return Utils::Win32::TaskDialog::NotHandled;
						},
					});
				}

				auto defaultRadioSet = false;
				for (const auto& [region, info] : XivAlex::FindGameLaunchers()) {
					const wchar_t* message = L"Install XivAlexander";
					const wchar_t* regionStr = nullptr;
					const auto selfVersion = Utils::StringSplit<std::string>(Utils::Win32::FormatModuleVersionString(GetModuleHandleW(nullptr)).first, ".");
					for (const auto name : {"d3d9.dll", "d3d11.dll"}) {
						const auto path = info.RootPath / "game" / name;
						try {
							if (XivAlex::IsXivAlexanderDll(path)) {
								const auto version = Utils::StringSplit<std::string>(Utils::Win32::FormatModuleVersionString(path).first, ".");
								if (selfVersion > version)
									message = L"Update XivAlexander";
								else if (selfVersion < version)
									message = L"Downgrade XivAlexander";
								else
									message = L"Reinstall XivAlexander";
							}
						} catch (...) {
						}
					}
					switch (region) {
						case XivAlex::GameRegion::International:
							regionStr = FindStringResourceEx(Dll::Module(), IDS_CLIENT_INTERNATIONAL) + 1;
							break;
						case XivAlex::GameRegion::Korean:
							regionStr = FindStringResourceEx(Dll::Module(), IDS_CLIENT_KOREAN) + 1;
							break;
						case XivAlex::GameRegion::Chinese:
							regionStr = FindStringResourceEx(Dll::Module(), IDS_CLIENT_CHINESE) + 1;
							break;
						default:
							continue;
					}
					builder.WithRadio({
						.Text = std::format(L"{}: {}\n{}", message, regionStr, info.RootPath.wstring()),
						.Callback = [&gamePath, &bootPath, info, &bootInjectable, &needChooser](auto&) {
							gamePath = info.RootPath / "game";
							bootPath = info.BootApp;
							bootInjectable = info.BootAppDirectlyInjectable;
							needChooser = false;
							return Utils::Win32::TaskDialog::Handled;
						},
					});
					if (!defaultRadioSet) {
						gamePath = info.RootPath / "game";
						bootPath = info.BootApp;
						bootInjectable = info.BootAppDirectlyInjectable;
						needChooser = false;
						defaultRadioSet = true;
					}
				}
				builder.WithRadio({
					.Text = L"Game installation is not listed above",
					.Callback = [&](auto&) {
						gamePath.clear();
						bootPath.clear();
						needChooser = true;
						return Utils::Win32::TaskDialog::Handled;
					},
				});

			} else {
				builder
					.WithMainInstruction(L"Run XivAlexander once")
					.WithContent(L"You can run XivAlexander right now without installation.\n\n* Restart using XivAlexander menu after loading to enable modding.");
				builder.WithButton({
					.Text = L"&Load XivAlexander\nLoad XivAlexander immediately to the selected process.",
					.Callback = [&](auto& dialog) {
						auto withHider = dialog.WithHiddenDialog();
						try {
							loadOrUnload(true);
							withHider.Cancel();
						} catch (const std::exception& e) {
							Dll::MessageBoxF(nullptr, MB_ICONERROR, IDS_ERROR_UNEXPECTED, e.what());
						}
						return Utils::Win32::TaskDialog::NotHandled;
					},
				});
				builder.WithButton({
					.Text = L"&Unload XivAlexander\nAttempt to unload XivAlexander from the selected process.",
					.Callback = [&](auto& dialog) {
						auto withHider = dialog.WithHiddenDialog();
						try {
							loadOrUnload(false);
							withHider.Cancel();
						} catch (const std::exception& e) {
							Dll::MessageBoxF(nullptr, MB_ICONERROR, IDS_ERROR_UNEXPECTED, e.what());
						}
						return Utils::Win32::TaskDialog::NotHandled;
					},
				});
				builder.WithButton({
					.Text = L"&Install...\nInstall XivAlexander for automatic loading when the game starts up.",
					.Callback = [&](auto&) {
						mode = Mode::Install;
						return Utils::Win32::TaskDialog::NotHandled;
					},
				});

				for (const auto pid : pids) {
					std::wstring path;
					try {
						path = Utils::Win32::Process(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid).PathOf();
					} catch (...) {
						try {
							path = Utils::Win32::Process(PROCESS_QUERY_INFORMATION, FALSE, pid).PathOf();
						} catch (...) {
							path = L"(Unknown)";
						}
					}
					builder.WithRadio({
						.Text = std::format(L"Process ID: {}\n{}", pid, path),
						.Callback = [&selectedPid, pid](auto&) {
							selectedPid = pid;
							return Utils::Win32::TaskDialog::Handled;
						},
					});
				}
			}

			auto refreshRequested = false;
			builder.WithHyperlinkHandler(L"refresh", [&](auto&) {
				refreshRequested = true;
				return Utils::Win32::TaskDialog::HyperlinkHandleResult::HandledCloseDialog;
			});
			builder.WithHyperlinkHandler(L"update", [&](auto& dialog) {
				auto withHider = dialog.WithHiddenDialog();
				try {
					CheckForUpdates(std::move(g_parameters->m_targetProcessHandles), true);
				} catch (const std::exception& e) {
					Dll::MessageBoxF(dialog.GetHwnd(), MB_ICONERROR, IDS_ERROR_UNEXPECTED, e.what());
				}
				return Utils::Win32::TaskDialog::HyperlinkHandleResult::HandledKeepDialog;
			});
			builder.WithHyperlinkHandler(L"homepage", [](auto& dialog) {
				try {
					SHELLEXECUTEINFOW shex{
						.cbSize = sizeof shex,
						.hwnd = dialog.GetHwnd(),
						.lpFile = FindStringResourceEx(Dll::Module(), IDS_URL_HOMEPAGE) + 1,
						.nShow = SW_SHOW,
					};
					if (!ShellExecuteExW(&shex))
						throw Utils::Win32::Error("ShellExecuteW");
				} catch (const std::exception& e) {
					Dll::MessageBoxF(dialog.GetHwnd(), MB_ICONERROR, IDS_ERROR_UNEXPECTED, e.what());
				}
				return Utils::Win32::TaskDialog::HyperlinkHandleResult::HandledKeepDialog;
			});
			auto dialog = builder
				.WithInstance(Dll::Module())
				.WithAllowDialogCancellation()
				.WithCanBeMinimized()
				.WithButtonCommandLinks()
				.WithWindowTitle(Dll::GetGenericMessageBoxTitle())
				.WithMainIcon(IDI_TRAY_ICON)
				.WithFooter(LR"(<a href="refresh">Refresh</a> | <a href="update">Check for updates</a> | <a href="homepage">Homepage</a>)")
				.Build();
			dialog.OnDialogConstructed = [](auto& dialog) {
				SetWindowPos(dialog.GetHwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			};
			const auto res = dialog.Show();
			if (res.Button == IDCANCEL && !refreshRequested)
				break;
		}
		return 0;
	}

	if (pids.empty()) {
		if (!g_parameters->m_quiet) {
			std::wstring errors;
			if (g_parameters->m_targetPids.empty() && g_parameters->m_targetSuffix.empty())
				errors = std::format(FindStringResourceEx(Dll::Module(), IDS_ERROR_NO_FFXIV_PROCESS) + 1, XivAlex::GameExecutableNameW);
			else
				errors = FindStringResourceEx(Dll::Module(), IDS_ERROR_NO_MATCHING_PROCESS) + 1;
			Dll::MessageBoxF(nullptr, MB_OK | MB_ICONERROR, errors);
		}
		return -1;
	}

	for (const auto pid : pids) {
		try {
			DoPidTask(pid, dllDir, dllPath);
		} catch (const std::exception& e) {
			if (!g_parameters->m_quiet)
				Dll::MessageBoxF(nullptr, MB_OK | MB_ICONERROR,
					L"PID: {}\n"
					L"\n"
					L"SeDebugPrivilege: {}\n"
					L"\n"
					L"* {}",
					pid,
					debugPrivilegeError,
					e.what()
				);
		}
	}
	return 0;
}
