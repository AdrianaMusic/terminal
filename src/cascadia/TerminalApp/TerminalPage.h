// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TerminalPage.g.h"
#include "TerminalTab.h"
#include "AppKeyBindings.h"
#include "AppCommandlineArgs.h"

static constexpr uint32_t DefaultRowsToScroll{ 3 };
static constexpr std::wstring_view TabletInputServiceKey{ L"TabletInputService" };
// fwdecl unittest classes
namespace TerminalAppLocalTests
{
    class TabTests;
    class SettingsTests;
};

namespace winrt::TerminalApp::implementation
{
    enum StartupState : int
    {
        NotInitialized = 0,
        InStartup = 1,
        Initialized = 2
    };

    enum ScrollDirection : int
    {
        ScrollUp = 0,
        ScrollDown = 1
    };

    struct TerminalPage : TerminalPageT<TerminalPage>
    {
    public:
        TerminalPage();

        // This implements shobjidl's IInitializeWithWindow, but due to a XAML Compiler bug we cannot
        // put it in our inheritance graph. https://github.com/microsoft/microsoft-ui-xaml/issues/3331
        STDMETHODIMP Initialize(HWND hwnd);

        winrt::fire_and_forget SetSettings(Microsoft::Terminal::Settings::Model::CascadiaSettings settings, bool needRefreshUI);

        void Create();

        hstring Title();

        void TitlebarClicked();

        float CalcSnappedDimension(const bool widthOrHeight, const float dimension) const;

        winrt::hstring ApplicationDisplayName();
        winrt::hstring ApplicationVersion();

        winrt::hstring ThirdPartyNoticesLink();

        winrt::fire_and_forget CloseWindow();

        void ToggleFocusMode();
        void ToggleFullscreen();
        void ToggleAlwaysOnTop();
        bool FocusMode() const;
        bool Fullscreen() const;
        bool AlwaysOnTop() const;

        void SetStartupActions(std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs>& actions);
        void SetInboundListener();
        static std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> ConvertExecuteCommandlineToActions(const Microsoft::Terminal::Settings::Model::ExecuteCommandlineArgs& args);

        winrt::TerminalApp::IDialogPresenter DialogPresenter() const;
        void DialogPresenter(winrt::TerminalApp::IDialogPresenter dialogPresenter);

        size_t GetLastActiveControlTaskbarState();
        size_t GetLastActiveControlTaskbarProgress();

        void ShowKeyboardServiceWarning();
        winrt::hstring KeyboardServiceDisabledText();

        winrt::fire_and_forget ProcessStartupActions(Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::ActionAndArgs> actions,
                                                     const bool initial,
                                                     const winrt::hstring cwd = L"");

        // -------------------------------- WinRT Events ---------------------------------
        TYPED_EVENT(TitleChanged, IInspectable, winrt::hstring);
        TYPED_EVENT(LastTabClosed, IInspectable, winrt::TerminalApp::LastTabClosedEventArgs);
        TYPED_EVENT(SetTitleBarContent, IInspectable, winrt::Windows::UI::Xaml::UIElement);
        TYPED_EVENT(FocusModeChanged, IInspectable, IInspectable);
        TYPED_EVENT(FullscreenChanged, IInspectable, IInspectable);
        TYPED_EVENT(AlwaysOnTopChanged, IInspectable, IInspectable);
        TYPED_EVENT(RaiseVisualBell, IInspectable, IInspectable);
        TYPED_EVENT(SetTaskbarProgress, IInspectable, IInspectable);
        TYPED_EVENT(Initialized, IInspectable, winrt::Windows::UI::Xaml::RoutedEventArgs);

    private:
        friend struct TerminalPageT<TerminalPage>; // for Xaml to bind events
        std::optional<HWND> _hostingHwnd;

        // If you add controls here, but forget to null them either here or in
        // the ctor, you're going to have a bad time. It'll mysteriously fail to
        // activate the app.
        // ALSO: If you add any UIElements as roots here, make sure they're
        // updated in App::_ApplyTheme. The roots currently is _tabRow
        // (which is a root when the tabs are in the titlebar.)
        Microsoft::UI::Xaml::Controls::TabView _tabView{ nullptr };
        TerminalApp::TabRowControl _tabRow{ nullptr };
        Windows::UI::Xaml::Controls::Grid _tabContent{ nullptr };
        Microsoft::UI::Xaml::Controls::SplitButton _newTabButton{ nullptr };

        Microsoft::Terminal::Settings::Model::CascadiaSettings _settings{ nullptr };

        Windows::Foundation::Collections::IObservableVector<TerminalApp::TabBase> _tabs;
        Windows::Foundation::Collections::IObservableVector<TerminalApp::TabBase> _mruTabs;
        static winrt::com_ptr<TerminalTab> _GetTerminalTabImpl(const TerminalApp::TabBase& tab);

        void _UpdateTabIndices();

        TerminalApp::SettingsTab _settingsTab{ nullptr };

        bool _isInFocusMode{ false };
        bool _isFullscreen{ false };
        bool _isAlwaysOnTop{ false };

        bool _rearranging;
        std::optional<int> _rearrangeFrom;
        std::optional<int> _rearrangeTo;
        bool _removing{ false };

        uint32_t _systemRowsToScroll{ DefaultRowsToScroll };

        // use a weak reference to prevent circular dependency with AppLogic
        winrt::weak_ref<winrt::TerminalApp::IDialogPresenter> _dialogPresenter;

        winrt::com_ptr<AppKeyBindings> _bindings{ winrt::make_self<implementation::AppKeyBindings>() };
        winrt::com_ptr<ShortcutActionDispatch> _actionDispatch{ winrt::make_self<implementation::ShortcutActionDispatch>() };

        winrt::Windows::UI::Xaml::Controls::Grid::LayoutUpdated_revoker _layoutUpdatedRevoker;
        StartupState _startupState{ StartupState::NotInitialized };

        Windows::Foundation::Collections::IVector<Microsoft::Terminal::Settings::Model::ActionAndArgs> _startupActions;
        bool _shouldStartInboundListener{ false };

        void _ShowAboutDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowCloseWarningDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowCloseReadOnlyDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowMultiLinePasteWarningDialog();
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> _ShowLargePasteWarningDialog();

        void _CreateNewTabFlyout();
        void _OpenNewTabDropdown();
        void _OpenNewTab(const Microsoft::Terminal::Settings::Model::NewTerminalArgs& newTerminalArgs, winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection = nullptr);
        void _CreateNewTabFromSettings(GUID profileGuid, Microsoft::Terminal::Settings::Model::TerminalSettings settings, winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection = nullptr);
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection _CreateConnectionFromSettings(GUID profileGuid, Microsoft::Terminal::Settings::Model::TerminalSettings settings);

        winrt::fire_and_forget _OpenNewWindow(const bool elevate, const Microsoft::Terminal::Settings::Model::NewTerminalArgs newTerminalArgs);

        bool _displayingCloseDialog{ false };
        void _SettingsButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _FeedbackButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _AboutButtonOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _ThirdPartyNoticesOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);

        void _KeyDownHandler(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e);
        void _SUIPreviewKeyDownHandler(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e);
        void _HookupKeyBindings(const Microsoft::Terminal::Settings::Model::KeyMapping& keymap) noexcept;
        void _RegisterActionCallbacks();

        void _UpdateTitle(const TerminalTab& tab);
        void _UpdateTabIcon(TerminalTab& tab);
        void _UpdateTabView();
        void _UpdateTabWidthMode();
        void _UpdateCommandsForPalette();

        static winrt::Windows::Foundation::Collections::IMap<winrt::hstring, Microsoft::Terminal::Settings::Model::Command> _ExpandCommands(Windows::Foundation::Collections::IMapView<winrt::hstring, Microsoft::Terminal::Settings::Model::Command> commandsToExpand,
                                                                                                                                            Windows::Foundation::Collections::IVectorView<Microsoft::Terminal::Settings::Model::Profile> profiles,
                                                                                                                                            Windows::Foundation::Collections::IMapView<winrt::hstring, Microsoft::Terminal::Settings::Model::ColorScheme> schemes);

        void _DuplicateFocusedTab();
        void _DuplicateTab(const TerminalTab& tab);
        void _RemoveTabViewItem(const Microsoft::UI::Xaml::Controls::TabViewItem& tabViewItem);
        winrt::Windows::Foundation::IAsyncAction _RemoveTab(winrt::TerminalApp::TabBase tab);
        winrt::fire_and_forget _RemoveTabs(const std::vector<winrt::TerminalApp::TabBase> tabs);

        void _RegisterTerminalEvents(Microsoft::Terminal::Control::TermControl term, TerminalTab& hostingTab);

        void _SelectNextTab(const bool bMoveRight, const Windows::Foundation::IReference<Microsoft::Terminal::Settings::Model::TabSwitcherMode>& customTabSwitcherMode);
        bool _SelectTab(const uint32_t tabIndex);
        void _MoveFocus(const Microsoft::Terminal::Settings::Model::FocusDirection& direction);

        winrt::Microsoft::Terminal::Control::TermControl _GetActiveControl();
        std::optional<uint32_t> _GetFocusedTabIndex() const noexcept;
        TerminalApp::TabBase _GetFocusedTab() const noexcept;
        winrt::com_ptr<TerminalTab> _GetFocusedTabImpl() const noexcept;

        winrt::fire_and_forget _SetFocusedTabIndex(const uint32_t tabIndex);
        void _CloseFocusedTab();
        winrt::fire_and_forget _CloseFocusedPane();

        winrt::fire_and_forget _RemoveOnCloseRoutine(Microsoft::UI::Xaml::Controls::TabViewItem tabViewItem, winrt::com_ptr<TerminalPage> page);

        // Todo: add more event implementations here
        // MSFT:20641986: Add keybindings for New Window
        void _Scroll(ScrollDirection scrollDirection, const Windows::Foundation::IReference<uint32_t>& rowsToScroll);

        void _SplitPane(const Microsoft::Terminal::Settings::Model::SplitState splitType,
                        const Microsoft::Terminal::Settings::Model::SplitType splitMode = Microsoft::Terminal::Settings::Model::SplitType::Manual,
                        const float splitSize = 0.5f,
                        const Microsoft::Terminal::Settings::Model::NewTerminalArgs& newTerminalArgs = nullptr);
        void _ResizePane(const Microsoft::Terminal::Settings::Model::ResizeDirection& direction);

        void _ScrollPage(ScrollDirection scrollDirection);
        void _ScrollToBufferEdge(ScrollDirection scrollDirection);
        void _SetAcceleratorForMenuItem(Windows::UI::Xaml::Controls::MenuFlyoutItem& menuItem, const winrt::Microsoft::Terminal::Control::KeyChord& keyChord);

        winrt::fire_and_forget _CopyToClipboardHandler(const IInspectable sender, const winrt::Microsoft::Terminal::Control::CopyToClipboardEventArgs copiedData);
        winrt::fire_and_forget _PasteFromClipboardHandler(const IInspectable sender,
                                                          const Microsoft::Terminal::Control::PasteFromClipboardEventArgs eventArgs);

        void _OpenHyperlinkHandler(const IInspectable sender, const Microsoft::Terminal::Control::OpenHyperlinkEventArgs eventArgs);
        bool _IsUriSupported(const winrt::Windows::Foundation::Uri& parsedUri);

        void _ShowCouldNotOpenDialog(winrt::hstring reason, winrt::hstring uri);
        bool _CopyText(const bool singleLine, const Windows::Foundation::IReference<Microsoft::Terminal::Control::CopyFormat>& formats);

        void _SetTaskbarProgressHandler(const IInspectable sender, const IInspectable eventArgs);

        void _PasteText();

        void _ControlNoticeRaisedHandler(const IInspectable sender, const Microsoft::Terminal::Control::NoticeEventArgs eventArgs);
        void _ShowControlNoticeDialog(const winrt::hstring& title, const winrt::hstring& message);

        fire_and_forget _LaunchSettings(const Microsoft::Terminal::Settings::Model::SettingsTarget target);

        void _OnTabClick(const IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs);
        void _OnTabSelectionChanged(const IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& eventArgs);
        void _OnTabItemsChanged(const IInspectable& sender, const Windows::Foundation::Collections::IVectorChangedEventArgs& eventArgs);
        void _OnContentSizeChanged(const IInspectable& /*sender*/, Windows::UI::Xaml::SizeChangedEventArgs const& e);
        void _OnTabCloseRequested(const IInspectable& sender, const Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs& eventArgs);
        void _OnFirstLayout(const IInspectable& sender, const IInspectable& eventArgs);
        void _UpdatedSelectedTab(const int32_t index);

        void _OnDispatchCommandRequested(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::Command& command);
        void _OnCommandLineExecutionRequested(const IInspectable& sender, const winrt::hstring& commandLine);
        void _OnSwitchToTabRequested(const IInspectable& sender, const winrt::TerminalApp::TabBase& tab);

        void _Find();

        winrt::Microsoft::Terminal::Control::TermControl _InitControl(const winrt::Microsoft::Terminal::Settings::Model::TerminalSettings& settings,
                                                                      const winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection& connection);

        winrt::fire_and_forget _RefreshUIForSettingsReload();

        void _SetNonClientAreaColors(const Windows::UI::Color& selectedTabColor);
        void _ClearNonClientAreaColors();
        void _SetNewTabButtonColor(const Windows::UI::Color& color, const Windows::UI::Color& accentColor);
        void _ClearNewTabButtonColor();

        void _CompleteInitialization();

        void _CommandPaletteClosed(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);

        void _UnZoomIfNeeded();

        void _OpenSettingsUI();

        static int _ComputeScrollDelta(ScrollDirection scrollDirection, const uint32_t rowsToScroll);
        static uint32_t _ReadSystemRowsToScroll();

        void _UpdateMRUTab(const uint32_t index);

        void _TryMoveTab(const uint32_t currentTabIndex, const int32_t suggestedNewTabIndex);

        bool _shouldMouseVanish{ false };
        bool _isMouseHidden{ false };
        Windows::UI::Core::CoreCursor _defaultPointerCursor{ nullptr };
        void _HidePointerCursorHandler(const IInspectable& sender, const IInspectable& eventArgs);
        void _RestorePointerCursorHandler(const IInspectable& sender, const IInspectable& eventArgs);

        void _OnNewConnection(winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection);

#pragma region ActionHandlers
        // These are all defined in AppActionHandlers.cpp
        void _HandleOpenNewTabDropdown(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleDuplicateTab(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleCloseTab(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleClosePane(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleScrollUp(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleScrollDown(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleNextTab(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandlePrevTab(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleSendInput(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleSplitPane(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleTogglePaneZoom(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleScrollUpPage(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleScrollDownPage(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleScrollToTop(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleScrollToBottom(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleOpenSettings(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandlePasteText(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleNewTab(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleSwitchToTab(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleResizePane(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleMoveFocus(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleCopyText(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleCloseWindow(const IInspectable&, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleAdjustFontSize(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleFind(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleResetFontSize(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleToggleShaderEffects(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleToggleFocusMode(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleToggleFullscreen(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleToggleAlwaysOnTop(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleSetColorScheme(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleSetTabColor(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleOpenTabColorPicker(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleRenameTab(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleOpenTabRenamer(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleExecuteCommandline(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleToggleCommandPalette(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleCloseOtherTabs(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleCloseTabsAfter(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleOpenTabSearch(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleMoveTab(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleBreakIntoDebugger(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleFindMatch(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleTogglePaneReadOnly(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleNewWindow(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);
        void _HandleToggleInboundPty(const IInspectable& sender, const Microsoft::Terminal::Settings::Model::ActionEventArgs& args);

        // Make sure to hook new actions up in _RegisterActionCallbacks!
#pragma endregion

        friend class TerminalAppLocalTests::TabTests;
        friend class TerminalAppLocalTests::SettingsTests;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TerminalPage);
}
