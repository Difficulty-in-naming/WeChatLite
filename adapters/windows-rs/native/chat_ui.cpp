// Runs in the adapter's own process. No native Weixin record/queue calls or DLL injection.
// This file is reached only from an explicit recall submission, never polling/startup.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C" {
struct WlUiText { const uint16_t *text; size_t length; uint32_t outgoing; uint32_t target; };
struct WlUiUniqueContext { size_t start, length; };
struct WlUiAccountRoot { uintptr_t landmark, root_slot, root, config; };
struct WlUiAccount {
    uint32_t pid;
    uintptr_t module_base;
    const uint8_t *wxid;
    size_t wxid_length;
    const WlUiAccountRoot *roots;
    size_t root_count;
};
struct WlUiRequest {
    uint32_t pid;
    uintptr_t base;
    const uint16_t *group;
    size_t group_length;
    const uint16_t *text;
    size_t text_length;
    int64_t create_time;
    const WlUiText *context;
    size_t context_length;
    const WlUiUniqueContext *unique_contexts;
    size_t unique_context_count;
    const void *history_context;
    uint32_t (*validate_history)(const void *context);
    WlUiAccount account;
};
}

namespace {
using Clock = std::chrono::steady_clock;
enum Error : uint32_t { Invalid = 1, Profile, Accessibility, Navigation, Interrupted, Mapping, Popup, NoRecall, InvokeFailed, RestoreFailed, Busy, AccountChanged, HistoryChanged };
struct Failure { Error code; };
void require(bool value, Error code) { if (!value) throw Failure{code}; }
void check(HRESULT value, Error code) { require(SUCCEEDED(value), code); }
void verifyHistory(const WlUiRequest &request) {
    require(request.history_context && request.validate_history,Invalid);
    require(request.validate_history(request.history_context)==0,HistoryChanged);
}
std::wstring utf16(const uint16_t *text, size_t length, size_t maximum = 65536) {
    require(text && length && length <= maximum, Invalid);
    std::wstring value;
    value.reserve(length);
    for (size_t i = 0; i < length; ++i) { require(text[i] != 0, Invalid); value.push_back(wchar_t(text[i])); }
    return value;
}
std::wstring bstr(BSTR text) {
    const size_t length = text ? SysStringLen(text) : 0;
    if (length > 65536) { SysFreeString(text); throw Failure{Invalid}; }
    std::wstring value(text ? text : L"", length);
    SysFreeString(text);
    return value;
}
std::wstring name(IUIAutomationElement *element) {
    BSTR text = nullptr; check(element->get_CurrentName(&text), Accessibility); return bstr(text);
}
std::wstring aid(IUIAutomationElement *element) {
    BSTR text = nullptr; check(element->get_CurrentAutomationId(&text), Accessibility); return bstr(text);
}
std::wstring cls(IUIAutomationElement *element) {
    BSTR text = nullptr; check(element->get_CurrentClassName(&text), Accessibility); return bstr(text);
}
std::wstring label(const std::wstring &text) {
    auto result = text.substr(0, text.find(L'\n'));
    if (!result.empty() && result.back() == L'\r') result.pop_back();
    return result;
}
bool aidPart(const std::wstring &value, const std::wstring &part) {
    size_t at = 0;
    while ((at = value.find(part, at)) != std::wstring::npos) {
        const size_t end = at + part.size();
        if ((at == 0 || value[at - 1] == L'.') && (end == value.size() || value[end] == L'.')) return true;
        ++at;
    }
    return false;
}
DWORD lastInput() { LASTINPUTINFO value{sizeof(value)}; require(GetLastInputInfo(&value) != FALSE, Interrupted); return value.dwTime; }
bool keyDown(SHORT state) { return (state & 0x8000)!=0; }
bool mouseHeld() {
    for (int key : {VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2}) if (keyDown(GetAsyncKeyState(key))) return true;
    return false;
}
bool inputHeld() {
    for (int key : {VK_CONTROL,VK_SHIFT,VK_MENU,VK_LWIN,VK_RWIN}) if (keyDown(GetAsyncKeyState(key))) return true;
    return mouseHeld();
}
bool samePid(HWND window, DWORD pid) { DWORD actual = 0; return IsWindow(window) && GetWindowThreadProcessId(window, &actual) && actual == pid; }
bool validAge(int64_t created, int64_t now) { return created > 0 && now >= created && now - created < 180; }
uint32_t outcome(bool invoked, bool success) { return success ? 0 : (invoked ? 2 : 1); }
struct Text { std::wstring value; bool outgoing; bool target; };

// Access has proved these 1..3-text windows unique in the complete group history.
// A native occurrence must be unique too, all rows fully visible, and every
// visible proof must locate the same target. No tail/bottom inference is needed.
size_t mapText(const std::vector<Text> &db, const std::vector<WlUiUniqueContext> &proofs,
    const std::vector<std::wstring> &ui, const std::vector<bool> &visible) {
    require(!db.empty() && db.size() <= 200 && !ui.empty() && ui.size() <= 400, Mapping);
    require(!proofs.empty() && proofs.size() <= 6 && visible.size()==ui.size(),Mapping);
    size_t target = SIZE_MAX, targetCount = 0;
    for (size_t i = 0; i < db.size(); ++i) if (db[i].target) { target = i; ++targetCount; }
    require(targetCount == 1 && db[target].outgoing && !db[target].value.empty(), Mapping);
    size_t result=SIZE_MAX;
    std::vector<WlUiUniqueContext> seen;
    for (const auto &proof:proofs) {
        require(proof.length>=1 && proof.length<=3 && proof.length<=db.size()
            && proof.start<=db.size()-proof.length && proof.start<=target && target-proof.start<proof.length,Mapping);
        for (const auto &prior:seen) require(prior.start!=proof.start || prior.length!=proof.length,Mapping);
        seen.push_back(proof);
        size_t occurrence=SIZE_MAX, count=0;
        for (size_t start=0;start+proof.length<=ui.size();++start) {
            bool equal=true;
            for (size_t offset=0;offset<proof.length;++offset) equal &= ui[start+offset]==db[proof.start+offset].value;
            if (equal) { occurrence=start; ++count; }
        }
        require(count<=1,Mapping); // Even a clipped duplicate makes this native view ambiguous.
        if (count==0) continue;
        bool fullyVisible=true;
        for (size_t offset=0;offset<proof.length;++offset) fullyVisible=fullyVisible && visible[occurrence+offset];
        if (!fullyVisible) continue;
        const size_t candidate=occurrence+target-proof.start;
        require(result==SIZE_MAX || result==candidate,Mapping);
        result=candidate;
    }
    require(result!=SIZE_MAX,Mapping);
    return result;
}
POINT ownRowPoint(const RECT &bounds, UINT dpi) {
    require(dpi>=48 && dpi<=960 && bounds.bottom>bounds.top,Navigation);
    // The provider exposes only a full-width row, not the bubble geometry. Use
    // one conservative candidate before the right-hand avatar, never scan. An
    // exact row hit and the native enabled recall menu remain mandatory. This
    // layout-derived candidate has not been live-validated for every scale.
    POINT point{bounds.right-MulDiv(88,int(dpi),96), bounds.top+(bounds.bottom-bounds.top)/2};
    require(PtInRect(&bounds,point)!=FALSE,Navigation); return point;
}

constexpr uintptr_t GateRva = 0xB135C38;
constexpr uint8_t Getter[]{0x0f,0xb6,0x05,0x71,0xb8,0x8e,0x0a,0xc3};
constexpr uint8_t Setter[]{0x88,0x0d,0x62,0xb8,0x8e,0x0a,0xc3};
constexpr uint8_t Query[]{0x80,0x3d,0x66,0xc0,0x8e,0x0a,0x00};
bool profileBytes(const uint8_t *getter, const uint8_t *setter, const uint8_t *query) {
    return std::memcmp(getter, Getter, sizeof(Getter)) == 0 && std::memcmp(setter, Setter, sizeof(Setter)) == 0
        && std::memcmp(query, Query, sizeof(Query)) == 0;
}
bool validGate(DWORD protection, DWORD state, DWORD type, uint8_t value) {
    return protection == PAGE_READWRITE && state == MEM_COMMIT && type == MEM_IMAGE && value <= 1;
}
struct Process {
    HANDLE value;
    explicit Process(DWORD pid) : value(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | SYNCHRONIZE, FALSE, pid)) { require(value != nullptr, Profile); }
    ~Process() { if (value) CloseHandle(value); }
    void read(uintptr_t address, void *target, size_t length) {
        SIZE_T count = 0;
        require(ReadProcessMemory(value, reinterpret_cast<void *>(address), target, length, &count) && count == length, Profile);
    }
};
struct InterprocessLock {
    HANDLE value=nullptr;
    bool acquired=false;
    explicit InterprocessLock(const std::wstring &key) : value(CreateMutexW(nullptr,FALSE,key.c_str())) {
        require(value!=nullptr,Busy);
        const DWORD result=WaitForSingleObject(value,0);
        acquired=result==WAIT_OBJECT_0 || result==WAIT_ABANDONED;
        if (!acquired) { CloseHandle(value); value=nullptr; throw Failure{Busy}; }
    }
    ~InterprocessLock() { if (value) { if (acquired) ReleaseMutex(value); CloseHandle(value); } }
};
uint64_t integer(const uint8_t *bytes) { uint64_t value = 0; std::memcpy(&value,bytes,sizeof(value)); return value; }
bool imageAddress(Process &process, uintptr_t base, uintptr_t address, size_t length) {
    if (!address || address > UINTPTR_MAX-length) return false;
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQueryEx(process.value,reinterpret_cast<void *>(address),&info,sizeof(info)) == sizeof(info)
        && reinterpret_cast<uintptr_t>(info.AllocationBase) == base && info.Type == MEM_IMAGE && info.State == MEM_COMMIT
        && address+length <= reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
}
template<class Read> std::string accountString(Read &read, uintptr_t config) {
    uint8_t record[32]{}; read(config+0x48,record,sizeof(record));
    const uint64_t length=integer(record+16), capacity=integer(record+24);
    require(length > 0 && length <= 127 && capacity >= length, AccountChanged);
    std::string value(size_t(length),'\0');
    if (capacity <= 15) std::memcpy(value.data(),record,size_t(length));
    else read(uintptr_t(integer(record)),value.data(),size_t(length));
    return value;
}
template<class Read, class Image> void verifyAccountData(const WlUiRequest &request, Read read, Image image) {
    const auto &account=request.account;
    require(account.pid == request.pid && account.module_base == request.base && account.wxid && account.wxid_length > 0
        && account.wxid_length <= 127 && account.roots && account.root_count > 0 && account.root_count <= 16, AccountChanged);
    const std::string expected(reinterpret_cast<const char *>(account.wxid),account.wxid_length);
    for (size_t i=0;i<account.root_count;++i) {
        const auto &root=account.roots[i];
        require(root.landmark >= 0x128 && (root.root_slot == root.landmark-0x128 || root.root_slot == root.landmark-0x120), AccountChanged);
        require(image(root.landmark,32) && image(root.root_slot,sizeof(uintptr_t)), AccountChanged);
        uint8_t landmark[32]{}; read(root.landmark,landmark,sizeof(landmark));
        require(std::memcmp(landmark,"global_config",13) == 0 && integer(landmark+16) == 13 && integer(landmark+24) == 15, AccountChanged);
        uintptr_t liveRoot=0, liveConfig=0;
        read(root.root_slot,&liveRoot,sizeof(liveRoot)); require(liveRoot == root.root && liveRoot != 0, AccountChanged);
        read(liveRoot+0x68,&liveConfig,sizeof(liveConfig)); require(liveConfig == root.config && liveConfig != 0, AccountChanged);
        require(accountString(read,liveConfig) == expected, AccountChanged);
        uintptr_t repeatedRoot=0,repeatedConfig=0;
        read(root.root_slot,&repeatedRoot,sizeof(repeatedRoot));
        read(liveRoot+0x68,&repeatedConfig,sizeof(repeatedConfig));
        require(repeatedRoot == liveRoot && repeatedConfig == liveConfig && accountString(read,liveConfig) == expected, AccountChanged);
    }
}
void verifyAccount(Process &process, const WlUiRequest &request) {
    auto read=[&](uintptr_t address,void *out,size_t length) { process.read(address,out,length); };
    auto image=[&](uintptr_t address,size_t length) { return imageAddress(process,request.base,address,length); };
    try { verifyAccountData(request,read,image); }
    catch (const Failure &) { throw Failure{AccountChanged}; }
}
struct Gate {
    Process &process;
    uintptr_t address;
    uint8_t original = 255;
    bool armed = false;
    Gate(Process &p, uintptr_t base) : process(p), address(base + GateRva) {
        uint8_t getter[sizeof(Getter)]{}, setter[sizeof(Setter)]{}, query[sizeof(Query)]{};
        p.read(base + 0x84A3C0, getter, sizeof(getter));
        p.read(base + 0x84A3D0, setter, sizeof(setter));
        p.read(base + 0x849BCB, query, sizeof(query));
        require(profileBytes(getter, setter, query), Profile);
        MEMORY_BASIC_INFORMATION info{};
        require(VirtualQueryEx(p.value, reinterpret_cast<void *>(address), &info, sizeof(info)) == sizeof(info)
                && reinterpret_cast<uintptr_t>(info.AllocationBase) == base, Profile);
        p.read(address, &original, 1);
        require(validGate(info.Protect, info.State, info.Type, original), Profile);
    }
    void enable() {
        if (original == 1) return;
        uint8_t enabled = 1; SIZE_T count = 0;
        armed = true; // Restoration remains armed even if the write reports an error.
        require(WriteProcessMemory(process.value, reinterpret_cast<void *>(address), &enabled, 1, &count) && count == 1, Accessibility);
        uint8_t actual = 255; process.read(address, &actual, 1); require(actual == 1, Accessibility);
    }
    bool restore() noexcept {
        if (!armed) return true;
        SIZE_T count = 0, read = 0; uint8_t actual = 255;
        bool ok = WriteProcessMemory(process.value, reinterpret_cast<void *>(address), &original, 1, &count) && count == 1
            && ReadProcessMemory(process.value, reinterpret_cast<void *>(address), &actual, 1, &read) && read == 1 && actual == original;
        if (ok) armed = false;
        return ok;
    }
    ~Gate() { if (armed) restore(); }
};
struct Com {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com() { check(initialized, Accessibility); }
    ~Com() { if (SUCCEEDED(initialized)) CoUninitialize(); }
};
struct Dpi {
    DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Dpi() { require(old != nullptr,Accessibility); }
    ~Dpi() { if (old) SetThreadDpiAwarenessContext(old); }
};
struct MainWindow { DWORD pid; HWND value = nullptr; unsigned count = 0; };
BOOL CALLBACK findMain(HWND window, LPARAM opaque) {
    auto &result = *reinterpret_cast<MainWindow *>(opaque);
    if (!samePid(window, result.pid) || GetWindow(window, GW_OWNER) || !IsWindowVisible(window)) return TRUE;
    wchar_t className[128]{};
    GetClassNameW(window, className, 128);
    if (std::wstring(className) == L"Qt51514QWindowIcon") { result.value = window; ++result.count; }
    return TRUE;
}
struct Row { ComPtr<IUIAutomationElement> element; std::wstring text; RECT rect{}; bool fullyVisible=false; };

class Session {
    DWORD pid;
    HWND window;
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationTreeWalker> walker;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(18);
    HWND foreground = GetForegroundWindow();
    POINT cursor{};
    std::wstring originalChat;
    DWORD expectedInput = 0;
    POINT expectedCursor{};
    bool controlsForeground = false, interrupted = false, minimized = false;
    HWND menuWindow = nullptr;
    std::wstring requestedGroup;

    bool owned(HWND candidate) const {
        if (!samePid(candidate, pid)) return false;
        for (unsigned i = 0; candidate && i < 12; ++i) {
            if (candidate == window || IsChild(window, candidate)) return true;
            candidate = GetWindow(candidate, GW_OWNER);
        }
        return false;
    }
    bool stillOwned() {
        if (inputHeld()) interrupted=true;
        if (!controlsForeground) return !interrupted;
        POINT point{};
        if (!GetCursorPos(&point) || lastInput() != expectedInput || point.x != expectedCursor.x || point.y != expectedCursor.y
            || !owned(GetForegroundWindow())) interrupted = true;
        return !interrupted;
    }
    void checkpoint() {
        require(Clock::now() < deadline && samePid(window, pid), Interrupted);
        require(stillOwned(), Interrupted);
    }
    ComPtr<IUIAutomationElement> root(HWND handle = nullptr) {
        ComPtr<IUIAutomationElement> value;
        check(automation->ElementFromHandle(handle ? handle : window, &value), Accessibility);
        return value;
    }
    std::vector<ComPtr<IUIAutomationElement>> find(IUIAutomationElement *parent, PROPERTYID property, const VARIANT &value) {
        ComPtr<IUIAutomationCondition> condition; ComPtr<IUIAutomationElementArray> elements;
        check(automation->CreatePropertyCondition(property, value, &condition), Accessibility);
        check(parent->FindAll(TreeScope_Subtree, condition.Get(), &elements), Accessibility);
        int length = 0; check(elements->get_Length(&length), Accessibility);
        require(length >= 0 && length <= 1200, Accessibility);
        std::vector<ComPtr<IUIAutomationElement>> out;
        for (int i = 0; i < length; ++i) {
            checkpoint(); ComPtr<IUIAutomationElement> element; int actualPid = 0;
            check(elements->GetElement(i, &element), Accessibility);
            check(element->get_CurrentProcessId(&actualPid), Accessibility);
            require(actualPid == int(pid), Accessibility); out.push_back(element);
        }
        return out;
    }
    std::vector<ComPtr<IUIAutomationElement>> byClass(const wchar_t *value, IUIAutomationElement *parent = nullptr) {
        auto base = parent ? ComPtr<IUIAutomationElement>(parent) : root();
        VARIANT property{}; property.vt = VT_BSTR; property.bstrVal = SysAllocString(value);
        std::vector<ComPtr<IUIAutomationElement>> out;
        try { out = find(base.Get(), UIA_ClassNamePropertyId, property); }
        catch (...) { VariantClear(&property); throw; }
        VariantClear(&property); return out;
    }
    std::vector<ComPtr<IUIAutomationElement>> byType(CONTROLTYPEID value, IUIAutomationElement *parent) {
        VARIANT property{}; property.vt = VT_I4; property.lVal = value; return find(parent, UIA_ControlTypePropertyId, property);
    }
    bool ancestor(IUIAutomationElement *child, IUIAutomationElement *parent) {
        ComPtr<IUIAutomationElement> current(child);
        for (unsigned depth = 0; current && depth < 48; ++depth) {
            int actualPid = 0; check(current->get_CurrentProcessId(&actualPid), Accessibility); require(actualPid == int(pid), Accessibility);
            BOOL equal = FALSE; check(automation->CompareElements(current.Get(), parent, &equal), Accessibility);
            if (equal) return true;
            ComPtr<IUIAutomationElement> next;
            check(walker->GetParentElement(current.Get(), &next), Accessibility); current = next;
        }
        return false;
    }
    bool hasAncestorAid(IUIAutomationElement *element, const std::wstring &part) {
        ComPtr<IUIAutomationElement> current(element);
        for (unsigned depth = 0; current && depth < 48; ++depth) {
            if (aidPart(aid(current.Get()), part)) return true;
            ComPtr<IUIAutomationElement> next;
            check(walker->GetParentElement(current.Get(), &next), Accessibility); current = next;
        }
        return false;
    }
    ComPtr<IUIAutomationElement> exactSession(const std::wstring &target) {
        ComPtr<IUIAutomationElement> result; unsigned count = 0;
        for (auto &element : byClass(L"mmui::ChatSessionCell")) {
            if (label(name(element.Get())) == target && aid(element.Get()) == L"session_item_" + target
                && hasAncestorAid(element.Get(), L"session_list")) { result = element; ++count; }
        }
        require(count == 1, Navigation); return result;
    }
    void foregroundForAction() {
        checkpoint();
        if (controlsForeground) return;
        DWORD activePid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &activePid);
        require(activePid != pid || GetTickCount() - lastInput() >= 800, Interrupted);
        minimized = IsIconic(window) != FALSE;
        if (minimized) ShowWindow(window, SW_RESTORE);
        require(SetForegroundWindow(window) || GetForegroundWindow() == window, Navigation);
        require(GetForegroundWindow() == window, Navigation);
        controlsForeground = true;
        expectedInput = lastInput(); GetCursorPos(&expectedCursor);
    }
    POINT hitPoint(IUIAutomationElement *element, HWND expectedRoot, bool recall) {
        checkpoint();
        BOOL enabled = FALSE, offscreen = TRUE; RECT bounds{}; POINT point{}; BOOL clickable = FALSE;
        check(element->get_CurrentIsEnabled(&enabled), Accessibility); check(element->get_CurrentIsOffscreen(&offscreen), Accessibility);
        check(element->get_CurrentBoundingRectangle(&bounds), Accessibility);
        require(enabled && !offscreen && bounds.right > bounds.left && bounds.bottom > bounds.top, Navigation);
        if (recall) point=ownRowPoint(bounds,GetDpiForWindow(window));
        else if (FAILED(element->GetClickablePoint(&point, &clickable)) || !clickable)
            point = {bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
        require(PtInRect(&bounds, point) != FALSE, Navigation);
        HWND hit = WindowFromPoint(point);
        require(samePid(hit, pid) && GetAncestor(hit, GA_ROOT) == expectedRoot, Navigation);
        ComPtr<IUIAutomationElement> under; check(automation->ElementFromPoint(point, &under), Accessibility);
        require(ancestor(under.Get(), element), Navigation);
        return point;
    }
    void click(IUIAutomationElement *element, bool right, const std::wstring &expectedText={}) {
        foregroundForAction();
        if (right) require(cls(element)==L"mmui::ChatTextItemView" && name(element)==expectedText,Mapping);
        const POINT point = hitPoint(element, window, right);
        checkpoint();
        const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        require(width > 1 && height > 1 && point.x >= left && point.y >= top
            && int64_t(point.x) < int64_t(left)+width && int64_t(point.y) < int64_t(top)+height, Navigation);
        INPUT move{}; const DWORD tick = GetTickCount(), stamp = tick ? tick : 1;
        move.type=INPUT_MOUSE; move.mi.time=stamp; move.mi.dwExtraInfo=0x574C4954;
        move.mi.dx = LONG((int64_t(point.x) - left) * 65535 / (width - 1));
        move.mi.dy = LONG((int64_t(point.y) - top) * 65535 / (height - 1));
        move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        require(GetForegroundWindow() == window && samePid(WindowFromPoint(point),pid)
            && GetAncestor(WindowFromPoint(point),GA_ROOT) == window, Interrupted);
        require(SendInput(1,&move,sizeof(INPUT))==1,Interrupted);
        expectedInput = stamp; expectedCursor = point;
        // SendInput returns before the desktop input queue necessarily consumes it.
        for (unsigned i = 0; i < 20 && lastInput() != stamp; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        // Absolute desktop coordinates can quantize by one pixel on some screen
        // sizes. Accept only that rounding, while retaining the exact actual
        // pointer as the ownership checkpoint for subsequent operations.
        POINT actual{}; require(GetCursorPos(&actual) != FALSE, Interrupted);
        require(std::abs(actual.x-point.x) <= 1 && std::abs(actual.y-point.y) <= 1, Interrupted);
        expectedCursor=actual;
        checkpoint();
        // Revalidate the actual quantized point before any button is pressed.
        // A provider's clickable point may be at an edge between native rows.
        require(samePid(WindowFromPoint(actual),pid) && GetAncestor(WindowFromPoint(actual),GA_ROOT)==window,Navigation);
        ComPtr<IUIAutomationElement> under; check(automation->ElementFromPoint(actual,&under),Accessibility);
        require(ancestor(under.Get(),element),Navigation); checkpoint();
        if (right) require(name(element)==expectedText && currentChat()==requestedGroup,Mapping);
        require(GetForegroundWindow()==window && samePid(WindowFromPoint(actual),pid)
            && GetAncestor(WindowFromPoint(actual),GA_ROOT)==window,Interrupted);
        INPUT buttons[2]{}; const DWORD buttonTick=GetTickCount(), buttonStamp=buttonTick ? buttonTick : 1;
        for (auto &button:buttons) { button.type=INPUT_MOUSE; button.mi.time=buttonStamp; button.mi.dwExtraInfo=0x574C4954; }
        buttons[0].mi.dwFlags=right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        buttons[1].mi.dwFlags=right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
        const UINT count=SendInput(2,buttons,sizeof(INPUT));
        if (count!=2) {
            INPUT release{}; release.type=INPUT_MOUSE; release.mi.dwFlags=right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
            SendInput(1,&release,sizeof(INPUT)); interrupted=true; throw Failure{Interrupted};
        }
        expectedInput=buttonStamp;
        for (unsigned i=0;i<20 && (lastInput()!=buttonStamp || mouseHeld());++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        checkpoint();
    }
    bool awaitChat(const std::wstring &target) {
        for (unsigned i = 0; i < 30; ++i) {
            checkpoint(); if (currentChat() == target) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        return false;
    }
    struct PopupWindow { DWORD pid; HWND main; std::vector<HWND> values; };
    static BOOL CALLBACK popups(HWND candidate, LPARAM opaque) {
        auto &state = *reinterpret_cast<PopupWindow *>(opaque);
        if (candidate == state.main || !samePid(candidate, state.pid) || !IsWindowVisible(candidate)) return TRUE;
        HWND owner = GetWindow(candidate, GW_OWNER);
        for (unsigned i = 0; owner && i < 12; ++i) {
            if (owner == state.main) { state.values.push_back(candidate); break; }
            owner = GetWindow(owner, GW_OWNER);
        }
        return state.values.size() < 32;
    }
    std::vector<HWND> popupWindows() { PopupWindow state{pid, window, {}}; EnumWindows(popups, reinterpret_cast<LPARAM>(&state)); return state.values; }
    ComPtr<IUIAutomationElement> recallMenu(const std::vector<HWND> &before) {
        ComPtr<IUIAutomationElement> result; unsigned count = 0;
        std::vector<ComPtr<IUIAutomationElement>> seen;
        for (HWND candidate : popupWindows()) {
            if (std::find(before.begin(), before.end(), candidate) != before.end()) continue;
            auto base = root(candidate);
            auto menus = byType(UIA_MenuControlTypeId, base.Get());
            CONTROLTYPEID baseType = 0; check(base->get_CurrentControlType(&baseType), Accessibility);
            if (baseType == UIA_MenuControlTypeId) menus.push_back(base);
            for (auto &menu : menus) for (auto &item : byType(UIA_MenuItemControlTypeId, menu.Get())) {
                BOOL enabled = FALSE, offscreen = TRUE;
                check(item->get_CurrentIsEnabled(&enabled), Accessibility); check(item->get_CurrentIsOffscreen(&offscreen), Accessibility);
                if (name(item.Get()) != L"撤回" || !enabled || offscreen || !ancestor(item.Get(), menu.Get())) continue;
                ComPtr<IUIAutomationInvokePattern> action;
                if (SUCCEEDED(item->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&action))) && action) {
                    bool duplicate=false;
                    for (const auto &previous : seen) { BOOL same=FALSE; check(automation->CompareElements(previous.Get(),item.Get(),&same),Popup); duplicate |= same != FALSE; }
                    if (duplicate) continue;
                    seen.push_back(item);
                    result = item; menuWindow = candidate; ++count;
                }
            }
        }
        require(count == 1, NoRecall); return result;
    }
public:
    Session(DWORD processId, HWND handle, std::wstring group) : pid(processId), window(handle), requestedGroup(std::move(group)) {
        GetCursorPos(&cursor);
        ComPtr<IUIAutomation2> modern;
        check(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&modern)), Accessibility);
        check(modern->put_AutoSetFocus(FALSE), Accessibility);
        check(modern->put_ConnectionTimeout(1500), Accessibility); check(modern->put_TransactionTimeout(1500), Accessibility);
        check(modern.As(&automation), Accessibility); check(automation->get_RawViewWalker(&walker), Accessibility);
        require(cls(root().Get()) == L"mmui::MainWindow", Accessibility);
        originalChat = currentChat();
    }
    std::wstring currentChat() {
        auto fields = byClass(L"mmui::ChatInputField"); require(fields.size() == 1, Navigation);
        auto value = name(fields[0].Get());
        for (const std::wstring suffix : {L"按住鼠标 语音输入文字", L"按住鼠标语音输入文字", L"语音输入文字"})
            if (value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
                value.resize(value.size() - suffix.size()); while (!value.empty() && value.back() == L' ') value.pop_back(); break;
            }
        return value;
    }
    void open() {
        require(!requestedGroup.empty(), Invalid);
        if (currentChat() != requestedGroup) { auto target = exactSession(requestedGroup); click(target.Get(), false); require(awaitChat(requestedGroup), Navigation); }
        require(byClass(L"mmui::ChatTitleBarChatRoomView").size() == 1, Navigation);
    }
    std::vector<Row> messages() {
        checkpoint(); require(currentChat() == requestedGroup, Navigation);
        std::vector<ComPtr<IUIAutomationElement>> lists;
        for (auto &candidate : byClass(L"mmui::RecyclerListView")) if (aidPart(aid(candidate.Get()), L"chat_message_list")) lists.push_back(candidate);
        require(lists.size() == 1, Mapping);
        RECT viewport{}; check(lists[0]->get_CurrentBoundingRectangle(&viewport), Accessibility);
        std::vector<Row> rows;
        for (auto &element : byClass(L"mmui::ChatTextItemView", lists[0].Get())) {
            BOOL offscreen = TRUE; RECT bounds{}, overlap{};
            check(element->get_CurrentIsOffscreen(&offscreen), Accessibility); check(element->get_CurrentBoundingRectangle(&bounds), Accessibility);
            require(aid(element.Get()) == L"chat_message_list.qt_scrollarea_viewport.chat_bubble_item_view", Mapping);
            require(bounds.right>bounds.left && bounds.bottom>bounds.top,Mapping);
            ComPtr<IUIAutomationElement> parent; check(walker->GetParentElement(element.Get(),&parent),Accessibility); require(parent!=nullptr,Mapping);
            BOOL direct=FALSE; check(automation->CompareElements(parent.Get(),lists[0].Get(),&direct),Accessibility);
            require(direct,Mapping); // Quote/card descendants are not plain text rows.
            const bool fullyVisible=!offscreen && IntersectRect(&overlap,&bounds,&viewport)
                && EqualRect(&overlap,&bounds);
            rows.push_back(Row{element, name(element.Get()), bounds,fullyVisible});
        }
        require(rows.size() <= 400, Mapping);
        std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.rect.top < b.rect.top; });
        for (size_t i = 1; i < rows.size(); ++i) require(rows[i - 1].rect.bottom <= rows[i].rect.top, Mapping);
        return rows;
    }
    size_t mapRows(const std::vector<Text> &context,const std::vector<WlUiUniqueContext> &proofs,const std::vector<Row> &rows) {
        std::vector<std::wstring> names; std::vector<bool> visible;
        for (const auto &row:rows) { names.push_back(row.text); visible.push_back(row.fullyVisible); }
        return mapText(context,proofs,names,visible);
    }
    void recall(const std::vector<Text> &context, const std::vector<WlUiUniqueContext> &proofs,
        const std::wstring &text, bool &invoked, Process &process, const WlUiRequest &request) {
        open();
        auto first = messages();
        const size_t index = mapRows(context,proofs,first); require(first[index].text == text, Mapping);
        foregroundForAction();
        auto fresh = messages(); require(fresh.size() == first.size(), Mapping);
        for (size_t i = 0; i < first.size(); ++i) require(fresh[i].text == first[i].text && fresh[i].fullyVisible==first[i].fullyVisible, Mapping);
        require(mapRows(context,proofs,fresh)==index,Mapping);
        auto priorPopups = popupWindows();
        click(fresh[index].element.Get(), true, text);
        ComPtr<IUIAutomationElement> item;
        for (unsigned i = 0; i < 20; ++i) {
            checkpoint();
            try { item = recallMenu(priorPopups); break; }
            catch (const Failure &error) { if (error.code != NoRecall) throw; }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        require(item != nullptr, NoRecall); checkpoint(); require(currentChat() == requestedGroup, Navigation);
        BOOL enabled = FALSE; check(item->get_CurrentIsEnabled(&enabled), Popup);
        require(enabled && name(item.Get()) == L"撤回" && menuWindow && IsWindowVisible(menuWindow) && owned(menuWindow), Popup);
        ComPtr<IUIAutomationInvokePattern> action;
        check(item->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&action)), Popup); require(action != nullptr, Popup);
        auto beforeInvoke=messages();
        const size_t finalIndex=mapRows(context,proofs,beforeInvoke);
        BOOL same=FALSE; check(automation->CompareElements(beforeInvoke[finalIndex].element.Get(),fresh[index].element.Get(),&same),Mapping);
        require(same && EqualRect(&beforeInvoke[finalIndex].rect,&fresh[index].rect),Mapping);
        verifyAccount(process,request);
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        require(validAge(request.create_time,now), Invalid);
        verifyHistory(request); // Rust metadata-only callback; no SQL/process/UI work.
        checkpoint();
        invoked = true; // Anything from this exact call onward is conservatively uncertain.
        check(action->Invoke(), InvokeFailed); // Exactly once; database polling establishes success.
    }
    ~Session() {
        try {
            if (!controlsForeground || !stillOwned()) return;
            if (menuWindow && IsWindow(menuWindow) && IsWindowVisible(menuWindow) && owned(menuWindow)) {
                PostMessageW(menuWindow, WM_CLOSE, 0, 0); menuWindow = nullptr;
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (!originalChat.empty() && originalChat != currentChat() && stillOwned()) {
                auto previous = exactSession(originalChat); click(previous.Get(), false); awaitChat(originalChat);
            }
            if (!stillOwned()) return;
            if (!IsWindow(foreground) || !owned(GetForegroundWindow())) return;
            if (!SetForegroundWindow(foreground) && GetForegroundWindow()!=foreground) return;
            if (!inputHeld() && GetForegroundWindow()==foreground && lastInput()==expectedInput) SetCursorPos(cursor.x,cursor.y);
            if (minimized && !inputHeld() && GetForegroundWindow()==foreground && lastInput()==expectedInput) ShowWindow(window,SW_MINIMIZE);
        } catch (...) { /* A changed/closed UI must never trigger forced cleanup input. */ }
    }
};

std::mutex recallMutex;

// Crosses a real C++ frame so the offline test detects missing /EHsc unwind
// semantics, which are required by the gate, COM, DPI and UI ownership guards.
__declspec(noinline) void failWithCleanup(bool &cleaned) {
    struct Cleanup { bool &value; ~Cleanup() { value=true; } } cleanup{cleaned};
    throw Failure{Profile};
}
}

extern "C" uint32_t wl_chat_ui_recall(const WlUiRequest *request, uint32_t *detail) noexcept {
    bool invoked = false, success = false; Error error = Invalid;
    if (detail) *detail = Invalid;
    std::unique_lock<std::mutex> lock(recallMutex, std::try_to_lock);
    if (!lock.owns_lock()) { if (detail) *detail = Busy; return 1; }
    try {
        require(request && request->pid && request->base && request->context && request->context_length >= 1 && request->context_length <= 200, Invalid);
        require(request->unique_contexts && request->unique_context_count>=1 && request->unique_context_count<=6,Invalid);
        const auto group = utf16(request->group, request->group_length, 256);
        const auto text = utf16(request->text, request->text_length);
        require(group.find_first_of(L"\r\n") == std::wstring::npos, Invalid);
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        require(validAge(request->create_time, now), Invalid);
        std::vector<Text> context; size_t targets = 0,target=SIZE_MAX;
        for (size_t i = 0; i < request->context_length; ++i) {
            const auto &row = request->context[i]; require(row.outgoing <= 1 && row.target <= 1, Invalid);
            context.push_back(Text{utf16(row.text, row.length), row.outgoing != 0, row.target != 0});
            if (row.target) { ++targets; target=i; require(row.outgoing && context.back().value == text, Invalid); }
        }
        require(targets == 1, Invalid);
        std::vector<WlUiUniqueContext> proofs;
        for (size_t i=0;i<request->unique_context_count;++i) {
            const auto proof=request->unique_contexts[i];
            require(proof.length>=1 && proof.length<=3 && proof.length<=context.size()
                && proof.start<=context.size()-proof.length && proof.start<=target && target-proof.start<proof.length,Invalid);
            for (const auto &prior:proofs) require(prior.start!=proof.start || prior.length!=proof.length,Invalid);
            proofs.push_back(proof);
        }
        require(!inputHeld(),Interrupted);
        InterprocessLock processLock(L"Local\\WeChatLite.Recall."+std::to_wstring(request->pid));
        verifyHistory(*request);
        MainWindow main{request->pid}; EnumWindows(findMain, reinterpret_cast<LPARAM>(&main)); require(main.count == 1, Navigation);
        Process process(request->pid); require(WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT, Profile);
        verifyAccount(process,*request);
        Gate gate(process, request->base);
        try {
            gate.enable(); Com com; Dpi dpi;
            Session session(request->pid, main.value, group);
            session.recall(context, proofs, text, invoked, process, *request);
            success = true;
        } catch (const Failure &failure) { error = failure.code; }
        catch (...) { error = Accessibility; }
        if (!gate.restore()) { success = false; error = RestoreFailed; }
    } catch (const Failure &failure) { error = failure.code; }
    catch (...) { error = Accessibility; }
    if (detail) *detail = success ? 0 : error;
    return outcome(invoked, success);
}

// Pure offline tests. No window enumeration, COM, process access, input or gate writes.
// Only the Rust cfg(test) reference keeps this function in test binaries.
extern "C" uint32_t wl_chat_ui_selftest() noexcept {
    try {
        bool cleaned=false;
        try { failWithCleanup(cleaned); } catch (const Failure &) {}
        require(cleaned,RestoreFailed);
        const std::vector<Text> db{{L"before",false,false},{L"target",true,true},{L"after",false,false}};
        const std::vector<WlUiUniqueContext> proof{{0,3}};
        auto locate=[](const std::vector<Text> &context,const std::vector<WlUiUniqueContext> &proofs,
            const std::vector<std::wstring> &ui,const std::vector<bool> &visible=std::vector<bool>{}) {
            return mapText(context,proofs,ui,visible.empty() ? std::vector<bool>(ui.size(),true) : visible);
        };
        auto rejected = [&](const std::vector<Text> &context,const std::vector<WlUiUniqueContext> &proofs,
            const std::vector<std::wstring> &ui,const std::vector<bool> &visible=std::vector<bool>{}) {
            try { locate(context,proofs,ui,visible); return false; } catch (const Failure &) { return true; }
        };
        // The uniquely identified target may be in the middle of a scrolled view.
        require(locate(db,proof,{L"outside",L"before",L"target",L"after",L"later"}) == 2, Mapping);
        require(rejected(db,proof,{L"before",L"target",L"target",L"after"}), Mapping);
        require(rejected(db,proof,{L"before",L"target",L"after",L"before",L"target",L"after"}), Mapping);
        require(rejected(db,proof,{L"before",L"target",L"after"},{false,true,true}), Mapping);
        require(rejected(db,proof,{L"target"}), Mapping);
        require(rejected(db,proof,{L"before",L"target",L"different"}), Mapping);
        require(rejected(db,{}, {L"before",L"target",L"after"}),Mapping);
        require(rejected(db,{{0,0}}, {L"before",L"target",L"after"}),Mapping);
        require(rejected(db,{{0,4}}, {L"before",L"target",L"after"}),Mapping);
        require(rejected(db,{{1,3}}, {L"before",L"target",L"after"}),Mapping);
        require(rejected(db,{{0,3},{0,3}}, {L"before",L"target",L"after"}),Mapping);
        auto incoming = db; incoming[1].outgoing = false;
        require(rejected(incoming,proof,{L"before",L"target",L"after"}), Mapping);
        auto noTarget = db; noTarget[1].target = false;
        require(rejected(noTarget,proof,{L"before",L"target",L"after"}), Mapping);
        require(!validAge(100,280) && validAge(100,279) && !validAge(101,100), Invalid);
        require(outcome(false,false) == 1 && outcome(true,false) == 2 && outcome(true,true) == 0, Invalid);
        uint8_t getter[sizeof(Getter)]; std::memcpy(getter,Getter,sizeof(getter));
        require(profileBytes(getter,Setter,Query), Profile); getter[3] ^= 1;
        require(!profileBytes(getter,Setter,Query), Profile);
        require(validGate(PAGE_READWRITE,MEM_COMMIT,MEM_IMAGE,0) && validGate(PAGE_READWRITE,MEM_COMMIT,MEM_IMAGE,1), Profile);
        require(!validGate(PAGE_EXECUTE_READWRITE,MEM_COMMIT,MEM_IMAGE,0)
                && !validGate(PAGE_READWRITE,MEM_COMMIT,MEM_IMAGE,2)
                && !validGate(PAGE_READWRITE,MEM_COMMIT,MEM_PRIVATE,0), Profile);
        require(label(L"测试群\npreview\ntime") == L"测试群" && label(L"测试群别名\npreview") != L"测试群", Invalid);
        require(aidPart(L"parent.session_list.child",L"session_list") && !aidPart(L"other_session_list",L"session_list"), Invalid);
        const std::vector<Text> repeated{{L"first",false,false},{L"ok",true,false},{L"next",false,false},
            {L"second",false,false},{L"ok",true,true},{L"last",false,false}};
        require(locate(repeated,{{3,3}},{L"first",L"ok",L"next",L"second",L"ok",L"last"})==4,Mapping);
        require(locate(repeated,{{3,3}},{L"second",L"ok",L"last"})==1,Mapping);
        require(rejected(repeated,{{3,3}},{L"first",L"ok",L"next"}),Mapping);
        const std::vector<Text> neighbors{{L"a",false,false},{L"b",false,false},{L"target",true,true},{L"c",false,false},{L"d",false,false}};
        require(locate(neighbors,{{0,3},{1,3},{2,3}},{L"b",L"target",L"c"})==1,Mapping); // Absent proofs are allowed.
        require(locate(neighbors,{{0,3},{1,3},{2,3}},{L"a",L"b",L"target",L"c",L"d"},{false,true,true,true,true})==2,Mapping);
        require(rejected(neighbors,{{0,3},{2,3}},{L"a",L"b",L"target",L"gap",L"target",L"c",L"d"}),Mapping);
        require(rejected(neighbors,{{0,3},{2,3}},{L"a",L"b",L"target",L"c",L"d",L"a",L"b",L"target"}),Mapping);
        const std::vector<Text> firstText{{L"globally unique first text",true,true}};
        require(locate(firstText,{{0,1}},{L"globally unique first text"})==0,Mapping);
        require(locate(firstText,{{0,1}},{L"before",L"globally unique first text",L"after"})==1,Mapping);
        require(rejected(firstText,{{0,1}},{L"globally unique first text",L"globally unique first text"}),Mapping);
        require(locate(repeated,{{3,2}},{L"first",L"ok",L"next",L"second",L"ok",L"last"})==4,Mapping);
        const auto point=ownRowPoint(RECT{-1000,-100,-200,-20},144);
        require(point.x==-332 && point.y==-60 && !keyDown(0) && !keyDown(1) && keyDown(SHORT(0x8000)),Invalid);
        struct HistoryState { unsigned calls=0; } history;
        WlUiRequest historyRequest{}; historyRequest.history_context=&history;
        historyRequest.validate_history=[](const void *context)->uint32_t {
            auto &state=*static_cast<HistoryState *>(const_cast<void *>(context));
            return ++state.calls==1 ? 0 : 1;
        };
        verifyHistory(historyRequest);
        bool submitted=false,historyChanged=false;
        try { verifyHistory(historyRequest); submitted=true; }
        catch (const Failure &error) { historyChanged=error.code==HistoryChanged; }
        require(historyChanged && !submitted && history.calls==2 && outcome(submitted,false)==1,HistoryChanged);
        // The account guard uses the identical validator with an offline reader.
        struct Block { uintptr_t address; std::vector<uint8_t> bytes; };
        const uintptr_t base=0x10000000, landmark=base+0x200, slot=landmark-0x128, root=0x20000000, config=0x30000000;
        WlUiAccountRoot accountRoot{landmark,slot,root,config};
        const std::string wxid="fixture";
        WlUiRequest request{}; request.pid=7; request.base=base;
        request.account={7,base,reinterpret_cast<const uint8_t *>(wxid.data()),wxid.size(),&accountRoot,1};
        auto pointer=[](uintptr_t value) { std::vector<uint8_t> out(sizeof(value)); std::memcpy(out.data(),&value,sizeof(value)); return out; };
        std::vector<Block> memory{{landmark,std::vector<uint8_t>(32)}, {slot,pointer(root)}, {root+0x68,pointer(config)}, {config+0x48,std::vector<uint8_t>(32)}};
        const uint64_t thirteen=13,fifteen=15,seven=7;
        std::memcpy(memory[0].bytes.data(),"global_config",13);
        std::memcpy(memory[0].bytes.data()+16,&thirteen,8); std::memcpy(memory[0].bytes.data()+24,&fifteen,8);
        std::memcpy(memory[3].bytes.data(),wxid.data(),wxid.size());
        std::memcpy(memory[3].bytes.data()+16,&seven,8); std::memcpy(memory[3].bytes.data()+24,&fifteen,8);
        auto read=[&](uintptr_t address,void *out,size_t length) {
            for (const auto &block:memory) if (block.address==address && block.bytes.size()==length) { std::memcpy(out,block.bytes.data(),length); return; }
            throw Failure{AccountChanged};
        };
        auto image=[&](uintptr_t address,size_t length) { return address>=base && address+length<=base+0x1000; };
        auto accountRejected=[&](const auto &reader) { try { verifyAccountData(request,reader,image); return false; } catch (const Failure &) { return true; } };
        verifyAccountData(request,read,image);
        request.account.pid=8; require(accountRejected(read),AccountChanged); request.account.pid=7;
        memory[3].bytes[0]^=1; require(accountRejected(read),AccountChanged); memory[3].bytes[0]^=1;
        memory[0].bytes[16]^=1; require(accountRejected(read),AccountChanged); memory[0].bytes[16]^=1;
        unsigned slotReads=0;
        auto changed=[&](uintptr_t address,void *out,size_t length) {
            read(address,out,length);
            if (address==slot && ++slotReads==2) { const uintptr_t switched=root+16; std::memcpy(out,&switched,sizeof(switched)); }
        };
        require(accountRejected(changed),AccountChanged);
        return 0;
    } catch (...) { return 1; }
}
