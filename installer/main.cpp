// One file that installs the mod.
//
// The TrackMania ModLoader has no "mods folder" to drop a DLL into: it keeps a
// product database under %LOCALAPPDATA%\TMLoader, a folder per mod and a folder
// per version inside it, each with a small description.yaml. Nobody should have
// to know that.
//
// So this carries the DLL inside itself as a resource, writes the three files,
// and says what to do next. No network, no dependencies, no install wizard with
// six Next buttons - it is one dialog either way.
//
//   100TMX-Installer.exe              install, with a confirmation dialog
//   100TMX-Installer.exe /quiet       install, say nothing unless it fails
//   100TMX-Installer.exe /uninstall   remove the product folder again
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

#include <string>

#include "resource.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

const wchar_t* kTitle = L"100% TMX + Bingo";
const wchar_t* kVersion = L"0.1.0";
const wchar_t* kModLoaderPage = L"https://tomashu.dev/software/tmloader/";

std::wstring localAppData() {
  wchar_t* raw = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) out = raw;
  if (raw) CoTaskMemFree(raw);
  return out;
}

bool writeFile(const std::wstring& path, const void* data, DWORD size) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, data, size, &written, nullptr) && written == size;
  CloseHandle(file);
  return ok;
}

bool writeText(const std::wstring& path, const std::string& text) {
  return writeFile(path, text.data(), static_cast<DWORD>(text.size()));
}

// Removes a directory and what is in it, one level deep - which is all this
// ever creates. Deliberately not recursive: an uninstaller that walks a tree is
// an uninstaller that can delete the wrong tree.
void removeVersionDir(const std::wstring& dir) {
  WIN32_FIND_DATAW find{};
  HANDLE handle = FindFirstFileW((dir + L"\\*").c_str(), &find);
  if (handle != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = find.cFileName;
      if (name == L"." || name == L"..") continue;
      if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        removeVersionDir(dir + L"\\" + name);
      } else {
        DeleteFileW((dir + L"\\" + name).c_str());
      }
    } while (FindNextFileW(handle, &find));
    FindClose(handle);
  }
  RemoveDirectoryW(dir.c_str());
}

int say(const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
  return MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | icon);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
  const std::wstring args = commandLine ? commandLine : L"";
  const bool quiet = args.find(L"/quiet") != std::wstring::npos;
  const bool uninstall = args.find(L"/uninstall") != std::wstring::npos;

  const std::wstring local = localAppData();
  if (local.empty()) {
    say(L"Could not find your AppData folder, which is where the ModLoader keeps its mods.", MB_ICONERROR);
    return 1;
  }

  const std::wstring loader = local + L"\\TMLoader";
  const std::wstring product = loader + L"\\database\\TmForever\\products\\100TMX";
  const std::wstring target = product + L"\\" + kVersion;

  if (uninstall) {
    removeVersionDir(product);
    if (!quiet) say(L"100% TMX + Bingo has been removed from the ModLoader.\n\nYour settings in Documents\\100TMX are left alone.");
    return 0;
  }

  if (GetFileAttributesW(loader.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const int answer = MessageBoxW(nullptr,
                                   L"The TrackMania ModLoader does not seem to be installed.\n\n"
                                   L"This mod is loaded by it, so it has to come first.\n\n"
                                   L"Open the ModLoader's download page now?",
                                   kTitle, MB_YESNO | MB_ICONWARNING);
    if (answer == IDYES) ShellExecuteW(nullptr, L"open", kModLoaderPage, nullptr, nullptr, SW_SHOWNORMAL);
    return 1;
  }

  // The DLL travels inside this executable, so there is one file to download
  // and nothing to unzip into the right place.
  HRSRC found = FindResourceW(instance, MAKEINTRESOURCEW(IDR_MOD_DLL), RT_RCDATA);
  HGLOBAL loaded = found ? LoadResource(instance, found) : nullptr;
  const void* data = loaded ? LockResource(loaded) : nullptr;
  const DWORD size = found ? SizeofResource(instance, found) : 0;
  if (!data || size == 0) {
    say(L"This installer is missing the mod itself - download it again.", MB_ICONERROR);
    return 1;
  }

  SHCreateDirectoryExW(nullptr, target.c_str(), nullptr);

  // The name in the ModLoader's list. Bingo leads, because that is the half
  // somebody is looking for when they scroll past it.
  const std::string productYaml =
      "name: 100% TMX + Bingo\n"
      "author: cheatoskar\n"
      "type: modification\n"
      "homepage: 'https://100tmx.com/'\n"
      "description: 'Bingo boards on screen while you drive - your tiles, the time to beat, and a button that starts "
      "any of their maps. Plus: is this map still open for the 100% TMX project, what is it worth, who finished it.'\n";

  // CoreMod is what actually loads mod DLLs into the game, so it is a real
  // dependency even though nothing in the mod calls into it.
  const std::string versionYaml =
      "executable: 100TMX.dll\n"
      "dependencies:\n"
      "  - id: CoreMod\n"
      "    version: ^1.0.1\n"
      "changelog: '- The bingo panel, map status, and map marks.'\n";

  const bool ok = writeText(product + L"\\description.yaml", productYaml) &&
                  writeText(target + L"\\description.yaml", versionYaml) &&
                  writeFile(target + L"\\100TMX.dll", data, size);

  if (!ok) {
    say(L"Could not write into the ModLoader's folder.\n\n"
        L"If TrackMania is running, close it and try again.",
        MB_ICONERROR);
    return 1;
  }

  if (!quiet) {
    say(L"100% TMX is installed.\n\n"
        L"1. Open the TrackMania ModLoader\n"
        L"2. Tick 100TMX in the list\n"
        L"3. Start the game and press F9\n\n"
        L"Then: Connection → Connect, and approve the code at 100tmx.com/link.");
  }
  return 0;
}
