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
// Set from CMake, which takes it from the project version or the CI tag. Never
// hardcode it here again: the ModLoader's list shows this string, and a build
// that lies about its own version is a support question nobody can answer.
#ifndef TMX_VERSION
#define TMX_VERSION L"0.0.0-dev"
#endif
const wchar_t* kVersion = TMX_VERSION;

// The ModLoader's list shows the product *folder*, not the name inside its
// description.yaml - that one only appears in the details panel. So the folder
// is what has to read like the mod's name, and the id in a profile is this same
// string, which is why renaming it has to carry the profiles along.
const wchar_t* kProduct = L"100% TMX + Bingo";
const wchar_t* kOldProducts[] = {L"100TMX"};
const wchar_t* kModLoaderPage = L"https://tomashu.dev/software/tmloader/";

std::wstring localAppData() {
  wchar_t* raw = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) out = raw;
  if (raw) CoTaskMemFree(raw);
  return out;
}

std::string wide(const std::wstring& in) {
  if (in.empty()) return std::string();
  int size = WideCharToMultiByte(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), &out[0], size, nullptr, nullptr);
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

bool g_quiet = false;

// Quiet means quiet, including when it goes wrong: a silent install that stops
// to argue in a dialog is not silent, and the exit code already carries it.
// Rewrite the mod's id where a profile has it ticked.
//
// A profile lists mods by id, and the id is the folder name - so renaming the
// folder without this would quietly untick the mod for anybody who already had
// it on, and leave them wondering why it stopped loading. Passing an empty
// `to` removes the line instead, which is what uninstalling wants.
void renameInProfiles(const std::wstring& loader, const std::wstring& to, const std::wstring& alsoRemove) {
  const std::wstring dir = loader + L"\\database\\TmForever\\profiles";

  WIN32_FIND_DATAW find{};
  HANDLE handle = FindFirstFileW((dir + L"\\*.yaml").c_str(), &find);
  if (handle == INVALID_HANDLE_VALUE) return;

  do {
    const std::wstring path = dir + L"\\" + find.cFileName;

    std::string text;
    {
      HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) continue;
      char buffer[8192];
      DWORD read = 0;
      while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) text.append(buffer, read);
      CloseHandle(file);
    }
    if (text.empty() || text.size() > 64 * 1024) continue;

    const std::string wanted = wide(to);
    bool changed = false;
    for (const wchar_t* old : kOldProducts) {
      const std::string needle = "id: " + wide(old);
      size_t at = text.find(needle);
      while (at != std::string::npos) {
        // Only a whole id: "100TMX" must not match inside "100TMX Legacy".
        const size_t after = at + needle.size();
        const bool wholeLine = after >= text.size() || text[after] == '\n' || text[after] == '\r';
        if (wholeLine) {
          text.replace(at, needle.size(), alsoRemove.empty() ? ("id: '" + wanted + "'") : std::string("id: __removed__"));
          changed = true;
        }
        at = text.find(needle, at + 1);
      }
    }
    if (!changed) continue;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
  } while (FindNextFileW(handle, &find));

  FindClose(handle);
}

int say(const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
  if (g_quiet) return IDOK;
  return MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | icon);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
  const std::wstring args = commandLine ? commandLine : L"";
  const bool quiet = args.find(L"/quiet") != std::wstring::npos;
  g_quiet = quiet;
  const bool uninstall = args.find(L"/uninstall") != std::wstring::npos;

  const std::wstring local = localAppData();
  if (local.empty()) {
    say(L"Could not find your AppData folder, which is where the ModLoader keeps its mods.", MB_ICONERROR);
    return 1;
  }

  const std::wstring loader = local + L"\\TMLoader";
  const std::wstring products = loader + L"\\database\\TmForever\\products";
  const std::wstring product = products + L"\\" + kProduct;
  const std::wstring target = product + L"\\" + kVersion;

  if (uninstall) {
    removeVersionDir(product);
    for (const wchar_t* old : kOldProducts) removeVersionDir(products + L"\\" + old);
    renameInProfiles(loader, kProduct, L"");
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

  // An older install sat under a different folder name, which is also the id a
  // profile ticked. Move both across before writing, so an update keeps working
  // instead of silently switching itself off.
  for (const wchar_t* old : kOldProducts) {
    const std::wstring previous = products + L"\\" + old;
    if (GetFileAttributesW(previous.c_str()) != INVALID_FILE_ATTRIBUTES) removeVersionDir(previous);
  }

  // And every *other* version of this product. The ModLoader keeps one folder
  // per version and lists them all, so without this an update leaves the old
  // one sitting beside the new - two entries for one mod, and no way for
  // somebody to tell which of them is running.
  {
    WIN32_FIND_DATAW find{};
    HANDLE handle = FindFirstFileW((product + L"\\*").c_str(), &find);
    if (handle != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = find.cFileName;
        if (name == L"." || name == L".." || name == kVersion) continue;
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          removeVersionDir(product + L"\\" + name);
        }
      } while (FindNextFileW(handle, &find));
      FindClose(handle);
    }
  }
  renameInProfiles(loader, kProduct, L"");

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
    say(L"100% TMX + Bingo is installed.\n\n"
        L"1. Open the TrackMania ModLoader\n"
        L"2. Tick \"100% TMX + Bingo\" in the list\n"
        L"3. Start the game\n\n"
        L"Then: Connect on the panel, or press F9 for the settings window.");
  }
  return 0;
}
