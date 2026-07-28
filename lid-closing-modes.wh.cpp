// ==WindhawkMod==
// @id              lid-closing-modes
// @name            Lid Closing Mode Button
// @description     Adds a taskbar button that switches lid closing between sleep and do nothing
// @version         1.6.1
// @author          Roma
// @include         explorer.exe
// @architecture    x86-64
// @license         MIT
// @compilerOptions -DWIN32_LEAN_AND_MEAN -lole32 -loleaut32 -lruntimeobject -lwindowsapp -luser32 -lversion -lpowrprof
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Lid Closing Mode Button

Adds a compact button immediately to the left of the Windows 11 system tray.
The button switches the **When I close the lid** action between:

- Sleep
- Do nothing

The button follows the current power source. While plugged in, it displays and
changes only the **Plugged in** value. On battery, it displays and changes only
the **On battery** value. The selected value is written to every power plan,
matching the global settings shown by `powercfg.cpl`. Switching the power
source updates the button immediately without polling.

The separate behavior can be disabled in the mod settings. In linked mode,
the button displays and changes the plugged-in and battery values together.

The button mirrors the native Windows 11 system tray button geometry and
visual states, including its hover border, pressed state, transition timing,
and light, dark, and high contrast colors.

The moon icon means **Sleep**. The blocked icon means **Do nothing**. Other
lid actions show a question mark, and the next click sets the current power
source's value to **Sleep**.

This mod targets the Windows 11 XAML taskbar.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- separatePowerSources: true
  $name: Separate plugged-in and battery modes
  $description: >-
    When enabled, the button displays and changes only the current power
    source's lid action. When disabled, the button displays and changes the
    plugged-in and battery actions together.
*/
// ==/WindhawkModSettings==

#undef GetCurrentTime

#include <powrprof.h>
#include <powersetting.h>
#include <unknwn.h>
#include <winver.h>

#include <windhawk_utils.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <functional>
#include <list>
#include <string>
#include <vector>

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Automation;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

namespace wuxi = winrt::Windows::UI::Xaml::Input;

namespace {

constexpr wchar_t kRootName[] = L"LidClosingModes_Root";
constexpr DWORD kDoNothingValue = 0;
constexpr DWORD kSleepValue = 1;

void DebugLog(std::wstring const& message) {
    std::wstring line =
        L"[LidClosingModes] " + message + L"\n";
    OutputDebugStringW(line.c_str());
}

#ifndef DEVICE_NOTIFY_CALLBACK
#define DEVICE_NOTIFY_CALLBACK 2
#endif

using DeviceNotifyCallbackRoutine =
    ULONG(CALLBACK*)(PVOID context, ULONG type, PVOID setting);

struct DeviceNotifySubscribeParameters {
    DeviceNotifyCallbackRoutine Callback;
    PVOID Context;
};

// GUID_SYSTEM_BUTTON_SUBGROUP
constexpr GUID kSystemButtonSubgroup = {
    0x4f971e89,
    0xeebd,
    0x4455,
    {0xa8, 0xde, 0x9e, 0x59, 0x04, 0x0e, 0x73, 0x47},
};

// GUID_LIDCLOSE_ACTION
constexpr GUID kLidCloseAction = {
    0x5ca83367,
    0x6e45,
    0x459f,
    {0xa2, 0x7b, 0x47, 0x6b, 0x1d, 0x01, 0xc9, 0x36},
};

// GUID_ACTIVE_POWERSCHEME
constexpr GUID kActivePowerScheme = {
    0x31f9f286,
    0x5084,
    0x42fe,
    {0xb7, 0x20, 0x2b, 0x02, 0x64, 0x99, 0x37, 0x63},
};

// GUID_ACDC_POWER_SOURCE
constexpr GUID kAcDcPowerSource = {
    0x5d3e9a59,
    0xe9d5,
    0x4b00,
    {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48},
};

enum class PowerSource {
    PluggedIn,
    OnBattery,
    Unknown,
};

enum class LidMode {
    DoNothing,
    Sleep,
    Other,
    Unavailable,
};

struct LidSetting {
    LidMode mode = LidMode::Unavailable;
    PowerSource source = PowerSource::Unknown;
    bool separatePowerSources = true;
    DWORD acValue = 0;
    DWORD dcValue = 0;
    DWORD activeValue = 0;
    DWORD error = ERROR_SUCCESS;
};

struct PowerSchemeLidValues {
    GUID scheme{};
    DWORD acValue = 0;
    DWORD dcValue = 0;
};

std::atomic<bool> g_unloading{false};
std::atomic<bool> g_separatePowerSources{true};
std::atomic<HWND> g_taskbarWnd{nullptr};
HPOWERNOTIFY g_lidCloseNotification = nullptr;
HPOWERNOTIFY g_activeSchemeNotification = nullptr;
HPOWERNOTIFY g_powerSourceNotification = nullptr;
std::atomic<unsigned> g_activePowerCallbacks{0};
std::atomic<bool> g_systemTrayModuleHooked{false};

Grid g_injectionRoot{nullptr};
Grid g_injectionParent{nullptr};
Grid g_button{nullptr};
Border g_buttonBackground{nullptr};
FontIcon g_icon{nullptr};
winrt::event_token g_tappedToken{};
winrt::event_token g_pointerEnteredToken{};
winrt::event_token g_pointerExitedToken{};
winrt::event_token g_pointerPressedToken{};
winrt::event_token g_pointerReleasedToken{};
winrt::event_token g_pointerCanceledToken{};
winrt::event_token g_pointerCaptureLostToken{};
winrt::event_token g_actualThemeChangedToken{};
std::list<FrameworkElement::Loaded_revoker> g_iconLoadedRevokers;

SolidColorBrush g_buttonNormalBackgroundBrush{nullptr};
SolidColorBrush g_buttonHoverBackgroundBrush{nullptr};
SolidColorBrush g_buttonPressedBackgroundBrush{nullptr};
SolidColorBrush g_buttonNormalBorderBrush{nullptr};
Brush g_buttonHoverBorderBrush{nullptr};
SolidColorBrush g_buttonNormalForegroundBrush{nullptr};
SolidColorBrush g_buttonPressedForegroundBrush{nullptr};
bool g_buttonPointerOver = false;
bool g_buttonPointerPressed = false;

DWORD g_lastOperationError = ERROR_SUCCESS;
ULONGLONG g_lastOperationErrorExpiresAt = 0;

using CTaskBand_GetTaskbarHost_t =
    void*(WINAPI*)(void* pThis, void* taskbarHostSharedPtr);
CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original = nullptr;

using TaskbarHost_FrameHeight_t = int(WINAPI*)(void* pThis);
TaskbarHost_FrameHeight_t TaskbarHost_FrameHeight_Original = nullptr;

using std__Ref_count_base__Decref_t = void(WINAPI*)(void* pThis);
std__Ref_count_base__Decref_t std__Ref_count_base__Decref_Original = nullptr;

void* CTaskBand_ITaskListWndSite_vftable = nullptr;

using RunFromWindowThreadProc = void (*)(void*);

void SyncButtonWithTaskbar();

void LoadSettings() {
    g_separatePowerSources.store(
        Wh_GetIntSetting(L"separatePowerSources") != 0,
        std::memory_order_release);
}

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc proc,
                         void* procParam) {
    static const UINT message =
        RegisterWindowMessageW(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct InvokeParam {
        RunFromWindowThreadProc proc;
        void* procParam;
    };

    DWORD threadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (!threadId) {
        return false;
    }

    if (threadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookExW(
        WH_CALLWNDPROC,
        [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (code == HC_ACTION) {
                const auto* messageData =
                    reinterpret_cast<const CWPSTRUCT*>(lParam);
                if (messageData->message ==
                    RegisterWindowMessageW(
                        L"Windhawk_RunFromWindowThread_" WH_MOD_ID)) {
                    auto* invoke =
                        reinterpret_cast<InvokeParam*>(messageData->lParam);
                    invoke->proc(invoke->procParam);
                }
            }

            return CallNextHookEx(nullptr, code, wParam, lParam);
        },
        nullptr,
        threadId);
    if (!hook) {
        return false;
    }

    InvokeParam invoke{proc, procParam};
    SendMessageW(hWnd, message, 0, reinterpret_cast<LPARAM>(&invoke));
    UnhookWindowsHookEx(hook);
    return true;
}

HWND FindCurrentProcessTaskbarWnd() {
    HWND result = nullptr;
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD processId = 0;
            wchar_t className[64] = {};
            if (GetWindowThreadProcessId(hWnd, &processId) &&
                processId == GetCurrentProcessId() &&
                GetClassNameW(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}

XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd) {
    if (!CTaskBand_GetTaskbarHost_Original ||
        !TaskbarHost_FrameHeight_Original ||
        !std__Ref_count_base__Decref_Original ||
        !CTaskBand_ITaskListWndSite_vftable) {
        DebugLog(L"GetTaskbarXamlRoot: taskbar symbols are unavailable");
        return nullptr;
    }

    HWND hTaskSwWnd =
        reinterpret_cast<HWND>(GetPropW(hTaskbarWnd, L"TaskbandHWND"));
    if (!hTaskSwWnd) {
        DebugLog(L"GetTaskbarXamlRoot: TaskbandHWND is unavailable");
        return nullptr;
    }

    void* taskBand =
        reinterpret_cast<void*>(GetWindowLongPtrW(hTaskSwWnd, 0));
    if (!taskBand) {
        DebugLog(L"GetTaskbarXamlRoot: CTaskBand is unavailable");
        return nullptr;
    }

    void* taskBandForSite = taskBand;
    for (int i = 0;
         *reinterpret_cast<void**>(taskBandForSite) !=
         CTaskBand_ITaskListWndSite_vftable;
         i++) {
        if (i == 20) {
            DebugLog(
                L"GetTaskbarXamlRoot: ITaskListWndSite was not found");
            return nullptr;
        }
        taskBandForSite =
            reinterpret_cast<void**>(taskBandForSite) + 1;
    }

    void* taskbarHostSharedPtr[2] = {};
    CTaskBand_GetTaskbarHost_Original(taskBandForSite,
                                      taskbarHostSharedPtr);
    if (!taskbarHostSharedPtr[0] || !taskbarHostSharedPtr[1]) {
        DebugLog(L"GetTaskbarXamlRoot: TaskbarHost is unavailable");
        if (taskbarHostSharedPtr[1]) {
            std__Ref_count_base__Decref_Original(
                taskbarHostSharedPtr[1]);
        }
        return nullptr;
    }

    size_t taskbarElementOffset = 0x10;
    const BYTE* frameHeightCode =
        reinterpret_cast<const BYTE*>(TaskbarHost_FrameHeight_Original);
    if (frameHeightCode[0] == 0x48 && frameHeightCode[1] == 0x83 &&
        frameHeightCode[2] == 0xEC && frameHeightCode[4] == 0x48 &&
        frameHeightCode[5] == 0x83 && frameHeightCode[6] == 0xC1 &&
        frameHeightCode[7] <= 0x7F) {
        taskbarElementOffset = frameHeightCode[7];
    } else {
        Wh_Log(L"Unsupported TaskbarHost::FrameHeight prologue");
    }

    auto* taskbarElementUnknown = *reinterpret_cast<IUnknown**>(
        reinterpret_cast<BYTE*>(taskbarHostSharedPtr[0]) +
        taskbarElementOffset);
    if (!taskbarElementUnknown) {
        DebugLog(L"GetTaskbarXamlRoot: taskbar element is unavailable");
        std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);
        return nullptr;
    }

    FrameworkElement taskbarElement{nullptr};
    taskbarElementUnknown->QueryInterface(
        winrt::guid_of<FrameworkElement>(),
        winrt::put_abi(taskbarElement));
    XamlRoot result =
        taskbarElement ? taskbarElement.XamlRoot() : nullptr;
    std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);
    if (!result) {
        DebugLog(L"GetTaskbarXamlRoot: XamlRoot is unavailable");
    }
    return result;
}

FrameworkElement FindChildRecursive(
    DependencyObject const& parent,
    const std::function<bool(FrameworkElement const&)>& predicate,
    int depth = 20) {
    if (!parent || depth <= 0) {
        return nullptr;
    }

    int childCount = VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < childCount; i++) {
        auto child = VisualTreeHelper::GetChild(parent, i)
                         .try_as<FrameworkElement>();
        if (!child) {
            continue;
        }

        if (predicate(child)) {
            return child;
        }

        auto descendant =
            FindChildRecursive(child, predicate, depth - 1);
        if (descendant) {
            return descendant;
        }
    }

    return nullptr;
}

Grid FindSystemTrayGrid() {
    HWND taskbarWnd = g_taskbarWnd.load(std::memory_order_acquire);
    if (!taskbarWnd) {
        DebugLog(L"FindSystemTrayGrid: taskbar window is unavailable");
        return nullptr;
    }

    auto xamlRoot = GetTaskbarXamlRoot(taskbarWnd);
    if (!xamlRoot) {
        return nullptr;
    }

    auto content = xamlRoot.Content().try_as<FrameworkElement>();
    if (!content) {
        DebugLog(L"FindSystemTrayGrid: XamlRoot content is unavailable");
        return nullptr;
    }

    auto result = FindChildRecursive(
                      content,
                      [](FrameworkElement const& element) {
                          return element.Name() ==
                                 L"SystemTrayFrameGrid";
                      })
                      .try_as<Grid>();
    if (!result) {
        DebugLog(
            L"FindSystemTrayGrid: SystemTrayFrameGrid was not found");
    }
    return result;
}

std::wstring FormatError(DWORD error) {
    wchar_t* message = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);

    std::wstring result;
    if (length && message) {
        result.assign(message, length);
        while (!result.empty() &&
               (result.back() == L'\r' || result.back() == L'\n' ||
                result.back() == L' ')) {
            result.pop_back();
        }
    } else {
        wchar_t buffer[32];
        swprintf_s(buffer, L"Win32 error %lu", error);
        result = buffer;
    }

    if (message) {
        LocalFree(message);
    }
    return result;
}

PowerSource GetCurrentPowerSource(DWORD* error) {
    SYSTEM_POWER_STATUS powerStatus{};
    if (!GetSystemPowerStatus(&powerStatus)) {
        if (error) {
            *error = GetLastError();
        }
        return PowerSource::Unknown;
    }

    if (powerStatus.ACLineStatus == 1) {
        if (error) {
            *error = ERROR_SUCCESS;
        }
        return PowerSource::PluggedIn;
    }
    if (powerStatus.ACLineStatus == 0) {
        if (error) {
            *error = ERROR_SUCCESS;
        }
        return PowerSource::OnBattery;
    }

    if (error) {
        *error = ERROR_NOT_READY;
    }
    return PowerSource::Unknown;
}

LidSetting ReadLidSetting() {
    LidSetting result;
    result.separatePowerSources =
        g_separatePowerSources.load(std::memory_order_acquire);

    DWORD sourceError = ERROR_SUCCESS;
    result.source = GetCurrentPowerSource(&sourceError);
    if (result.separatePowerSources &&
        result.source == PowerSource::Unknown) {
        result.error = sourceError;
        return result;
    }

    GUID* activeScheme = nullptr;
    result.error = PowerGetActiveScheme(nullptr, &activeScheme);
    if (result.error != ERROR_SUCCESS || !activeScheme) {
        return result;
    }

    if (!result.separatePowerSources) {
        result.error = PowerReadACValueIndex(
            nullptr,
            activeScheme,
            &kSystemButtonSubgroup,
            &kLidCloseAction,
            &result.acValue);
        if (result.error == ERROR_SUCCESS) {
            result.error = PowerReadDCValueIndex(
                nullptr,
                activeScheme,
                &kSystemButtonSubgroup,
                &kLidCloseAction,
                &result.dcValue);
        }
    } else if (result.source == PowerSource::PluggedIn) {
        result.error = PowerReadACValueIndex(
            nullptr,
            activeScheme,
            &kSystemButtonSubgroup,
            &kLidCloseAction,
            &result.activeValue);
        result.acValue = result.activeValue;
    } else {
        result.error = PowerReadDCValueIndex(
            nullptr,
            activeScheme,
            &kSystemButtonSubgroup,
            &kLidCloseAction,
            &result.activeValue);
        result.dcValue = result.activeValue;
    }
    LocalFree(activeScheme);

    if (result.error != ERROR_SUCCESS) {
        return result;
    }

    if (!result.separatePowerSources) {
        if (result.acValue == result.dcValue) {
            result.activeValue = result.acValue;
        } else {
            result.mode = LidMode::Other;
            return result;
        }
    }

    if (result.activeValue == kSleepValue) {
        result.mode = LidMode::Sleep;
    } else if (result.activeValue == kDoNothingValue) {
        result.mode = LidMode::DoNothing;
    } else {
        result.mode = LidMode::Other;
    }

    return result;
}

DWORD ReadLidModeForSource(const GUID* activeScheme,
                           PowerSource source,
                           DWORD* value) {
    if (source == PowerSource::PluggedIn) {
        return PowerReadACValueIndex(nullptr,
                                     activeScheme,
                                     &kSystemButtonSubgroup,
                                     &kLidCloseAction,
                                     value);
    }
    if (source == PowerSource::OnBattery) {
        return PowerReadDCValueIndex(nullptr,
                                     activeScheme,
                                     &kSystemButtonSubgroup,
                                     &kLidCloseAction,
                                     value);
    }
    return ERROR_NOT_READY;
}

DWORD WriteLidModeForSource(const GUID* activeScheme,
                            PowerSource source,
                            DWORD value) {
    if (source == PowerSource::PluggedIn) {
        return PowerWriteACValueIndex(nullptr,
                                      activeScheme,
                                      &kSystemButtonSubgroup,
                                      &kLidCloseAction,
                                      value);
    }
    if (source == PowerSource::OnBattery) {
        return PowerWriteDCValueIndex(nullptr,
                                      activeScheme,
                                      &kSystemButtonSubgroup,
                                      &kLidCloseAction,
                                      value);
    }
    return ERROR_NOT_READY;
}

DWORD EnumeratePowerSchemeLidValues(
    std::vector<PowerSchemeLidValues>* schemes,
    bool readAc,
    bool readDc) {
    if (!schemes) {
        return ERROR_INVALID_PARAMETER;
    }

    schemes->clear();
    for (ULONG index = 0;; index++) {
        PowerSchemeLidValues values;
        DWORD schemeSize = sizeof(values.scheme);
        DWORD error = PowerEnumerate(
            nullptr,
            nullptr,
            nullptr,
            ACCESS_SCHEME,
            index,
            reinterpret_cast<UCHAR*>(&values.scheme),
            &schemeSize);
        if (error == ERROR_NO_MORE_ITEMS) {
            return schemes->empty() ? ERROR_NOT_FOUND : ERROR_SUCCESS;
        }
        if (error != ERROR_SUCCESS) {
            return error;
        }
        if (schemeSize != sizeof(values.scheme)) {
            return ERROR_INVALID_DATA;
        }

        if (readAc) {
            error = PowerReadACValueIndex(
                nullptr,
                &values.scheme,
                &kSystemButtonSubgroup,
                &kLidCloseAction,
                &values.acValue);
            if (error != ERROR_SUCCESS) {
                return error;
            }
        }
        if (readDc) {
            error = PowerReadDCValueIndex(
                nullptr,
                &values.scheme,
                &kSystemButtonSubgroup,
                &kLidCloseAction,
                &values.dcValue);
            if (error != ERROR_SUCCESS) {
                return error;
            }
        }

        schemes->push_back(values);
    }
}

void RestoreLidModeForSource(
    const std::vector<PowerSchemeLidValues>& schemes,
    PowerSource source,
    size_t count) {
    count = (std::min)(count, schemes.size());
    for (size_t i = 0; i < count; i++) {
        DWORD oldValue = source == PowerSource::PluggedIn
                             ? schemes[i].acValue
                             : schemes[i].dcValue;
        DWORD error = WriteLidModeForSource(
            &schemes[i].scheme, source, oldValue);
        if (error != ERROR_SUCCESS) {
            Wh_Log(L"Failed to restore a power plan's lid action: %lu",
                   error);
        }
    }
}

DWORD WriteLidMode(PowerSource source, DWORD value) {
    if (source == PowerSource::Unknown) {
        return ERROR_NOT_READY;
    }

    DWORD sourceError = ERROR_SUCCESS;
    if (GetCurrentPowerSource(&sourceError) != source) {
        return sourceError == ERROR_SUCCESS ? ERROR_RETRY : sourceError;
    }

    std::vector<PowerSchemeLidValues> schemes;
    DWORD error = EnumeratePowerSchemeLidValues(
        &schemes,
        source == PowerSource::PluggedIn,
        source == PowerSource::OnBattery);
    if (error != ERROR_SUCCESS) {
        return error;
    }

    GUID* activeScheme = nullptr;
    error = PowerGetActiveScheme(nullptr, &activeScheme);
    if (error != ERROR_SUCCESS || !activeScheme) {
        return error != ERROR_SUCCESS ? error : ERROR_INVALID_DATA;
    }

    size_t writtenCount = 0;
    for (auto const& scheme : schemes) {
        error =
            WriteLidModeForSource(&scheme.scheme, source, value);
        if (error != ERROR_SUCCESS) {
            break;
        }
        writtenCount++;
    }

    if (error != ERROR_SUCCESS) {
        RestoreLidModeForSource(schemes, source, writtenCount);
    } else {
        error = PowerSetActiveScheme(nullptr, activeScheme);
        if (error != ERROR_SUCCESS) {
            RestoreLidModeForSource(
                schemes, source, schemes.size());
            PowerSetActiveScheme(nullptr, activeScheme);
        }
    }

    LocalFree(activeScheme);
    return error;
}

void RestoreLinkedLidModes(
    const std::vector<PowerSchemeLidValues>& schemes,
    size_t acCount,
    size_t dcCount) {
    acCount = (std::min)(acCount, schemes.size());
    dcCount = (std::min)(dcCount, schemes.size());
    for (size_t i = 0; i < dcCount; i++) {
        DWORD error = PowerWriteDCValueIndex(
            nullptr,
            &schemes[i].scheme,
            &kSystemButtonSubgroup,
            &kLidCloseAction,
            schemes[i].dcValue);
        if (error != ERROR_SUCCESS) {
            Wh_Log(L"Failed to restore a power plan's DC lid action: %lu",
                   error);
        }
    }
    for (size_t i = 0; i < acCount; i++) {
        DWORD error = PowerWriteACValueIndex(
            nullptr,
            &schemes[i].scheme,
            &kSystemButtonSubgroup,
            &kLidCloseAction,
            schemes[i].acValue);
        if (error != ERROR_SUCCESS) {
            Wh_Log(L"Failed to restore a power plan's AC lid action: %lu",
                   error);
        }
    }
}

DWORD WriteLinkedLidMode(DWORD value) {
    std::vector<PowerSchemeLidValues> schemes;
    DWORD error =
        EnumeratePowerSchemeLidValues(&schemes, true, true);
    if (error != ERROR_SUCCESS) {
        return error;
    }

    GUID* activeScheme = nullptr;
    error = PowerGetActiveScheme(nullptr, &activeScheme);
    if (error != ERROR_SUCCESS || !activeScheme) {
        return error != ERROR_SUCCESS ? error : ERROR_INVALID_DATA;
    }

    size_t acWrittenCount = 0;
    size_t dcWrittenCount = 0;
    for (auto const& scheme : schemes) {
        error = PowerWriteACValueIndex(nullptr,
                                       &scheme.scheme,
                                       &kSystemButtonSubgroup,
                                       &kLidCloseAction,
                                       value);
        if (error != ERROR_SUCCESS) {
            break;
        }
        acWrittenCount++;

        error = PowerWriteDCValueIndex(nullptr,
                                       &scheme.scheme,
                                       &kSystemButtonSubgroup,
                                       &kLidCloseAction,
                                       value);
        if (error != ERROR_SUCCESS) {
            break;
        }
        dcWrittenCount++;
    }

    if (error != ERROR_SUCCESS) {
        RestoreLinkedLidModes(
            schemes, acWrittenCount, dcWrittenCount);
    } else {
        error = PowerSetActiveScheme(nullptr, activeScheme);
        if (error != ERROR_SUCCESS) {
            RestoreLinkedLidModes(
                schemes, schemes.size(), schemes.size());
            PowerSetActiveScheme(nullptr, activeScheme);
        }
    }

    LocalFree(activeScheme);
    return error;
}

std::wstring PowerValueName(DWORD value) {
    switch (value) {
        case 0:
            return L"Do nothing";
        case 1:
            return L"Sleep";
        case 2:
            return L"Hibernate";
        case 3:
            return L"Shut down";
        default: {
            wchar_t buffer[32];
            swprintf_s(buffer, L"%lu", value);
            return buffer;
        }
    }
}

std::wstring PowerSourceName(PowerSource source) {
    if (source == PowerSource::PluggedIn) {
        return L"Power source: Plugged in";
    }
    if (source == PowerSource::OnBattery) {
        return L"Power source: On battery";
    }
    return L"Power source: Unknown";
}

std::wstring PowerScopeName(LidSetting const& setting) {
    if (!setting.separatePowerSources) {
        return L"Power source: Plugged in and on battery";
    }
    return PowerSourceName(setting.source);
}

// Mirrors NormalIconView from Windows 11 SystemTrayResources.xbf.
winrt::Windows::UI::Color SystemColor(int index) {
    COLORREF color = GetSysColor(index);
    return {
        0xFF,
        GetRValue(color),
        GetGValue(color),
        GetBValue(color),
    };
}

bool IsHighContrastEnabled() {
    HIGHCONTRASTW highContrast{sizeof(highContrast)};
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST,
               sizeof(highContrast),
               &highContrast,
               0) &&
           (highContrast.dwFlags & HCF_HIGHCONTRASTON);
}

bool IsButtonThemeLight() {
    if (!g_button) {
        return false;
    }

    ElementTheme theme = g_button.ActualTheme();
    if (theme == ElementTheme::Light) {
        return true;
    }
    if (theme == ElementTheme::Dark) {
        return false;
    }

    return Application::Current().RequestedTheme() ==
           ApplicationTheme::Light;
}

LinearGradientBrush CreateHoverBorderBrush(bool light) {
    LinearGradientBrush brush;
    brush.MappingMode(BrushMappingMode::Absolute);
    brush.StartPoint({0, 0});
    brush.EndPoint({0, 3});

    GradientStop first;
    first.Offset(0.33);
    GradientStop second;
    second.Offset(1);

    if (light) {
        first.Color({0x0F, 0x00, 0x00, 0x00});
        second.Color({0x05, 0x00, 0x00, 0x00});

        ScaleTransform transform;
        transform.ScaleY(-1);
        transform.CenterY(0.5);
        brush.RelativeTransform(transform);
    } else {
        first.Color({0x1A, 0xFF, 0xFF, 0xFF});
        second.Color({0x0F, 0xFF, 0xFF, 0xFF});
    }

    brush.GradientStops().Append(first);
    brush.GradientStops().Append(second);
    return brush;
}

void UpdateButtonInteractionVisual() {
    if (!g_buttonBackground || !g_icon) {
        return;
    }

    if (g_buttonPointerPressed) {
        g_buttonBackground.Background(
            g_buttonPressedBackgroundBrush);
        g_buttonBackground.BorderBrush(g_buttonHoverBorderBrush);
        g_icon.Foreground(g_buttonPressedForegroundBrush);
    } else if (g_buttonPointerOver) {
        g_buttonBackground.Background(
            g_buttonHoverBackgroundBrush);
        g_buttonBackground.BorderBrush(g_buttonHoverBorderBrush);
        g_icon.Foreground(g_buttonNormalForegroundBrush);
    } else {
        g_buttonBackground.Background(
            g_buttonNormalBackgroundBrush);
        g_buttonBackground.BorderBrush(g_buttonNormalBorderBrush);
        g_icon.Foreground(g_buttonNormalForegroundBrush);
    }
}

void UpdateButtonThemeColors() {
    if (!g_buttonNormalBackgroundBrush ||
        !g_buttonHoverBackgroundBrush ||
        !g_buttonPressedBackgroundBrush ||
        !g_buttonNormalBorderBrush ||
        !g_buttonNormalForegroundBrush ||
        !g_buttonPressedForegroundBrush) {
        return;
    }

    if (IsHighContrastEnabled()) {
        g_buttonNormalBackgroundBrush.Color(
            SystemColor(COLOR_WINDOW));
        g_buttonHoverBackgroundBrush.Color(
            SystemColor(COLOR_HIGHLIGHTTEXT));
        g_buttonPressedBackgroundBrush.Color(
            SystemColor(COLOR_WINDOW));
        g_buttonNormalBorderBrush.Color(
            SystemColor(COLOR_WINDOW));
        g_buttonNormalForegroundBrush.Color(
            SystemColor(COLOR_WINDOWTEXT));
        g_buttonPressedForegroundBrush.Color(
            SystemColor(COLOR_HIGHLIGHT));

        auto hoverBorder = SolidColorBrush();
        hoverBorder.Color(SystemColor(COLOR_HIGHLIGHT));
        g_buttonHoverBorderBrush = hoverBorder;
    } else if (IsButtonThemeLight()) {
        g_buttonNormalBackgroundBrush.Color(
            {0x00, 0xFF, 0xFF, 0xFF});
        g_buttonHoverBackgroundBrush.Color(
            {0x80, 0xFF, 0xFF, 0xFF});
        g_buttonPressedBackgroundBrush.Color(
            {0x4C, 0xFF, 0xFF, 0xFF});
        g_buttonNormalBorderBrush.Color(
            {0x00, 0xFF, 0xFF, 0xFF});
        g_buttonNormalForegroundBrush.Color(
            {0xE4, 0x00, 0x00, 0x00});
        g_buttonPressedForegroundBrush.Color(
            {0x9E, 0x00, 0x00, 0x00});
        g_buttonHoverBorderBrush =
            CreateHoverBorderBrush(true);
    } else {
        g_buttonNormalBackgroundBrush.Color(
            {0x00, 0xFF, 0xFF, 0xFF});
        g_buttonHoverBackgroundBrush.Color(
            {0x0F, 0xFF, 0xFF, 0xFF});
        g_buttonPressedBackgroundBrush.Color(
            {0x0A, 0xFF, 0xFF, 0xFF});
        g_buttonNormalBorderBrush.Color(
            {0x00, 0xFF, 0xFF, 0xFF});
        g_buttonNormalForegroundBrush.Color(
            {0xFF, 0xFF, 0xFF, 0xFF});
        g_buttonPressedForegroundBrush.Color(
            {0xC5, 0xFF, 0xFF, 0xFF});
        g_buttonHoverBorderBrush =
            CreateHoverBorderBrush(false);
    }

    UpdateButtonInteractionVisual();
}

void ApplyTaskbarButtonStyle(Grid const& button) {
    SolidColorBrush hitTestBrush;
    hitTestBrush.Color({0x00, 0xFF, 0xFF, 0xFF});
    button.Background(hitTestBrush);
    button.MinWidth(32);
    button.VerticalAlignment(VerticalAlignment::Stretch);
    button.HorizontalAlignment(HorizontalAlignment::Stretch);

    g_buttonNormalBackgroundBrush = SolidColorBrush();
    g_buttonHoverBackgroundBrush = SolidColorBrush();
    g_buttonPressedBackgroundBrush = SolidColorBrush();
    g_buttonNormalBorderBrush = SolidColorBrush();
    g_buttonNormalForegroundBrush = SolidColorBrush();
    g_buttonPressedForegroundBrush = SolidColorBrush();
    UpdateButtonThemeColors();
}

void UpdateButtonVisual() {
    if (!g_button || !g_icon) {
        return;
    }

    LidSetting setting = ReadLidSetting();
    std::wstring glyph;
    std::wstring accessibleName;
    std::wstring tooltip;

    bool showOperationError =
        g_lastOperationError != ERROR_SUCCESS &&
        GetTickCount64() < g_lastOperationErrorExpiresAt;
    if (showOperationError) {
        glyph = L"!";
        accessibleName = L"Failed to change the lid close action";
        tooltip =
            accessibleName + L":\n" +
            FormatError(g_lastOperationError);
    } else if (setting.mode == LidMode::Sleep) {
        glyph = L"\xE708";
        accessibleName = L"Lid close action: Sleep";
        tooltip =
            accessibleName +
            L"\n" + PowerScopeName(setting) +
            L"\nClick to switch to Do nothing";
    } else if (setting.mode == LidMode::DoNothing) {
        glyph = L"\xE733";
        accessibleName = L"Lid close action: Do nothing";
        tooltip =
            accessibleName +
            L"\n" + PowerScopeName(setting) +
            L"\nClick to switch to Sleep";
    } else if (setting.mode == LidMode::Other) {
        glyph = L"?";
        if (!setting.separatePowerSources) {
            if (setting.acValue != setting.dcValue) {
                accessibleName = L"Lid close actions differ";
            } else {
                accessibleName =
                    L"Lid close action: " +
                    PowerValueName(setting.acValue);
            }
            tooltip =
                accessibleName +
                L"\nPlugged in: " +
                PowerValueName(setting.acValue) +
                L"\nOn battery: " +
                PowerValueName(setting.dcValue) +
                L"\nClick to set both to Sleep";
        } else {
            accessibleName =
                L"Lid close action: " +
                PowerValueName(setting.activeValue);
            tooltip =
                accessibleName +
                L"\n" + PowerScopeName(setting) +
                L"\nClick to switch to Sleep";
        }
    } else {
        glyph = L"!";
        accessibleName = L"Lid close action is unavailable";
        tooltip =
            accessibleName +
            L"\n" + FormatError(setting.error);
    }

    g_icon.Glyph(glyph);
    AutomationProperties::SetName(g_button, accessibleName);
    AutomationProperties::SetHelpText(g_button, tooltip);
    ToolTipService::SetToolTip(
        g_button,
        winrt::box_value(winrt::hstring(tooltip)));
}

void OnButtonClick() {
    LidSetting current = ReadLidSetting();
    DWORD target = current.mode == LidMode::Sleep
                       ? kDoNothingValue
                       : kSleepValue;
    DWORD error = current.error;
    if (error == ERROR_SUCCESS) {
        error = current.separatePowerSources
                    ? WriteLidMode(current.source, target)
                    : WriteLinkedLidMode(target);
    }

    if (error == ERROR_RETRY) {
        g_lastOperationError = ERROR_SUCCESS;
        g_lastOperationErrorExpiresAt = 0;
        Wh_Log(L"Power source changed during click; no value was changed");
    } else if (error == ERROR_SUCCESS) {
        g_lastOperationError = ERROR_SUCCESS;
        g_lastOperationErrorExpiresAt = 0;
        if (current.separatePowerSources) {
            Wh_Log(L"Lid close action changed to %ls for %ls "
                   L"across all power plans",
                   target == kSleepValue ? L"sleep" : L"do nothing",
                   current.source == PowerSource::PluggedIn ? L"AC" : L"DC");
        } else {
            Wh_Log(L"Lid close action changed to %ls for AC and DC "
                   L"across all power plans",
                   target == kSleepValue ? L"sleep" : L"do nothing");
        }
    } else {
        g_lastOperationError = error;
        g_lastOperationErrorExpiresAt = GetTickCount64() + 8000;
        Wh_Log(L"Failed to change lid close action: %lu (%ls)",
               error,
               FormatError(error).c_str());
    }

    UpdateButtonVisual();
}

void ReleaseButtonReferences() {
    if (g_button) {
        try {
            if (g_tappedToken.value) {
                g_button.Tapped(g_tappedToken);
            }
            if (g_pointerEnteredToken.value) {
                g_button.PointerEntered(g_pointerEnteredToken);
            }
            if (g_pointerExitedToken.value) {
                g_button.PointerExited(g_pointerExitedToken);
            }
            if (g_pointerPressedToken.value) {
                g_button.PointerPressed(g_pointerPressedToken);
            }
            if (g_pointerReleasedToken.value) {
                g_button.PointerReleased(g_pointerReleasedToken);
            }
            if (g_pointerCanceledToken.value) {
                g_button.PointerCanceled(g_pointerCanceledToken);
            }
            if (g_pointerCaptureLostToken.value) {
                g_button.PointerCaptureLost(
                    g_pointerCaptureLostToken);
            }
            if (g_actualThemeChangedToken.value) {
                g_button.ActualThemeChanged(
                    g_actualThemeChangedToken);
            }
        } catch (...) {
        }
    }

    g_tappedToken = {};
    g_pointerEnteredToken = {};
    g_pointerExitedToken = {};
    g_pointerPressedToken = {};
    g_pointerReleasedToken = {};
    g_pointerCanceledToken = {};
    g_pointerCaptureLostToken = {};
    g_actualThemeChangedToken = {};
    g_buttonPointerOver = false;
    g_buttonPointerPressed = false;
    g_buttonNormalBackgroundBrush = nullptr;
    g_buttonHoverBackgroundBrush = nullptr;
    g_buttonPressedBackgroundBrush = nullptr;
    g_buttonNormalBorderBrush = nullptr;
    g_buttonHoverBorderBrush = nullptr;
    g_buttonNormalForegroundBrush = nullptr;
    g_buttonPressedForegroundBrush = nullptr;
    g_buttonBackground = nullptr;
    g_icon = nullptr;
    g_button = nullptr;
    g_injectionRoot = nullptr;
    g_injectionParent = nullptr;
}

bool IsCurrentRootInGrid(Grid const& trayGrid) {
    if (!trayGrid || !g_injectionRoot ||
        g_injectionParent != trayGrid) {
        return false;
    }

    uint32_t index = 0;
    return trayGrid.Children().IndexOf(g_injectionRoot, index);
}

void RemoveRootAndColumn(Grid const& trayGrid,
                         FrameworkElement const& root) {
    uint32_t rootIndex = 0;
    if (!trayGrid || !root ||
        !trayGrid.Children().IndexOf(root, rootIndex)) {
        return;
    }

    int column = Grid::GetColumn(root);
    trayGrid.Children().RemoveAt(rootIndex);

    if (column < 0 ||
        column >= static_cast<int>(
                      trayGrid.ColumnDefinitions().Size())) {
        return;
    }

    for (uint32_t i = 0; i < trayGrid.Children().Size(); i++) {
        auto child =
            trayGrid.Children().GetAt(i).try_as<FrameworkElement>();
        if (!child) {
            continue;
        }

        int childColumn = Grid::GetColumn(child);
        if (childColumn > column) {
            Grid::SetColumn(child, childColumn - 1);
        }
    }

    trayGrid.ColumnDefinitions().RemoveAt(column);
}

FrameworkElement FindInjectedRoot(Grid const& trayGrid) {
    if (!trayGrid) {
        return nullptr;
    }

    for (auto const& childObject : trayGrid.Children()) {
        auto child = childObject.try_as<FrameworkElement>();
        if (child && child.Name() == kRootName) {
            return child;
        }
    }
    return nullptr;
}

Grid BuildButtonRoot() {
    Grid root;
    root.Name(kRootName);
    root.MinWidth(32);
    root.VerticalAlignment(VerticalAlignment::Stretch);
    root.HorizontalAlignment(HorizontalAlignment::Stretch);
    Canvas::SetZIndex(root, 10000);

    Border background;
    background.Margin({0, 4, 0, 4});
    background.BorderThickness({1, 1, 1, 1});
    background.CornerRadius({4, 4, 4, 4});
    background.BackgroundSizing(
        BackgroundSizing::InnerBorderEdge);

    BrushTransition transition;
    transition.Duration(
        winrt::Windows::Foundation::TimeSpan{830000});
    background.BackgroundTransition(transition);

    FontIcon icon;
    icon.FontFamily(
        winrt::Windows::UI::Xaml::Media::FontFamily(
            L"Segoe Fluent Icons"));
    icon.FontSize(16);
    icon.HorizontalAlignment(HorizontalAlignment::Center);
    icon.VerticalAlignment(VerticalAlignment::Center);

    g_button = root;
    g_buttonBackground = background;
    g_icon = icon;
    ApplyTaskbarButtonStyle(root);

    g_tappedToken = root.Tapped(
        [](winrt::Windows::Foundation::IInspectable const&,
           wuxi::TappedRoutedEventArgs const&) {
            if (!g_unloading) {
                OnButtonClick();
            }
        });
    g_pointerEnteredToken = root.PointerEntered(
        [](winrt::Windows::Foundation::IInspectable const&,
           wuxi::PointerRoutedEventArgs const&) {
            if (!g_unloading) {
                g_buttonPointerOver = true;
                UpdateButtonInteractionVisual();
                UpdateButtonVisual();
            }
        });
    g_pointerExitedToken = root.PointerExited(
        [](winrt::Windows::Foundation::IInspectable const&,
           wuxi::PointerRoutedEventArgs const&) {
            if (!g_unloading) {
                g_buttonPointerOver = false;
                g_buttonPointerPressed = false;
                UpdateButtonInteractionVisual();
            }
        });
    g_pointerPressedToken = root.PointerPressed(
        [](winrt::Windows::Foundation::IInspectable const&,
           wuxi::PointerRoutedEventArgs const& args) {
            if (!g_unloading &&
                args.GetCurrentPoint(g_button)
                    .Properties()
                    .IsLeftButtonPressed()) {
                g_buttonPointerPressed = true;
                UpdateButtonInteractionVisual();
            }
        });
    g_pointerReleasedToken = root.PointerReleased(
        [](winrt::Windows::Foundation::IInspectable const&,
           wuxi::PointerRoutedEventArgs const&) {
            if (!g_unloading) {
                g_buttonPointerPressed = false;
                UpdateButtonInteractionVisual();
            }
        });
    g_pointerCanceledToken = root.PointerCanceled(
        [](winrt::Windows::Foundation::IInspectable const&,
           wuxi::PointerRoutedEventArgs const&) {
            if (!g_unloading) {
                g_buttonPointerPressed = false;
                UpdateButtonInteractionVisual();
            }
        });
    g_pointerCaptureLostToken = root.PointerCaptureLost(
        [](winrt::Windows::Foundation::IInspectable const&,
           wuxi::PointerRoutedEventArgs const&) {
            if (!g_unloading) {
                g_buttonPointerPressed = false;
                UpdateButtonInteractionVisual();
            }
        });
    g_actualThemeChangedToken = root.ActualThemeChanged(
        [](FrameworkElement const&,
           winrt::Windows::Foundation::IInspectable const&) {
            if (!g_unloading) {
                UpdateButtonThemeColors();
            }
        });

    root.Children().Append(background);
    root.Children().Append(icon);
    g_injectionRoot = root;
    return root;
}

bool EnsureButtonInjected() {
    Grid trayGrid{nullptr};
    Grid newRoot{nullptr};
    bool columnInserted = false;

    try {
        DebugLog(L"EnsureButtonInjected: starting");
        trayGrid = FindSystemTrayGrid();
        if (!trayGrid) {
            return false;
        }

        if (IsCurrentRootInGrid(trayGrid)) {
            DebugLog(L"EnsureButtonInjected: button is already present");
            UpdateButtonVisual();
            return true;
        }

        ReleaseButtonReferences();

        while (auto orphan = FindInjectedRoot(trayGrid)) {
            RemoveRootAndColumn(trayGrid, orphan);
        }

        newRoot = BuildButtonRoot();

        ColumnDefinition column;
        column.Width({1, GridUnitType::Auto});
        trayGrid.ColumnDefinitions().InsertAt(0, column);
        columnInserted = true;
        for (uint32_t i = 0; i < trayGrid.Children().Size(); i++) {
            auto child =
                trayGrid.Children().GetAt(i).try_as<FrameworkElement>();
            if (child) {
                Grid::SetColumn(child, Grid::GetColumn(child) + 1);
            }
        }

        Grid::SetColumn(newRoot, 0);
        trayGrid.Children().Append(newRoot);
        g_injectionParent = trayGrid;
        UpdateButtonThemeColors();
        UpdateButtonVisual();
        Wh_Log(L"Lid closing mode button injected");
        DebugLog(L"EnsureButtonInjected: button injected");
        return true;
    } catch (const winrt::hresult_error& error) {
        Wh_Log(L"Button injection failed: 0x%08X (%ls)",
               static_cast<unsigned>(error.code().value),
               error.message().c_str());
        wchar_t code[32];
        swprintf_s(code,
                   L"EnsureButtonInjected: failed with 0x%08X: ",
                   static_cast<unsigned>(error.code().value));
        DebugLog(std::wstring(code) + error.message().c_str());
    } catch (...) {
        Wh_Log(L"Button injection failed with an unknown exception");
        DebugLog(
            L"EnsureButtonInjected: failed with an unknown exception");
    }

    if (trayGrid && columnInserted) {
        try {
            uint32_t rootIndex = 0;
            if (newRoot &&
                trayGrid.Children().IndexOf(newRoot, rootIndex)) {
                trayGrid.Children().RemoveAt(rootIndex);
            }
            for (uint32_t i = 0; i < trayGrid.Children().Size(); i++) {
                auto child =
                    trayGrid.Children().GetAt(i)
                        .try_as<FrameworkElement>();
                if (child && Grid::GetColumn(child) > 0) {
                    Grid::SetColumn(child, Grid::GetColumn(child) - 1);
                }
            }
            if (trayGrid.ColumnDefinitions().Size() > 0) {
                trayGrid.ColumnDefinitions().RemoveAt(0);
            }
        } catch (...) {
            Wh_Log(L"Failed to roll back taskbar column injection");
            DebugLog(
                L"EnsureButtonInjected: failed to roll back injection");
        }
    }

    ReleaseButtonReferences();
    return false;
}

void RemoveButton() {
    try {
        Grid trayGrid = FindSystemTrayGrid();
        if (trayGrid) {
            if (IsCurrentRootInGrid(trayGrid)) {
                auto root = g_injectionRoot;
                ReleaseButtonReferences();
                RemoveRootAndColumn(trayGrid, root);
                return;
            }

            while (auto orphan = FindInjectedRoot(trayGrid)) {
                RemoveRootAndColumn(trayGrid, orphan);
            }
        }
    } catch (...) {
        Wh_Log(L"Failed to remove the lid closing mode button cleanly");
    }

    ReleaseButtonReferences();
}

void EnsureButtonOnTaskbarThread(void*) {
    if (!g_unloading) {
        EnsureButtonInjected();
    }
}

void RemoveButtonOnTaskbarThread(void*) {
    g_iconLoadedRevokers.clear();
    RemoveButton();
}

void SyncButtonWithTaskbar() {
    HWND hWnd = FindCurrentProcessTaskbarWnd();
    if (!hWnd) {
        DebugLog(L"SyncButtonWithTaskbar: taskbar window was not found");
        return;
    }

    g_taskbarWnd.store(hWnd, std::memory_order_release);
    if (!RunFromWindowThread(
            hWnd, EnsureButtonOnTaskbarThread, nullptr)) {
        Wh_Log(L"Failed to marshal button update to the taskbar thread");
        DebugLog(
            L"SyncButtonWithTaskbar: taskbar thread marshal failed");
    }
}

ULONG CALLBACK PowerSettingNotificationCallback(PVOID,
                                                 ULONG type,
                                                 PVOID setting) {
    g_activePowerCallbacks.fetch_add(1, std::memory_order_acq_rel);

    if (!g_unloading && type == PBT_POWERSETTINGCHANGE && setting) {
        const auto* powerSetting =
            static_cast<const POWERBROADCAST_SETTING*>(setting);
        if (InlineIsEqualGUID(powerSetting->PowerSetting,
                              kLidCloseAction) ||
            InlineIsEqualGUID(powerSetting->PowerSetting,
                              kActivePowerScheme) ||
            InlineIsEqualGUID(powerSetting->PowerSetting,
                              kAcDcPowerSource)) {
            SyncButtonWithTaskbar();
        }
    }

    g_activePowerCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    return ERROR_SUCCESS;
}

bool RegisterPowerNotifications() {
    DeviceNotifySubscribeParameters parameters{
        PowerSettingNotificationCallback,
        nullptr,
    };

    DWORD error = PowerSettingRegisterNotification(
        &kLidCloseAction,
        DEVICE_NOTIFY_CALLBACK,
        reinterpret_cast<HANDLE>(&parameters),
        &g_lidCloseNotification);
    if (error != ERROR_SUCCESS) {
        Wh_Log(L"Failed to register lid setting notifications: %lu",
               error);
        g_lidCloseNotification = nullptr;
        return false;
    }

    error = PowerSettingRegisterNotification(
        &kActivePowerScheme,
        DEVICE_NOTIFY_CALLBACK,
        reinterpret_cast<HANDLE>(&parameters),
        &g_activeSchemeNotification);
    if (error != ERROR_SUCCESS) {
        Wh_Log(L"Failed to register power scheme notifications: %lu",
               error);
        PowerSettingUnregisterNotification(g_lidCloseNotification);
        g_lidCloseNotification = nullptr;
        g_activeSchemeNotification = nullptr;
        return false;
    }

    error = PowerSettingRegisterNotification(
        &kAcDcPowerSource,
        DEVICE_NOTIFY_CALLBACK,
        reinterpret_cast<HANDLE>(&parameters),
        &g_powerSourceNotification);
    if (error != ERROR_SUCCESS) {
        Wh_Log(L"Failed to register power source notifications: %lu",
               error);
        PowerSettingUnregisterNotification(
            g_activeSchemeNotification);
        PowerSettingUnregisterNotification(g_lidCloseNotification);
        g_lidCloseNotification = nullptr;
        g_activeSchemeNotification = nullptr;
        g_powerSourceNotification = nullptr;
        return false;
    }

    Wh_Log(L"Power setting notifications registered");
    return true;
}

void UnregisterPowerNotifications() {
    HPOWERNOTIFY lidCloseNotification = g_lidCloseNotification;
    HPOWERNOTIFY activeSchemeNotification =
        g_activeSchemeNotification;
    HPOWERNOTIFY powerSourceNotification =
        g_powerSourceNotification;
    g_lidCloseNotification = nullptr;
    g_activeSchemeNotification = nullptr;
    g_powerSourceNotification = nullptr;

    if (lidCloseNotification) {
        DWORD error =
            PowerSettingUnregisterNotification(lidCloseNotification);
        if (error != ERROR_SUCCESS) {
            Wh_Log(L"Failed to unregister lid setting notifications: %lu",
                   error);
        }
    }

    if (activeSchemeNotification) {
        DWORD error = PowerSettingUnregisterNotification(
            activeSchemeNotification);
        if (error != ERROR_SUCCESS) {
            Wh_Log(
                L"Failed to unregister power scheme notifications: %lu",
                error);
        }
    }

    if (powerSourceNotification) {
        DWORD error = PowerSettingUnregisterNotification(
            powerSourceNotification);
        if (error != ERROR_SUCCESS) {
            Wh_Log(
                L"Failed to unregister power source notifications: %lu",
                error);
        }
    }

    while (g_activePowerCallbacks.load(std::memory_order_acquire) != 0) {
        Sleep(1);
    }
}

VS_FIXEDFILEINFO* GetModuleVersionInfo(HMODULE module) {
    HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    if (!resource) {
        return nullptr;
    }

    HGLOBAL resourceData = LoadResource(module, resource);
    if (!resourceData) {
        return nullptr;
    }

    void* data = LockResource(resourceData);
    if (!data) {
        return nullptr;
    }

    VS_FIXEDFILEINFO* versionInfo = nullptr;
    UINT versionInfoSize = 0;
    if (!VerQueryValueW(data,
                        L"\\",
                        reinterpret_cast<void**>(&versionInfo),
                        &versionInfoSize) ||
        versionInfoSize < sizeof(VS_FIXEDFILEINFO)) {
        return nullptr;
    }
    return versionInfo;
}

HMODULE GetSystemTrayModuleHandle() {
    HMODULE module = GetModuleHandleW(L"SystemTray.dll");
    if (module) {
        return module;
    }

    module = GetModuleHandleW(L"Taskbar.View.dll");
    if (module) {
        VS_FIXEDFILEINFO* versionInfo =
            GetModuleVersionInfo(module);
        WORD moduleMajor =
            versionInfo ? HIWORD(versionInfo->dwFileVersionMS) : 0;
        if (!moduleMajor || moduleMajor >= 2604) {
            module = nullptr;
        }
    }

    if (!module) {
        module = GetModuleHandleW(L"ExplorerExtensions.dll");
    }
    return module;
}

using IconView_IconView_t = void*(WINAPI*)(void* pThis);
IconView_IconView_t IconView_IconView_Original = nullptr;

void* WINAPI IconView_IconView_Hook(void* pThis) {
    void* result = IconView_IconView_Original(pThis);
    if (g_unloading) {
        return result;
    }

    FrameworkElement iconView{nullptr};
    reinterpret_cast<IUnknown**>(pThis)[1]->QueryInterface(
        winrt::guid_of<FrameworkElement>(),
        winrt::put_abi(iconView));
    if (!iconView) {
        SyncButtonWithTaskbar();
        return result;
    }

    g_iconLoadedRevokers.emplace_back();
    auto revokerIterator = std::prev(g_iconLoadedRevokers.end());
    *revokerIterator = iconView.Loaded(
        winrt::auto_revoke_t{},
        [revokerIterator](
            winrt::Windows::Foundation::IInspectable const&,
            RoutedEventArgs const&) {
            g_iconLoadedRevokers.erase(revokerIterator);
            if (!g_unloading) {
                SyncButtonWithTaskbar();
            }
        });

    return result;
}

bool HookSystemTraySymbols(HMODULE module) {
    WindhawkUtils::SYMBOL_HOOK hooks[] = {{
        {LR"(public: __cdecl winrt::SystemTray::implementation::IconView::IconView(void))"},
        &IconView_IconView_Original,
        IconView_IconView_Hook,
    }};

    return WindhawkUtils::HookSymbols(
        module, hooks, ARRAYSIZE(hooks));
}

void HandleLoadedModuleIfSystemTray(HMODULE module) {
    if (g_unloading || !module ||
        GetSystemTrayModuleHandle() != module) {
        return;
    }

    bool expected = false;
    if (!g_systemTrayModuleHooked.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }

    if (HookSystemTraySymbols(module)) {
        Wh_ApplyHookOperations();
        Wh_Log(L"System tray reconstruction hook installed");
    } else {
        g_systemTrayModuleHooked = false;
        Wh_Log(L"Failed to hook system tray reconstruction");
    }
}

using LoadLibraryExW_t =
    HMODULE(WINAPI*)(LPCWSTR fileName, HANDLE file, DWORD flags);
LoadLibraryExW_t LoadLibraryExW_Original = nullptr;

HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR fileName,
                                   HANDLE file,
                                   DWORD flags) {
    HMODULE module = LoadLibraryExW_Original(fileName, file, flags);
    if (module) {
        HandleLoadedModuleIfSystemTray(module);
    }
    return module;
}

bool ResolveTaskbarSymbols() {
    HMODULE taskbarModule = LoadLibraryExW(
        L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!taskbarModule) {
        Wh_Log(L"Failed to load taskbar.dll");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {
            {LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"},
            &CTaskBand_ITaskListWndSite_vftable,
        },
        {
            {LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
            &CTaskBand_GetTaskbarHost_Original,
        },
        {
            {LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
            &TaskbarHost_FrameHeight_Original,
        },
        {
            {LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"},
            &std__Ref_count_base__Decref_Original,
        },
    };

    return WindhawkUtils::HookSymbols(
        taskbarModule, hooks, ARRAYSIZE(hooks));
}

bool PrepareSystemTrayHook() {
    if (HMODULE systemTrayModule = GetSystemTrayModuleHandle()) {
        g_systemTrayModuleHooked = true;
        if (!HookSystemTraySymbols(systemTrayModule)) {
            g_systemTrayModuleHooked = false;
            Wh_Log(L"Failed to hook system tray reconstruction");
            return false;
        }
        return true;
    }

    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    auto loadLibraryExW = kernelBase
        ? reinterpret_cast<LoadLibraryExW_t>(
              GetProcAddress(kernelBase, "LoadLibraryExW"))
        : nullptr;
    if (!loadLibraryExW) {
        Wh_Log(L"Failed to locate LoadLibraryExW");
        return false;
    }

    WindhawkUtils::SetFunctionHook(
        loadLibraryExW,
        LoadLibraryExW_Hook,
        &LoadLibraryExW_Original);
    return true;
}

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L"Initializing Lid Closing Mode Button");
    LoadSettings();
    if (!ResolveTaskbarSymbols()) {
        Wh_Log(L"Failed to resolve taskbar symbols");
        return FALSE;
    }
    if (!PrepareSystemTrayHook()) {
        return FALSE;
    }
    if (!RegisterPowerNotifications()) {
        return FALSE;
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_systemTrayModuleHooked) {
        HandleLoadedModuleIfSystemTray(
            GetSystemTrayModuleHandle());
    }
    SyncButtonWithTaskbar();
}

void Wh_ModUninit() {
    g_unloading = true;
    UnregisterPowerNotifications();

    HWND hWnd = g_taskbarWnd.load(std::memory_order_acquire);
    if (!hWnd || !IsWindow(hWnd)) {
        hWnd = FindCurrentProcessTaskbarWnd();
    }
    if (hWnd) {
        RunFromWindowThread(
            hWnd, RemoveButtonOnTaskbarThread, nullptr);
    } else {
        g_iconLoadedRevokers.clear();
        ReleaseButtonReferences();
    }

    Wh_Log(L"Lid Closing Mode Button unloaded");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    g_lastOperationError = ERROR_SUCCESS;
    g_lastOperationErrorExpiresAt = 0;
    SyncButtonWithTaskbar();
}
