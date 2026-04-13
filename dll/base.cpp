/* Copyright (C) 2019 Mr Goldberg
   This file is part of the Goldberg Emulator

   The Goldberg Emulator is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   The Goldberg Emulator is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the Goldberg Emulator; if not, see
   <http://www.gnu.org/licenses/>.  */

#include "dll/base.h"
#include "dll/settings_parser.h"

#ifndef EMU_RELEASE_BUILD
#include "dbg_log/dbg_log.hpp"
#endif


std::recursive_mutex global_mutex{};
// some arbitrary counter/time for reference
extern const std::chrono::time_point<std::chrono::high_resolution_clock> startup_counter = std::chrono::high_resolution_clock::now();
extern const std::chrono::time_point<std::chrono::system_clock> startup_time = std::chrono::system_clock::now();

#ifndef EMU_RELEASE_BUILD
dbg_log dbg_logger(get_full_program_path() + "STEAM_LOG_" + std::to_string(common_helpers::rand_number(UINT32_MAX)) + ".log");
#endif


#ifdef __WINDOWS__

void randombytes(char *buf, size_t size)
{
    // NT_SUCCESS is: return value >= 0, including Ntdef.h causes so many errors
    while (BCryptGenRandom(NULL, (PUCHAR) buf, (ULONG) size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        PRINT_DEBUG("ERROR");
        Sleep(100);
    }
    
}

std::string get_env_variable(const std::string &name)
{
    wchar_t env_variable[1024]{};
    DWORD ret = GetEnvironmentVariableW(utf8_decode(name).c_str(), env_variable, _countof(env_variable));
    if (ret <= 0 || !env_variable[0]) {
        return std::string();
    }

    env_variable[ret] = 0;
    return utf8_encode(env_variable);
}

bool set_env_variable(const std::string &name, const std::string &value)
{
    return SetEnvironmentVariableW(utf8_decode(name).c_str(), utf8_decode(value).c_str());
}

#else

static int fd = -1;

void randombytes(char *buf, size_t size)
{
  int i{};

  if (fd == -1) {
    for (;;) {
      fd = open("/dev/urandom",O_RDONLY);
      if (fd != -1) break;
      sleep(1);
    }
  }

  while (size > 0) {
    if (size < 1048576) i = size; else i = 1048576;

    i = read(fd,buf,i);
    if (i < 1) {
      sleep(1);
      continue;
    }

    buf += i;
    size -= i;
  }
}

std::string get_env_variable(const std::string &name)
{
    char *env = getenv(name.c_str());
    if (!env) {
        return std::string();
    }

    return std::string(env);
}

bool set_env_variable(const std::string &name, const std::string &value)
{
    return setenv(name.c_str(), value.c_str(), 1) == 0;
}

#endif


unsigned generate_account_id()
{
    int a = 0;
    randombytes((char *)&a, sizeof(a));
    a = abs(a);
    if (!a) ++a;
    return a;
}

CSteamID generate_steam_anon_user()
{
    return CSteamID(generate_account_id(), k_unSteamUserDefaultInstance, k_EUniversePublic, k_EAccountTypeAnonUser);
}

SteamAPICall_t generate_steam_api_call_id() {
    static SteamAPICall_t a;
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    
    randombytes((char *)&a, sizeof(a));
    ++a;
    if (a == 0) ++a;
    return a;
}

CSteamID generate_steam_id_user()
{
    return CSteamID(generate_account_id(), k_unSteamUserDefaultInstance, k_EUniversePublic, k_EAccountTypeIndividual);
}

CSteamID generate_steam_id_server()
{
    return CSteamID(generate_account_id(), k_unSteamUserDefaultInstance, k_EUniversePublic, k_EAccountTypeGameServer);
}

CSteamID generate_steam_id_anonserver()
{
    return CSteamID(generate_account_id(), k_unSteamUserDefaultInstance, k_EUniversePublic, k_EAccountTypeAnonGameServer);
}

CSteamID generate_steam_id_lobby()
{
    return CSteamID(generate_account_id(), k_EChatInstanceFlagLobby | k_EChatInstanceFlagMMSLobby, k_EUniversePublic, k_EAccountTypeChat);
}

bool check_timedout(std::chrono::high_resolution_clock::time_point old, double timeout, std::chrono::high_resolution_clock::time_point now) // in seconds
{
    if (timeout == 0.0) return true;

    if (std::chrono::duration_cast<std::chrono::duration<double>>(now - old).count() > timeout) {
        return true;
    }

    return false;
}

std::string get_full_exe_path()
{
    // https://github.com/gpakosz/whereami/blob/master/src/whereami.c
    // https://stackoverflow.com/q/1023306

    static std::string exe_path{};
    static std::recursive_mutex mtx{};

    if (!exe_path.empty()) {
        return exe_path;
    }

    std::lock_guard lock(mtx);
    // check again in case we didn't win this thread arbitration
    if (!exe_path.empty()) {
        return exe_path;
    }

#if defined(__WINDOWS__)
    static wchar_t path[8192]{};
    auto ret = ::GetModuleFileNameW(nullptr, path, _countof(path));
    if (ret >= _countof(path) || 0 == ret) {
        path[0] = '.';
        path[1] = 0;
    }
    exe_path = canonical_path(utf8_encode(path));
#else
    // https://man7.org/linux/man-pages/man5/proc.5.html
    // https://linux.die.net/man/5/proc
    // https://man7.org/linux/man-pages/man2/readlink.2.html
    // https://linux.die.net/man/3/readlink
    static char path[8192]{};
    auto read = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (-1 == read) {
        path[0] = '.';
        read = 1;
    }
    path[read] = 0;
    exe_path = canonical_path(path);
#endif // __WINDOWS__

    return exe_path;
}

std::string get_exe_dirname()
{
    std::string env_exe_dir = get_env_variable("GseExeDir");
    if (!env_exe_dir.empty()) {
        if (env_exe_dir.back() != PATH_SEPARATOR[0]) {
            env_exe_dir = env_exe_dir.append(PATH_SEPARATOR);
        }

        return env_exe_dir;
    }

    std::string full_exe_path = get_full_exe_path();
    return full_exe_path.substr(0, full_exe_path.rfind(PATH_SEPARATOR)).append(PATH_SEPARATOR);

}

#ifdef __LINUX__
std::string get_lib_path()
{
    Dl_info info;
    if (dladdr((void*)get_lib_path, &info))
    {
        return std::string(info.dli_fname);
    }
    return ".";
}
#endif

std::string get_full_lib_path()
{
    std::string program_path;
#if defined(__WINDOWS__)
    wchar_t   DllPath[2048] = {0};
    GetModuleFileNameW((HINSTANCE)&__ImageBase, DllPath, _countof(DllPath));
    program_path = utf8_encode(DllPath);
#else
    program_path = get_lib_path();
#endif
    return program_path;
}

std::string get_full_program_path()
{
    std::string env_program_path = get_env_variable("GseAppPath");
    if (env_program_path.length()) {
        if (env_program_path.back() != PATH_SEPARATOR[0]) {
            env_program_path = env_program_path.append(PATH_SEPARATOR);
        }

        return env_program_path;
    }

    std::string program_path{};
    program_path = get_full_lib_path();
    return program_path.substr(0, program_path.rfind(PATH_SEPARATOR)).append(PATH_SEPARATOR);
}

std::string get_current_path()
{
    std::string path;
#if defined(STEAM_WIN32)
    char *buffer = _getcwd( NULL, 0 );
#else
    char *buffer = get_current_dir_name();
#endif
    if (buffer) {
        path = buffer;
        path.append(PATH_SEPARATOR);
        free(buffer);
    }

    return path;
}

std::string canonical_path(const std::string &path)
{
    std::string output;
#if defined(STEAM_WIN32)
    wchar_t *buffer = _wfullpath(NULL, utf8_decode(path).c_str(), 0);
    if (buffer) {
        output = utf8_encode(buffer);
        free(buffer);
    }
#else
    char *buffer = canonicalize_file_name(path.c_str());
    if (buffer) {
        output = buffer;
        free(buffer);
    }
#endif

    return output;
}

bool file_exists_(const std::string &full_path)
{
#if defined(STEAM_WIN32)
    struct _stat buffer{};
    if (_wstat(utf8_decode(full_path).c_str(), &buffer) != 0)
        return false;

    if ( buffer.st_mode & S_IFDIR)
        return false;
#else
    struct stat buffer{};
    if (stat(full_path.c_str(), &buffer) != 0)
        return false;

    if (S_ISDIR(buffer.st_mode))
        return false;
#endif

    return true;
}

unsigned int file_size_(const std::string &full_path)
{
#if defined(STEAM_WIN32)
    struct _stat buffer{};
    if (_wstat(utf8_decode(full_path).c_str(), &buffer) != 0) return 0;
#else
    struct stat buffer{};
    if (stat (full_path.c_str(), &buffer) != 0) return 0;
#endif
    return buffer.st_size;
}


#ifdef EMU_EXPERIMENTAL_BUILD

static std::vector<void*> loaded_libs{};

static void load_dlls()
{
    constexpr static const char LIB_EXTENSION[] =
#ifdef __WINDOWS__
        ".dll"
#else
        ".so"
#endif
        ;

    std::string path(Local_Storage::get_game_settings_path() + "load_dlls" + PATH_SEPARATOR);

    std::vector<std::string> paths(Local_Storage::get_filenames_path(path));
    for (auto & p: paths) {
        std::string full_path(path + p);
        if (!common_helpers::ends_with_i(full_path, LIB_EXTENSION)) continue;

        PRINT_DEBUG("loading '%s'", full_path.c_str());
        auto lib_handle =
#ifdef __WINDOWS__
            LoadLibraryW(utf8_decode(full_path).c_str());
#else
            dlopen(full_path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif

        if (lib_handle != nullptr) {
            loaded_libs.push_back(reinterpret_cast<void *>(lib_handle));
            PRINT_DEBUG(" LOADED");
        } else {
#ifdef __WINDOWS__
            PRINT_DEBUG(" FAILED, error code 0x%X", GetLastError());
#else
            PRINT_DEBUG(" FAILED, error string '%s'", dlerror());
#endif
        }
    }
}

static void unload_dlls()
{
    for (auto lib_handle : loaded_libs) {
#ifdef __WINDOWS__
        FreeLibrary(reinterpret_cast<HMODULE>(lib_handle));
#else
        dlclose(lib_handle);
#endif
    }
}

#ifdef __WINDOWS__

#include <TlHelp32.h>

struct ips_test {
    uint32_t ip_from;
    uint32_t ip_to;
};

static std::vector<struct ips_test> whitelist_ips;

void set_whitelist_ips(uint32_t *from, uint32_t *to, unsigned num_ips)
{
    whitelist_ips.clear();
    for (unsigned i = 0; i < num_ips; ++i) {
        struct ips_test ip_a;
        PRINT_DEBUG("from: %hhu.%hhu.%hhu.%hhu", ((unsigned char *)&from[i])[0], ((unsigned char *)&from[i])[1], ((unsigned char *)&from[i])[2], ((unsigned char *)&from[i])[3]);
        PRINT_DEBUG("to: %hhu.%hhu.%hhu.%hhu", ((unsigned char *)&to[i])[0], ((unsigned char *)&to[i])[1], ((unsigned char *)&to[i])[2], ((unsigned char *)&to[i])[3]);
        ip_a.ip_from = ntohl(from[i]);
        ip_a.ip_to = ntohl(to[i]);
        if (ip_a.ip_to < ip_a.ip_from) continue;
        if ((ip_a.ip_to - ip_a.ip_from) > (1 << 25)) continue;
        PRINT_DEBUG("added ip to whitelist");
        whitelist_ips.push_back(ip_a);
    }
}

static bool is_whitelist_ip(unsigned char *ip)
{
    uint32_t ip_temp = 0;
    memcpy(&ip_temp, ip, sizeof(ip_temp));
    ip_temp = ntohl(ip_temp);

    for (auto &i : whitelist_ips) {
        if (i.ip_from <= ip_temp && ip_temp <= i.ip_to) {
            PRINT_DEBUG("IP IS WHITELISTED %hhu.%hhu.%hhu.%hhu", ip[0], ip[1], ip[2], ip[3]);
            return true;
        }
    }

    return false;
}

static bool is_lan_ipv4(unsigned char *ip)
{
    PRINT_DEBUG("CHECK LAN IP %hhu.%hhu.%hhu.%hhu", ip[0], ip[1], ip[2], ip[3]);
    if (is_whitelist_ip(ip)) return true;
    if (ip[0] == 127) return true;
    if (ip[0] == 10) return true;
    if (ip[0] == 192 && ip[1] == 168) return true;
    if (ip[0] == 169 && ip[1] == 254 && ip[2] != 0) return true;
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return true;
    if ((ip[0] == 100) && ((ip[1] & 0xC0) == 0x40)) return true;
    if (ip[0] == 239) return true; //multicast
    if (ip[0] == 0) return true; //Current network
    if (ip[0] == 192 && (ip[1] == 18 || ip[1] == 19)) return true; //Used for benchmark testing of inter-network communications between two separate subnets.
    if (ip[0] >= 224) return true; //ip multicast (224 - 239) future use (240.0.0.0 - 255.255.255.254) broadcast (255.255.255.255)
    return false;
}

static bool is_lan_ip(const sockaddr *addr, int namelen)
{
    if (!namelen) return false;

    if (addr->sa_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
        unsigned char ip[4];
        memcpy(ip, &addr_in->sin_addr, sizeof(ip));
        if (is_lan_ipv4(ip)) return true;
    } else if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)addr;
        unsigned char ip[16];
        unsigned char zeroes[16] = {};
        memcpy(ip, &addr_in6->sin6_addr, sizeof(ip));
        PRINT_DEBUG("CHECK LAN IP6 %hhu.%hhu.%hhu.%hhu.%hhu.%hhu.%hhu.%hhu.%hhu.%hhu.%hhu.%hhu.%hhu.%hhu.%hhu.%hhu", ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7], ip[8], ip[9], ip[10], ip[11], ip[12], ip[13], ip[14], ip[15]);
        if (((ip[0] == 0xFF) && (ip[1] < 3) && (ip[15] == 1)) ||
        ((ip[0] == 0xFE) && ((ip[1] & 0xC0) == 0x80))) return true;
        if (memcmp(zeroes, ip, sizeof(ip)) == 0) return true;
        if (memcmp(zeroes, ip, sizeof(ip) - 1) == 0 && ip[15] == 1) return true;
        if (ip[0] == 0xff) return true; //multicast
        if (ip[0] == 0xfc) return true; //unique local
        if (ip[0] == 0xfd) return true; //unique local

        unsigned char ipv4_mapped[12] = {};
        ipv4_mapped[10] = 0xFF;
        ipv4_mapped[11] = 0xFF;
        if (memcmp(ipv4_mapped, ip, sizeof(ipv4_mapped)) == 0) {
            if (is_lan_ipv4(ip + 12)) return true;
        }
    }

    PRINT_DEBUG("NOT LAN IP");
    return false;
}

int ( WINAPI *Real_SendTo )( SOCKET s, const char *buf, int len, int flags, const sockaddr *to, int tolen) = sendto;
int ( WINAPI *Real_Connect )( SOCKET s, const sockaddr *addr, int namelen ) = connect;
int ( WINAPI *Real_WSAConnect )( SOCKET s, const sockaddr *addr, int namelen, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS) = WSAConnect;

static int WINAPI Mine_SendTo( SOCKET s, const char *buf, int len, int flags, const sockaddr *to, int tolen)
{
    PRINT_DEBUG_ENTRY();
    if (is_lan_ip(to, tolen)) {
        return Real_SendTo( s, buf, len, flags, to, tolen );
    } else {
        return len;
    }
}

static int WINAPI Mine_Connect( SOCKET s, const sockaddr *addr, int namelen )
{
    PRINT_DEBUG_ENTRY();
    if (is_lan_ip(addr, namelen)) {
        return Real_Connect(s, addr, namelen);
    } else {
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
}

static int WINAPI Mine_WSAConnect( SOCKET s, const sockaddr *addr, int namelen, LPWSABUF lpCallerData, LPWSABUF lpCalleeData, LPQOS lpSQOS, LPQOS lpGQOS)
{
    PRINT_DEBUG_ENTRY();
    if (is_lan_ip(addr, namelen)) {
        return Real_WSAConnect(s, addr, namelen, lpCallerData, lpCalleeData, lpSQOS, lpGQOS);
    } else {
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
}

inline bool file_exists (const std::string& name)
{
  struct stat buffer;   
  return (stat (name.c_str(), &buffer) == 0); 
}

#ifdef DETOURS_64BIT
    #define DLL_NAME "steam_api64.dll"
#else
    #define DLL_NAME "steam_api.dll"
#endif

HMODULE (WINAPI *Real_GetModuleHandleA)(LPCSTR lpModuleName) = GetModuleHandleA;
HMODULE WINAPI Mine_GetModuleHandleA(LPCSTR lpModuleName)
{
    PRINT_DEBUG("%s", lpModuleName);
    if (!lpModuleName) return Real_GetModuleHandleA(lpModuleName);
    std::string in(lpModuleName);
    if (in == std::string(DLL_NAME)) {
        in = std::string("crack") + in;
    }

    return Real_GetModuleHandleA(in.c_str());
}

static void redirect_crackdll()
{
    DetourTransactionBegin();
    DetourUpdateThread( GetCurrentThread() );
    DetourAttach( reinterpret_cast<PVOID*>(&Real_GetModuleHandleA), reinterpret_cast<PVOID>(Mine_GetModuleHandleA) );
    DetourTransactionCommit();
}

static void unredirect_crackdll()
{
    DetourTransactionBegin();
    DetourUpdateThread( GetCurrentThread() );
    DetourDetach( reinterpret_cast<PVOID*>(&Real_GetModuleHandleA), reinterpret_cast<PVOID>(Mine_GetModuleHandleA) );
    DetourTransactionCommit();
}

HMODULE crack_dll_handle{};
static void load_crack_dll()
{
    std::string path(get_full_program_path() + "crack" + DLL_NAME);
    PRINT_DEBUG("searching for crack file '%s'", path.c_str());
    if (file_exists(path)) {
        redirect_crackdll();
        crack_dll_handle = LoadLibraryW(utf8_decode(path).c_str());
        unredirect_crackdll();
        PRINT_DEBUG("Loaded crack file");
    }
}

#include "dll/local_storage.h"

//For some reason when this function is optimized it breaks the shogun 2 prophet (reloaded) crack.
#pragma optimize( "", off )
bool crack_SteamAPI_RestartAppIfNecessary(uint32 unOwnAppID)
{
    if (crack_dll_handle) {
        bool (__stdcall* restart_app)(uint32) = (bool (__stdcall *)(uint32))GetProcAddress(crack_dll_handle, "SteamAPI_RestartAppIfNecessary");
        if (restart_app) {
            PRINT_DEBUG("Calling crack SteamAPI_RestartAppIfNecessary");
            redirect_crackdll();
            bool ret = restart_app(unOwnAppID);
            unredirect_crackdll();
            return ret;
        }
    }

    return false;
}
#pragma optimize( "", on )

bool crack_SteamAPI_Init()
{
    if (crack_dll_handle) {
        bool (__stdcall* init_app)() = (bool (__stdcall *)())GetProcAddress(crack_dll_handle, "SteamAPI_Init");
        if (init_app) {
            PRINT_DEBUG("Calling crack SteamAPI_Init");
            redirect_crackdll();
            bool ret = init_app();
            unredirect_crackdll();
            return ret;
        }
    }

    return false;
}

HINTERNET (WINAPI *Real_WinHttpConnect)(
  IN HINTERNET     hSession,
  IN LPCWSTR       pswzServerName,
  IN INTERNET_PORT nServerPort,
  IN DWORD         dwReserved
);

HINTERNET WINAPI Mine_WinHttpConnect(
  IN HINTERNET     hSession,
  IN LPCWSTR       pswzServerName,
  IN INTERNET_PORT nServerPort,
  IN DWORD         dwReserved
) {
    PRINT_DEBUG("%ls %u", pswzServerName, nServerPort);
    struct sockaddr_in ip4;
    struct sockaddr_in6 ip6;
    ip4.sin_family = AF_INET;
    ip6.sin6_family = AF_INET6;

    if ((InetPtonW(AF_INET, pswzServerName, &(ip4.sin_addr)) && is_lan_ip((sockaddr *)&ip4, sizeof(ip4))) || (InetPtonW(AF_INET6, pswzServerName, &(ip6.sin6_addr)) && is_lan_ip((sockaddr *)&ip6, sizeof(ip6)))) {
        return Real_WinHttpConnect(hSession, pswzServerName, nServerPort, dwReserved);
    } else {
        return Real_WinHttpConnect(hSession, L"127.1.33.7", nServerPort, dwReserved);
    }
}

HINTERNET (WINAPI *Real_WinHttpOpenRequest)(
  IN HINTERNET hConnect,
  IN LPCWSTR   pwszVerb,
  IN LPCWSTR   pwszObjectName,
  IN LPCWSTR   pwszVersion,
  IN LPCWSTR   pwszReferrer,
  IN LPCWSTR   *ppwszAcceptTypes,
  IN DWORD     dwFlags
);

HINTERNET WINAPI Mine_WinHttpOpenRequest(
  IN HINTERNET hConnect,
  IN LPCWSTR   pwszVerb,
  IN LPCWSTR   pwszObjectName,
  IN LPCWSTR   pwszVersion,
  IN LPCWSTR   pwszReferrer,
  IN LPCWSTR   *ppwszAcceptTypes,
  IN DWORD     dwFlags
) {
    PRINT_DEBUG("%ls %ls %ls %ls %i", pwszVerb, pwszObjectName, pwszVersion, pwszReferrer, dwFlags);
    if (dwFlags & WINHTTP_FLAG_SECURE) {
        dwFlags ^= WINHTTP_FLAG_SECURE;
    }

    return Real_WinHttpOpenRequest(hConnect, pwszVerb, pwszObjectName, pwszVersion, pwszReferrer, ppwszAcceptTypes, dwFlags);
}


static bool network_functions_attached = false;

// read command line from a remote process via its PEB
static std::wstring read_process_cmdline(HANDLE hProc, DWORD pid)
{
    if (pid == GetCurrentProcessId()) {
        return GetCommandLineW();
    }

    // dynamically resolve NtQueryInformationProcess from ntdll
    typedef LONG(NTAPI* NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static NtQIP_t pNtQIP = (NtQIP_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!pNtQIP) return L"";

    // ProcessBasicInformation (class 0) gives us the PEB address
    struct { PVOID R1; PVOID PebBaseAddress; PVOID R2[2]; ULONG_PTR UniqueProcessId; PVOID R3; } pbi{};
    ULONG ret_len = 0;
    if (pNtQIP(hProc, 0, &pbi, sizeof(pbi), &ret_len) != 0 || !pbi.PebBaseAddress)
        return L"";

    // PEB.ProcessParameters offset: 0x20 on x64, 0x10 on x86
    // RTL_USER_PROCESS_PARAMETERS.CommandLine offset: 0x70 on x64, 0x40 on x86
#ifdef _WIN64
    constexpr SIZE_T peb_params_off = 0x20;
    constexpr SIZE_T cmdline_off = 0x70;
    constexpr SIZE_T us_ptr_off = 8; // UNICODE_STRING: USHORT Len, USHORT MaxLen, 4-pad, PWSTR(8)
#else
    constexpr SIZE_T peb_params_off = 0x10;
    constexpr SIZE_T cmdline_off = 0x40;
    constexpr SIZE_T us_ptr_off = 4; // UNICODE_STRING: USHORT Len, USHORT MaxLen, PWSTR(4)
#endif

    // read ProcessParameters pointer from PEB
    PVOID params_ptr = nullptr;
    SIZE_T n = 0;
    if (!ReadProcessMemory(hProc, (BYTE*)pbi.PebBaseAddress + peb_params_off, &params_ptr, sizeof(params_ptr), &n) || !params_ptr)
        return L"";

    // read CommandLine UNICODE_STRING: { USHORT Length, USHORT MaximumLength, [pad], PWSTR Buffer }
    BYTE us_buf[16]{};
    if (!ReadProcessMemory(hProc, (BYTE*)params_ptr + cmdline_off, us_buf, sizeof(us_buf), &n))
        return L"";

    USHORT length = *(USHORT*)us_buf;
    PVOID buffer = *(PVOID*)(us_buf + us_ptr_off);
    if (!buffer || length == 0 || length > 32768) return L"";

    // read the actual command line string
    std::wstring cmdline(length / sizeof(wchar_t), L'\0');
    if (!ReadProcessMemory(hProc, buffer, &cmdline[0], length, &n))
        return L"";

    return cmdline;
}

// dump the full process tree of the current process to steam_api(64).dll.txt
// this runs very early in DLL_PROCESS_ATTACH, before any game code or SteamAPI_Init
static void dump_process_tree()
{
    // get our DLL path to determine where to write the file and compute relative paths
    static const char anchor = 0;
    HMODULE our_module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&anchor),
        &our_module
    );
    if (!our_module) return;

    wchar_t dll_path_w[MAX_PATH]{};
    if (!GetModuleFileNameW(our_module, dll_path_w, MAX_PATH)) return;

    // derive output file path: <dll_name>.txt next to the DLL
    std::wstring out_path(dll_path_w);
    out_path += L".txt";

    // derive DLL directory for relative path computation
    std::wstring dll_dir(dll_path_w);
    auto last_sep = dll_dir.find_last_of(L"\\/");
    if (last_sep != std::wstring::npos) dll_dir.resize(last_sep + 1);

    // get DLL filename for the header
    const wchar_t* dll_name = (last_sep != std::wstring::npos) ? &dll_path_w[last_sep + 1] : dll_path_w;

    // walk the process tree: current → parent → grandparent → ...
    struct ProcessInfo {
        DWORD pid;
        DWORD parent_pid;
        std::wstring exe_name;
        std::wstring full_path;
        std::wstring cmdline;
        std::string start_time; // formatted creation timestamp
    };
    std::vector<ProcessInfo> tree;

    // snapshot all processes
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    // build a map of pid → (parent_pid, exe_name)
    std::map<DWORD, std::pair<DWORD, std::wstring>> proc_map;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            proc_map[pe.th32ProcessID] = { pe.th32ParentProcessID, pe.szExeFile };
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    // walk from current process up
    DWORD current_pid = GetCurrentProcessId();
    std::set<DWORD> visited; // prevent infinite loops from pid reuse
    DWORD walk_pid = current_pid;
    while (walk_pid && visited.insert(walk_pid).second) {
        auto it = proc_map.find(walk_pid);
        if (it == proc_map.end()) break;

        ProcessInfo info{};
        info.pid = walk_pid;
        info.parent_pid = it->second.first;
        info.exe_name = it->second.second;

        // try to get full path, command line, and creation time
        // PROCESS_VM_READ needed for PEB command line reading
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, walk_pid);
        bool full_access = (hProc != nullptr);
        if (!hProc) hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, walk_pid);
        if (hProc) {
            wchar_t path_buf[MAX_PATH]{};
            DWORD path_size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, path_buf, &path_size)) {
                info.full_path = path_buf;
            }
            if (full_access) {
                info.cmdline = read_process_cmdline(hProc, walk_pid);
            }
            // get process creation time
            FILETIME ft_create{}, ft_exit{}, ft_kernel{}, ft_user{};
            if (GetProcessTimes(hProc, &ft_create, &ft_exit, &ft_kernel, &ft_user) && (ft_create.dwHighDateTime || ft_create.dwLowDateTime)) {
                SYSTEMTIME st_utc{}, st_local{};
                FileTimeToSystemTime(&ft_create, &st_utc);
                SystemTimeToTzSpecificLocalTime(nullptr, &st_utc, &st_local);
                char tbuf[64]{};
                snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d",
                    st_local.wYear, st_local.wMonth, st_local.wDay,
                    st_local.wHour, st_local.wMinute, st_local.wSecond);
                info.start_time = tbuf;
            }
            CloseHandle(hProc);
        }

        tree.push_back(std::move(info));
        walk_pid = it->second.first;
    }

    // compute relative path from DLL directory
    auto make_relative = [&dll_dir](const std::wstring& full_path) -> std::wstring {
        if (full_path.empty()) return L"(unknown)";
        // case-insensitive prefix check
        if (full_path.size() >= dll_dir.size() &&
            _wcsnicmp(full_path.c_str(), dll_dir.c_str(), dll_dir.size()) == 0) {
            std::wstring rel = L".\\" + full_path.substr(dll_dir.size());
            return rel;
        }
        return full_path; // different drive/root, return absolute
    };

    // sanitize user profile path
    wchar_t profile_w[MAX_PATH]{};
    DWORD profile_len = GetEnvironmentVariableW(L"USERPROFILE", profile_w, MAX_PATH);

    auto sanitize = [&profile_w, profile_len](const std::wstring& path) -> std::wstring {
        if (profile_len == 0 || path.size() < profile_len) return path;
        if (_wcsnicmp(path.c_str(), profile_w, profile_len) == 0) {
            return L"%USERPROFILE%" + path.substr(profile_len);
        }
        return path;
    };

    // write the file
    FILE* f = _wfopen(out_path.c_str(), L"w");
    if (!f) return;

    // timestamp
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(f, "=== %ls Process Tree ===\n", dll_name);
    fprintf(f, "Timestamp: %04d-%02d-%02d %02d:%02d:%02d\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // sanitize DLL path for display
    char dll_path_a[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, dll_path_w, -1, dll_path_a, MAX_PATH, nullptr, nullptr);
    char profile_a[MAX_PATH]{};
    GetEnvironmentVariableA("USERPROFILE", profile_a, MAX_PATH);
    size_t plen = strlen(profile_a);
    std::string dll_display(dll_path_a);
    if (plen > 0 && dll_display.size() >= plen && _strnicmp(dll_display.c_str(), profile_a, plen) == 0) {
        dll_display.replace(0, plen, "%USERPROFILE%");
    }
    fprintf(f, "DLL Path: %s\n\n", dll_display.c_str());

    // print tree (first entry is current process, last is the topmost ancestor we could reach)
    for (size_t i = 0; i < tree.size(); ++i) {
        auto& p = tree[i];
        bool is_current = (p.pid == current_pid);

        std::wstring sanitized_path = sanitize(p.full_path);
        std::wstring rel = make_relative(p.full_path);
        std::wstring sanitized_rel = sanitize(rel);

        char name_a[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, p.exe_name.c_str(), -1, name_a, MAX_PATH, nullptr, nullptr);
        char path_a[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_UTF8, 0, sanitized_path.c_str(), -1, path_a, sizeof(path_a), nullptr, nullptr);
        char rel_a[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_UTF8, 0, sanitized_rel.c_str(), -1, rel_a, sizeof(rel_a), nullptr, nullptr);

        fprintf(f, "[PID %lu] %s%s\n", p.pid, name_a, is_current ? "  (current process)" : "");
        fprintf(f, "  Path: %s\n", path_a);
        fprintf(f, "  Relative: %s\n", rel_a);
        if (!p.start_time.empty()) {
            fprintf(f, "  Started: %s\n", p.start_time.c_str());
        }
        if (!p.cmdline.empty()) {
            std::wstring sanitized_cmd = sanitize(p.cmdline);
            int cmd_size = WideCharToMultiByte(CP_UTF8, 0, sanitized_cmd.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (cmd_size > 0) {
                std::string cmd_a(cmd_size - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, sanitized_cmd.c_str(), -1, &cmd_a[0], cmd_size, nullptr, nullptr);
                fprintf(f, "  CmdLine: %s\n", cmd_a.c_str());
            }
        }
        fprintf(f, "\n");

        PRINT_DEBUG("process tree [PID %lu]: %s%s | path: %s", p.pid, name_a,
            is_current ? " (current)" : "", path_a);
    }

    fclose(f);
}

// append detected renderer(s) and third-party modules for every process that loaded us
// called from SteamAPI_RunCallbacks (first call only), when the game's renderer is initialized
void append_renderer_info()
{
    // get our DLL path to derive the output file path
    static const char anchor = 0;
    HMODULE our_module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&anchor),
        &our_module
    );
    if (!our_module) return;

    wchar_t dll_path_w[MAX_PATH]{};
    if (!GetModuleFileNameW(our_module, dll_path_w, MAX_PATH)) return;

    std::wstring out_path(dll_path_w);
    out_path += L".txt";

    // renderer DLLs to check (name, label)
    struct { const wchar_t* dll; const char* label; } renderers[] = {
        { L"d3d9.dll",       "DirectX 9" },
        { L"d3d10.dll",      "DirectX 10" },
        { L"d3d10_1.dll",    "DirectX 10.1" },
        { L"d3d11.dll",      "DirectX 11" },
        { L"d3d12.dll",      "DirectX 12" },
        { L"vulkan-1.dll",   "Vulkan" },
        { L"opengl32.dll",   "OpenGL" },
        { L"dxgi.dll",       "DXGI" },
    };

    std::vector<std::string> detected;
    for (auto& r : renderers) {
        if (GetModuleHandleW(r.dll)) {
            detected.push_back(r.label);
        }
    }

    // append to file
    FILE* f = _wfopen(out_path.c_str(), L"a");
    if (!f) return;

    fprintf(f, "=== Renderer Detection (PID %lu) ===\n", GetCurrentProcessId());
    if (detected.empty()) {
        fprintf(f, "  (none detected)\n");
    } else {
        for (auto& d : detected) {
            fprintf(f, "  %s\n", d.c_str());
            PRINT_DEBUG("renderer detected: %s", d.c_str());
        }
    }
    fprintf(f, "\n");

    fclose(f);
}

BOOL WINAPI DllMain( HINSTANCE, DWORD dwReason, LPVOID )
{
    switch ( dwReason ) {
        case DLL_PROCESS_ATTACH:
            PRINT_DEBUG("experimental DLL_PROCESS_ATTACH");
            dump_process_tree();
            if (!settings_disable_lan_only()) {
                PRINT_DEBUG("Hooking lan only functions");
                DetourTransactionBegin();
                DetourUpdateThread( GetCurrentThread() );
                DetourAttach( reinterpret_cast<PVOID*>(&Real_SendTo), reinterpret_cast<PVOID>(Mine_SendTo) );
                DetourAttach( reinterpret_cast<PVOID*>(&Real_Connect), reinterpret_cast<PVOID>(Mine_Connect) );
                DetourAttach( reinterpret_cast<PVOID*>(&Real_WSAConnect), reinterpret_cast<PVOID>(Mine_WSAConnect) );

                HMODULE winhttp = GetModuleHandleA("winhttp.dll");
                if (winhttp) {
                    Real_WinHttpConnect = (decltype(Real_WinHttpConnect))GetProcAddress(winhttp, "WinHttpConnect");
                    DetourAttach( reinterpret_cast<PVOID*>(&Real_WinHttpConnect), reinterpret_cast<PVOID>(Mine_WinHttpConnect) );
                    // Real_WinHttpOpenRequest = (decltype(Real_WinHttpOpenRequest))GetProcAddress(winhttp, "WinHttpOpenRequest");
                    // DetourAttach( reinterpret_cast<PVOID*>(&Real_WinHttpOpenRequest), reinterpret_cast<PVOID>(Mine_WinHttpOpenRequest) );
                }
    
                DetourTransactionCommit();
                network_functions_attached = true;
            }
            load_crack_dll();
            load_dlls();
        break;

        case DLL_PROCESS_DETACH:
            PRINT_DEBUG("experimental DLL_PROCESS_DETACH");
            if (network_functions_attached) {
                DetourTransactionBegin();
                DetourUpdateThread( GetCurrentThread() );
                DetourDetach( reinterpret_cast<PVOID*>(&Real_SendTo), reinterpret_cast<PVOID>(Mine_SendTo) );
                DetourDetach( reinterpret_cast<PVOID*>(&Real_Connect), reinterpret_cast<PVOID>(Mine_Connect) );
                DetourDetach( reinterpret_cast<PVOID*>(&Real_WSAConnect), reinterpret_cast<PVOID>(Mine_WSAConnect) );
                if (Real_WinHttpConnect) {
                    DetourDetach( reinterpret_cast<PVOID*>(&Real_WinHttpConnect), reinterpret_cast<PVOID>(Mine_WinHttpConnect) );
                    // DetourDetach( reinterpret_cast<PVOID*>(&Real_WinHttpOpenRequest), reinterpret_cast<PVOID>(Mine_WinHttpOpenRequest) );
                }
                DetourTransactionCommit();
            }

            unload_dlls();
        break;
    }

    return TRUE;
}

#else

// dump the full process tree of the current process to <so_name>.txt
// uses /proc filesystem for process info, command lines, and parent traversal
static void dump_process_tree()
{
    // get our .so path via dladdr
    static const char anchor = 0;
    Dl_info dl_info{};
    if (!dladdr((void*)&anchor, &dl_info) || !dl_info.dli_fname) return;

    std::string so_path(dl_info.dli_fname);
    // resolve to absolute path
    char resolved[PATH_MAX]{};
    if (realpath(so_path.c_str(), resolved)) so_path = resolved;

    // derive output file path: <so_name>.txt next to the .so
    std::string out_path = so_path + ".txt";

    // derive directory for relative path computation
    std::string so_dir = so_path;
    auto last_sep = so_dir.find_last_of('/');
    if (last_sep != std::string::npos) so_dir.resize(last_sep + 1);

    // get .so filename for header
    std::string so_name = (last_sep != std::string::npos) ? so_path.substr(last_sep + 1) : so_path;

    struct ProcessInfo {
        pid_t pid;
        pid_t parent_pid;
        std::string exe_name;
        std::string full_path;
        std::string cmdline;
        std::string start_time; // formatted creation timestamp
    };
    std::vector<ProcessInfo> tree;

    // walk from current process up through parents
    pid_t walk_pid = getpid();
    std::set<pid_t> visited;
    while (walk_pid > 0 && visited.insert(walk_pid).second) {
        ProcessInfo info{};
        info.pid = walk_pid;

        // read exe path from /proc/<pid>/exe
        char link_path[64]{};
        snprintf(link_path, sizeof(link_path), "/proc/%d/exe", walk_pid);
        char exe_buf[PATH_MAX]{};
        ssize_t len = readlink(link_path, exe_buf, sizeof(exe_buf) - 1);
        if (len > 0) {
            exe_buf[len] = '\0';
            info.full_path = exe_buf;
            auto slash = info.full_path.find_last_of('/');
            info.exe_name = (slash != std::string::npos) ? info.full_path.substr(slash + 1) : info.full_path;
        }

        // read command line from /proc/<pid>/cmdline (NUL-separated args)
        char cmd_path[64]{};
        snprintf(cmd_path, sizeof(cmd_path), "/proc/%d/cmdline", walk_pid);
        FILE* cf = fopen(cmd_path, "r");
        if (cf) {
            char cmd_buf[4096]{};
            size_t n = fread(cmd_buf, 1, sizeof(cmd_buf) - 1, cf);
            fclose(cf);
            // replace NUL separators with spaces
            for (size_t i = 0; i < n; ++i) {
                if (cmd_buf[i] == '\0') cmd_buf[i] = ' ';
            }
            if (n > 0 && cmd_buf[n - 1] == ' ') cmd_buf[n - 1] = '\0';
            info.cmdline = cmd_buf;
        }

        // read parent PID and start time from /proc/<pid>/stat
        info.parent_pid = 0;
        char stat_path[64]{};
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", walk_pid);
        FILE* sf = fopen(stat_path, "r");
        if (sf) {
            char stat_buf[4096]{};
            fread(stat_buf, 1, sizeof(stat_buf) - 1, sf);
            fclose(sf);
            // format: pid (comm) state ppid ... field22=starttime
            // find the last ')' to skip comm which may contain spaces/parens
            char* comm_end = strrchr(stat_buf, ')');
            if (comm_end) {
                int ppid = 0;
                char state;
                if (sscanf(comm_end + 1, " %c %d", &state, &ppid) == 2) {
                    info.parent_pid = ppid;
                }
                // parse starttime (field 22, which is field 20 after comm_end)
                // fields after ')': state(1) ppid(2) pgrp(3) session(4) tty_nr(5) tpgid(6)
                //   flags(7) minflt(8) cminflt(9) majflt(10) cmajflt(11) utime(12) stime(13)
                //   cutime(14) cstime(15) priority(16) nice(17) num_threads(18) itrealvalue(19)
                //   starttime(20)
                unsigned long long starttime = 0;
                char* p = comm_end + 2; // skip ') '
                int field = 0;
                while (*p && field < 19) {
                    while (*p == ' ') ++p;
                    if (!*p) break;
                    if (field == 19) break;
                    while (*p && *p != ' ') ++p;
                    ++field;
                }
                while (*p == ' ') ++p;
                if (*p) {
                    sscanf(p, "%llu", &starttime);
                    if (starttime > 0) {
                        // convert clock ticks since boot to wall clock time
                        long hz = sysconf(_SC_CLK_TCK);
                        if (hz > 0) {
                            // read boot time from /proc/stat
                            FILE* bf = fopen("/proc/stat", "r");
                            unsigned long long btime = 0;
                            if (bf) {
                                char line[256]{};
                                while (fgets(line, sizeof(line), bf)) {
                                    if (strncmp(line, "btime ", 6) == 0) {
                                        sscanf(line + 6, "%llu", &btime);
                                        break;
                                    }
                                }
                                fclose(bf);
                            }
                            if (btime > 0) {
                                time_t proc_start = (time_t)(btime + starttime / hz);
                                struct tm tm_start{};
                                localtime_r(&proc_start, &tm_start);
                                char tbuf[64]{};
                                snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d",
                                    tm_start.tm_year + 1900, tm_start.tm_mon + 1, tm_start.tm_mday,
                                    tm_start.tm_hour, tm_start.tm_min, tm_start.tm_sec);
                                info.start_time = tbuf;
                            }
                        }
                    }
                }
            }
        }

        tree.push_back(std::move(info));
        walk_pid = tree.back().parent_pid;
    }

    // compute relative path from .so directory
    auto make_relative = [&so_dir](const std::string& full_path) -> std::string {
        if (full_path.empty()) return "(unknown)";
        if (full_path.size() >= so_dir.size() &&
            strncmp(full_path.c_str(), so_dir.c_str(), so_dir.size()) == 0) {
            return "./" + full_path.substr(so_dir.size());
        }
        return full_path;
    };

    // sanitize home directory
    const char* home = getenv("HOME");
    size_t home_len = home ? strlen(home) : 0;
    auto sanitize = [home, home_len](const std::string& path) -> std::string {
        if (!home || home_len == 0 || path.size() < home_len) return path;
        if (strncmp(path.c_str(), home, home_len) == 0) {
            return "$HOME" + path.substr(home_len);
        }
        return path;
    };

    // write the file
    FILE* f = fopen(out_path.c_str(), "w");
    if (!f) return;

    // timestamp
    time_t now = time(nullptr);
    struct tm tm_buf{};
    localtime_r(&now, &tm_buf);
    fprintf(f, "=== %s Process Tree ===\n", so_name.c_str());
    fprintf(f, "Timestamp: %04d-%02d-%02d %02d:%02d:%02d\n",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    fprintf(f, "SO Path: %s\n\n", sanitize(so_path).c_str());

    pid_t current_pid = getpid();
    for (size_t i = 0; i < tree.size(); ++i) {
        auto& p = tree[i];
        bool is_current = (p.pid == current_pid);

        fprintf(f, "[PID %d] %s%s\n", p.pid, p.exe_name.c_str(), is_current ? "  (current process)" : "");
        fprintf(f, "  Path: %s\n", sanitize(p.full_path).c_str());
        fprintf(f, "  Relative: %s\n", sanitize(make_relative(p.full_path)).c_str());
        if (!p.start_time.empty()) {
            fprintf(f, "  Started: %s\n", p.start_time.c_str());
        }
        if (!p.cmdline.empty()) {
            fprintf(f, "  CmdLine: %s\n", sanitize(p.cmdline).c_str());
        }
        fprintf(f, "\n");

        PRINT_DEBUG("process tree [PID %d]: %s%s | path: %s", p.pid, p.exe_name.c_str(),
            is_current ? " (current)" : "", sanitize(p.full_path).c_str());
    }

    fclose(f);
}

// append detected renderer(s) for the current process
// called from SteamAPI_RunCallbacks (first call only), when the game's renderer is initialized
void append_renderer_info()
{
    // get our .so path via dladdr
    static const char anchor = 0;
    Dl_info dl_info{};
    if (!dladdr((void*)&anchor, &dl_info) || !dl_info.dli_fname) return;

    std::string so_path(dl_info.dli_fname);
    char resolved[PATH_MAX]{};
    if (realpath(so_path.c_str(), resolved)) so_path = resolved;

    std::string out_path = so_path + ".txt";

    // check /proc/self/maps for renderer libraries
    struct { const char* lib; const char* label; } renderers[] = {
        { "libvulkan.so",  "Vulkan" },
        { "libGL.so",      "OpenGL" },
        { "libGLX.so",     "GLX" },
        { "libEGL.so",     "EGL" },
        { "libGLESv2.so",  "OpenGL ES" },
    };

    std::vector<std::string> detected;
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512]{};
        std::set<std::string> seen;
        while (fgets(line, sizeof(line), maps)) {
            for (auto& r : renderers) {
                if (strstr(line, r.lib) && seen.insert(r.label).second) {
                    detected.push_back(r.label);
                }
            }
        }
        fclose(maps);
    }

    // append to file
    FILE* f = fopen(out_path.c_str(), "a");
    if (!f) return;

    fprintf(f, "=== Renderer Detection (PID %d) ===\n", getpid());
    if (detected.empty()) {
        fprintf(f, "  (none detected)\n");
    } else {
        for (auto& d : detected) {
            fprintf(f, "  %s\n", d.c_str());
            PRINT_DEBUG("renderer detected: %s", d.c_str());
        }
    }
    fprintf(f, "\n");

    fclose(f);
}


// this acts as both an entry and an exit points for the library
// avoid "__attribute__((__constructor__))" and "__attribute__((__destructor__))"
// since they run before the C+++ runtime has been initialized
// causing problems related to uninitialized global objects
struct CppRuntimeTrick {
    CppRuntimeTrick()
    {
        PRINT_DEBUG_ENTRY();
        dump_process_tree();
        load_dlls();
    }

    ~CppRuntimeTrick()
    {
        PRINT_DEBUG_ENTRY();
        unload_dlls();
    }
} static g_cpp_rt{};


void set_whitelist_ips(uint32_t *from, uint32_t *to, unsigned num_ips)
{

}

#endif // __WINDOWS__

#else

void set_whitelist_ips(uint32_t *from, uint32_t *to, unsigned num_ips)
{

}

#endif // EMU_EXPERIMENTAL_BUILD
