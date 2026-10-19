// CDN downloads and filtered completion receipts for Windows WeChat 4.1.15.13.
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include "httplib.h"
#include "json.hpp"
#include "MinHook.h"

#if defined(DOWNLOAD_SELFTEST) && defined(NDEBUG)
#error The offline self-test must keep assertions enabled.
#endif

namespace {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr uint64_t kMaxSize = 512ULL * 1024 * 1024;
constexpr uintptr_t kCtor = 0xA10C10, kDtor = 0xA13C70, kStringGrow = 0x3E150;
constexpr uintptr_t kContext = 0xDBB870, kManager = 0x236A2B0, kDownload = 0x236B4B0;
constexpr uintptr_t kImpl = 0x94DE7A8, kCallback = 0x94DE828, kOwner = 0x90B38F8;
uintptr_t base = 0;
HMODULE module = nullptr;
std::filesystem::path configPath, mediaRoot;
std::string authToken, failure;
bool enabled = true;
bool hooksReady = false;
std::mutex mutex, attemptsMutex;
struct Attempt {
    std::string fingerprint;
    Clock::time_point time;
    bool finished = false;
    int code = 0;
    uint64_t size = 0;
    std::string md5;
};
std::map<std::string, Attempt> attempts;

bool hex(const std::string& value, size_t size) {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
bool groupId(const std::string& group) {
    constexpr char suffix[] = "@chatroom";
    return group.size() > 9 && group.size() <= 137 && group.ends_with(suffix) &&
        std::all_of(group.begin(), group.end() - 9, [](unsigned char c) { return c >= '0' && c <= '9'; });
}
std::filesystem::path utf8Path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}
std::string fingerprint(const std::string& input) {
    unsigned char digest[32];
    if (input.size() > ULONG_MAX || BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), digest, sizeof(digest)) < 0)
        throw std::runtime_error("download fingerprint unavailable");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
struct Setting { std::string token; int port; std::set<std::string> groups; std::filesystem::path root; };
Setting readSetting() {
    if (std::filesystem::is_symlink(configPath) || std::filesystem::file_size(configPath) > 1024 * 1024)
        throw std::invalid_argument("invalid download configuration file");
    std::ifstream stream(configPath);
    const auto config = json::parse(stream);
    Setting result{config.at("token").get<std::string>(), config.at("port").get<int>(), {},
                   std::filesystem::canonical(utf8Path(config.at("media_root").get<std::string>()))};
    if (!hex(result.token, 64) || result.port < 1 || result.port > 65535 || config.value("capability", "") != "media_download")
        throw std::invalid_argument("invalid download configuration");
    const auto& groups = config.at("groups");
    if (!groups.is_array() || groups.size() > 4096) throw std::invalid_argument("invalid download groups");
    for (const auto& item : groups) {
        const auto group = item.get<std::string>();
        if (!groupId(group)) throw std::invalid_argument("invalid download group");
        result.groups.insert(group);
    }
    return result;
}
bool authorized(const httplib::Request& request) {
    const auto expected = "Bearer " + authToken, actual = request.get_header_value("Authorization");
    size_t different = expected.size() ^ actual.size();
    for (size_t i = 0; i < expected.size(); ++i) different |= expected[i] ^ (i < actual.size() ? actual[i] : 0);
    return !authToken.empty() && !different;
}
struct Request {
    std::string group, id, file, aes, kind;
    uint64_t length;
    int type;
};
Request parse(const json& body) {
    Request result{body.at("group").get<std::string>(), body.at("request_id").get<std::string>(),
        body.at("file_id").get<std::string>(), body.at("aes_key").get<std::string>(),
        body.at("kind").get<std::string>(), body.at("length").get<uint64_t>(), 0};
    if (!groupId(result.group) || !hex(result.id, 64) || !hex(result.aes, 32) ||
        result.file.size() < 32 || result.file.size() > 4096 || result.file.size() % 2 || !hex(result.file, result.file.size()) ||
        !result.length || result.length > kMaxSize) throw std::invalid_argument("invalid media download parameters");
    // The native task factory dispatches image=2, stream=4/6 and generic attachment=5.
    if (result.kind == "image") result.type = 1;
    else if (result.kind == "image_mid") result.type = 2;
    else if (result.kind == "file") result.type = 5;
    else if (result.kind == "video") result.type = 4;
    else if (result.kind == "video_original") result.type = 6;
    else throw std::invalid_argument("unsupported media download kind");
    if (result.file.starts_with("7f0c")) result.type = 7;
    return result;
}
bool readable(const void* object, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    const auto start = reinterpret_cast<uintptr_t>(object);
    return object && start <= UINTPTR_MAX - size && VirtualQuery(object, &info, sizeof(info)) && info.State == MEM_COMMIT &&
        !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && start + size <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}
template<class Fn> Fn native(uintptr_t rva) { return reinterpret_cast<Fn>(base + rva); }
struct NativeString { union { char small[16]; char* pointer; }; uint64_t length, capacity; };
struct Shared { void* payload; void* owner; };
static_assert(sizeof(NativeString) == 32 && sizeof(Shared) == 16);
std::string readString(const NativeString* value, size_t limit) {
    if (!readable(value, sizeof(*value)) || value->length > limit || value->capacity < value->length)
        throw std::runtime_error("invalid CDN receipt string");
    const auto data = value->capacity >= 16 ? value->pointer : value->small;
    if (!readable(data, static_cast<size_t>(value->length))) throw std::runtime_error("unreadable CDN receipt string");
    return {data, static_cast<size_t>(value->length)};
}
using Finished = uintptr_t(*)(void*, const NativeString*, const void*);
Finished originalFinished = nullptr;
void observeFinished(const NativeString* key, const void* result) {
    const auto id = readString(key, 64);
    std::lock_guard lock(attemptsMutex);
    const auto found = attempts.find(id);
    if (found == attempts.end() || !readable(result, 0x110)) return;
    const auto bytes = static_cast<const unsigned char*>(result);
    auto& attempt = found->second;
    attempt.finished = true;
    attempt.code = *reinterpret_cast<const int*>(bytes + 4);
    if (*reinterpret_cast<const int*>(bytes) != 0 && !attempt.code) attempt.code = -1;
    if (attempt.code) return;
    const auto size = *reinterpret_cast<const uint64_t*>(bytes + 0x88);
    const auto md5 = readString(reinterpret_cast<const NativeString*>(bytes + 0xF0), 32);
    if (!size || size > kMaxSize || !hex(md5, 32)) { attempt.code = -1; return; }
    attempt.size = size; attempt.md5 = md5;
}
uintptr_t finished(void* self, const NativeString* key, const void* result) {
    try { observeFinished(key, result); } catch (...) {}
    return originalFinished(self, key, result);
}
struct CdnRequest {
    alignas(8) unsigned char bytes[0x588]{};
    CdnRequest() { native<void*(*)(void*)>(kCtor)(bytes); }
    ~CdnRequest() { native<void(*)(void*)>(kDtor)(bytes); }
    CdnRequest(const CdnRequest&) = delete;
};
void assign(void* object, size_t offset, const std::string& value) {
    auto target = reinterpret_cast<NativeString*>(static_cast<char*>(object) + offset);
    if (value.size() > target->capacity)
        native<void*(*)(NativeString*, uint64_t, uint64_t, const char*)>(kStringGrow)(target, value.size(), 0, value.data());
    else {
        auto data = target->capacity >= 16 ? target->pointer : target->small;
        std::memcpy(data, value.data(), value.size()); data[value.size()] = 0; target->length = value.size();
    }
}
void queue(const Request& input, const std::string& path) {
    CdnRequest request;
    NativeString name{}; std::memcpy(name.small, "default", 8); name.length = 7; name.capacity = 15;
    auto context = native<void*(*)(NativeString*)>(kContext)(&name);
    auto manager = context ? native<void*(*)(void*)>(kManager)(context) : nullptr;
    if (!readable(manager, 0x58)) throw std::runtime_error("WeChat CDN manager not ready");
    auto impl = *reinterpret_cast<char**>(static_cast<char*>(manager) + 0x30);
    if (!readable(impl, 0x20) || *reinterpret_cast<uintptr_t*>(impl) != base + kImpl ||
        *reinterpret_cast<uintptr_t*>(impl + 8) != base + kCallback) throw std::runtime_error("WeChat CDN callback layout mismatch");
    auto owner = *reinterpret_cast<char**>(impl + 0x18);
    if (!readable(owner, 0x20) || owner + 0x10 != impl || *reinterpret_cast<uintptr_t*>(owner) != base + kOwner)
        throw std::runtime_error("WeChat CDN callback ownership mismatch");
    auto count = reinterpret_cast<volatile LONG*>(owner + 8);
    LONG current = InterlockedCompareExchange(count, 0, 0);
    for (;;) {
        if (current <= 0 || current == LONG_MAX) throw std::runtime_error("WeChat CDN callback expired");
        const LONG seen = InterlockedCompareExchange(count, current + 1, current);
        if (seen == current) break;
        current = seen;
    }
    // The native request destructor owns this retained reference and all copied strings.
    auto callback = reinterpret_cast<Shared*>(request.bytes);
    callback->payload = impl + 8; callback->owner = owner;
    assign(request.bytes, 0x50, input.id); assign(request.bytes, 0x70, input.file);
    assign(request.bytes, 0x90, input.aes); assign(request.bytes, 0xB0, path);
    *reinterpret_cast<uint32_t*>(request.bytes + 0xD0) = input.type;
    *reinterpret_cast<uint64_t*>(request.bytes + 0x198) = input.length;
    request.bytes[0x328] = 1; request.bytes[0x329] = 1;
    const auto result = native<int(*)(void*)>(kDownload)(request.bytes);
    if (result) throw std::runtime_error("WeChat CDN rejected download: " + std::to_string(result));
}
json download(const json& body) {
    const auto input = parse(body);
    const auto setting = readSetting();
    if (!enabled || !failure.empty() || setting.token != authToken || setting.root != mediaRoot || !setting.groups.contains(input.group))
        throw std::invalid_argument("media download is not authorized");
    const auto directory = mediaRoot / L"wechat-download";
    std::filesystem::create_directory(directory);
    const auto folder = std::filesystem::canonical(directory);
    if (folder.parent_path() != mediaRoot) throw std::invalid_argument("download directory outside media root");
    const auto path = folder / (input.id + ".part");
    if (std::filesystem::is_symlink(path) || (std::filesystem::exists(path) && std::filesystem::canonical(path).parent_path() != folder))
        throw std::invalid_argument("download path outside media root");
    const auto signature = fingerprint(body.dump());
    const auto now = Clock::now();
    {
        std::lock_guard lock(attemptsMutex);
        const auto previous = attempts.find(input.id);
        if (previous != attempts.end()) {
            const auto& attempt = previous->second;
            if (attempt.fingerprint != signature) throw std::invalid_argument("download request_id reused");
            if (attempt.finished && !attempt.code)
                return {{"ret",0},{"status","complete"},{"path",utf8(path)},{"request_id",input.id},{"size",attempt.size},{"md5",attempt.md5}};
            if (now - attempt.time < std::chrono::minutes(5))
                return {{"ret",0},{"status",attempt.finished ? "failed" : "downloading"},{"path",utf8(path)},
                        {"request_id",input.id},{"code",attempt.code}};
        }
        if (attempts.size() >= 4096 && previous == attempts.end()) {
            std::erase_if(attempts, [&](const auto& item) { return item.second.finished && now - item.second.time >= std::chrono::minutes(5); });
            if (attempts.size() >= 4096) throw std::runtime_error("download queue is full");
        }
        attempts[input.id] = {signature, now};
    }
    queue(input, utf8(path));
    return {{"ret", 0}, {"status", "downloading"}, {"path", utf8(path)}, {"request_id",input.id}};
}
void initialize() {
    base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Weixin.dll"));
    if (!readable(reinterpret_cast<void*>(base), 4096)) throw std::runtime_error("Weixin.dll not present");
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096) throw std::runtime_error("invalid Weixin PE");
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != 1789961080 || nt->OptionalHeader.SizeOfImage != 202039296)
        throw std::runtime_error("unsupported WeChat native module");
    auto check = [](uintptr_t rva, std::initializer_list<unsigned char> bytes) {
        if (!readable(reinterpret_cast<void*>(base + rva), bytes.size()) ||
            !std::equal(bytes.begin(), bytes.end(), reinterpret_cast<const unsigned char*>(base + rva)))
            throw std::runtime_error("native download fingerprint mismatch");
    };
    check(kCtor, {0x55,0x41,0x57,0x41,0x56,0x56,0x57,0x53});
    check(kDtor, {0x56,0x57,0x53,0x48,0x83,0xEC,0x20});
    check(kStringGrow, {0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53});
    check(kContext, {0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53});
    check(kManager, {0x55,0x41,0x57,0x41,0x56,0x56,0x57,0x53});
    check(kDownload, {0x55,0x56,0x57,0x48,0x81,0xEC,0x60,0x01,0,0});
    // Prove the constructor/destructor pair before accepting any downloads.
    { CdnRequest probe; }
    // One verified callback is observed without changing its arguments or native result.
    // Foreign hook chains require a normal WeChat restart, never guessed trampolines.
    check(0x3790AC0, {0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0x78,0x05,0,0});
    const auto status = MH_Initialize();
    if ((status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(reinterpret_cast<void*>(base + 0x3790AC0), finished, reinterpret_cast<void**>(&originalFinished)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(base + 0x3790AC0)) != MH_OK)
        throw std::runtime_error("native CDN completion observer unavailable; restart WeChat after upgrading");
    hooksReady = true;
}
DWORD WINAPI run(void*) {
    try {
        wchar_t path[32768]; const auto length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        if (!length || length >= std::size(path)) return 1;
        configPath = std::filesystem::path(path).parent_path() / L"native.json";
        const auto setting = readSetting(); authToken = setting.token; mediaRoot = setting.root;
        try { initialize(); } catch (const std::exception& error) { failure = error.what(); }
        httplib::Server api;
        api.new_task_queue = [] { return new httplib::ThreadPool(1); };
        api.set_payload_max_length(16384); api.set_read_timeout(5); api.set_write_timeout(5); api.set_keep_alive_max_count(2);
        api.set_pre_routing_handler([](const auto& request, auto& response) {
            if (authorized(request)) return httplib::Server::HandlerResponse::Unhandled;
            response.status = 401; response.set_content("{\"error\":\"unauthorized\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        });
        api.Get("/health", [](const auto&, auto& response) {
            std::lock_guard lock(mutex);
            response.set_content(json({{"pid",GetCurrentProcessId()},{"version","4.1.15.13"},{"api_version",1},
                {"capabilities",json::array({"media_download","download_receipts"})},{"max_size",kMaxSize},{"hooks_installed",hooksReady},
                {"enabled",enabled},{"error",failure}}).dump(), "application/json");
        });
        api.Post("/start", [](const auto&, auto& response) { std::lock_guard lock(mutex); enabled = true; response.set_content("{\"enabled\":true}", "application/json"); });
        api.Post("/stop", [](const auto&, auto& response) { std::lock_guard lock(mutex); enabled = false; response.set_content("{\"disabled\":true}", "application/json"); });
        api.Post("/DownloadMedia", [](const auto& request, auto& response) {
            std::lock_guard lock(mutex);
            try { response.set_content(download(json::parse(request.body)).dump(), "application/json"); }
            catch (const std::exception& error) { response.status = 400; response.set_content(json({{"ret",-1},{"error",error.what()}}).dump(), "application/json"); }
        });
        api.listen("127.0.0.1", setting.port);
        return 0;
    } catch (...) { return 9; }
}
}

#ifndef DOWNLOAD_SELFTEST
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        module = instance; DisableThreadLibraryCalls(instance);
        auto thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
#else
int main() {
    const auto body = json({{"group","123456@chatroom"},{"request_id",std::string(64,'a')},
        {"file_id",std::string(194,'b')},{"aes_key",std::string(32,'c')},{"length",kMaxSize},{"kind","file"}});
    assert(parse(body).type == 5);
    for (const auto& [kind, type] : std::map<std::string,int>{{"image",1},{"image_mid",2},{"video",4},{"video_original",6}}) {
        auto input = body; input["kind"] = kind; assert(parse(input).type == type);
    }
    for (const auto& change : std::vector<json>{{{"length",0}},{{"length",kMaxSize+1}},{{"length",-1}},{{"kind","unknown"}},
             {{"group","someone"}},{{"file_id","https://example.invalid"}},{{"aes_key","invalid"}},{{"request_id","../escape"}}}) {
        auto input = body; input.update(change); bool rejected = false;
        try { parse(input); } catch (...) { rejected = true; } assert(rejected);
    }
    assert(fingerprint("hello") == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    auto big = body; big["file_id"] = "7f0c" + std::string(60,'0'); assert(parse(big).type == 7);
    NativeString id{}; const std::string key(64,'a'); id.pointer = const_cast<char*>(key.data()); id.length = 64; id.capacity = 64;
    alignas(8) unsigned char result[0x110]{};
    const std::string digest(32,'b');
    auto hash = reinterpret_cast<NativeString*>(result + 0xF0);
    hash->pointer = const_cast<char*>(digest.data()); hash->length = 32; hash->capacity = 32;
    *reinterpret_cast<uint64_t*>(result + 0x88) = 843587;
    // A real CDN completion may describe a transcoded video with different XML metadata.
    attempts[key] = {"offline",Clock::now()}; observeFinished(&id,result);
    assert(attempts[key].finished && attempts[key].code == 0 && attempts[key].size == 843587 && attempts[key].md5 == digest);
    *reinterpret_cast<int*>(result + 4) = -5103017; observeFinished(&id,result); assert(attempts[key].code == -5103017);
    attempts.clear(); observeFinished(&id,result); assert(attempts.empty());
    // The parser and fingerprint tests never initialize WeChat or enter a native queue.
    assert(base == 0 && attempts.empty() && !hooksReady);
    return 0;
}
#endif
