// Optional native actions for the fingerprint-gated Windows WeChat 4.1.15.13.
// No keyboard, clipboard, window interaction, remote host or background sends.
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "MinHook.h"
#include "httplib.h"
#include "json.hpp"

#if defined(ACTION_SELFTEST) && defined(NDEBUG)
#error The offline self-test must keep assertions enabled.
#endif

namespace {
using json = nlohmann::json;
using RecordKey = std::pair<std::string, uint64_t>;
constexpr size_t kRecordSize = 0x440;
constexpr std::time_t kWindow = 180;
constexpr size_t kMaxRecords = 32, kMaxAttempts = 1024;
constexpr uintptr_t kCopy = 0x1A5B50, kDestroy = 0x1AEE40;
constexpr uintptr_t kAlloc = 0x74AB89C, kFree = 0x74AB8E0;
constexpr uintptr_t kQueue = 0x19DA130, kExecute = 0x19DA5B0, kWork = 0x2315AF0;
uintptr_t base = 0;
HMODULE module = nullptr;
std::filesystem::path configPath;
std::string authToken, initialSelf, failure;
std::set<std::string> allowed;
std::mutex recordsMutex, attemptsMutex;
std::atomic<bool> enabled{false};
bool stopped = true;
bool hooksReady = false;
std::atomic<uint64_t> observed{0};
std::atomic<bool> maintain{true};
#ifdef ACTION_SELFTEST
bool queueFailureForTest = false;
#endif
thread_local bool copying = false;
std::map<RecordKey, void*> records;

struct Rejected : std::runtime_error { using std::runtime_error::runtime_error; };
struct NativeString { union { char small[16]; char* pointer; }; uint64_t length, capacity; };
struct Shared {
    void* payload = nullptr;
    void* owner = nullptr;
    ~Shared() {
        if (!owner) return;
        auto counts = reinterpret_cast<volatile LONG*>(static_cast<char*>(owner) + 8);
        auto table = *reinterpret_cast<void***>(owner);
        using Release = void(*)(void*);
        if (InterlockedDecrement(counts) == 0) {
            reinterpret_cast<Release>(table[0])(owner);
            if (InterlockedDecrement(counts + 1) == 0) reinterpret_cast<Release>(table[1])(owner);
        }
    }
};
struct Options {
    Shared cancel;
    std::function<void()> done, error, phase;
    uint64_t sequence = 0;
    Shared extra;
};
static_assert(sizeof(NativeString) == 32);
static_assert(sizeof(Options) == 0xE8);
using Copy = void*(*)(void*, const void*);
using Execute = uintptr_t(*)(void*);
using Work = uintptr_t(*)(void*, void*, void*);
Copy copy = nullptr;
Execute execute = nullptr;
Work work = nullptr;
struct Attempt {
    RecordKey record;
    uint32_t time = 0, local = 0;
    bool executed = false, processed = false, success = false;
};
std::map<std::string, Attempt> attempts;
std::map<RecordKey, std::string> submitted;

template<class F> F native(uintptr_t rva) { return reinterpret_cast<F>(base + rva); }
bool readable(const void* pointer, size_t size) {
    if (!pointer) return false;
    MEMORY_BASIC_INFORMATION info{};
    auto begin = reinterpret_cast<uintptr_t>(pointer);
    if (begin > std::numeric_limits<uintptr_t>::max() - size) return false;
    return VirtualQuery(pointer, &info, sizeof(info)) && info.State == MEM_COMMIT &&
        !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        begin + size <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}
struct HookProfile {
    uintptr_t rva;
    const char* name;
    std::array<unsigned char, 9> original;
    uintptr_t legacyTargetRva;
    std::array<unsigned char, 16> legacyTargetPrefix;
};
// Existing relay/target identities were independently read without modifying WeChat.
// Only this exact older, version-verified component is accepted as a previous hook.
constexpr char kLegacyHash[] = "df39db26f7175ce600d31717d71d0732f0a40e6771468708d96760542713b1d9";
constexpr wchar_t kLegacyName[] = L"WeChatLocal_df39db26f7175ce6.dll";
constexpr HookProfile kProfiles[] = {
    {kCopy, "copy", {0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54}, 0x2D1A0,
        {0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x6C,0x24,0x20,0x56,0x57,0x41,0x56,0x48,0x83}},
    {kExecute, "execute", {0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xEC}, 0x311F0,
        {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x83,0xC1,0x08,0xE8,0x8E,0x3E}},
    {kWork, "work", {0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54}, 0x45990,
        {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x56,0x57,0x41,0x56,0x48,0x83}},
};
bool executableReadable(uintptr_t address, size_t size) {
    if (!address || address > std::numeric_limits<uintptr_t>::max() - size) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto protection = info.Protect & 0xFF;
    return (protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY) &&
        address + size <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}
uintptr_t branchDestination(uintptr_t instruction, int32_t relative) {
    if (instruction > std::numeric_limits<uintptr_t>::max() - 5) throw Rejected("existing hook jump overflows");
    const uintptr_t next = instruction + 5;
    if (relative >= 0) {
        if (next > std::numeric_limits<uintptr_t>::max() - static_cast<uint32_t>(relative))
            throw Rejected("existing hook jump overflows");
        return next + static_cast<uint32_t>(relative);
    }
    const auto backwards = static_cast<uintptr_t>(-static_cast<int64_t>(relative));
    if (backwards > next) throw Rejected("existing hook jump underflows");
    return next - backwards;
}
// Return 0 for the exact original prologue; otherwise return the one FF25 relay target.
// This decoder alone grants no trust: validateHookEntry also verifies the loaded DLL.
uintptr_t existingHookTarget(uintptr_t entry, const HookProfile& profile) {
    if (!executableReadable(entry, profile.original.size())) throw Rejected("native function is not readable executable code");
    const auto* code = reinterpret_cast<const unsigned char*>(entry);
    if (!std::memcmp(code, profile.original.data(), profile.original.size())) return 0;
    if (code[0] != 0xE9 || std::memcmp(code + 5, profile.original.data() + 5, profile.original.size() - 5))
        throw Rejected("native function prologue or preserved hook tail mismatch");
    int32_t displacement = 0; std::memcpy(&displacement, code + 1, sizeof(displacement));
    const auto relay = branchDestination(entry, displacement);
    if ((relay >= entry && relay < entry + profile.original.size()) || !executableReadable(relay, 14))
        throw Rejected("existing native hook relay is not external executable code");
    constexpr unsigned char absoluteJump[] = {0xFF,0x25,0,0,0,0};
    if (std::memcmp(reinterpret_cast<void*>(relay), absoluteJump, sizeof(absoluteJump)))
        throw Rejected("existing native hook relay is not a standard MinHook FF25 jump");
    uintptr_t target = 0; std::memcpy(&target, reinterpret_cast<void*>(relay + 6), sizeof(target));
    if (!target || target == entry || target == relay || !executableReadable(target, profile.legacyTargetPrefix.size()))
        throw Rejected("existing native hook target is not executable code");
    return target;
}
std::string fileSha256(const std::filesystem::path& path) {
    struct Algorithm {
        BCRYPT_ALG_HANDLE value = nullptr;
        ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
    } algorithm;
    if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw Rejected("previous native hook SHA256 provider unavailable");
    DWORD objectSize = 0, received = 0;
    if (BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
        sizeof(objectSize), &received, 0) < 0 || !objectSize || objectSize > 1024 * 1024)
        throw Rejected("previous native hook SHA256 state invalid");
    std::vector<unsigned char> object(objectSize);
    struct Hash {
        BCRYPT_HASH_HANDLE value = nullptr;
        ~Hash() { if (value) BCryptDestroyHash(value); }
    } hash;
    if (BCryptCreateHash(algorithm.value, &hash.value, object.data(), objectSize, nullptr, 0, 0) < 0)
        throw Rejected("previous native hook SHA256 initialization failed");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw Rejected("previous native hook module file unavailable");
    std::array<unsigned char, 65536> block{}; size_t total = 0;
    while (file) {
        file.read(reinterpret_cast<char*>(block.data()), block.size());
        const auto count = file.gcount(); total += static_cast<size_t>(count);
        if (total > 16 * 1024 * 1024) throw Rejected("previous native hook module file exceeds verification limit");
        if (count && BCryptHashData(hash.value, block.data(), static_cast<ULONG>(count), 0) < 0)
            throw Rejected("previous native hook SHA256 update failed");
    }
    if (!file.eof() || !total) throw Rejected("previous native hook module file read failed");
    std::array<unsigned char, 32> bytes{};
    if (BCryptFinishHash(hash.value, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0)
        throw Rejected("previous native hook SHA256 verification failed");
    constexpr char hex[] = "0123456789abcdef"; std::string result(64, '0');
    for (size_t i = 0; i < bytes.size(); ++i) { result[2*i] = hex[bytes[i] >> 4]; result[2*i+1] = hex[bytes[i] & 15]; }
    return result;
}
bool legacyIdentityMatches(const HookProfile& profile, const std::wstring& name, const std::string& hash,
    uintptr_t rva, const unsigned char* prefix) {
    return _wcsicmp(name.c_str(), kLegacyName) == 0 && hash == kLegacyHash &&
        rva == profile.legacyTargetRva && !std::memcmp(prefix, profile.legacyTargetPrefix.data(), profile.legacyTargetPrefix.size());
}
void validateLegacyTarget(uintptr_t target, const HookProfile& profile) {
    MEMORY_BASIC_INFORMATION memory{}; HMODULE owner = nullptr;
    if (!VirtualQuery(reinterpret_cast<void*>(target), &memory, sizeof(memory)) || memory.Type != MEM_IMAGE ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(target), &owner) || memory.AllocationBase != owner)
        throw Rejected("previous native hook target does not belong to a loaded image");
    const auto start = reinterpret_cast<uintptr_t>(owner);
    if (target < start || !readable(owner, sizeof(IMAGE_DOS_HEADER))) throw Rejected("previous native hook image invalid");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(owner);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096 ||
        !readable(reinterpret_cast<void*>(start + dos->e_lfanew), sizeof(IMAGE_NT_HEADERS64)))
        throw Rejected("previous native hook image header invalid");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(start + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || target - start >= nt->OptionalHeader.SizeOfImage ||
        profile.legacyTargetPrefix.size() > nt->OptionalHeader.SizeOfImage - (target - start))
        throw Rejected("previous native hook target lies outside its image");
    wchar_t path[32768]; const auto count = GetModuleFileNameW(owner, path, static_cast<DWORD>(std::size(path)));
    if (!count || count >= std::size(path)) throw Rejected("previous native hook module identity unavailable");
    const std::filesystem::path file(path);
    if (!legacyIdentityMatches(profile, file.filename().wstring(), fileSha256(file), target - start,
        reinterpret_cast<const unsigned char*>(target))) throw Rejected("previous native hook module hash, target RVA or bytes not trusted");
    // The older hook must remain resident for the lifetime of our trampoline chain.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(target), &pinned) || pinned != owner)
        throw Rejected("previous native hook module could not be retained");
}
void validateHookEntry(uintptr_t entry, const HookProfile& profile) {
    try { if (const auto target = existingHookTarget(entry, profile)) validateLegacyTarget(target, profile); }
    catch (const std::exception& error) { throw Rejected(std::string("native ") + profile.name + " hook validation failed: " + error.what()); }
}
std::string identifier(const void* pointer) {
    if (!readable(pointer, sizeof(NativeString))) return {};
    const auto& value = *static_cast<const NativeString*>(pointer);
    if (!value.length || value.length > 128 || value.length > value.capacity) return {};
    const char* data = value.capacity >= 16 ? value.pointer : value.small;
    if (!readable(data, static_cast<size_t>(value.length))) return {};
    for (size_t i = 0; i < value.length; ++i) {
        auto c = static_cast<unsigned char>(data[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '@' || c == '.')) return {};
    }
    return { data, static_cast<size_t>(value.length) };
}
RecordKey recordKey(const void* record) {
    if (!readable(record, kRecordSize)) return {};
    const auto* data = static_cast<const char*>(record);
    return { identifier(data + 0x70), *reinterpret_cast<const uint64_t*>(data + 0xB8) };
}
uint32_t stamp(const void* record) { return *reinterpret_cast<const uint32_t*>(static_cast<const char*>(record) + 0xD0); }
uint32_t localId(const void* record) { return *reinterpret_cast<const uint32_t*>(static_cast<const char*>(record) + 0x94); }
void release(void* record) {
#ifdef ACTION_SELFTEST
    delete[] static_cast<unsigned char*>(record);
#else
    native<void(*)(void*)>(kDestroy)(record);
    native<void(*)(void*, size_t)>(kFree)(record, kRecordSize);
#endif
}
void* retain(const void* record) {
#ifdef ACTION_SELFTEST
    auto saved = new unsigned char[kRecordSize];
    std::memcpy(saved, record, kRecordSize);
    return saved;
#else
    auto saved = native<void*(*)(size_t)>(kAlloc)(kRecordSize);
    if (!saved) return nullptr;
    struct CopyGuard { bool previous=copying; CopyGuard() { copying = true; } ~CopyGuard() { copying = previous; } } guard;
    copy(saved, record);
    return saved;
#endif
}
bool validAge(uint32_t time, std::time_t now) { return now >= time && now - time < kWindow; }
void expire(std::time_t now) {
    for (auto it = records.begin(); it != records.end();) {
        if (!allowed.contains(it->first.first) || !validAge(stamp(it->second), now)) {
            release(it->second); it = records.erase(it);
        } else ++it;
    }
}
void clearRecords() { for (const auto& [key, record] : records) release(record); records.clear(); }
bool isHex(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
struct Setting { std::string token, self; int port; std::set<std::string> groups; };
Setting parseSetting(const json& value) {
    Setting setting{value.at("token").get<std::string>(), value.at("self_wxid").get<std::string>(),
        value.at("port").get<int>(), {}};
    if (!isHex(setting.token) || setting.port < 1 || setting.port > 65535 ||
        !std::regex_match(setting.self, std::regex("[A-Za-z0-9_-]{1,128}"))) throw Rejected("invalid local actions configuration");
    const auto& groups = value.at("groups");
    if (!groups.is_array() || groups.size() > 10000) throw Rejected("invalid actions group allowlist");
    for (const auto& group : groups) {
        auto id = group.get<std::string>();
        if (!std::regex_match(id, std::regex("[0-9]{5,24}@chatroom"))) throw Rejected("invalid actions group identifier");
        setting.groups.insert(id);
    }
    return setting;
}
Setting readSetting() {
    std::ifstream stream(configPath, std::ios::binary);
    if (!stream) throw Rejected("actions configuration unavailable");
    stream.seekg(0, std::ios::end); const auto size = stream.tellg();
    if (size <= 0 || size > 65536) throw Rejected("actions configuration size invalid");
    stream.seekg(0); return parseSetting(json::parse(stream));
}
// recordsMutex owns selection and record pointers. Callers never hold it across refresh.
void applySetting(const Setting& setting) {
    std::lock_guard lock(recordsMutex);
    if (setting.token != authToken || setting.self != initialSelf) {
        enabled = false; allowed.clear(); clearRecords();
        throw Rejected("local authentication or current account changed; actions disabled");
    }
    allowed = setting.groups;
    expire(std::time(nullptr));
    enabled = !stopped && !allowed.empty();
}
void refreshSetting() { applySetting(readSetting()); }
void stop() {
    std::lock_guard lock(recordsMutex);
    stopped = true; enabled = false; allowed.clear(); clearRecords();
}
void startObservation() {
    refreshSetting();
    std::lock_guard lock(recordsMutex);
#ifdef WECHAT_QUOTE_ONLY
    if (!hooksReady || !failure.empty()) throw Rejected("native quote constructor verification unavailable");
#else
    if (!hooksReady || !failure.empty()) throw Rejected("native action hooks unavailable");
#endif
    if (allowed.empty()) throw Rejected("no selected forwarding group");
    stopped = false; enabled = true;
}
void validateRecord(const void* record, const RecordKey& expected, uint32_t time, uint32_t local, std::time_t now) {
    const auto key = recordKey(record);
    if (!key.second || key != expected || !allowed.contains(key.first) ||
        identifier(static_cast<const char*>(record) + 0x10) != initialSelf)
        throw Rejected("only current account messages in a selected group may be recalled");
    if (!validAge(stamp(record), now)) throw Rejected("outside three minute window or unconfirmed message");
    if (stamp(record) != time || localId(record) != local) throw Rejected("native message locator mismatch");
}
void observe(const void* record) {
    if (!enabled || copying || !readable(record, kRecordSize)) return;
    const auto key = recordKey(record);
    if (key.first.empty() || !key.second) return;
    std::unique_lock lock(recordsMutex, std::try_to_lock);
    if (!lock || !enabled || !allowed.contains(key.first) ||
        identifier(static_cast<const char*>(record) + 0x10) != initialSelf || !validAge(stamp(record), std::time(nullptr))) return;
    { std::lock_guard attemptLock(attemptsMutex); if (submitted.contains(key)) return; }
    expire(std::time(nullptr));
    if (!records.contains(key) && records.size() >= kMaxRecords) return;
    auto saved = retain(record); if (!saved) return;
    auto previous = records.find(key); if (previous != records.end()) release(previous->second);
    records[key] = saved; ++observed;
}
void* copied(void* destination, const void* source) {
    auto result = copy(destination, source);
    try { observe(destination); } catch (...) { /* Optional observation cannot affect WeChat. */ }
    return result;
}
uintptr_t executing(void* task) {
    try {
        const auto key = recordKey(static_cast<char*>(task) + 8);
        std::lock_guard lock(attemptsMutex);
        auto found = submitted.find(key);
        if (found != submitted.end()) attempts.at(found->second).executed = true;
    } catch (...) {}
    return execute(task);
}
uintptr_t working(void* service, void* output, void* vector) {
    RecordKey key;
    if (readable(vector, 24)) key = recordKey(*static_cast<void**>(vector));
    auto result = work(service, output, vector);
    try {
        std::lock_guard lock(attemptsMutex);
        auto found = submitted.find(key);
        if (found != submitted.end()) {
            auto& attempt = attempts.at(found->second); attempt.processed = true;
            attempt.success = output && native<bool(*)(void*)>(0x15E4E80)(output);
        }
    } catch (...) {}
    return result;
}
struct Request { RecordKey record; uint32_t time, local; std::string id; };
Request parseRequest(const json& body) {
    const auto group = body.at("group").get<std::string>();
    const auto server = body.at("server_id").get<std::string>();
    const auto id = body.at("request_id").get<std::string>();
    if (!isHex(id) || !std::regex_match(group, std::regex("[0-9]{5,24}@chatroom")) ||
        !std::regex_match(server, std::regex("[1-9][0-9]{0,19}"))) throw Rejected("invalid recall request identifier");
    const auto time = body.at("create_time").get<uint64_t>(), local = body.at("local_id").get<uint64_t>();
    if (!body.at("create_time").is_number_integer() || !body.at("local_id").is_number_integer() ||
        time > UINT32_MAX || !local || local > UINT32_MAX) throw Rejected("invalid recall message locator");
    uint64_t number = 0;
    try { number = std::stoull(server); } catch (...) { throw Rejected("invalid server identifier"); }
    return {{group, number}, static_cast<uint32_t>(time), static_cast<uint32_t>(local), id};
}
json receipt(const Request& request) {
    return {{"status", "queued"}, {"server_id", std::to_string(request.record.second)}, {"request_id", request.id}};
}
// Call while holding recordsMutex; inserting a receipt precedes the first native queue call.
bool reserve(const Request& request) {
    if (!enabled || !allowed.contains(request.record.first)) throw Rejected("recall disabled or group not selected");
    std::lock_guard lock(attemptsMutex);
    if (auto found = attempts.find(request.id); found != attempts.end()) {
        const auto& old = found->second;
        if (old.record != request.record || old.time != request.time || old.local != request.local)
            throw Rejected("request_id reused with different message locator");
        return false;
    }
    if (submitted.contains(request.record)) throw Rejected("message already submitted; automatic resubmission forbidden");
    if (attempts.size() >= kMaxAttempts) throw Rejected("recall receipt cache full; request not submitted");
    auto found = records.find(request.record);
    if (found == records.end()) throw Rejected("complete native record unavailable; open the selected group first; no request submitted");
    validateRecord(found->second, request.record, request.time, request.local, std::time(nullptr));
    attempts.emplace(request.id, Attempt{request.record, request.time, request.local});
    submitted.emplace(request.record, request.id);
    return true;
}
json recall(const Request& request) {
    refreshSetting();
    std::lock_guard lock(recordsMutex);
    if (!reserve(request)) return receipt(request);
    auto found = records.find(request.record); auto record = static_cast<char*>(found->second);
#ifdef ACTION_SELFTEST
    if (queueFailureForTest) throw std::runtime_error("simulated queue outcome unknown");
#else
    alignas(8) unsigned char task[0x450]{};
    *reinterpret_cast<uintptr_t*>(task) = base + 0x91137A8;
    struct Guard { bool previous=copying; Guard() { copying = true; } ~Guard() { copying = previous; } };
    { Guard guard; copy(task + 8, record); }
    task[0x448] = 1;
    std::function<void()> done, error, phase;
    Shared empty; Options options;
    native<void*(*)(Options*, void*, void*, void*, Shared*, uint64_t)>(0xF910)
        (&options, &done, &error, &phase, &empty, 0);
    // A queue or subsequent transport failure remains an attempted action. Never erase its receipt.
    native<void(*)(void*, Options*)>(kQueue)(task, &options);
    native<void(*)(void*)>(kDestroy)(task + 8);
#endif
    release(record); records.erase(found);
    return receipt(request);
}
bool authorized(const httplib::Request& request, httplib::Response& response) {
    const auto provided = request.get_header_value("Authorization"), expected = "Bearer " + authToken;
    size_t different = provided.size() ^ expected.size();
    for (size_t i = 0; i < expected.size(); ++i) different |= static_cast<unsigned char>(expected[i] ^ (i < provided.size() ? provided[i] : 0));
    if (!authToken.empty() && !different) return true;
    response.status = 401; response.set_content("{\"error\":\"local authentication required\"}", "application/json"); return false;
}
void errorResponse(httplib::Response& response, int code, const std::string& status, const std::string& message) {
    response.status = code;
    response.set_content(json({{"status", status}, {"error", message}}).dump(), "application/json");
}
std::pair<int, json> recallResponse(const std::string& body) {
    bool reserved = false;
    try {
        const auto parsed = parseRequest(json::parse(body));
        try { return {200, recall(parsed)}; }
        catch (...) {
            { std::lock_guard lock(attemptsMutex); reserved = attempts.contains(parsed.id); }
            throw;
        }
    } catch (const Rejected& error) {
        return {400, {{"status", "rejected"}, {"error", error.what()}}};
    } catch (const std::exception& error) {
        return {reserved ? 500 : 400, {{"status", reserved ? "uncertain" : "rejected"}, {"error", error.what()}}};
    }
}
#ifdef WECHAT_QUOTE_ONLY
// An independently built, hook-free extension. These RVAs and field transfers were
// checked in the exact SHA256-gated 4.1.15.13 image. Actions itself stays unchanged.
constexpr uintptr_t kDbCtor = 0xA3B2A0, kDbDtor = 0x1C50F0, kDbToRecord = 0xA58DD0;
constexpr uintptr_t kQuoteFactory = 0x3995970, kStringGrow = 0x3E150;
constexpr uintptr_t kSendBatch = 0x19D0BC0, kBatchVtable = 0x9112308, kTextOwner = 0x8E534F8;
struct QuoteSource {
    std::string group, from, to, chat, content;
    uint64_t server = 0;
    uint32_t local = 0, time = 0;
};
struct QuoteRequest { QuoteSource source; std::string reply, atList, id; };
struct QuoteExtras { std::string selection, prefix; uint64_t flags = 0; std::string md5; };
struct SendBatch { uintptr_t vtable; Shared* begin; Shared* end; Shared* capacity; uint64_t flags; };
static_assert(sizeof(std::string) == 0x20 && sizeof(QuoteExtras) == 0x68);
static_assert(offsetof(QuoteExtras, flags) == 0x40 && offsetof(QuoteExtras, md5) == 0x48);
static_assert(sizeof(SendBatch) == 0x28 && sizeof(std::function<void()>) == 64);
std::map<std::string, std::pair<std::string, json>> quoteAttempts;
#ifdef ACTION_SELFTEST
unsigned quoteQueueCalls = 0;
bool quoteQueueFailure = false;
#endif
bool validUtf8(const std::string& value, size_t maximum, bool empty = false) {
    return (empty || !value.empty()) && value.size() <= maximum && value.find('\0') == std::string::npos &&
        (value.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0));
}
QuoteRequest parseQuote(const json& body) {
    QuoteRequest request;
    request.id = body.at("request_id").get<std::string>();
    request.reply = body.at("text").get<std::string>();
    request.atList = body.value("atlist", std::string());
    auto& source = request.source; const auto& value = body.at("source");
    source.group = body.at("group").get<std::string>();
    if (value.at("group") != source.group) throw Rejected("quoted source belongs to another group; no request submitted");
    if (!value.at("type").is_number_integer() || value.at("type") != 1)
        throw Rejected("native quote currently supports a confirmed plain text source; no summary fallback");
    source.from = value.at("fromusr").get<std::string>();
    source.to = value.at("tousr").get<std::string>();
    source.chat = value.at("chatusr").get<std::string>();
    source.content = value.at("content").get<std::string>();
    const auto server = value.at("server_id").get<std::string>();
    const auto local = value.at("local_id").get<uint64_t>(), time = value.at("create_time").get<uint64_t>();
    if (!std::regex_match(source.group, std::regex("[0-9]{5,24}@chatroom")) || !isHex(request.id) ||
        !std::regex_match(server, std::regex("[1-9][0-9]{0,19}")) ||
        !value.at("local_id").is_number_integer() || !value.at("create_time").is_number_integer() ||
        !local || local > UINT32_MAX || !time || time > UINT32_MAX || time > static_cast<uint64_t>(std::time(nullptr)) ||
        !validUtf8(request.reply, 24000) || !validUtf8(source.content, 24000) ||
        request.atList.size() > 4096 || (!request.atList.empty() && !std::regex_match(request.atList,
            std::regex("[A-Za-z0-9_-]{1,128}(,[A-Za-z0-9_-]{1,128})*"))))
        throw Rejected("invalid native quote message, source locator or mention list");
    try { source.server = std::stoull(server); } catch (...) { throw Rejected("invalid quoted server identifier"); }
    source.local = static_cast<uint32_t>(local); source.time = static_cast<uint32_t>(time);
    if (!std::regex_match(source.chat, std::regex("[A-Za-z0-9_-]{1,128}")) ||
        !((source.from == initialSelf && source.to == source.group && source.chat == initialSelf) ||
          (source.from == source.group && source.to == initialSelf)))
        throw Rejected("quoted source participants do not match the selected group/current account");
    return request;
}
void assignNative(void* object, size_t offset, const std::string& value) {
    auto* destination = reinterpret_cast<NativeString*>(static_cast<char*>(object) + offset);
    if (value.size() > destination->capacity)
        native<NativeString*(*)(NativeString*, uint64_t, uint64_t, const char*)>(kStringGrow)
            (destination, value.size(), 0, value.data());
    else {
        char* bytes = destination->capacity >= 16 ? destination->pointer : destination->small;
        std::memcpy(bytes, value.data(), value.size()); bytes[value.size()] = 0; destination->length = value.size();
    }
}
std::string nativeText(const void* object, size_t offset, size_t maximum = 24000) {
    auto* value = reinterpret_cast<const NativeString*>(static_cast<const char*>(object) + offset);
    if (!readable(value, sizeof(*value)) || value->length > maximum || value->length > value->capacity)
        throw Rejected("native quote string layout mismatch");
    auto* bytes = value->capacity >= 16 ? value->pointer : value->small;
    if (!readable(bytes, static_cast<size_t>(value->length) + 1) || bytes[value->length] != 0)
        throw Rejected("native quote string allocation mismatch");
    return {bytes, static_cast<size_t>(value->length)};
}
std::string queueQuote(const QuoteRequest& request, bool submit,
    const std::function<void(const std::string&)>& prepared = {}) {
#ifdef ACTION_SELFTEST
    (void)request;
    if (submit) {
        if (prepared) prepared("00000000-0000-4000-8000-000000000001");
        ++quoteQueueCalls; if (quoteQueueFailure) throw std::runtime_error("simulated unknown queue outcome");
    }
    return "00000000-0000-4000-8000-000000000001";
#else
    // Native default construction initializes every member, map and ownership slot.
    // A58DD0 clones this DB message through its real native vtable into Record+2C0.
    struct DbMessage {
        alignas(8) unsigned char bytes[0x2E8];
        DbMessage() { native<void*(*)(void*)>(kDbCtor)(bytes); }
        ~DbMessage() { native<void(*)(void*)>(kDbDtor)(bytes); }
    } source;
    const auto& item = request.source;
    if (*reinterpret_cast<uintptr_t*>(source.bytes) != base + 0x8D9F958)
        throw Rejected("native quoted DB message constructor mismatch");
    *reinterpret_cast<uint32_t*>(source.bytes + 0xC) = 1;
    assignNative(source.bytes, 0x18, item.from);
    assignNative(source.bytes, 0x38, item.to);
    assignNative(source.bytes, 0x58, item.chat);
    assignNative(source.bytes, 0x180, item.content);
    *reinterpret_cast<uint32_t*>(source.bytes + 0x144) = item.local;
    *reinterpret_cast<uint64_t*>(source.bytes + 0x148) = item.server;
    *reinterpret_cast<uint32_t*>(source.bytes + 0x164) = item.time;
    struct SourceRecord {
        alignas(8) unsigned char bytes[kRecordSize];
        bool initialized = false;
        ~SourceRecord() { if (initialized) native<void(*)(void*)>(kDestroy)(bytes); }
    } record;
    native<void(*)(void*, void*)>(kDbToRecord)(source.bytes, record.bytes); record.initialized = true;
    if (recordKey(record.bytes) != RecordKey{item.group, item.server} || localId(record.bytes) != item.local ||
        stamp(record.bytes) != item.time || nativeText(record.bytes, 0x10) != item.from ||
        nativeText(record.bytes, 0x50) != item.chat || nativeText(record.bytes, 0xE0) != item.content ||
        !*reinterpret_cast<void**>(record.bytes + 0x2C0) || !*reinterpret_cast<void**>(record.bytes + 0x2C8))
        throw Rejected("native quoted source conversion mismatch; no request submitted");
    Shared message; QuoteExtras extra;
    native<Shared*(*)(Shared*, const void*, const std::string*, const QuoteExtras*)>(kQuoteFactory)
        (&message, record.bytes, &request.reply, &extra);
    if (!message.payload || !message.owner || *reinterpret_cast<uintptr_t*>(message.owner) != base + kTextOwner)
        throw Rejected("native quote factory ownership mismatch");
    assignNative(message.payload, 0xB0, item.group);
    if (!request.atList.empty()) assignNative(message.payload, 0x778, request.atList);
    auto* payload = static_cast<char*>(message.payload);
    const auto uuid = nativeText(payload, 0x6F8, 128);
    if (*reinterpret_cast<uint64_t*>(payload + 0x118) != 0x3900000031ULL ||
        nativeText(payload, 0xB0, 128) != item.group || nativeText(payload, 0x758) != request.reply ||
        nativeText(payload, 0x778, 4096) != request.atList || payload[0x688] != 1 ||
        recordKey(payload + 0x248) != RecordKey{item.group, item.server} ||
        !std::regex_match(uuid, std::regex("[a-fA-F0-9-]{36}")))
        throw Rejected("native quote factory result mismatch; no request submitted");
    if (submit) {
        if (prepared) prepared(uuid); // Retain this locator before the first queue call, including an unknown outcome.
        std::function<void()> done = [] {}, error = [] {}, phase = [] {};
        Shared empty; Options options;
        native<Options*(*)(Options*, void*, void*, void*, Shared*, uint64_t)>(0xF910)
            (&options, &done, &error, &phase, &empty, 0);
        SendBatch batch{base + kBatchVtable, &message, &message + 1, &message + 1, 1};
        native<void(*)(SendBatch*, Options*)>(kSendBatch)(&batch, &options);
    }
    return uuid;
#endif
}
std::string quoteFingerprint(const QuoteRequest& request) {
    const auto& value = request.source;
    const auto body = json({{"group",value.group},{"from",value.from},{"to",value.to},{"chat",value.chat},
        {"source",value.content},{"server",value.server},{"local",value.local},{"time",value.time},
        {"reply",request.reply},{"atlist",request.atList}}).dump();
    std::array<unsigned char,32> bytes{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(body.data())),
        static_cast<ULONG>(body.size()), bytes.data(), static_cast<ULONG>(bytes.size())) < 0)
        throw Rejected("native quote fingerprint unavailable");
    std::string result; constexpr char hex[] = "0123456789abcdef";
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; } return result;
}
std::pair<int,json> quoteResponse(const std::string& body) {
    bool reserved = false;
    json* stored = nullptr;
    auto fail = [&](const std::exception& error) -> std::pair<int,json> {
        json result = stored ? *stored : json::object();
        result["status"] = reserved ? "uncertain" : "rejected"; result["error"] = error.what();
        return {reserved ? 500 : 400, result};
    };
    try {
        auto request = parseQuote(json::parse(body)); refreshSetting();
        std::lock_guard lock(recordsMutex);
        if (!enabled || !allowed.contains(request.source.group)) throw Rejected("native quote disabled or group not selected");
        const auto fingerprint = quoteFingerprint(request);
        if (auto found = quoteAttempts.find(request.id); found != quoteAttempts.end()) {
            if (found->second.first != fingerprint) throw Rejected("native quote request_id reused with different source or reply");
            return {200, found->second.second};
        }
        if (quoteAttempts.size() >= kMaxAttempts) throw Rejected("native quote receipt cache full; no request submitted");
        // All constructor-only validation runs before a durable attempt becomes uncertain.
        queueQuote(request, false);
        auto& receipt = quoteAttempts.emplace(request.id, std::make_pair(fingerprint,
            json({{"status","uncertain"},{"request_id",request.id},{"group",request.source.group},
                {"source_server_id",std::to_string(request.source.server)}}))).first->second.second;
        reserved = true; stored = &receipt;
        queueQuote(request, true, [&](const std::string& uuid) { receipt["uuid"] = uuid; });
        receipt["status"] = "queued";
        return {200, receipt};
    } catch (const Rejected& error) {
        return fail(error);
    } catch (const std::exception& error) {
        return fail(error);
    }
}
void initializeQuoteProfile() {
    base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Weixin.dll"));
    if (!base || !readable(reinterpret_cast<void*>(base), sizeof(IMAGE_DOS_HEADER))) throw Rejected("Weixin.dll not present");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096 ||
        !readable(reinterpret_cast<void*>(base + dos->e_lfanew), sizeof(IMAGE_NT_HEADERS64))) throw Rejected("Weixin PE header invalid");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != 1789961080 ||
        nt->OptionalHeader.SizeOfImage != 202039296) throw Rejected("unsupported native quote module");
    auto check = [](uintptr_t rva, std::initializer_list<unsigned char> bytes) {
        if (!executableReadable(base + rva, bytes.size()) ||
            !std::equal(bytes.begin(), bytes.end(), reinterpret_cast<const unsigned char*>(base + rva)))
            throw Rejected("native quote function fingerprint mismatch");
    };
    check(kDbCtor,{0x55,0x56,0x48,0x81,0xEC,0xA8,0,0,0});
    check(kDbToRecord,{0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x83,0xEC,0x68});
    check(kDbDtor,{0x56,0x57,0x48,0x83,0xEC,0x28,0x48,0x89,0xCE});
    check(kQuoteFactory,{0x55,0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53,0x48,0x83,0xEC,0x40});
    check(kStringGrow,{0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53});
    check(kSendBatch,{0x55,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xEC,0xE0,0x01,0,0});
    check(0x6F4C70,{0x55,0x56,0x57,0x53,0x48,0x83,0xEC,0x38});
    check(0xF910,{0x55,0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53});
    if (*reinterpret_cast<uintptr_t*>(base + kBatchVtable + 16) != base + kSendBatch ||
        *reinterpret_cast<uintptr_t*>(base + 0x8D9F958 + 32) != base + 0xA58D30)
        throw Rejected("native quote vtable fingerprint mismatch");
    // Exercise the real constructors/destructors without entering the send queue.
    // Zero source IDs/time cannot enter any existing Actions record observation.
    if (allowed.empty()) throw Rejected("no selected group for native quote constructor verification");
    QuoteRequest probe; probe.id = std::string(64,'0'); probe.reply = "native quote layout probe";
    probe.source = {*allowed.begin(), *allowed.begin(), initialSelf, initialSelf, "native source layout probe", 0, 0, 0};
    queueQuote(probe, false); hooksReady = true;
}
#endif
void routes(httplib::Server& api) {
    api.new_task_queue = [] { return new httplib::ThreadPool(1); };
#ifdef WECHAT_QUOTE_ONLY
    api.set_payload_max_length(320 * 1024); api.set_read_timeout(5); api.set_write_timeout(5); api.set_keep_alive_max_count(2);
#else
    api.set_payload_max_length(2048); api.set_read_timeout(5); api.set_write_timeout(5); api.set_keep_alive_max_count(2);
#endif
    api.Get("/health", [](const auto& request, auto& response) {
        if (!authorized(request, response)) return;
        std::lock_guard lock(recordsMutex);
#ifdef WECHAT_QUOTE_ONLY
        auto health = json({{"pid",GetCurrentProcessId()},{"version","4.1.15.13"},{"api_version",1},
            {"capabilities",json::array({"quote_text"})},{"source_types",json::array({1})},
            {"enabled",enabled.load()},{"selected_groups",allowed.size()},{"hooks_installed",false}});
#else
        auto health = json({{"pid", GetCurrentProcessId()}, {"version", "4.1.15.13"}, {"api_version", 1},
            {"capabilities", json::array({"recall"})}, {"enabled", enabled.load()}, {"observed", observed.load()},
            {"records", records.size()}, {"selected_groups", allowed.size()}, {"window_seconds", kWindow}});
#endif
        if (!failure.empty()) health["error"] = failure;
        response.set_content(health.dump(), "application/json");
    });
    api.Post("/start", [](const auto& request, auto& response) {
        if (!authorized(request, response)) return;
        try { startObservation(); response.set_content("{\"enabled\":true}", "application/json"); }
        catch (const std::exception& error) { errorResponse(response, 400, "rejected", error.what()); }
    });
    api.Post("/stop", [](const auto& request, auto& response) {
        if (!authorized(request, response)) return;
        stop(); response.set_content("{\"disabled\":true,\"record_memory_released\":true}", "application/json");
    });
#ifdef WECHAT_QUOTE_ONLY
    api.Post("/quote", [](const auto& request, auto& response) {
        if (!authorized(request, response)) return;
        const auto [code,result] = quoteResponse(request.body);
        response.status = code; response.set_content(result.dump(), "application/json");
    });
    api.Get("/result", [](const auto& request, auto& response) {
        if (!authorized(request, response)) return;
        const auto id = request.get_param_value("request_id"), group = request.get_param_value("group");
        if (!isHex(id)) { errorResponse(response,400,"rejected","invalid quote request_id"); return; }
        try { refreshSetting(); }
        catch (const std::exception& error) { errorResponse(response,400,"rejected",error.what()); return; }
        std::lock_guard lock(recordsMutex);
        if (!allowed.contains(group)) { errorResponse(response,403,"rejected","quoted group is no longer selected"); return; }
        auto found = quoteAttempts.find(id);
        if (found == quoteAttempts.end() || found->second.second.value("group",std::string()) != group) { response.status=404; return; }
        // This reports only local queue acceptance. The Rust bridge must confirm the exact type57 XML/server_id in its DB.
        response.set_content(found->second.second.dump(), "application/json");
    });
#else
    api.Post("/recall", [](const auto& request, auto& response) {
        if (!authorized(request, response)) return;
        const auto [code, result] = recallResponse(request.body);
        response.status = code; response.set_content(result.dump(), "application/json");
    });
    api.Get("/result", [](const auto& request, auto& response) {
        if (!authorized(request, response)) return;
        const auto id = request.get_param_value("request_id"), group = request.get_param_value("group");
        if (!isHex(id)) { errorResponse(response, 400, "rejected", "invalid request_id"); return; }
        std::lock_guard lock(attemptsMutex);
        auto found = attempts.find(id);
        if (found == attempts.end() || found->second.record.first != group) { response.status = 404; return; }
        const auto& attempt = found->second;
        response.set_content(json({{"server_id", std::to_string(attempt.record.second)}, {"request_id", id},
            {"executed", attempt.executed}, {"processed", attempt.processed}, {"success", attempt.success}}).dump(), "application/json");
    });
#endif
}
void initializeHooks() {
#ifdef WECHAT_QUOTE_ONLY
    initializeQuoteProfile();
#else
    base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Weixin.dll"));
    if (!base) throw Rejected("Weixin.dll not present");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096) throw Rejected("Weixin PE header invalid");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != 1789961080 ||
        nt->OptionalHeader.SizeOfImage != 202039296) throw Rejected("unsupported WeChat native module");
    std::array<std::array<unsigned char, 9>, 3> originalEntries{};
    for (size_t i = 0; i < std::size(kProfiles); ++i) {
        const auto& profile = kProfiles[i];
        if (!executableReadable(base + profile.rva, originalEntries[i].size()))
            throw Rejected("native hook entry is not readable executable code");
        std::memcpy(originalEntries[i].data(), reinterpret_cast<void*>(base + profile.rva), originalEntries[i].size());
        validateHookEntry(base + profile.rva, profile);
    }
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) throw Rejected("native hook runtime unavailable");
    for (size_t i = 0; i < std::size(kProfiles); ++i) {
        if (std::memcmp(reinterpret_cast<void*>(base + kProfiles[i].rva), originalEntries[i].data(), originalEntries[i].size()))
            throw Rejected("native hook entry changed during identity verification");
    }
    if (MH_CreateHook(reinterpret_cast<void*>(base + kCopy), copied, reinterpret_cast<void**>(&copy)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(base + kExecute), executing, reinterpret_cast<void**>(&execute)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(base + kWork), working, reinterpret_cast<void**>(&work)) != MH_OK)
        throw Rejected("native action observation unavailable");
    for (size_t i = 0; i < std::size(kProfiles); ++i) {
        if (std::memcmp(reinterpret_cast<void*>(base + kProfiles[i].rva), originalEntries[i].data(), originalEntries[i].size()))
            throw Rejected("native hook entry changed while creating observation trampolines");
    }
    for (size_t i = 0; i < std::size(kProfiles); ++i) {
        const auto entry = base + kProfiles[i].rva;
        if (std::memcmp(reinterpret_cast<void*>(entry), originalEntries[i].data(), originalEntries[i].size()) ||
            MH_EnableHook(reinterpret_cast<void*>(entry)) != MH_OK) {
            for (auto old : {kCopy, kExecute, kWork}) MH_DisableHook(reinterpret_cast<void*>(base + old));
            throw Rejected("native action observation could not be enabled or entry changed");
        }
    }
    hooksReady = true;
#endif
}
DWORD WINAPI maintenance(void*) {
    while (maintain) {
        Sleep(1000);
        if (!maintain) break;
        try { refreshSetting(); }
        catch (...) { std::lock_guard lock(recordsMutex); enabled = false; allowed.clear(); clearRecords(); }
    }
    return 0;
}
DWORD WINAPI run(void*) {
    try {
        wchar_t path[32768];
        auto length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        if (!length || length >= std::size(path)) return 1;
        configPath = std::filesystem::path(path).parent_path() / L"native.json";
        auto setting = readSetting(); authToken = setting.token; initialSelf = setting.self; applySetting(setting);
        try { initializeHooks(); } catch (const std::exception& error) { failure = error.what(); }
        auto thread = CreateThread(nullptr, 0, maintenance, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread); else failure = "actions maintenance thread unavailable";
        httplib::Server api; routes(api); api.listen("127.0.0.1", setting.port);
        maintain = false; stop(); return 0;
    } catch (...) { return 9; }
}
} // namespace

#ifndef ACTION_SELFTEST
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        module = instance; DisableThreadLibraryCalls(instance);
        auto thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
#else
#include <iostream>
#ifdef WECHAT_QUOTE_ONLY
int main() {
    // Offline only: queueQuote is a stub; no WeChat process/native code/network is used.
    constexpr char group[]="12345678901@chatroom", otherGroup[]="12345678902@chatroom";
    authToken=std::string(64,'a'); initialSelf="wxid_offline_owner"; hooksReady=true;
    configPath=std::filesystem::temp_directory_path()/(L"quote-offline-"+std::to_wstring(GetCurrentProcessId())+L".json");
    auto config=json({{"token",authToken},{"self_wxid",initialSelf},{"port",30112},{"groups",json::array({group})}});
    auto save=[&]{std::ofstream stream(configPath);stream<<config;};save();startObservation();
    auto message=json({{"group",group},{"text","reply <&> 中文"},{"atlist","wxid_member"},{"request_id",std::string(64,'b')},
        {"source",{{"group",group},{"type",1},{"server_id","42"},{"local_id",7},{"create_time",1},
            {"fromusr",initialSelf},{"tousr",group},{"chatusr",initialSelf},{"content","original <&> 文本"}}}});
    auto rejects=[](auto action){try{action();return false;}catch(const std::exception&){return true;}};
    auto parsed=parseQuote(message);assert(parsed.source.server==42&&parsed.source.time==1&&parsed.source.content=="original <&> 文本");
    assert(parsed.reply=="reply <&> 中文"&&parsed.atList=="wxid_member"); // 1. Native source and reply remain separate, including old sources/XML characters.
    auto incoming=message;incoming["source"]["fromusr"]=group;incoming["source"]["tousr"]=initialSelf;incoming["source"]["chatusr"]="wxid_member";
    assert(parseQuote(incoming).source.chat=="wxid_member"); // 2. Incoming and own same-group sources.
    auto invalid=message;invalid["source"]["group"]=otherGroup;assert(rejects([&]{parseQuote(invalid);}));
    invalid=message;invalid["source"]["chatusr"]="wxid_forged";assert(rejects([&]{parseQuote(invalid);}));
    invalid=incoming;invalid["source"]["tousr"]="wxid_another_account";assert(rejects([&]{parseQuote(invalid);})); // 3. Cross-group/account participant rejection.
    for(auto type:{3,43,47,49,57}){invalid=message;invalid["source"]["type"]=type;assert(rejects([&]{parseQuote(invalid);}));} // 4. Unsupported media/nested source cannot silently become summary text.
    invalid=message;invalid["text"]=std::string("a\0b",3);assert(rejects([&]{parseQuote(invalid);}));
    invalid=message;invalid["source"]["content"]=std::string(24001,'x');assert(rejects([&]{parseQuote(invalid);}));
    invalid=message;invalid["text"]=std::string("\xC0\xAF",2);assert(rejects([&]{parseQuote(invalid);})); // 5. Strict UTF-8/size/NUL bounds.
    auto maximum=message;maximum["text"]=std::string(24000,'x');maximum["source"]["content"]=std::string(24000,'y');
    assert(parseQuote(maximum).reply.size()==24000&&parseQuote(maximum).source.content.size()==24000);
    maximum["text"]=std::string(24001,'x');assert(rejects([&]{parseQuote(maximum);}));
    maximum["text"]=std::string(24000,'\x01');maximum["source"]["content"]=std::string(24000,'\x01');
    assert(maximum.dump().size()>64*1024&&maximum.dump().size()<320*1024); // Separate source/reply byte limits, including worst-case JSON escaping.
    auto first=quoteResponse(message.dump());assert(first.first==200&&first.second["status"]=="queued"&&quoteQueueCalls==1);
    assert(quoteResponse(message.dump()).second==first.second&&quoteQueueCalls==1);
    invalid=message;invalid["text"]="changed";assert(quoteResponse(invalid.dump()).first==400&&quoteQueueCalls==1);
    invalid=message;invalid["source"]["server_id"]="43";assert(quoteResponse(invalid.dump()).first==400&&quoteQueueCalls==1); // 6. Exact source/body idempotency; no duplicate queue calls.
    auto uncertain=message;uncertain["request_id"]=std::string(64,'c');quoteQueueFailure=true;
    const auto failed=quoteResponse(uncertain.dump());assert(failed.first==500&&failed.second.contains("uuid")&&quoteQueueCalls==2);quoteQueueFailure=false;
    const auto repeated=quoteResponse(uncertain.dump());assert(repeated.second["status"]=="uncertain"&&repeated.second["uuid"]==failed.second["uuid"]&&quoteQueueCalls==2); // 7. An unknown queue outcome retains its UUID and is never resubmitted.
    config["groups"]=json::array();save();assert(quoteResponse(message.dump()).first==400&&quoteQueueCalls==2);
    config["groups"]=json::array({group});save();startObservation();stop();assert(quoteResponse(message.dump()).first==400);
    startObservation();assert(quoteResponse(message.dump()).second==first.second&&quoteQueueCalls==2); // 8. Hot revocation/stop precede cached receipt access; restart preserves dedup.
    for(auto field:{"local_id","create_time"}){invalid=message;invalid["source"][field]=0;assert(rejects([&]{parseQuote(invalid);}));}
    invalid=message;invalid["source"]["server_id"]="18446744073709551616";assert(rejects([&]{parseQuote(invalid);}));
    invalid=message;invalid["source"]["create_time"]=static_cast<uint64_t>(std::time(nullptr))+100;assert(rejects([&]{parseQuote(invalid);}));
    invalid=message;invalid["atlist"]="wxid_member,<xml>";assert(rejects([&]{parseQuote(invalid);})); // 9. Confirmed numeric locators/time/mention metadata only.
    while(quoteAttempts.size()<kMaxAttempts)quoteAttempts.emplace("fixture-"+std::to_string(quoteAttempts.size()),std::make_pair(std::string(64,'0'),json::object()));
    invalid=message;invalid["request_id"]=std::string(64,'d');assert(quoteResponse(invalid.dump()).first==400&&quoteQueueCalls==2);
    assert(quoteResponse("not JSON").first==400); // 10. Bounded receipts and malformed JSON are rejected before submission.
    stop();assert(std::filesystem::remove(configPath));
    std::cout<<"PASS 10 offline quote checks; no WeChat process, hook or native action was used\n";
    return 0;
}
#else
void selftestHookCompatibility() {
    auto rejects = [](auto action) { try { action(); return false; } catch (const std::exception&) { return true; } };
    struct Scratch {
        unsigned char* data = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        ~Scratch() { if (data) VirtualFree(data, 0, MEM_RELEASE); }
    } scratch;
    assert(scratch.data);
    const auto entry = reinterpret_cast<uintptr_t>(scratch.data);
    const auto relay = entry + 64, target = entry + 128;
    static constexpr unsigned char jump[] = {0xFF,0x25,0,0,0,0};
    auto prepare = [&](const HookProfile& profile) {
        std::memset(scratch.data, 0xCC, 4096);
        std::memcpy(scratch.data, profile.original.data(), profile.original.size());
        scratch.data[0] = 0xE9;
        const auto displacement = static_cast<int32_t>(relay - (entry + 5));
        std::memcpy(scratch.data + 1, &displacement, sizeof(displacement));
        std::memcpy(reinterpret_cast<void*>(relay), jump, sizeof(jump));
        std::memcpy(reinterpret_cast<void*>(relay + 6), &target, sizeof(target));
        std::memcpy(reinterpret_cast<void*>(target), profile.legacyTargetPrefix.data(), profile.legacyTargetPrefix.size());
    };
    for (const auto& profile : kProfiles) {
        std::memcpy(scratch.data, profile.original.data(), profile.original.size());
        assert(existingHookTarget(entry, profile) == 0);
        prepare(profile); assert(existingHookTarget(entry, profile) == target);
        assert(legacyIdentityMatches(profile, kLegacyName, kLegacyHash, profile.legacyTargetRva, profile.legacyTargetPrefix.data()));
        assert(!legacyIdentityMatches(profile, L"unknown.dll", kLegacyHash, profile.legacyTargetRva, profile.legacyTargetPrefix.data()));
        assert(!legacyIdentityMatches(profile, kLegacyName, std::string(64,'0'), profile.legacyTargetRva, profile.legacyTargetPrefix.data()));
        assert(!legacyIdentityMatches(profile, kLegacyName, kLegacyHash, profile.legacyTargetRva + 1, profile.legacyTargetPrefix.data()));
        auto changedPrefix = profile.legacyTargetPrefix; changedPrefix[0] ^= 1;
        assert(!legacyIdentityMatches(profile, kLegacyName, kLegacyHash, profile.legacyTargetRva, changedPrefix.data()));
        // Executable private memory has the right bytes but cannot impersonate a loaded, pinned image.
        assert(rejects([&] { validateHookEntry(entry, profile); }));
        scratch.data[5] ^= 1; assert(rejects([&] { existingHookTarget(entry, profile); })); prepare(profile);
        scratch.data[0] = 0xEB; assert(rejects([&] { existingHookTarget(entry, profile); })); prepare(profile);
        scratch.data[66] = 1; assert(rejects([&] { existingHookTarget(entry, profile); })); prepare(profile);
        std::memcpy(scratch.data + 70, &entry, sizeof(entry));
        assert(rejects([&] { existingHookTarget(entry, profile); })); prepare(profile);
        const auto insideEntry = static_cast<int32_t>(-4); std::memcpy(scratch.data + 1, &insideEntry, sizeof(insideEntry));
        assert(rejects([&] { existingHookTarget(entry, profile); }));
    }
    assert(rejects([] { branchDestination(std::numeric_limits<uintptr_t>::max(), 0); }));
    assert(rejects([] { branchDestination(1, -10); }));
    DWORD previous = 0; assert(VirtualProtect(scratch.data, 4096, PAGE_READWRITE, &previous));
    assert(!executableReadable(entry, 9)); assert(VirtualProtect(scratch.data, 4096, previous, &previous));

    // Use the actual vendored MinHook decoder without enabling a hook or executing any scratch code.
    prepare(kProfiles[0]); std::array<unsigned char,9> before{}; std::memcpy(before.data(), scratch.data, before.size());
    const auto initialized = MH_Initialize(); assert(initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED);
    void* trampoline = nullptr;
    assert(MH_CreateHook(scratch.data, scratch.data + 256, &trampoline) == MH_OK);
    assert(trampoline && !std::memcmp(trampoline, jump, sizeof(jump)));
    uintptr_t retainedRelay = 0; std::memcpy(&retainedRelay, static_cast<unsigned char*>(trampoline) + 6, sizeof(retainedRelay));
    assert(retainedRelay == relay && !std::memcmp(scratch.data, before.data(), before.size()));
    assert(MH_RemoveHook(scratch.data) == MH_OK); // Only our own never-enabled fixture is removed.

    const auto hashFile = std::filesystem::temp_directory_path() / (L"actions-sha-offline-" + std::to_wstring(GetCurrentProcessId()));
    { std::ofstream file(hashFile, std::ios::binary); file << "abc"; }
    assert(fileSha256(hashFile) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(std::filesystem::remove(hashFile));
}
int main() {
    // This executable never resolves or calls a WeChat native address, loads a DLL,
    // opens a WeChat process, binds a network listener or submits an action.
    selftestHookCompatibility();
    constexpr char self[] = "wxid_offline_owner", other[] = "wxid_other_owner";
    constexpr char groupA[] = "12345678901@chatroom", groupB[] = "12345678902@chatroom";
    authToken = std::string(64, 'a'); initialSelf = self; hooksReady = true;
    configPath = std::filesystem::temp_directory_path() / (L"actions-offline-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    auto config = json({{"token",authToken},{"self_wxid",self},{"port",30111},{"groups",json::array({groupA,groupB})}});
    auto save = [&] {std::ofstream stream(configPath); stream << config;}; save();
    auto rejects = [](auto action) { try { action(); return false; } catch (const std::exception&) { return true; } };
    auto setting = readSetting(); assert(setting.groups.size() == 2); assert(setting.self == self);
    auto handle = CreateFileW(configPath.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(handle != INVALID_HANDLE_VALUE); CloseHandle(handle); // File handles must not block atomic config replacement.
    startObservation(); assert(enabled); // 1. Config, explicit start and closed config handle.

    alignas(8) unsigned char record[kRecordSize]{};
    auto set = [&](size_t offset, const char* text) {
        auto* value = reinterpret_cast<NativeString*>(record + offset);
        value->length = std::strlen(text); value->capacity = value->length < 16 ? 15 : value->length;
        if (value->capacity >= 16) value->pointer = const_cast<char*>(text);
        else {std::memset(value->small,0,16);std::memcpy(value->small,text,value->length);}
    };
    set(0x10,self);set(0x70,groupA);
    *reinterpret_cast<uint64_t*>(record+0xB8)=7;
    *reinterpret_cast<uint32_t*>(record+0x94)=9;
    *reinterpret_cast<uint32_t*>(record+0xD0)=1000;
    validateRecord(record,{groupA,7},1000,9,1179);
    assert(rejects([&]{validateRecord(record,{groupA,7},1000,9,1180);}));
    assert(rejects([&]{validateRecord(record,{groupA,7},1000,9,999);})); // 2. Exact three-minute boundary and future timestamps.
    set(0x10,other);assert(rejects([&]{validateRecord(record,{groupA,7},1000,9,1001);}));set(0x10,self);
    assert(rejects([&]{validateRecord(record,{groupB,7},1000,9,1001);}));
    assert(rejects([&]{validateRecord(record,{groupA,7},1000,8,1001);}));
    assert(rejects([&]{validateRecord(record,{groupA,7},1001,9,1001);})); // 3. Self, exact group and DB locator.

    auto now=static_cast<uint32_t>(std::time(nullptr));*reinterpret_cast<uint32_t*>(record+0xD0)=now;
    observe(record);assert(records.size()==1);
    set(0x70,groupB);observe(record);assert(records.size()==2); // 4. Same server ID in different selected groups cannot collide.
    config["groups"]=json::array({groupB});save();refreshSetting();assert(records.size()==1&&records.begin()->first.first==groupB);
    set(0x70,groupA);observe(record);assert(records.size()==1); // 5. Hot revocation releases records and blocks observation.

    auto request=parseRequest(json({{"group",groupB},{"server_id","7"},{"create_time",now},{"local_id",9},{"request_id",std::string(64,'b')}}));
    assert(recall(request)["status"]=="queued");assert(attempts.size()==1);
    assert(recall(request)["status"]=="queued");assert(attempts.size()==1);
    auto changed=request;changed.local=10;assert(rejects([&]{recall(changed);}));
    changed=request;changed.id=std::string(64,'c');assert(rejects([&]{recall(changed);}));
    changed=request;changed.id=std::string(64,'d');changed.record.second=8;assert(rejects([&]{recall(changed);})); // 6. Stable idempotency, mismatch, no-record failure.

    stop();assert(!enabled&&records.empty());assert(rejects([&]{recall(request);}));
    startObservation();assert(enabled); // 7. Stop and restart reuse the same module without unloading.
    config["self_wxid"]=other;save();assert(rejects([]{refreshSetting();}));assert(!enabled&&records.empty());
    config["self_wxid"]=self;config["token"]=std::string(64,'e');save();assert(rejects([]{refreshSetting();}));assert(!enabled); // 8. Account/token drift fails closed.
    config["token"]=authToken;save();startObservation();
    assert(rejects([&]{auto invalid=config;invalid["groups"]=json::array({"unselected"});parseSetting(invalid);}));
    assert(rejects([&]{parseRequest(json({{"group",groupB},{"server_id","18446744073709551616"},{"create_time",now},{"local_id",9},{"request_id",std::string(64,'b')}}));}));
    httplib::Request auth;httplib::Response response;auth.headers.emplace("Authorization","Bearer "+authToken);assert(authorized(auth,response));
    auth.headers.clear();assert(!authorized(auth,response)&&response.status==401); // 9. Strict config/request parsing and token.
    set(0x70,groupB);*reinterpret_cast<uint64_t*>(record+0xB8)=20;observe(record);
    auto uncertain=json({{"group",groupB},{"server_id","20"},{"create_time",now},{"local_id",9},{"request_id",std::string(64,'f')}});
    queueFailureForTest=true;auto result=recallResponse(uncertain.dump());assert(result.first==500&&result.second["status"]=="uncertain");
    queueFailureForTest=false;assert(recallResponse(uncertain.dump()).first==200);assert(attempts.size()==2);
    uncertain["local_id"]=10;assert(recallResponse(uncertain.dump()).first==400);assert(recallResponse("not JSON").first==400); // 10. Post-reservation failures remain uncertain and never resubmit.
    for(uint64_t id=100;id<150;++id){*reinterpret_cast<uint64_t*>(record+0xB8)=id;observe(record);}assert(records.size()==kMaxRecords);
    {std::lock_guard lock(recordsMutex);expire(static_cast<std::time_t>(now)+kWindow);}assert(records.empty()); // 11. Bounded cache and expiry release owned native copies.
    stop();std::filesystem::remove(configPath);
    std::cout << "PASS 15 offline action checks including pinned hook identity, malformed chain rejection, retained MinHook trampoline and SHA256; no WeChat process or native action was used\n";
    return 0;
}
#endif
#endif
