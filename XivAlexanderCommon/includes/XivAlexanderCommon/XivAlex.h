#pragma once

#include <chrono>
#include <filesystem>
#include <set>
#include <string>

namespace XivAlex {
	[[nodiscard]] std::tuple<std::wstring, std::wstring> ResolveGameReleaseRegion();
	[[nodiscard]] std::tuple<std::wstring, std::wstring> ResolveGameReleaseRegion(const std::filesystem::path& path);

	struct VersionInformation {
		std::string Name;
		std::string Body;
		std::chrono::zoned_time<std::chrono::seconds> PublishDate;
		std::string DownloadLink;
		size_t DownloadSize;
	};

	VersionInformation CheckUpdates();

	enum class GameRegion {
		Unspecified,
		International,
		Korean,
		Chinese,
	};

	struct GameRegionInfo {
		GameRegion Type;
		std::filesystem::path RootPath;
		std::filesystem::path BootApp;
		bool BootAppRequiresAdmin;
		bool BootAppDirectlyInjectable;
		std::set<std::filesystem::path> RelatedApps;
	};

	std::vector<std::pair<XivAlex::GameRegion, XivAlex::GameRegionInfo>> FindGameLaunchers();

	const wchar_t GameExecutable32NameW[] = L"ffxiv.exe";
	const wchar_t GameExecutable64NameW[] = L"ffxiv_dx11.exe";
	const wchar_t XivAlexLoader32NameW[] = L"XivAlexanderLoader32.exe";
	const wchar_t XivAlexLoader64NameW[] = L"XivAlexanderLoader64.exe";
	const wchar_t XivAlexDll32NameW[] = L"XivAlexander32.dll";
	const wchar_t XivAlexDll64NameW[] = L"XivAlexander64.dll";

#if INTPTR_MAX == INT32_MAX

	const wchar_t GameExecutableNameW[] = L"ffxiv.exe";
	const wchar_t XivAlexDllNameW[] = L"XivAlexander32.dll";
	const char XivAlexDllName[] = "XivAlexander32.dll";
	const wchar_t XivAlexLoaderNameW[] = L"XivAlexanderLoader32.exe";
	const wchar_t GameExecutableOppositeNameW[] = L"ffxiv_dx11.exe";
	const wchar_t XivAlexDllOppositeNameW[] = L"XivAlexander64.dll";
	const char XivAlexDllOppositeName[] = "XivAlexander64.dll";
	const wchar_t XivAlexLoaderOppositeNameW[] = L"XivAlexanderLoader64.exe";

#elif INTPTR_MAX == INT64_MAX

	const wchar_t GameExecutableNameW[] = L"ffxiv_dx11.exe";
	const wchar_t XivAlexDllNameW[] = L"XivAlexander64.dll";
	const char XivAlexDllName[] = "XivAlexander64.dll";
	const wchar_t XivAlexLoaderNameW[] = L"XivAlexanderLoader64.exe";
	const wchar_t GameExecutableOppositeNameW[] = L"ffxiv.exe";
	const wchar_t XivAlexDllOppositeNameW[] = L"XivAlexander32.dll";
	const char XivAlexDllOppositeName[] = "XivAlexander32.dll";
	const wchar_t XivAlexLoaderOppositeNameW[] = L"XivAlexanderLoader32.exe";

#else
#error "Environment not x86 or x64."
#endif

	bool IsXivAlexanderDll(const std::filesystem::path& dllPath);
}
