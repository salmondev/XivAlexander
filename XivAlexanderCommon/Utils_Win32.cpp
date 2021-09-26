#include "pch.h"
#include "Utils_Win32.h"

#include "Utils_Win32_Closeable.h"
#include "Utils_Win32_Handle.h"
#include "Utils_Win32_Process.h"
#include "Utils_Win32_Resource.h"

HANDLE Utils::Win32::g_hDefaultHeap = 0;

std::string Utils::Win32::FormatWindowsErrorMessage(unsigned int errorCode, int languageId) {
	std::set<std::string> messages;
	for (const auto langId : {
		languageId,
		MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
#ifdef _DEBUG
		MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN),
		MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN),
#endif
		}) {
		LPWSTR errorText = nullptr;
		FormatMessageW(
			FORMAT_MESSAGE_FROM_SYSTEM
			| FORMAT_MESSAGE_ALLOCATE_BUFFER
			| FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr,
			errorCode,
			langId,
			reinterpret_cast<LPWSTR>(&errorText), // output 
			0, // minimum size for output buffer
			nullptr); // arguments - see note 
		if (nullptr != errorText) {
			messages.insert(ToUtf8(errorText));
			LocalFree(errorText);
		}
	}
	std::string res;
	for (const auto& message : messages) {
		if (!res.empty())
			res += " / ";
		res += StringTrim<std::string>(message);
	}
	return res;
}

std::pair<std::string, std::string> Utils::Win32::FormatModuleVersionString(const void* pBlock) {
	UINT size = 0;
	LPVOID lpBuffer = nullptr;
	if (!VerQueryValueW(pBlock, L"\\", &lpBuffer, &size))
		throw std::runtime_error("Failed to query version information.");
	const VS_FIXEDFILEINFO& versionInfo = *static_cast<const VS_FIXEDFILEINFO*>(lpBuffer);
	if (versionInfo.dwSignature != 0xfeef04bd)
		throw std::runtime_error("Invalid version info found.");
	return std::make_pair<>(
		std::format("{}.{}.{}.{}",
			(versionInfo.dwFileVersionMS >> 16) & 0xFFFF,
			(versionInfo.dwFileVersionMS >> 0) & 0xFFFF,
			(versionInfo.dwFileVersionLS >> 16) & 0xFFFF,
			(versionInfo.dwFileVersionLS >> 0) & 0xFFFF),
		std::format("{}.{}.{}.{}",
			(versionInfo.dwProductVersionMS >> 16) & 0xFFFF,
			(versionInfo.dwProductVersionMS >> 0) & 0xFFFF,
			(versionInfo.dwProductVersionLS >> 16) & 0xFFFF,
			(versionInfo.dwProductVersionLS >> 0) & 0xFFFF));
}

std::pair<std::string, std::string> Utils::Win32::FormatModuleVersionString(const std::filesystem::path& path) {
	const auto pathw = path.wstring();
	DWORD verHandle = 0;
	std::vector<BYTE> buf;
	buf.resize(GetFileVersionInfoSizeW(pathw.c_str(), &verHandle));
	if (buf.empty())
		throw Error("GetFileVersionInfoSizeW");
	if (!GetFileVersionInfoW(pathw.c_str(), 0, static_cast<DWORD>(buf.size()), &buf[0]))
		throw Error("GetFileVersionInfoW");
	return FormatModuleVersionString(&buf[0]);
}

std::pair<std::string, std::string> Utils::Win32::FormatModuleVersionString(HMODULE hModule) {
	const auto hDllVersion = FindResourceW(hModule, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
	if (hDllVersion == nullptr)
		throw std::runtime_error("Failed to find version resource.");
	const auto hVersionResource = GlobalResource(LoadResource(hModule, hDllVersion),
		nullptr,
		"FormatModuleVersionString: Failed to load version resource.");
	const auto lpVersionInfo = LockResource(hVersionResource);  // no need to "UnlockResource"
	return FormatModuleVersionString(lpVersionInfo);
}

bool Utils::Win32::EnableTokenPrivilege(HANDLE hToken, LPCTSTR Privilege, bool bEnablePrivilege) {
	TOKEN_PRIVILEGES tp;
	LUID luid;
	TOKEN_PRIVILEGES tpPrevious;
	DWORD cbPrevious = sizeof(TOKEN_PRIVILEGES);

	if (!LookupPrivilegeValue(nullptr, Privilege, &luid)) return false;

	// 
	// first pass.  get current privilege setting
	// 
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = 0;

	AdjustTokenPrivileges(
		hToken,
		FALSE,
		&tp,
		sizeof(TOKEN_PRIVILEGES),
		&tpPrevious,
		&cbPrevious
	);

	if (GetLastError() != ERROR_SUCCESS) return false;

	// 
	// second pass.  set privilege based on previous setting
	// 
	tpPrevious.PrivilegeCount = 1;
	tpPrevious.Privileges[0].Luid = luid;

	if (bEnablePrivilege)
		tpPrevious.Privileges[0].Attributes |= (SE_PRIVILEGE_ENABLED);
	else
		tpPrevious.Privileges[0].Attributes ^= (SE_PRIVILEGE_ENABLED & tpPrevious.Privileges[0].Attributes);

	AdjustTokenPrivileges(hToken, FALSE, &tpPrevious, cbPrevious, nullptr, nullptr);

	if (GetLastError() != ERROR_SUCCESS) return false;

	return true;
}

void Utils::Win32::SetThreadDescription(HANDLE hThread, const std::wstring& description) {
	decltype(&::SetThreadDescription) pfnSetThreadDescription = nullptr;

	if (const auto hMod = LoadedModule(L"kernel32.dll", LOAD_LIBRARY_SEARCH_SYSTEM32, false))
		pfnSetThreadDescription = hMod.GetProcAddress<decltype(pfnSetThreadDescription)>("SetThreadDescription");
	else if (const auto hMod = LoadedModule(L"KernelBase.dll", LOAD_LIBRARY_SEARCH_SYSTEM32, false))
		pfnSetThreadDescription = hMod.GetProcAddress<decltype(pfnSetThreadDescription)>("SetThreadDescription");

	if (pfnSetThreadDescription)
		pfnSetThreadDescription(hThread, description.data());
	else
		DebugPrint(L"SetThreadDescription not supported");
}

int Utils::Win32::MessageBoxF(HWND hWnd, UINT uType, const wchar_t* lpCaption, const std::wstring& text) {
	return MessageBoxF(hWnd, uType, lpCaption, text.c_str());
}

int Utils::Win32::MessageBoxF(HWND hWnd, UINT uType, const wchar_t* lpCaption, const std::string& text) {
	return MessageBoxF(hWnd, uType, lpCaption, FromUtf8(text));
}

int Utils::Win32::MessageBoxF(HWND hWnd, UINT uType, const wchar_t* lpCaption, const wchar_t* text) {
	return MessageBoxW(hWnd, text, lpCaption, uType);
}

int Utils::Win32::MessageBoxF(HWND hWnd, UINT uType, const wchar_t* lpCaption, const char* text) {
	return MessageBoxF(hWnd, uType, lpCaption, FromUtf8(text));
}

void Utils::Win32::SetMenuState(HMENU hMenu, DWORD nMenuId, bool bChecked, bool bEnabled, std::wstring newText) {
	MENUITEMINFOW mii = { sizeof(MENUITEMINFOW) };
	mii.fMask = MIIM_STATE | (!newText.empty() ? MIIM_STRING : 0);

	GetMenuItemInfoW(hMenu, nMenuId, false, &mii);
	mii.fState &= ~(MFS_CHECKED | MFS_ENABLED | MFS_DISABLED);
	mii.fState |= (bChecked ? MFS_CHECKED : 0) | (bEnabled ? MFS_ENABLED : MFS_DISABLED);
	if (!newText.empty()) {
		mii.dwTypeData = &newText[0];
		mii.cch = static_cast<UINT>(newText.size());
	}
	SetMenuItemInfoW(hMenu, nMenuId, false, &mii);
}

void Utils::Win32::AddDebugPrivilege() {
	Handle token;
	{
		HANDLE hToken;
		if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, FALSE, &hToken)) {
			if (GetLastError() == ERROR_NO_TOKEN) {
				if (!ImpersonateSelf(SecurityImpersonation))
					throw Error("AddDebugPrivilege: ImpersonateSelf(SecurityImpersonation)");

				if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, FALSE, &hToken))
					throw Error("AddDebugPrivilege: OpenThreadToken#2");
			} else
				throw Error("AddDebugPrivilege: OpenThreadToken#1");
		}
		token = Handle(hToken, INVALID_HANDLE_VALUE, "AddDebugPrivilege: Invalid");
	}

	if (!EnableTokenPrivilege(token, SE_DEBUG_NAME, TRUE))
		throw Error("AddDebugPrivilege/EnableTokenPrivilege(SeDebugPrivilege)");
}

bool Utils::Win32::IsUserAnAdmin() {
	SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
	PSID adminGroup;
	if (!AllocateAndInitializeSid(
		&authority,
		2,
		SECURITY_BUILTIN_DOMAIN_RID,
		DOMAIN_ALIAS_RID_ADMINS,
		0, 0, 0, 0, 0, 0,
		&adminGroup))
		return false;
	const auto cleanup = CallOnDestruction([adminGroup]() {FreeSid(adminGroup); });

	if (BOOL b = FALSE; CheckTokenMembership(nullptr, adminGroup, &b) && b)
		return true;
	return false;
}

std::filesystem::path Utils::Win32::GetMappedImageNativePath(HANDLE hProcess, void* lpMem) {
	std::wstring result;
	result.resize(PATHCCH_MAX_CCH);
	result.resize(GetMappedFileNameW(hProcess, lpMem, &result[0], static_cast<DWORD>(result.size())));
	if (result.starts_with(LR"(\Device\)"))
		return LR"(\\?\)" + result.substr(8);
	throw std::runtime_error(std::format("Path unprocessable: {}", result));
}

std::filesystem::path Utils::Win32::ToNativePath(const std::filesystem::path& path) {
	return File::Create(path.wstring().c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0).ResolveName(false, true);
}

std::filesystem::path Utils::Win32::GetSystem32Path() {
	std::wstring sysDir;
	sysDir.resize(PATHCCH_MAX_CCH);  // assume
	sysDir.resize(GetSystemDirectoryW(&sysDir[0], static_cast<UINT>(sysDir.size())));
	if (sysDir.empty())
		throw Error("GetSystemWindowsDirectoryW");
	return { std::move(sysDir) };
}

std::filesystem::path Utils::Win32::EnsureKnownFolderPath(const KNOWNFOLDERID& rfid) {
	PWSTR pszPath;
	const auto result = SHGetKnownFolderPath(rfid, KF_FLAG_CREATE | KF_FLAG_INIT, nullptr, &pszPath);
	if (result != S_OK)
		throw Error(_com_error(result));

	const auto freepath = CallOnDestruction([pszPath]() { CoTaskMemFree(pszPath); });
	return std::filesystem::path(pszPath);
}

static Utils::CallOnDestruction WithRunAsInvoker() {
	static constexpr auto NeverElevateEnvKey = L"__COMPAT_LAYER";
	static constexpr auto NeverElevateEnvVal = L"RunAsInvoker";

	std::wstring env;
	env.resize(32768);
	env.resize(GetEnvironmentVariableW(NeverElevateEnvKey, &env[0], static_cast<DWORD>(env.size())));
	const auto envNone = env.empty() && GetLastError() == ERROR_ENVVAR_NOT_FOUND;
	if (!envNone && env.empty())
		throw Utils::Win32::Error("GetEnvironmentVariableW");
	if (!SetEnvironmentVariableW(NeverElevateEnvKey, NeverElevateEnvVal))
		throw Utils::Win32::Error("SetEnvironmentVariableW");
	return { [env = std::move(env), envNone] () {
		SetEnvironmentVariableW(NeverElevateEnvKey, envNone ? nullptr : &env[0]);
	} };
}

bool Utils::Win32::RunProgram(RunProgramParams params) {
	CallOnDestruction::Multiple cleanup;

	if (params.path.empty())
		params.path = Process::Current().PathOf();
	else if (!exists(params.path)) {
		std::wstring buf;
		buf.resize(PATHCCH_MAX_CCH);
		buf.resize(SearchPathW(nullptr, params.path.c_str(), L".exe", static_cast<DWORD>(buf.size()), &buf[0], nullptr));
		if (buf.empty())
			throw Error("SearchPath");
		params.path = buf;
	}

	switch (params.elevateMode) {
		case RunProgramParams::Normal:
		case RunProgramParams::Force:
		case RunProgramParams::NeverUnlessAlreadyElevated:
		{
			if (params.elevateMode == RunProgramParams::NeverUnlessAlreadyElevated)
				cleanup += WithRunAsInvoker();

			SHELLEXECUTEINFOW sei{};
			sei.cbSize = sizeof sei;
			sei.fMask = SEE_MASK_NOCLOSEPROCESS;
			sei.lpVerb = params.elevateMode == RunProgramParams::Force ? L"runas" : L"open";
			sei.lpFile = params.path.c_str();
			sei.lpParameters = params.args.c_str();
			sei.lpDirectory = params.dir.empty() ? nullptr : params.dir.c_str();
			sei.nShow = SW_SHOW;
			if (!ShellExecuteExW(&sei)) {
				const auto err = GetLastError();
				if (err == ERROR_CANCELLED && !params.throwOnCancel)
					return false;

				throw Error(err, "ShellExecuteExW");
			}
			if (!sei.hProcess)  // should not happen, unless registry is messed up.
				throw std::runtime_error("Failed to execute a new program.");
			if (params.wait)
				WaitForSingleObject(sei.hProcess, INFINITE);
			CloseHandle(sei.hProcess);
			return true;
		}

		case RunProgramParams::NeverUnlessShellIsElevated:
		case RunProgramParams::CancelIfRequired:
		case RunProgramParams::NoElevationIfDenied:
		{
			if (params.elevateMode == RunProgramParams::NeverUnlessShellIsElevated) {
				if (!IsUserAnAdmin()) {
					params.elevateMode = RunProgramParams::NeverUnlessAlreadyElevated;
					return RunProgram(params);
				}
				cleanup += WithRunAsInvoker();
			}

			try {
				const auto [process, thread] = ProcessBuilder()
					.WithPath(params.path)
					.WithParent(params.elevateMode == RunProgramParams::NeverUnlessShellIsElevated ? GetShellWindow() : nullptr)
					.WithWorkingDirectory(params.dir)
					.WithArgument(true, params.args)
					.Run();
				if (params.wait)
					process.Wait();

			} catch (const Error& e) {
				if (e.Code() == ERROR_ELEVATION_REQUIRED) {
					if (params.elevateMode == RunProgramParams::CancelIfRequired && !params.throwOnCancel)
						return false;
					else if (params.elevateMode == RunProgramParams::NoElevationIfDenied) {
						params.elevateMode = RunProgramParams::Normal;
						params.throwOnCancel = false;
						if (RunProgram(params))
							return true;

						params.elevateMode = RunProgramParams::NeverUnlessAlreadyElevated;
						return RunProgram(params);
					}
				}
				throw;
			}
			return true;
		}
	}
	throw std::out_of_range("invalid elevateMode");
}

std::wstring Utils::Win32::GetCommandLineWithoutProgramName() {
	const auto lpCmdLine = GetCommandLineW();
	auto inQuote = false;
	auto ptr = lpCmdLine;
	for (; *ptr && ((*ptr != L' ' && *ptr != L'\t') || inQuote); ++ptr) {
		if (*ptr == L'\"')
			inQuote = !inQuote;
	}
	if (*ptr)
		++ptr;
	return ptr;
}

std::wstring Utils::Win32::ReverseCommandLineToArgv(const std::wstring& arg) {
	if (arg.find_first_of(L"\" ") != std::wstring::npos) {
		std::wostringstream ss;
		ss << L"\"";
		for (size_t pos = 0; pos < arg.size();) {
			const auto special = arg.find_first_of(L"\\\"", pos);
			if (special == std::wstring::npos) {
				ss << arg.substr(pos);
				break;
			} else {
				ss << arg.substr(pos, special - pos) << L"\\" << arg[special];
				pos = special + 1;
			}
		}
		ss << L"\"";
		return ss.str();

	} else {
		return arg;
	}
}

std::wstring Utils::Win32::ReverseCommandLineToArgv(const std::span<const std::wstring>& argv) {
	std::wostringstream ss;
	for (const auto& arg : argv) {
		if (ss.tellp())
			ss << L" ";

		ss << ReverseCommandLineToArgv(arg);
	}
	return ss.str();
}

std::wstring Utils::Win32::ReverseCommandLineToArgv(const std::initializer_list<const std::wstring>& argv) {
	return ReverseCommandLineToArgv(std::vector(argv.begin(), argv.end()));
}

std::string Utils::Win32::ReverseCommandLineToArgv(const std::string& arg) {
	if (arg.find_first_of("\" ") != std::wstring::npos) {
		std::ostringstream ss;
		ss << "\"";
		for (size_t pos = 0; pos < arg.size();) {
			const auto special = arg.find_first_of("\\\"", pos);
			if (special == std::wstring::npos) {
				ss << arg.substr(pos);
				break;
			} else {
				ss << arg.substr(pos, special - pos) << "\\" << arg[special];
				pos = special + 1;
			}
		}
		ss << "\"";
		return ss.str();

	} else {
		return arg;
	}
}

std::string Utils::Win32::ReverseCommandLineToArgv(const std::span<const std::string>& argv) {
	std::ostringstream ss;
	for (const auto& arg : argv) {
		if (ss.tellp())
			ss << " ";

		ss << ReverseCommandLineToArgv(arg);
	}
	return ss.str();
}

std::string Utils::Win32::ReverseCommandLineToArgv(const std::initializer_list<const std::string>& argv) {
	return ReverseCommandLineToArgv(std::vector(argv.begin(), argv.end()));
}

std::vector<DWORD> Utils::Win32::GetProcessList() {
	std::vector<DWORD> res;
	DWORD cb = 0;
	do {
		res.resize(res.size() + 1024);
		EnumProcesses(&res[0], static_cast<DWORD>(sizeof res[0] * res.size()), &cb);
	} while (cb == sizeof res[0] * res.size());
	res.resize(cb / sizeof res[0]);
	return res;
}

int Utils::Win32::Error::DefaultLanguageId = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);

Utils::Win32::Error::Error(DWORD errorCode, const std::string& msg)
	: std::runtime_error(FormatWindowsErrorMessage(errorCode, DefaultLanguageId) + ": " + msg)
	, m_nErrorCode(errorCode) {
}

Utils::Win32::Error::Error(const std::string& msg) : Error(GetLastError(), msg) {
}

Utils::Win32::Error::Error(const _com_error& e)
	: std::runtime_error(
		std::format("[0x{:08x}] {} ({})",
			static_cast<uint32_t>(e.Error()),
			ToUtf8(e.ErrorMessage()),
			ToUtf8(e.Description().length() ? static_cast<const wchar_t*>(e.Description()) : L""))
	),
	m_nErrorCode(e.Error()) {
}

void Utils::Win32::Error::ThrowIfFailed(HRESULT hresult, bool expectCancel) {
	if (expectCancel && hresult == HRESULT_FROM_WIN32(ERROR_CANCELLED))
		throw CancelledError(_com_error(hresult));
	if (FAILED(hresult))
		throw Error(_com_error(hresult));
}
