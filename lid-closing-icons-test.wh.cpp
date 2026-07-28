// ==WindhawkMod==
// @id              lid-closing-icons-test
// @name            Lid Closing Icon Preview
// @description     Shows five taskbar icon sets without changing power settings
// @version         1.0.0
// @author          Roma
// @include         explorer.exe
// @architecture    x86-64
// @license         MIT
// @compilerOptions -DWIN32_LEAN_AND_MEAN -lole32 -loleaut32 -lruntimeobject -lwindowsapp -luser32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Lid Closing Icon Preview

Adds five preview buttons immediately to the left of the Windows 11 system
tray. Click each button to cycle its icon through Sleep, Do nothing, and Shut
down.

The buttons, from left to right:

1. Standard QuietHours / Blocked / PowerButton glyphs
2. QuietHours with a custom diagonal strike-through
3. QuietHours with a small Blocked2 status badge
4. ActionCenterQuiet / Blocked2 system glyphs
5. MobQuietHours / custom circle-minus composition

This test mod never reads or changes power settings. It only changes the local
preview icons. Disable the production Lid Closing Mode Button while comparing
the sets if you want the preview row to be easier to identify.
*/
// ==/WindhawkModReadme==

#undef GetCurrentTime

#include <unknwn.h>

#include <windhawk_utils.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <array>
#include <atomic>
#include <functional>
#include <string>

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Automation;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

namespace wuxi = winrt::Windows::UI::Xaml::Input;

namespace {

constexpr wchar_t kRootName[] = L"LidClosingIconsTest_Root";
constexpr size_t kPreviewCount = 5;
constexpr size_t kStateCount = 3;

constexpr wchar_t kQuietHoursGlyph[] = L"\xE708";
constexpr wchar_t kBlockedGlyph[] = L"\xE733";
constexpr wchar_t kPowerButtonGlyph[] = L"\xE7E8";
constexpr wchar_t kActionCenterQuietGlyph[] = L"\xEE79";
constexpr wchar_t kMobQuietHoursGlyph[] = L"\xEC46";
constexpr wchar_t kBlocked2Glyph[] = L"\xECE4";
constexpr wchar_t kCircleRingGlyph[] = L"\xEA3A";
constexpr wchar_t kRemoveGlyph[] = L"\xE738";

std::atomic<bool> g_unloading{false};
std::atomic<HWND> g_taskbarWnd{nullptr};

Grid g_injectionRoot{nullptr};
Grid g_injectionParent{nullptr};

struct PreviewButton {
    Grid button{nullptr};
    Border background{nullptr};
    Grid iconHost{nullptr};
    FontIcon mainIcon{nullptr};
    FontIcon badgeIcon{nullptr};
    Border slash{nullptr};

    SolidColorBrush normalBackground{nullptr};
    SolidColorBrush hoverBackground{nullptr};
    SolidColorBrush pressedBackground{nullptr};
    SolidColorBrush normalBorder{nullptr};
    Brush hoverBorder{nullptr};
    SolidColorBrush normalForeground{nullptr};
    SolidColorBrush pressedForeground{nullptr};

    winrt::event_token tappedToken{};
    winrt::event_token pointerEnteredToken{};
    winrt::event_token pointerExitedToken{};
    winrt::event_token pointerPressedToken{};
    winrt::event_token pointerReleasedToken{};
    winrt::event_token pointerCanceledToken{};
    winrt::event_token pointerCaptureLostToken{};
    winrt::event_token actualThemeChangedToken{};

    size_t state = 0;
    bool pointerOver = false;
    bool pointerPressed = false;
};

std::array<PreviewButton, kPreviewCount> g_previews;

using CTaskBand_GetTaskbarHost_t =
    void*(WINAPI*)(void* pThis, void* taskbarHostSharedPtr);
CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original = nullptr;

using TaskbarHost_FrameHeight_t = int(WINAPI*)(void* pThis);
TaskbarHost_FrameHeight_t TaskbarHost_FrameHeight_Original = nullptr;

using std__Ref_count_base__Decref_t = void(WINAPI*)(void* pThis);
std__Ref_count_base__Decref_t std__Ref_count_base__Decref_Original = nullptr;

void* CTaskBand_ITaskListWndSite_vftable = nullptr;

using RunFromWindowThreadProc = void (*)(void*);

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
        return nullptr;
    }

    HWND hTaskSwWnd =
        reinterpret_cast<HWND>(GetPropW(hTaskbarWnd, L"TaskbandHWND"));
    if (!hTaskSwWnd) {
        return nullptr;
    }

    void* taskBand =
        reinterpret_cast<void*>(GetWindowLongPtrW(hTaskSwWnd, 0));
    if (!taskBand) {
        return nullptr;
    }

    void* taskBandForSite = taskBand;
    for (int i = 0;
         *reinterpret_cast<void**>(taskBandForSite) !=
         CTaskBand_ITaskListWndSite_vftable;
         i++) {
        if (i == 20) {
            return nullptr;
        }
        taskBandForSite =
            reinterpret_cast<void**>(taskBandForSite) + 1;
    }

    void* taskbarHostSharedPtr[2] = {};
    CTaskBand_GetTaskbarHost_Original(taskBandForSite,
                                      taskbarHostSharedPtr);
    if (!taskbarHostSharedPtr[0] || !taskbarHostSharedPtr[1]) {
        if (taskbarHostSharedPtr[1]) {
            std__Ref_count_base__Decref_Original(
                taskbarHostSharedPtr[1]);
        }
        return nullptr;
    }

    size_t taskbarElementOffset = 0x48;
#if defined(_M_X64)
    const BYTE* frameHeightCode =
        reinterpret_cast<const BYTE*>(TaskbarHost_FrameHeight_Original);
    if (frameHeightCode[0] == 0x48 && frameHeightCode[1] == 0x83 &&
        frameHeightCode[2] == 0xEC && frameHeightCode[4] == 0x48 &&
        frameHeightCode[5] == 0x83 && frameHeightCode[6] == 0xC1 &&
        frameHeightCode[7] <= 0x7F) {
        taskbarElementOffset = frameHeightCode[7];
    }
#endif

    auto* taskbarElementUnknown = *reinterpret_cast<IUnknown**>(
        reinterpret_cast<BYTE*>(taskbarHostSharedPtr[0]) +
        taskbarElementOffset);
    if (!taskbarElementUnknown) {
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
        return nullptr;
    }

    auto xamlRoot = GetTaskbarXamlRoot(taskbarWnd);
    if (!xamlRoot) {
        return nullptr;
    }

    auto content = xamlRoot.Content().try_as<FrameworkElement>();
    if (!content) {
        return nullptr;
    }

    return FindChildRecursive(
               content,
               [](FrameworkElement const& element) {
                   return element.Name() == L"SystemTrayFrameGrid";
               })
        .try_as<Grid>();
}

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

bool IsThemeLight(PreviewButton const& preview) {
    ElementTheme theme = preview.button.ActualTheme();
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

void SetIconForeground(PreviewButton& preview, Brush const& brush) {
    preview.mainIcon.Foreground(brush);
    preview.badgeIcon.Foreground(brush);
    preview.slash.Background(brush);
}

void UpdateInteractionVisual(PreviewButton& preview) {
    if (preview.pointerPressed) {
        preview.background.Background(preview.pressedBackground);
        preview.background.BorderBrush(preview.hoverBorder);
        SetIconForeground(preview, preview.pressedForeground);
    } else if (preview.pointerOver) {
        preview.background.Background(preview.hoverBackground);
        preview.background.BorderBrush(preview.hoverBorder);
        SetIconForeground(preview, preview.normalForeground);
    } else {
        preview.background.Background(preview.normalBackground);
        preview.background.BorderBrush(preview.normalBorder);
        SetIconForeground(preview, preview.normalForeground);
    }
}

void UpdateThemeColors(PreviewButton& preview) {
    if (IsHighContrastEnabled()) {
        preview.normalBackground.Color(SystemColor(COLOR_WINDOW));
        preview.hoverBackground.Color(SystemColor(COLOR_HIGHLIGHTTEXT));
        preview.pressedBackground.Color(SystemColor(COLOR_WINDOW));
        preview.normalBorder.Color(SystemColor(COLOR_WINDOW));
        preview.normalForeground.Color(SystemColor(COLOR_WINDOWTEXT));
        preview.pressedForeground.Color(SystemColor(COLOR_HIGHLIGHT));
        auto hoverBorder = SolidColorBrush();
        hoverBorder.Color(SystemColor(COLOR_HIGHLIGHT));
        preview.hoverBorder = hoverBorder;
    } else if (IsThemeLight(preview)) {
        preview.normalBackground.Color({0x00, 0xFF, 0xFF, 0xFF});
        preview.hoverBackground.Color({0x80, 0xFF, 0xFF, 0xFF});
        preview.pressedBackground.Color({0x4C, 0xFF, 0xFF, 0xFF});
        preview.normalBorder.Color({0x00, 0xFF, 0xFF, 0xFF});
        preview.normalForeground.Color({0xE4, 0x00, 0x00, 0x00});
        preview.pressedForeground.Color({0x9E, 0x00, 0x00, 0x00});
        preview.hoverBorder = CreateHoverBorderBrush(true);
    } else {
        preview.normalBackground.Color({0x00, 0xFF, 0xFF, 0xFF});
        preview.hoverBackground.Color({0x0F, 0xFF, 0xFF, 0xFF});
        preview.pressedBackground.Color({0x0A, 0xFF, 0xFF, 0xFF});
        preview.normalBorder.Color({0x00, 0xFF, 0xFF, 0xFF});
        preview.normalForeground.Color({0xFF, 0xFF, 0xFF, 0xFF});
        preview.pressedForeground.Color({0xC5, 0xFF, 0xFF, 0xFF});
        preview.hoverBorder = CreateHoverBorderBrush(false);
    }
    UpdateInteractionVisual(preview);
}

void UpdateAllThemeColors() {
    for (auto& preview : g_previews) {
        if (preview.button) {
            UpdateThemeColors(preview);
        }
    }
}

std::wstring StateName(size_t state) {
    switch (state % kStateCount) {
        case 0:
            return L"Sleep";
        case 1:
            return L"Do nothing";
        default:
            return L"Shut down";
    }
}

void ResetIconLayout(PreviewButton& preview) {
    preview.mainIcon.Visibility(Visibility::Visible);
    preview.mainIcon.FontSize(16);
    preview.mainIcon.Margin({0, 0, 0, 0});
    preview.mainIcon.HorizontalAlignment(HorizontalAlignment::Center);
    preview.mainIcon.VerticalAlignment(VerticalAlignment::Center);

    preview.badgeIcon.Visibility(Visibility::Collapsed);
    preview.badgeIcon.FontSize(9);
    preview.badgeIcon.Margin({0, 0, 1, 1});
    preview.badgeIcon.HorizontalAlignment(HorizontalAlignment::Right);
    preview.badgeIcon.VerticalAlignment(VerticalAlignment::Bottom);

    preview.slash.Visibility(Visibility::Collapsed);
}

void UpdatePreviewIcon(size_t index) {
    if (index >= g_previews.size()) {
        return;
    }

    auto& preview = g_previews[index];
    ResetIconLayout(preview);
    size_t state = preview.state % kStateCount;

    if (state == 2) {
        preview.mainIcon.Glyph(kPowerButtonGlyph);
    } else {
        switch (index) {
            case 0:
                // Standard Fluent glyphs.
                preview.mainIcon.Glyph(
                    state == 0 ? kQuietHoursGlyph : kBlockedGlyph);
                break;
            case 1:
                // The same moon with a native-width diagonal strike.
                preview.mainIcon.Glyph(kQuietHoursGlyph);
                if (state == 1) {
                    preview.slash.Visibility(Visibility::Visible);
                }
                break;
            case 2:
                // A small prohibited badge preserves the moon silhouette.
                preview.mainIcon.Glyph(kQuietHoursGlyph);
                if (state == 1) {
                    preview.mainIcon.FontSize(15);
                    preview.mainIcon.Margin({-2, -2, 0, 0});
                    preview.badgeIcon.Glyph(kBlocked2Glyph);
                    preview.badgeIcon.Visibility(Visibility::Visible);
                }
                break;
            case 3:
                // Alternative system-provided quiet and blocked glyphs.
                preview.mainIcon.Glyph(
                    state == 0 ? kActionCenterQuietGlyph
                               : kBlocked2Glyph);
                break;
            default:
                // Compact quiet glyph and a composed circle-minus.
                if (state == 0) {
                    preview.mainIcon.Glyph(kMobQuietHoursGlyph);
                } else {
                    preview.mainIcon.Glyph(kCircleRingGlyph);
                    preview.mainIcon.FontSize(17);
                    preview.badgeIcon.Glyph(kRemoveGlyph);
                    preview.badgeIcon.FontSize(10);
                    preview.badgeIcon.Margin({0, 0, 0, 0});
                    preview.badgeIcon.HorizontalAlignment(
                        HorizontalAlignment::Center);
                    preview.badgeIcon.VerticalAlignment(
                        VerticalAlignment::Center);
                    preview.badgeIcon.Visibility(Visibility::Visible);
                }
                break;
        }
    }

    std::wstring stateName = StateName(state);
    std::wstring nextStateName =
        StateName((state + 1) % kStateCount);
    std::wstring accessibleName =
        L"Icon set " + std::to_wstring(index + 1) +
        L": " + stateName;
    std::wstring tooltip =
        accessibleName +
        L"\nClick to preview " + nextStateName;
    AutomationProperties::SetName(preview.button, accessibleName);
    AutomationProperties::SetHelpText(preview.button, tooltip);
    ToolTipService::SetToolTip(
        preview.button,
        winrt::box_value(winrt::hstring(tooltip)));
}

void CyclePreview(size_t index) {
    if (index >= g_previews.size()) {
        return;
    }
    g_previews[index].state =
        (g_previews[index].state + 1) % kStateCount;
    UpdatePreviewIcon(index);
}

Grid CreatePreviewButton(size_t index) {
    auto& preview = g_previews[index];

    Grid button;
    SolidColorBrush hitTestBrush;
    hitTestBrush.Color({0x00, 0xFF, 0xFF, 0xFF});
    button.Background(hitTestBrush);
    button.MinWidth(32);
    button.VerticalAlignment(VerticalAlignment::Stretch);
    button.HorizontalAlignment(HorizontalAlignment::Stretch);

    Border background;
    background.Margin({0, 4, 0, 4});
    background.BorderThickness({1, 1, 1, 1});
    background.CornerRadius({4, 4, 4, 4});
    background.BackgroundSizing(BackgroundSizing::InnerBorderEdge);
    BrushTransition transition;
    transition.Duration(
        winrt::Windows::Foundation::TimeSpan{830000});
    background.BackgroundTransition(transition);

    Grid iconHost;
    iconHost.Width(24);
    iconHost.Height(24);
    iconHost.HorizontalAlignment(HorizontalAlignment::Center);
    iconHost.VerticalAlignment(VerticalAlignment::Center);
    iconHost.IsHitTestVisible(false);

    FontIcon mainIcon;
    mainIcon.FontFamily(FontFamily(L"Segoe Fluent Icons"));
    FontIcon badgeIcon;
    badgeIcon.FontFamily(FontFamily(L"Segoe Fluent Icons"));

    Border slash;
    slash.Width(18);
    slash.Height(1.5);
    slash.CornerRadius({0.75, 0.75, 0.75, 0.75});
    slash.HorizontalAlignment(HorizontalAlignment::Center);
    slash.VerticalAlignment(VerticalAlignment::Center);
    slash.RenderTransformOrigin({0.5, 0.5});
    RotateTransform slashRotation;
    slashRotation.Angle(-48);
    slash.RenderTransform(slashRotation);

    preview.button = button;
    preview.background = background;
    preview.iconHost = iconHost;
    preview.mainIcon = mainIcon;
    preview.badgeIcon = badgeIcon;
    preview.slash = slash;
    preview.normalBackground = SolidColorBrush();
    preview.hoverBackground = SolidColorBrush();
    preview.pressedBackground = SolidColorBrush();
    preview.normalBorder = SolidColorBrush();
    preview.normalForeground = SolidColorBrush();
    preview.pressedForeground = SolidColorBrush();

    preview.tappedToken = button.Tapped(
        [index](winrt::Windows::Foundation::IInspectable const&,
                wuxi::TappedRoutedEventArgs const&) {
            if (!g_unloading) {
                CyclePreview(index);
            }
        });
    preview.pointerEnteredToken = button.PointerEntered(
        [index](winrt::Windows::Foundation::IInspectable const&,
                wuxi::PointerRoutedEventArgs const&) {
            if (!g_unloading) {
                g_previews[index].pointerOver = true;
                UpdateInteractionVisual(g_previews[index]);
            }
        });
    preview.pointerExitedToken = button.PointerExited(
        [index](winrt::Windows::Foundation::IInspectable const&,
                wuxi::PointerRoutedEventArgs const&) {
            if (!g_unloading) {
                g_previews[index].pointerOver = false;
                g_previews[index].pointerPressed = false;
                UpdateInteractionVisual(g_previews[index]);
            }
        });
    preview.pointerPressedToken = button.PointerPressed(
        [index](winrt::Windows::Foundation::IInspectable const&,
                wuxi::PointerRoutedEventArgs const& args) {
            auto& item = g_previews[index];
            if (!g_unloading &&
                args.GetCurrentPoint(item.button)
                    .Properties()
                    .IsLeftButtonPressed()) {
                item.pointerPressed = true;
                UpdateInteractionVisual(item);
            }
        });
    preview.pointerReleasedToken = button.PointerReleased(
        [index](winrt::Windows::Foundation::IInspectable const&,
                wuxi::PointerRoutedEventArgs const&) {
            if (!g_unloading) {
                g_previews[index].pointerPressed = false;
                UpdateInteractionVisual(g_previews[index]);
            }
        });
    preview.pointerCanceledToken = button.PointerCanceled(
        [index](winrt::Windows::Foundation::IInspectable const&,
                wuxi::PointerRoutedEventArgs const&) {
            if (!g_unloading) {
                g_previews[index].pointerPressed = false;
                UpdateInteractionVisual(g_previews[index]);
            }
        });
    preview.pointerCaptureLostToken = button.PointerCaptureLost(
        [index](winrt::Windows::Foundation::IInspectable const&,
                wuxi::PointerRoutedEventArgs const&) {
            if (!g_unloading) {
                g_previews[index].pointerPressed = false;
                UpdateInteractionVisual(g_previews[index]);
            }
        });
    preview.actualThemeChangedToken = button.ActualThemeChanged(
        [](FrameworkElement const&,
           winrt::Windows::Foundation::IInspectable const&) {
            if (!g_unloading) {
                UpdateAllThemeColors();
            }
        });

    iconHost.Children().Append(mainIcon);
    iconHost.Children().Append(badgeIcon);
    iconHost.Children().Append(slash);
    button.Children().Append(background);
    button.Children().Append(iconHost);

    UpdateThemeColors(preview);
    UpdatePreviewIcon(index);
    return button;
}

void ReleasePreviewReferences() {
    for (auto& preview : g_previews) {
        if (preview.button) {
            try {
                if (preview.tappedToken.value) {
                    preview.button.Tapped(preview.tappedToken);
                }
                if (preview.pointerEnteredToken.value) {
                    preview.button.PointerEntered(
                        preview.pointerEnteredToken);
                }
                if (preview.pointerExitedToken.value) {
                    preview.button.PointerExited(
                        preview.pointerExitedToken);
                }
                if (preview.pointerPressedToken.value) {
                    preview.button.PointerPressed(
                        preview.pointerPressedToken);
                }
                if (preview.pointerReleasedToken.value) {
                    preview.button.PointerReleased(
                        preview.pointerReleasedToken);
                }
                if (preview.pointerCanceledToken.value) {
                    preview.button.PointerCanceled(
                        preview.pointerCanceledToken);
                }
                if (preview.pointerCaptureLostToken.value) {
                    preview.button.PointerCaptureLost(
                        preview.pointerCaptureLostToken);
                }
                if (preview.actualThemeChangedToken.value) {
                    preview.button.ActualThemeChanged(
                        preview.actualThemeChangedToken);
                }
            } catch (...) {
            }
        }
        preview = {};
    }
    g_injectionRoot = nullptr;
    g_injectionParent = nullptr;
}

Grid BuildPreviewRoot() {
    Grid root;
    root.Name(kRootName);
    root.VerticalAlignment(VerticalAlignment::Stretch);
    root.HorizontalAlignment(HorizontalAlignment::Stretch);
    Canvas::SetZIndex(root, 10000);

    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.VerticalAlignment(VerticalAlignment::Stretch);
    for (size_t i = 0; i < g_previews.size(); i++) {
        row.Children().Append(CreatePreviewButton(i));
    }
    root.Children().Append(row);
    g_injectionRoot = root;
    return root;
}

bool IsCurrentRootInGrid(Grid const& trayGrid) {
    if (!trayGrid || !g_injectionRoot ||
        g_injectionParent != trayGrid) {
        return false;
    }
    uint32_t index = 0;
    return trayGrid.Children().IndexOf(g_injectionRoot, index);
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
        if (child && Grid::GetColumn(child) > column) {
            Grid::SetColumn(child, Grid::GetColumn(child) - 1);
        }
    }
    trayGrid.ColumnDefinitions().RemoveAt(column);
}

bool EnsurePreviewInjected() {
    Grid trayGrid{nullptr};
    Grid newRoot{nullptr};
    bool columnInserted = false;

    try {
        trayGrid = FindSystemTrayGrid();
        if (!trayGrid) {
            return false;
        }
        if (IsCurrentRootInGrid(trayGrid)) {
            return true;
        }

        ReleasePreviewReferences();
        while (auto orphan = FindInjectedRoot(trayGrid)) {
            RemoveRootAndColumn(trayGrid, orphan);
        }

        newRoot = BuildPreviewRoot();
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
        Wh_Log(L"Five lid icon preview buttons injected");
        return true;
    } catch (const winrt::hresult_error& error) {
        Wh_Log(L"Icon preview injection failed: 0x%08X (%ls)",
               static_cast<unsigned>(error.code().value),
               error.message().c_str());
    } catch (...) {
        Wh_Log(L"Icon preview injection failed");
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
        }
    }
    ReleasePreviewReferences();
    return false;
}

void RemovePreview() {
    try {
        Grid trayGrid = FindSystemTrayGrid();
        if (trayGrid) {
            if (IsCurrentRootInGrid(trayGrid)) {
                auto root = g_injectionRoot;
                ReleasePreviewReferences();
                RemoveRootAndColumn(trayGrid, root);
                return;
            }
            while (auto orphan = FindInjectedRoot(trayGrid)) {
                RemoveRootAndColumn(trayGrid, orphan);
            }
        }
    } catch (...) {
        Wh_Log(L"Failed to remove icon preview buttons cleanly");
    }
    ReleasePreviewReferences();
}

void EnsurePreviewOnTaskbarThread(void*) {
    if (!g_unloading) {
        EnsurePreviewInjected();
    }
}

void RemovePreviewOnTaskbarThread(void*) {
    RemovePreview();
}

void SyncPreviewWithTaskbar() {
    HWND hWnd = FindCurrentProcessTaskbarWnd();
    if (!hWnd) {
        return;
    }
    g_taskbarWnd.store(hWnd, std::memory_order_release);
    RunFromWindowThread(
        hWnd, EnsurePreviewOnTaskbarThread, nullptr);
}

bool ResolveTaskbarSymbols() {
    HMODULE taskbarModule = LoadLibraryExW(
        L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!taskbarModule) {
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

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L"Initializing Lid Closing Icon Preview");
    return ResolveTaskbarSymbols();
}

void Wh_ModAfterInit() {
    SyncPreviewWithTaskbar();
}

void Wh_ModUninit() {
    g_unloading = true;
    HWND hWnd = g_taskbarWnd.load(std::memory_order_acquire);
    if (!hWnd || !IsWindow(hWnd)) {
        hWnd = FindCurrentProcessTaskbarWnd();
    }
    if (hWnd) {
        RunFromWindowThread(
            hWnd, RemovePreviewOnTaskbarThread, nullptr);
    } else {
        ReleasePreviewReferences();
    }
    Wh_Log(L"Lid Closing Icon Preview unloaded");
}
