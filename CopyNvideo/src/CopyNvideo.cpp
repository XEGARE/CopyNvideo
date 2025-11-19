#include <iostream>
#include <filesystem>
#include <unordered_set>
#include <set>
#include <windows.h>
#include <cstdlib>
#include <shlobj.h>
#include <string>
#include <mutex>

namespace fs = std::filesystem;

std::wstring GetNvidiaGalleryLocation()
{
	HKEY key = NULL;
	std::wstring result;

	LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\NVIDIA Corporation\\Global\\ShadowPlay\\NVSPCAPS", 0, KEY_READ, &key);
	if(status != ERROR_SUCCESS) return L"";

	BYTE buffer[1024] = {};
	DWORD size = sizeof(buffer);
	DWORD type = 0;

	status = RegQueryValueExW(key, L"DefaultPathW", nullptr, &type, buffer, &size);

	RegCloseKey(key);

	if(status != ERROR_SUCCESS || type != REG_BINARY) return L"";

	return std::wstring(reinterpret_cast<wchar_t*>(buffer));
}

bool IsRecordingDone(const fs::path& file)
{
	HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(handle == INVALID_HANDLE_VALUE) return false;
	CloseHandle(handle);
	return true;
}

bool CopyFileToClipboard(const fs::path& file)
{
	if(!fs::exists(file)) return false;

	std::wstring wpath = file.wstring();

	SIZE_T memSize = sizeof(DROPFILES) + (wpath.size() + 2) * sizeof(wchar_t);

	HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, memSize);
	if(!handle) return false;

	DROPFILES* drop = (DROPFILES*)GlobalLock(handle);
	if(!drop)
	{
		GlobalFree(handle);
		return false;
	}

	drop->pFiles = sizeof(DROPFILES);
	drop->fWide = TRUE;

	wchar_t* data = (wchar_t*)((BYTE*)drop + sizeof(DROPFILES));
	memcpy(data, wpath.c_str(), (wpath.size() + 1) * sizeof(wchar_t));

	GlobalUnlock(handle);

	if(!OpenClipboard(nullptr))
	{
		GlobalFree(handle);
		return false;
	}

	EmptyClipboard();

	if(!SetClipboardData(CF_HDROP, handle))
	{
		CloseClipboard();
		GlobalFree(handle);
		return false;
	}

	CloseClipboard();
	return true;
}

static const std::unordered_set<std::wstring> allowedExt = {
	L".mp4",
	L".png",
	L".jxr"
};

std::set<std::wstring> knownFiles;
std::set<std::wstring> processing;

std::mutex setsMutex;
std::vector<std::jthread> workers;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	HANDLE appMutex = CreateMutexW(NULL, FALSE, L"CopyNvideo_Mutex");
	if(GetLastError() == ERROR_ALREADY_EXISTS) return 0;

	std::wstring path = GetNvidiaGalleryLocation();
	if(path.empty())
	{
		MessageBoxW(NULL, L"Failed to read NVIDIA Gallery path.", L"CopyNvideo | Error", MB_ICONERROR | MB_OK);
		return 1;
	}

	for(const auto& entry : fs::recursive_directory_iterator(path))
	{
		auto ext = entry.path().extension().wstring();
		if(entry.is_regular_file() && allowedExt.count(ext) > 0)
		{
			knownFiles.insert(entry.path().wstring());
		}
	}

	while(true)
	{
		for(const auto& entry : fs::recursive_directory_iterator(path))
		{
			auto ext = entry.path().extension().wstring();
			if(entry.is_regular_file() && allowedExt.count(ext) > 0)
			{
				auto filePath = entry.path().wstring();
				bool startThread = false;

				{
					std::scoped_lock lock(setsMutex);
					if(!knownFiles.contains(filePath) && !processing.contains(filePath))
					{
						processing.insert(filePath);
						startThread = true;
					}
				}

				if(startThread)
				{
					auto pathCopy = entry.path();
					workers.emplace_back([pathCopy, filePath](std::stop_token stoken)
					{
						while(!IsRecordingDone(pathCopy))
						{
							if(stoken.stop_requested()) return;
							std::this_thread::sleep_for(std::chrono::milliseconds(100));
						}

						{
							std::scoped_lock lock(setsMutex);
							knownFiles.insert(filePath);
							processing.erase(filePath);
						}

						CopyFileToClipboard(pathCopy);
					});
				}
			}
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	return 0;
}