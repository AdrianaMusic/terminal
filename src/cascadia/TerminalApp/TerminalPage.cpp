// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"
#include "Utils.h"
#include "../../types/inc/utils.hpp"

#include <LibraryResources.h>

#include "TerminalPage.g.cpp"
#include <winrt/Windows.Storage.h>

#include "TabRowControl.h"
#include "ColorHelper.h"
#include "DebugTapConnection.h"
#include "SettingsTab.h"

using namespace winrt;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::System;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
}

namespace winrt::TerminalApp::implementation
{
    TerminalPage::TerminalPage() :
        _tabs{ winrt::single_threaded_observable_vector<TerminalApp::TabBase>() },
        _mruTabs{ winrt::single_threaded_observable_vector<TerminalApp::TabBase>() },
        _startupActions{ winrt::single_threaded_vector<ActionAndArgs>() },
        _hostingHwnd{}
    {
        InitializeComponent();
    }

    // Method Description:
    // - implements the IInitializeWithWindow interface from shobjidl_core.
    HRESULT TerminalPage::Initialize(HWND hwnd)
    {
        _hostingHwnd = hwnd;
        return S_OK;
    }

    // Function Description:
    // - Recursively check our commands to see if there's a keybinding for
    //   exactly their action. If there is, label that command with the text
    //   corresponding to that key chord.
    // - Will recurse into nested commands as well.
    // Arguments:
    // - settings: The settings who's keybindings we should use to look up the key chords from
    // - commands: The list of commands to label.
    static void _recursiveUpdateCommandKeybindingLabels(CascadiaSettings settings,
                                                        IMapView<winrt::hstring, Command> commands)
    {
        for (const auto& nameAndCmd : commands)
        {
            const auto& command = nameAndCmd.Value();
            // If there's a keybinding that's bound to exactly this command,
            // then get the string for that keychord and display it as a
            // part of the command in the UI. Each Command's KeyChordText is
            // unset by default, so we don't need to worry about clearing it
            // if there isn't a key associated with it.
            auto keyChord{ settings.KeyMap().GetKeyBindingForActionWithArgs(command.Action()) };

            if (keyChord)
            {
                command.KeyChordText(KeyChordSerialization::ToString(keyChord));
            }
            if (command.HasNestedCommands())
            {
                _recursiveUpdateCommandKeybindingLabels(settings, command.NestedCommands());
            }
        }
    }

    winrt::fire_and_forget TerminalPage::SetSettings(CascadiaSettings settings, bool needRefreshUI)
    {
        _settings = settings;

        auto weakThis{ get_weak() };
        co_await winrt::resume_foreground(Dispatcher());
        if (auto page{ weakThis.get() })
        {
            // Make sure to _UpdateCommandsForPalette before
            // _RefreshUIForSettingsReload. _UpdateCommandsForPalette will make
            // sure the KeyChordText of Commands is updated, which needs to
            // happen before the Settings UI is reloaded and tries to re-read
            // those values.
            _UpdateCommandsForPalette();
            CommandPalette().SetKeyMap(_settings.KeyMap());

            if (needRefreshUI)
            {
                _RefreshUIForSettingsReload();
            }

            // Upon settings update we reload the system settings for scrolling as well.
            // TODO: consider reloading this value periodically.
            _systemRowsToScroll = _ReadSystemRowsToScroll();
        }
    }

    void TerminalPage::Create()
    {
        // Hookup the key bindings
        _HookupKeyBindings(_settings.KeyMap());

        _tabContent = this->TabContent();
        _tabRow = this->TabRow();
        _tabView = _tabRow.TabView();
        _rearranging = false;

        // GH#2455 - Make sure to try/catch calls to Application::Current,
        // because that _won't_ be an instance of TerminalApp::App in the
        // LocalTests
        auto isElevated = false;
        try
        {
            // GH#3581 - There's a platform limitation that causes us to crash when we rearrange tabs.
            // Xaml tries to send a drag visual (to wit: a screenshot) to the drag hosting process,
            // but that process is running at a different IL than us.
            // For now, we're disabling elevated drag.
            isElevated = ::winrt::Windows::UI::Xaml::Application::Current().as<::winrt::TerminalApp::App>().Logic().IsElevated();
        }
        CATCH_LOG();

        _tabRow.PointerMoved({ get_weak(), &TerminalPage::_RestorePointerCursorHandler });
        _tabView.CanReorderTabs(!isElevated);
        _tabView.CanDragTabs(!isElevated);

        _tabView.TabDragStarting([weakThis{ get_weak() }](auto&& /*o*/, auto&& /*a*/) {
            if (auto page{ weakThis.get() })
            {
                page->_rearranging = true;
                page->_rearrangeFrom = std::nullopt;
                page->_rearrangeTo = std::nullopt;
            }
        });

        _tabView.TabDragCompleted([weakThis{ get_weak() }](auto&& /*o*/, auto&& /*a*/) {
            if (auto page{ weakThis.get() })
            {
                auto& from{ page->_rearrangeFrom };
                auto& to{ page->_rearrangeTo };

                if (from.has_value() && to.has_value() && to != from)
                {
                    auto& tabs{ page->_tabs };
                    auto tab = tabs.GetAt(from.value());
                    tabs.RemoveAt(from.value());
                    tabs.InsertAt(to.value(), tab);
                    page->_UpdateTabIndices();
                }

                page->_rearranging = false;
                from = std::nullopt;
                to = std::nullopt;
            }
        });

        auto tabRowImpl = winrt::get_self<implementation::TabRowControl>(_tabRow);
        _newTabButton = tabRowImpl->NewTabButton();

        if (_settings.GlobalSettings().ShowTabsInTitlebar())
        {
            // Remove the TabView from the page. We'll hang on to it, we need to
            // put it in the titlebar.
            uint32_t index = 0;
            if (this->Root().Children().IndexOf(_tabRow, index))
            {
                this->Root().Children().RemoveAt(index);
            }

            // Inform the host that our titlebar content has changed.
            _SetTitleBarContentHandlers(*this, _tabRow);
        }

        // Hookup our event handlers to the ShortcutActionDispatch
        _RegisterActionCallbacks();

        // Hook up inbound connection event handler
        TerminalConnection::ConptyConnection::NewConnection({ this, &TerminalPage::_OnNewConnection });

        //Event Bindings (Early)
        _newTabButton.Click([weakThis{ get_weak() }](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                // if alt is pressed, open a pane
                const CoreWindow window = CoreWindow::GetForCurrentThread();
                const auto rAltState = window.GetKeyState(VirtualKey::RightMenu);
                const auto lAltState = window.GetKeyState(VirtualKey::LeftMenu);
                const bool altPressed = WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) ||
                                        WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);

                const auto shiftState{ window.GetKeyState(VirtualKey::Shift) };
                const auto rShiftState = window.GetKeyState(VirtualKey::RightShift);
                const auto lShiftState = window.GetKeyState(VirtualKey::LeftShift);
                const auto shiftPressed{ WI_IsFlagSet(shiftState, CoreVirtualKeyStates::Down) ||
                                         WI_IsFlagSet(lShiftState, CoreVirtualKeyStates::Down) ||
                                         WI_IsFlagSet(rShiftState, CoreVirtualKeyStates::Down) };

                // Check for DebugTap
                bool debugTap = page->_settings.GlobalSettings().DebugFeaturesEnabled() &&
                                WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) &&
                                WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);

                if (altPressed && !debugTap)
                {
                    page->_SplitPane(SplitState::Automatic,
                                     SplitType::Manual,
                                     0.5f,
                                     nullptr);
                }
                else if (shiftPressed && !debugTap)
                {
                    page->_OpenNewWindow(false, NewTerminalArgs());
                }
                else
                {
                    page->_OpenNewTab(nullptr);
                }
            }
        });
        _tabView.SelectionChanged({ this, &TerminalPage::_OnTabSelectionChanged });
        _tabView.TabCloseRequested({ this, &TerminalPage::_OnTabCloseRequested });
        _tabView.TabItemsChanged({ this, &TerminalPage::_OnTabItemsChanged });

        _CreateNewTabFlyout();

        _UpdateTabWidthMode();

        _tabContent.SizeChanged({ this, &TerminalPage::_OnContentSizeChanged });

        // When the visibility of the command palette changes to "collapsed",
        // the palette has been closed. Toss focus back to the currently active
        // control.
        CommandPalette().RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), [this](auto&&, auto&&) {
            if (CommandPalette().Visibility() == Visibility::Collapsed)
            {
                _CommandPaletteClosed(nullptr, nullptr);
            }
        });
        CommandPalette().DispatchCommandRequested({ this, &TerminalPage::_OnDispatchCommandRequested });
        CommandPalette().CommandLineExecutionRequested({ this, &TerminalPage::_OnCommandLineExecutionRequested });
        CommandPalette().SwitchToTabRequested({ this, &TerminalPage::_OnSwitchToTabRequested });

        // Settings AllowDependentAnimations will affect whether animations are
        // enabled application-wide, so we don't need to check it each time we
        // want to create an animation.
        WUX::Media::Animation::Timeline::AllowDependentAnimations(!_settings.GlobalSettings().DisableAnimations());

        // Once the page is actually laid out on the screen, trigger all our
        // startup actions. Things like Panes need to know at least how big the
        // window will be, so they can subdivide that space.
        //
        // _OnFirstLayout will remove this handler so it doesn't get called more than once.
        _layoutUpdatedRevoker = _tabContent.LayoutUpdated(winrt::auto_revoke, { this, &TerminalPage::_OnFirstLayout });

        _isAlwaysOnTop = _settings.GlobalSettings().AlwaysOnTop();

        // Setup mouse vanish attributes
        SystemParametersInfoW(SPI_GETMOUSEVANISH, 0, &_shouldMouseVanish, false);

        // Store cursor, so we can restore it, e.g., after mouse vanishing
        // (we'll need to adapt this logic once we make cursor context aware)
        try
        {
            _defaultPointerCursor = CoreWindow::GetForCurrentThread().PointerCursor();
        }
        CATCH_LOG();
    }

    // Method Description:
    // - This method is called once command palette action was chosen for dispatching
    //   We'll use this event to dispatch this command.
    // Arguments:
    // - command - command to dispatch
    // Return Value:
    // - <none>
    void TerminalPage::_OnDispatchCommandRequested(const IInspectable& /*sender*/, const Microsoft::Terminal::Settings::Model::Command& command)
    {
        const auto& actionAndArgs = command.Action();
        _actionDispatch->DoAction(actionAndArgs);
    }

    // Method Description:
    // - This method is called once command palette command line was chosen for execution
    //   We'll use this event to create a command line execution command and dispatch it.
    // Arguments:
    // - command - command to dispatch
    // Return Value:
    // - <none>
    void TerminalPage::_OnCommandLineExecutionRequested(const IInspectable& /*sender*/, const winrt::hstring& commandLine)
    {
        ExecuteCommandlineArgs args{ commandLine };
        ActionAndArgs actionAndArgs{ ShortcutAction::ExecuteCommandline, args };
        _actionDispatch->DoAction(actionAndArgs);
    }

    // Method Description:
    // - This method is called once a tab was selected in tab switcher
    //   We'll use this event to select the relevant tab
    // Arguments:
    // - tab - tab to select
    // Return Value:
    // - <none>
    void TerminalPage::_OnSwitchToTabRequested(const IInspectable& /*sender*/, const winrt::TerminalApp::TabBase& tab)
    {
        uint32_t index{};
        if (_tabs.IndexOf(tab, index))
        {
            _SelectTab(index);
        }
    }

    // Method Description:
    // - This method is called once on startup, on the first LayoutUpdated event.
    //   We'll use this event to know that we have an ActualWidth and
    //   ActualHeight, so we can now attempt to process our list of startup
    //   actions.
    // - We'll remove this event handler when the event is first handled.
    // - If there are no startup actions, we'll open a single tab with the
    //   default profile.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TerminalPage::_OnFirstLayout(const IInspectable& /*sender*/, const IInspectable& /*eventArgs*/)
    {
        // Only let this succeed once.
        _layoutUpdatedRevoker.revoke();

        // This event fires every time the layout changes, but it is always the
        // last one to fire in any layout change chain. That gives us great
        // flexibility in finding the right point at which to initialize our
        // renderer (and our terminal). Any earlier than the last layout update
        // and we may not know the terminal's starting size.
        if (_startupState == StartupState::NotInitialized)
        {
            _startupState = StartupState::InStartup;
            ProcessStartupActions(_startupActions, true);

            // If we were told that the COM server needs to be started to listen for incoming
            // default application connections, start it now.
            // This MUST be done after we've registered the event listener for the new connections
            // or the COM server might start receiving requests on another thread and dispatch
            // them to nowhere.
            if (_shouldStartInboundListener)
            {
                try
                {
                    winrt::Microsoft::Terminal::TerminalConnection::ConptyConnection::StartInboundListener();
                }
                // If we failed to start the listener, it will throw.
                // We should fail fast here or the Terminal will be in a very strange state.
                // We only start the listener if the Terminal was started with the COM server
                // `-Embedding` flag and we make no tabs as a result.
                // Therefore, if the listener cannot start itself up to make that tab with
                // the inbound connection that caused the COM activation in the first place...
                // we would be left with an empty terminal frame with no tabs.
                // Instead, crash out so COM sees the server die and things unwind
                // without a weird empty frame window.
                CATCH_FAIL_FAST()
            }
        }
    }

    // Method Description:
    // - Process all the startup actions in the provided list of startup
    //   actions. We'll do this all at once here.
    // Arguments:
    // - actions: a winrt vector of actions to process. Note that this must NOT
    //   be an IVector&, because we need the collection to be accessible on the
    //   other side of the co_await.
    // - initial: if true, we're parsing these args during startup, and we
    //   should fire an Initialized event.
    // - cwd: If not empty, we should try switching to this provided directory
    //   while processing these actions. This will allow something like `wt -w 0
    //   nt -d .` from inside another directory to work as expected.
    // Return Value:
    // - <none>
    winrt::fire_and_forget TerminalPage::ProcessStartupActions(Windows::Foundation::Collections::IVector<ActionAndArgs> actions,
                                                               const bool initial,
                                                               const winrt::hstring cwd)
    {
        auto weakThis{ get_weak() };

        // Handle it on a subsequent pass of the UI thread.
        co_await winrt::resume_foreground(Dispatcher(), CoreDispatcherPriority::Normal);

        // If the caller provided a CWD, switch to that directory, then switch
        // back once we're done. This looks weird though, because we have to set
        // up the scope_exit _first_. We'll release the scope_exit if we don't
        // actually need it.
        std::wstring originalCwd{ wil::GetCurrentDirectoryW<std::wstring>() };
        auto restoreCwd = wil::scope_exit([&originalCwd]() {
            // ignore errors, we'll just power on through. We'd rather do
            // something rather than fail silently if the directory doesn't
            // actually exist.
            LOG_IF_WIN32_BOOL_FALSE(SetCurrentDirectory(originalCwd.c_str()));
        });
        if (cwd.empty())
        {
            restoreCwd.release();
        }
        else
        {
            // ignore errors, we'll just power on through. We'd rather do
            // something rather than fail silently if the directory doesn't
            // actually exist.
            LOG_IF_WIN32_BOOL_FALSE(SetCurrentDirectory(cwd.c_str()));
        }

        if (auto page{ weakThis.get() })
        {
            for (const auto& action : actions)
            {
                if (auto page{ weakThis.get() })
                {
                    _actionDispatch->DoAction(action);
                }
                else
                {
                    co_return;
                }
            }
        }
        if (initial)
        {
            _CompleteInitialization();
        }
    }

    // Method Description:
    // - Perform and steps that need to be done once our initial state is all
    //   set up. This includes entering fullscreen mode and firing our
    //   Initialized event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_CompleteInitialization()
    {
        _startupState = StartupState::Initialized;
        _InitializedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Show a dialog with "About" information. Displays the app's Display
    //   Name, version, getting started link, documentation link, release
    //   Notes link, and privacy policy link.
    void TerminalPage::_ShowAboutDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            presenter.ShowDialog(FindName(L"AboutDialog").try_as<WUX::Controls::ContentDialog>());
        }
    }

    winrt::hstring TerminalPage::ApplicationDisplayName()
    {
        return CascadiaSettings::ApplicationDisplayName();
    }

    winrt::hstring TerminalPage::ApplicationVersion()
    {
        return CascadiaSettings::ApplicationVersion();
    }

    void TerminalPage::_ThirdPartyNoticesOnClick(const IInspectable& /*sender*/, const Windows::UI::Xaml::RoutedEventArgs& /*eventArgs*/)
    {
        std::filesystem::path currentPath{ wil::GetModuleFileNameW<std::wstring>(nullptr) };
        currentPath.replace_filename(L"NOTICE.html");
        ShellExecute(nullptr, nullptr, currentPath.c_str(), nullptr, nullptr, SW_SHOW);
    }

    // Method Description:
    // - Displays a dialog for warnings found while closing the terminal app using
    //   key binding with multiple tabs opened. Display messages to warn user
    //   that more than 1 tab is opened, and once the user clicks the OK button, remove
    //   all the tabs and shut down and app. If cancel is clicked, the dialog will close
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowCloseWarningDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(L"CloseAllDialog").try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Displays a dialog for warnings found while closing the terminal tab marked as read-only
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowCloseReadOnlyDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(L"CloseReadOnlyDialog").try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Displays a dialog to warn the user about the fact that the text that
    //   they are trying to paste contains the "new line" character which can
    //   have the effect of starting commands without the user's knowledge if
    //   it is pasted on a shell where the "new line" character marks the end
    //   of a command.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowMultiLinePasteWarningDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(L"MultiLinePasteDialog").try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Displays a dialog to warn the user about the fact that the text that
    //   they are trying to paste is very long, in case they did not mean to
    //   paste it but pressed the paste shortcut by accident.
    // - Only one dialog can be visible at a time. If another dialog is visible
    //   when this is called, nothing happens. See _ShowDialog for details
    winrt::Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::_ShowLargePasteWarningDialog()
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            co_return co_await presenter.ShowDialog(FindName(L"LargePasteDialog").try_as<WUX::Controls::ContentDialog>());
        }
        co_return ContentDialogResult::None;
    }

    // Method Description:
    // - Builds the flyout (dropdown) attached to the new tab button, and
    //   attaches it to the button. Populates the flyout with one entry per
    //   Profile, displaying the profile's name. Clicking each flyout item will
    //   open a new tab with that profile.
    //   Below the profiles are the static menu items: settings, feedback
    void TerminalPage::_CreateNewTabFlyout()
    {
        auto newTabFlyout = WUX::Controls::MenuFlyout{};
        auto keyBindings = _settings.KeyMap();

        const auto defaultProfileGuid = _settings.GlobalSettings().DefaultProfile();
        // the number of profiles should not change in the loop for this to work
        auto const profileCount = gsl::narrow_cast<int>(_settings.ActiveProfiles().Size());
        for (int profileIndex = 0; profileIndex < profileCount; profileIndex++)
        {
            const auto profile = _settings.ActiveProfiles().GetAt(profileIndex);
            auto profileMenuItem = WUX::Controls::MenuFlyoutItem{};

            // Add the keyboard shortcuts based on the number of profiles defined
            // Look for a keychord that is bound to the equivalent
            // NewTab(ProfileIndex=N) action
            NewTerminalArgs newTerminalArgs{ profileIndex };
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionAndArgs actionAndArgs{ ShortcutAction::NewTab, newTabArgs };
            auto profileKeyChord{ keyBindings.GetKeyBindingForActionWithArgs(actionAndArgs) };

            // make sure we find one to display
            if (profileKeyChord)
            {
                _SetAcceleratorForMenuItem(profileMenuItem, profileKeyChord);
            }

            auto profileName = profile.Name();
            profileMenuItem.Text(profileName);

            // If there's an icon set for this profile, set it as the icon for
            // this flyout item.
            if (!profile.Icon().empty())
            {
                const auto iconSource{ IconPathConverter().IconSourceWUX(profile.Icon()) };

                WUX::Controls::IconSourceElement iconElement;
                iconElement.IconSource(iconSource);
                profileMenuItem.Icon(iconElement);
                Automation::AutomationProperties::SetAccessibilityView(iconElement, Automation::Peers::AccessibilityView::Raw);
            }

            if (profile.Guid() == defaultProfileGuid)
            {
                // Contrast the default profile with others in font weight.
                profileMenuItem.FontWeight(FontWeights::Bold());
            }

            auto newTabRun = WUX::Documents::Run();
            newTabRun.Text(RS_(L"NewTabRun/Text"));
            auto newPaneRun = WUX::Documents::Run();
            newPaneRun.Text(RS_(L"NewPaneRun/Text"));
            newPaneRun.FontStyle(FontStyle::Italic);
            auto newWindowRun = WUX::Documents::Run();
            newWindowRun.Text(RS_(L"NewWindowRun/Text"));
            newWindowRun.FontStyle(FontStyle::Italic);

            auto textBlock = WUX::Controls::TextBlock{};
            textBlock.Inlines().Append(newTabRun);
            textBlock.Inlines().Append(WUX::Documents::LineBreak{});
            textBlock.Inlines().Append(newPaneRun);
            textBlock.Inlines().Append(WUX::Documents::LineBreak{});
            textBlock.Inlines().Append(newWindowRun);

            auto toolTip = WUX::Controls::ToolTip{};
            toolTip.Content(textBlock);
            WUX::Controls::ToolTipService::SetToolTip(profileMenuItem, toolTip);

            profileMenuItem.Click([profileIndex, weakThis{ get_weak() }](auto&&, auto&&) {
                if (auto page{ weakThis.get() })
                {
                    NewTerminalArgs newTerminalArgs{ profileIndex };

                    // if alt is pressed, open a pane
                    const CoreWindow window = CoreWindow::GetForCurrentThread();
                    const auto rAltState = window.GetKeyState(VirtualKey::RightMenu);
                    const auto lAltState = window.GetKeyState(VirtualKey::LeftMenu);
                    const bool altPressed = WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) ||
                                            WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);

                    const auto shiftState{ window.GetKeyState(VirtualKey::Shift) };
                    const auto rShiftState = window.GetKeyState(VirtualKey::RightShift);
                    const auto lShiftState = window.GetKeyState(VirtualKey::LeftShift);
                    const auto shiftPressed{ WI_IsFlagSet(shiftState, CoreVirtualKeyStates::Down) ||
                                             WI_IsFlagSet(lShiftState, CoreVirtualKeyStates::Down) ||
                                             WI_IsFlagSet(rShiftState, CoreVirtualKeyStates::Down) };

                    // Check for DebugTap
                    bool debugTap = page->_settings.GlobalSettings().DebugFeaturesEnabled() &&
                                    WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) &&
                                    WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);

                    if (altPressed && !debugTap)
                    {
                        page->_SplitPane(SplitState::Automatic,
                                         SplitType::Manual,
                                         0.5f,
                                         newTerminalArgs);
                    }
                    else if (shiftPressed && !debugTap)
                    {
                        // Manually fill in the evaluated profile.
                        newTerminalArgs.Profile(::Microsoft::Console::Utils::GuidToString(page->_settings.GetProfileForArgs(newTerminalArgs)));
                        page->_OpenNewWindow(false, newTerminalArgs);
                    }
                    else
                    {
                        page->_OpenNewTab(newTerminalArgs);
                    }
                }
            });
            newTabFlyout.Items().Append(profileMenuItem);
        }

        // add menu separator
        auto separatorItem = WUX::Controls::MenuFlyoutSeparator{};
        newTabFlyout.Items().Append(separatorItem);

        // add static items
        {
            // GH#2455 - Make sure to try/catch calls to Application::Current,
            // because that _won't_ be an instance of TerminalApp::App in the
            // LocalTests
            auto isUwp = false;
            try
            {
                isUwp = ::winrt::Windows::UI::Xaml::Application::Current().as<::winrt::TerminalApp::App>().Logic().IsUwp();
            }
            CATCH_LOG();

            if (!isUwp)
            {
                // Create the settings button.
                auto settingsItem = WUX::Controls::MenuFlyoutItem{};
                settingsItem.Text(RS_(L"SettingsMenuItem"));

                WUX::Controls::SymbolIcon ico{};
                ico.Symbol(WUX::Controls::Symbol::Setting);
                settingsItem.Icon(ico);

                settingsItem.Click({ this, &TerminalPage::_SettingsButtonOnClick });
                newTabFlyout.Items().Append(settingsItem);

                Microsoft::Terminal::Settings::Model::OpenSettingsArgs args{ SettingsTarget::SettingsUI };
                Microsoft::Terminal::Settings::Model::ActionAndArgs settingsAction{ ShortcutAction::OpenSettings, args };
                const auto settingsKeyChord{ keyBindings.GetKeyBindingForActionWithArgs(settingsAction) };
                if (settingsKeyChord)
                {
                    _SetAcceleratorForMenuItem(settingsItem, settingsKeyChord);
                }

                // Create the feedback button.
                auto feedbackFlyout = WUX::Controls::MenuFlyoutItem{};
                feedbackFlyout.Text(RS_(L"FeedbackMenuItem"));

                WUX::Controls::FontIcon feedbackIcon{};
                feedbackIcon.Glyph(L"\xE939");
                feedbackIcon.FontFamily(Media::FontFamily{ L"Segoe MDL2 Assets" });
                feedbackFlyout.Icon(feedbackIcon);

                feedbackFlyout.Click({ this, &TerminalPage::_FeedbackButtonOnClick });
                newTabFlyout.Items().Append(feedbackFlyout);
            }

            // Create the about button.
            auto aboutFlyout = WUX::Controls::MenuFlyoutItem{};
            aboutFlyout.Text(RS_(L"AboutMenuItem"));

            WUX::Controls::SymbolIcon aboutIcon{};
            aboutIcon.Symbol(WUX::Controls::Symbol::Help);
            aboutFlyout.Icon(aboutIcon);

            aboutFlyout.Click({ this, &TerminalPage::_AboutButtonOnClick });
            newTabFlyout.Items().Append(aboutFlyout);
        }

        // Before opening the fly-out set focus on the current tab
        // so no matter how fly-out is closed later on the focus will return to some tab.
        // We cannot do it on closing because if the window loses focus (alt+tab)
        // the closing event is not fired.
        // It is important to set the focus on the tab
        // Since the previous focus location might be discarded in the background,
        // e.g., the command palette will be dismissed by the menu,
        // and then closing the fly-out will move the focus to wrong location.
        newTabFlyout.Opening([this](auto&&, auto&&) {
            if (auto index{ _GetFocusedTabIndex() })
            {
                _tabs.GetAt(*index).Focus(FocusState::Programmatic);
                _UpdateMRUTab(index.value());
            }
        });
        _newTabButton.Flyout(newTabFlyout);
    }

    // Function Description:
    // Called when the openNewTabDropdown keybinding is used.
    // Adds the flyout show option to left-align the dropdown with the split button.
    // Shows the dropdown flyout.
    void TerminalPage::_OpenNewTabDropdown()
    {
        WUX::Controls::Primitives::FlyoutShowOptions options{};
        options.Placement(WUX::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
        _newTabButton.Flyout().ShowAt(_newTabButton, options);
    }

    // Method Description:
    // - Open a new tab. This will create the TerminalControl hosting the
    //   terminal, and add a new Tab to our list of tabs. The method can
    //   optionally be provided a NewTerminalArgs, which will be used to create
    //   a tab using the values in that object.
    // Arguments:
    // - newTerminalArgs: An object that may contain a blob of parameters to
    //   control which profile is created and with possible other
    //   configurations. See TerminalSettings::CreateWithNewTerminalArgs for more details.
    // - existingConnection: An optional connection that is already established to a PTY
    //   for this tab to host instead of creating one.
    //   If not defined, the tab will create the connection.
    void TerminalPage::_OpenNewTab(const NewTerminalArgs& newTerminalArgs, winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection existingConnection)
    try
    {
        const auto profileGuid{ _settings.GetProfileForArgs(newTerminalArgs) };
        const auto settings{ TerminalSettings::CreateWithNewTerminalArgs(_settings, newTerminalArgs, *_bindings) };

        _CreateNewTabFromSettings(profileGuid, settings, existingConnection);

        const uint32_t tabCount = _tabs.Size();
        const bool usedManualProfile = (newTerminalArgs != nullptr) &&
                                       (newTerminalArgs.ProfileIndex() != nullptr ||
                                        newTerminalArgs.Profile().empty());

        // Lookup the name of the color scheme used by this profile.
        const auto scheme = _settings.GetColorSchemeForProfile(profileGuid);
        // If they explicitly specified `null` as the scheme (indicating _no_ scheme), log
        // that as the empty string.
        const auto schemeName = scheme ? scheme.Name() : L"\0";

        TraceLoggingWrite(
            g_hTerminalAppProvider, // handle to TerminalApp tracelogging provider
            "TabInformation",
            TraceLoggingDescription("Event emitted upon new tab creation in TerminalApp"),
            TraceLoggingUInt32(1u, "EventVer", "Version of this event"),
            TraceLoggingUInt32(tabCount, "TabCount", "Count of tabs currently opened in TerminalApp"),
            TraceLoggingBool(usedManualProfile, "ProfileSpecified", "Whether the new tab specified a profile explicitly"),
            TraceLoggingGuid(profileGuid, "ProfileGuid", "The GUID of the profile spawned in the new tab"),
            TraceLoggingBool(settings.UseAcrylic(), "UseAcrylic", "The acrylic preference from the settings"),
            TraceLoggingFloat64(settings.TintOpacity(), "TintOpacity", "Opacity preference from the settings"),
            TraceLoggingWideString(settings.FontFace().c_str(), "FontFace", "Font face chosen in the settings"),
            TraceLoggingWideString(schemeName.data(), "SchemeName", "Color scheme set in the settings"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));
    }
    CATCH_LOG();

    winrt::fire_and_forget TerminalPage::_RemoveOnCloseRoutine(Microsoft::UI::Xaml::Controls::TabViewItem tabViewItem, winrt::com_ptr<TerminalPage> page)
    {
        co_await winrt::resume_foreground(page->_tabView.Dispatcher());

        page->_RemoveTabViewItem(tabViewItem);
    }

    // Method Description:
    // - Creates a new tab with the given settings. If the tab bar is not being
    //      currently displayed, it will be shown.
    // Arguments:
    // - profileGuid: ID to use to lookup profile settings for this connection
    // - settings: the TerminalSettings object to use to create the TerminalControl with.
    // - existingConnection: optionally receives a connection from the outside world instead of attempting to create one
    void TerminalPage::_CreateNewTabFromSettings(GUID profileGuid, TerminalSettings settings, TerminalConnection::ITerminalConnection existingConnection)
    {
        // Initialize the new tab
        // Create a connection based on the values in our settings object if we weren't given one.
        auto connection = existingConnection ? existingConnection : _CreateConnectionFromSettings(profileGuid, settings);

        TerminalConnection::ITerminalConnection debugConnection{ nullptr };
        if (_settings.GlobalSettings().DebugFeaturesEnabled())
        {
            const CoreWindow window = CoreWindow::GetForCurrentThread();
            const auto rAltState = window.GetKeyState(VirtualKey::RightMenu);
            const auto lAltState = window.GetKeyState(VirtualKey::LeftMenu);
            const bool bothAltsPressed = WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) &&
                                         WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);
            if (bothAltsPressed)
            {
                std::tie(connection, debugConnection) = OpenDebugTapConnection(connection);
            }
        }

        // Give term control a child of the settings so that any overrides go in the child
        // This way, when we do a settings reload we just update the parent and the overrides remain
        auto term = _InitControl(settings, connection);

        auto newTabImpl = winrt::make_self<TerminalTab>(profileGuid, term);

        // Add the new tab to the list of our tabs.
        _tabs.Append(*newTabImpl);
        _mruTabs.Append(*newTabImpl);

        newTabImpl->SetDispatch(*_actionDispatch);
        newTabImpl->SetKeyMap(_settings.KeyMap());

        // Give the tab its index in the _tabs vector so it can manage its own SwitchToTab command.
        _UpdateTabIndices();

        // Hookup our event handlers to the new terminal
        _RegisterTerminalEvents(term, *newTabImpl);

        // Don't capture a strong ref to the tab. If the tab is removed as this
        // is called, we don't really care anymore about handling the event.
        auto weakTab = make_weak(newTabImpl);

        // When the tab's active pane changes, we'll want to lookup a new icon
        // for it. The Title change will be propagated upwards through the tab's
        // PropertyChanged event handler.
        newTabImpl->ActivePaneChanged([weakTab, weakThis{ get_weak() }]() {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };

            if (page && tab)
            {
                // Possibly update the icon of the tab.
                page->_UpdateTabIcon(*tab);
            }
        });

        // The RaiseVisualBell event has been bubbled up to here from the pane,
        // the next part of the chain is bubbling up to app logic, which will
        // forward it to app host.
        newTabImpl->TabRaiseVisualBell([weakTab, weakThis{ get_weak() }]() {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };

            if (page && tab)
            {
                page->_RaiseVisualBellHandlers(nullptr, nullptr);
            }
        });

        newTabImpl->DuplicateRequested([weakTab, weakThis{ get_weak() }]() {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };

            if (page && tab)
            {
                page->_DuplicateTab(*tab);
            }
        });

        auto tabViewItem = newTabImpl->TabViewItem();
        _tabView.TabItems().Append(tabViewItem);

        // Set this tab's icon to the icon from the user's profile
        const auto profile = _settings.FindProfile(profileGuid);
        if (profile != nullptr && !profile.Icon().empty())
        {
            newTabImpl->UpdateIcon(profile.Icon());
        }

        tabViewItem.PointerPressed({ this, &TerminalPage::_OnTabClick });

        // When the tab is closed, remove it from our list of tabs.
        newTabImpl->Closed([tabViewItem, weakThis{ get_weak() }](auto&& /*s*/, auto&& /*e*/) {
            if (auto page{ weakThis.get() })
            {
                page->_RemoveOnCloseRoutine(tabViewItem, page);
            }
        });

        newTabImpl->TabRenamerDeactivated([weakThis{ get_weak() }](auto&& /*s*/, auto&& /*e*/) {
            if (const auto page{ weakThis.get() })
            {
                if (!page->_newTabButton.Flyout().IsOpen())
                {
                    if (const auto tab{ page->_GetFocusedTab() })
                    {
                        tab.Focus(FocusState::Programmatic);
                    }
                }
            }
        });

        if (debugConnection) // this will only be set if global debugging is on and tap is active
        {
            auto newControl = _InitControl(settings, debugConnection);
            _RegisterTerminalEvents(newControl, *newTabImpl);
            // Split (auto) with the debug tap.
            newTabImpl->SplitPane(SplitState::Automatic, 0.5f, profileGuid, newControl);
        }

        // This kicks off TabView::SelectionChanged, in response to which
        // we'll attach the terminal's Xaml control to the Xaml root.
        _tabView.SelectedItem(tabViewItem);
    }

    // Method Description:
    // - Creates a new connection based on the profile settings
    // Arguments:
    // - the profile GUID we want the settings from
    // - the terminal settings
    // Return value:
    // - the desired connection
    TerminalConnection::ITerminalConnection TerminalPage::_CreateConnectionFromSettings(GUID profileGuid,
                                                                                        TerminalSettings settings)
    {
        const auto profile = _settings.FindProfile(profileGuid);

        TerminalConnection::ITerminalConnection connection{ nullptr };

        winrt::guid connectionType = profile.ConnectionType();
        winrt::guid sessionGuid{};

        if (connectionType == TerminalConnection::AzureConnection::ConnectionType() &&
            TerminalConnection::AzureConnection::IsAzureConnectionAvailable())
        {
            // TODO GH#4661: Replace this with directly using the AzCon when our VT is better
            std::filesystem::path azBridgePath{ wil::GetModuleFileNameW<std::wstring>(nullptr) };
            azBridgePath.replace_filename(L"TerminalAzBridge.exe");
            connection = TerminalConnection::ConptyConnection(azBridgePath.wstring(),
                                                              L".",
                                                              L"Azure",
                                                              nullptr,
                                                              settings.InitialRows(),
                                                              settings.InitialCols(),
                                                              winrt::guid());
        }

        else
        {
            std::wstring guidWString = Utils::GuidToString(profileGuid);

            StringMap envMap{};
            envMap.Insert(L"WT_PROFILE_ID", guidWString);
            envMap.Insert(L"WSLENV", L"WT_PROFILE_ID");

            // Update the path to be relative to whatever our CWD is.
            //
            // Refer to the examples in
            // https://en.cppreference.com/w/cpp/filesystem/path/append
            //
            // We need to do this here, to ensure we tell the ConptyConnection
            // the correct starting path. If we're being invoked from another
            // terminal instance (e.g. wt -w 0 -d .), then we have switched our
            // CWD to the provided path. We should treat the StartingDirectory
            // as relative to the current CWD.
            //
            // The connection must be informed of the current CWD on
            // construction, because the connection might not spawn the child
            // process until later, on another thread, after we've already
            // restored the CWD to it's original value.
            std::wstring cwdString{ wil::GetCurrentDirectoryW<std::wstring>() };
            std::filesystem::path cwd{ cwdString };
            cwd /= settings.StartingDirectory().c_str();

            auto conhostConn = TerminalConnection::ConptyConnection(
                settings.Commandline(),
                winrt::hstring{ cwd.c_str() },
                settings.StartingTitle(),
                envMap.GetView(),
                settings.InitialRows(),
                settings.InitialCols(),
                winrt::guid());

            sessionGuid = conhostConn.Guid();
            connection = conhostConn;
        }

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "ConnectionCreated",
            TraceLoggingDescription("Event emitted upon the creation of a connection"),
            TraceLoggingGuid(connectionType, "ConnectionTypeGuid", "The type of the connection"),
            TraceLoggingGuid(profileGuid, "ProfileGuid", "The profile's GUID"),
            TraceLoggingGuid(sessionGuid, "SessionGuid", "The WT_SESSION's GUID"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));

        return connection;
    }

    // Method Description:
    // - Called when the settings button is clicked. Launches a background
    //   thread to open the settings file in the default JSON editor.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_SettingsButtonOnClick(const IInspectable&,
                                              const RoutedEventArgs&)
    {
        const CoreWindow window = CoreWindow::GetForCurrentThread();

        // check alt state
        const auto rAltState{ window.GetKeyState(VirtualKey::RightMenu) };
        const auto lAltState{ window.GetKeyState(VirtualKey::LeftMenu) };
        const bool altPressed{ WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) ||
                               WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down) };

        // check shift state
        const auto shiftState{ window.GetKeyState(VirtualKey::Shift) };
        const auto lShiftState{ window.GetKeyState(VirtualKey::LeftShift) };
        const auto rShiftState{ window.GetKeyState(VirtualKey::RightShift) };
        const auto shiftPressed{ WI_IsFlagSet(shiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(lShiftState, CoreVirtualKeyStates::Down) ||
                                 WI_IsFlagSet(rShiftState, CoreVirtualKeyStates::Down) };

        auto target{ SettingsTarget::SettingsUI };
        if (shiftPressed)
        {
            target = SettingsTarget::SettingsFile;
        }
        else if (altPressed)
        {
            target = SettingsTarget::DefaultsFile;
        }
        _LaunchSettings(target);
    }

    // Method Description:
    // - Called when the feedback button is clicked. Launches github in your
    //   default browser, navigated to the "issues" page of the Terminal repo.
    void TerminalPage::_FeedbackButtonOnClick(const IInspectable&,
                                              const RoutedEventArgs&)
    {
        const auto feedbackUriValue = RS_(L"FeedbackUriValue");
        winrt::Windows::Foundation::Uri feedbackUri{ feedbackUriValue };

        winrt::Windows::System::Launcher::LaunchUriAsync(feedbackUri);
    }

    // Method Description:
    // - Called when the about button is clicked. See _ShowAboutDialog for more info.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TerminalPage::_AboutButtonOnClick(const IInspectable&,
                                           const RoutedEventArgs&)
    {
        _ShowAboutDialog();
    }

    // Method Description:
    // Called when the users pressed keyBindings while CommandPalette is open.
    // Arguments:
    // - e: the KeyRoutedEventArgs containing info about the keystroke.
    // Return Value:
    // - <none>
    void TerminalPage::_KeyDownHandler(Windows::Foundation::IInspectable const& /*sender*/, Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e)
    {
        auto key = e.OriginalKey();
        auto const ctrlDown = WI_IsFlagSet(CoreWindow::GetForCurrentThread().GetKeyState(winrt::Windows::System::VirtualKey::Control), CoreVirtualKeyStates::Down);
        auto const altDown = WI_IsFlagSet(CoreWindow::GetForCurrentThread().GetKeyState(winrt::Windows::System::VirtualKey::Menu), CoreVirtualKeyStates::Down);
        auto const shiftDown = WI_IsFlagSet(CoreWindow::GetForCurrentThread().GetKeyState(winrt::Windows::System::VirtualKey::Shift), CoreVirtualKeyStates::Down);

        winrt::Microsoft::Terminal::Control::KeyChord kc{ ctrlDown, altDown, shiftDown, static_cast<int32_t>(key) };
        const auto actionAndArgs = _settings.KeyMap().TryLookup(kc);
        if (actionAndArgs)
        {
            if (CommandPalette().Visibility() == Visibility::Visible && actionAndArgs.Action() != ShortcutAction::ToggleCommandPalette)
            {
                CommandPalette().Visibility(Visibility::Collapsed);
            }
            _actionDispatch->DoAction(actionAndArgs);
            e.Handled(true);
        }
    }

    // Method Description:
    // Handles preview key on the SUI tab, by handling close tab / next tab / previous tab
    // This is a temporary solution - we need to fix all key-bindings work from SUI as long as they don't harm
    // the SUI behavior
    // Arguments:
    // - e: the KeyRoutedEventArgs containing info about the keystroke.
    // Return Value:
    // - <none>
    void TerminalPage::_SUIPreviewKeyDownHandler(Windows::Foundation::IInspectable const& /*sender*/, Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e)
    {
        auto key = e.OriginalKey();
        auto const ctrlDown = WI_IsFlagSet(CoreWindow::GetForCurrentThread().GetKeyState(winrt::Windows::System::VirtualKey::Control), CoreVirtualKeyStates::Down);
        auto const altDown = WI_IsFlagSet(CoreWindow::GetForCurrentThread().GetKeyState(winrt::Windows::System::VirtualKey::Menu), CoreVirtualKeyStates::Down);
        auto const shiftDown = WI_IsFlagSet(CoreWindow::GetForCurrentThread().GetKeyState(winrt::Windows::System::VirtualKey::Shift), CoreVirtualKeyStates::Down);

        winrt::Microsoft::Terminal::Control::KeyChord kc{ ctrlDown, altDown, shiftDown, static_cast<int32_t>(key) };
        const auto actionAndArgs = _settings.KeyMap().TryLookup(kc);
        if (actionAndArgs && (actionAndArgs.Action() == ShortcutAction::CloseTab || actionAndArgs.Action() == ShortcutAction::NextTab || actionAndArgs.Action() == ShortcutAction::PrevTab || actionAndArgs.Action() == ShortcutAction::ClosePane))
        {
            _actionDispatch->DoAction(actionAndArgs);
            e.Handled(true);
        }
    }

    // Method Description:
    // - Configure the AppKeyBindings to use our ShortcutActionDispatch and the updated KeyMapping
    // as the object to handle dispatching ShortcutAction events.
    // Arguments:
    // - bindings: A AppKeyBindings object to wire up with our event handlers
    void TerminalPage::_HookupKeyBindings(const KeyMapping& keymap) noexcept
    {
        _bindings->SetDispatch(*_actionDispatch);
        _bindings->SetKeyMapping(keymap);
    }

    // Method Description:
    // - Register our event handlers with our ShortcutActionDispatch. The
    //   ShortcutActionDispatch is responsible for raising the appropriate
    //   events for an ActionAndArgs. WE'll handle each possible event in our
    //   own way.
    // Arguments:
    // - <none>
    void TerminalPage::_RegisterActionCallbacks()
    {
        // Hook up the ShortcutActionDispatch object's events to our handlers.
        // They should all be hooked up here, regardless of whether or not
        // there's an actual keychord for them.
        _actionDispatch->OpenNewTabDropdown({ this, &TerminalPage::_HandleOpenNewTabDropdown });
        _actionDispatch->DuplicateTab({ this, &TerminalPage::_HandleDuplicateTab });
        _actionDispatch->CloseTab({ this, &TerminalPage::_HandleCloseTab });
        _actionDispatch->ClosePane({ this, &TerminalPage::_HandleClosePane });
        _actionDispatch->CloseWindow({ this, &TerminalPage::_HandleCloseWindow });
        _actionDispatch->ScrollUp({ this, &TerminalPage::_HandleScrollUp });
        _actionDispatch->ScrollDown({ this, &TerminalPage::_HandleScrollDown });
        _actionDispatch->NextTab({ this, &TerminalPage::_HandleNextTab });
        _actionDispatch->PrevTab({ this, &TerminalPage::_HandlePrevTab });
        _actionDispatch->SendInput({ this, &TerminalPage::_HandleSendInput });
        _actionDispatch->SplitPane({ this, &TerminalPage::_HandleSplitPane });
        _actionDispatch->TogglePaneZoom({ this, &TerminalPage::_HandleTogglePaneZoom });
        _actionDispatch->ScrollUpPage({ this, &TerminalPage::_HandleScrollUpPage });
        _actionDispatch->ScrollDownPage({ this, &TerminalPage::_HandleScrollDownPage });
        _actionDispatch->ScrollToTop({ this, &TerminalPage::_HandleScrollToTop });
        _actionDispatch->ScrollToBottom({ this, &TerminalPage::_HandleScrollToBottom });
        _actionDispatch->OpenSettings({ this, &TerminalPage::_HandleOpenSettings });
        _actionDispatch->PasteText({ this, &TerminalPage::_HandlePasteText });
        _actionDispatch->NewTab({ this, &TerminalPage::_HandleNewTab });
        _actionDispatch->SwitchToTab({ this, &TerminalPage::_HandleSwitchToTab });
        _actionDispatch->ResizePane({ this, &TerminalPage::_HandleResizePane });
        _actionDispatch->MoveFocus({ this, &TerminalPage::_HandleMoveFocus });
        _actionDispatch->CopyText({ this, &TerminalPage::_HandleCopyText });
        _actionDispatch->AdjustFontSize({ this, &TerminalPage::_HandleAdjustFontSize });
        _actionDispatch->Find({ this, &TerminalPage::_HandleFind });
        _actionDispatch->ResetFontSize({ this, &TerminalPage::_HandleResetFontSize });
        _actionDispatch->ToggleShaderEffects({ this, &TerminalPage::_HandleToggleShaderEffects });
        _actionDispatch->ToggleFocusMode({ this, &TerminalPage::_HandleToggleFocusMode });
        _actionDispatch->ToggleFullscreen({ this, &TerminalPage::_HandleToggleFullscreen });
        _actionDispatch->ToggleAlwaysOnTop({ this, &TerminalPage::_HandleToggleAlwaysOnTop });
        _actionDispatch->ToggleCommandPalette({ this, &TerminalPage::_HandleToggleCommandPalette });
        _actionDispatch->SetColorScheme({ this, &TerminalPage::_HandleSetColorScheme });
        _actionDispatch->SetTabColor({ this, &TerminalPage::_HandleSetTabColor });
        _actionDispatch->OpenTabColorPicker({ this, &TerminalPage::_HandleOpenTabColorPicker });
        _actionDispatch->RenameTab({ this, &TerminalPage::_HandleRenameTab });
        _actionDispatch->OpenTabRenamer({ this, &TerminalPage::_HandleOpenTabRenamer });
        _actionDispatch->ExecuteCommandline({ this, &TerminalPage::_HandleExecuteCommandline });
        _actionDispatch->CloseOtherTabs({ this, &TerminalPage::_HandleCloseOtherTabs });
        _actionDispatch->CloseTabsAfter({ this, &TerminalPage::_HandleCloseTabsAfter });
        _actionDispatch->TabSearch({ this, &TerminalPage::_HandleOpenTabSearch });
        _actionDispatch->MoveTab({ this, &TerminalPage::_HandleMoveTab });
        _actionDispatch->BreakIntoDebugger({ this, &TerminalPage::_HandleBreakIntoDebugger });
        _actionDispatch->FindMatch({ this, &TerminalPage::_HandleFindMatch });
        _actionDispatch->TogglePaneReadOnly({ this, &TerminalPage::_HandleTogglePaneReadOnly });
        _actionDispatch->NewWindow({ this, &TerminalPage::_HandleNewWindow });
    }

    // Method Description:
    // - Get the title of the currently focused terminal control. If this tab is
    //   the focused tab, then also bubble this title to any listeners of our
    //   TitleChanged event.
    // Arguments:
    // - tab: the Tab to update the title for.
    void TerminalPage::_UpdateTitle(const TerminalTab& tab)
    {
        auto newTabTitle = tab.Title();

        if (_settings.GlobalSettings().ShowTitleInTitlebar() && tab == _GetFocusedTab())
        {
            _TitleChangedHandlers(*this, newTabTitle);
        }
    }

    // Method Description:
    // - Get the icon of the currently focused terminal control, and set its
    //   tab's icon to that icon.
    // Arguments:
    // - tab: the Tab to update the title for.
    void TerminalPage::_UpdateTabIcon(TerminalTab& tab)
    {
        const auto lastFocusedProfileOpt = tab.GetFocusedProfile();
        if (lastFocusedProfileOpt.has_value())
        {
            const auto lastFocusedProfile = lastFocusedProfileOpt.value();
            const auto matchingProfile = _settings.FindProfile(lastFocusedProfile);
            if (matchingProfile)
            {
                tab.UpdateIcon(matchingProfile.Icon());
            }
            else
            {
                tab.UpdateIcon({});
            }
        }
    }

    // Method Description:
    // - Handle changes to the tab width set by the user
    void TerminalPage::_UpdateTabWidthMode()
    {
        _tabView.TabWidthMode(_settings.GlobalSettings().TabWidthMode());
    }

    // Method Description:
    // - Handle changes in tab layout.
    void TerminalPage::_UpdateTabView()
    {
        // Never show the tab row when we're fullscreen. Otherwise:
        // Show tabs when there's more than 1, or the user has chosen to always
        // show the tab bar.
        const bool isVisible = (!_isFullscreen && !_isInFocusMode) &&
                               (_settings.GlobalSettings().ShowTabsInTitlebar() ||
                                (_tabs.Size() > 1) ||
                                _settings.GlobalSettings().AlwaysShowTabs());

        // collapse/show the tabs themselves
        _tabView.Visibility(isVisible ? Visibility::Visible : Visibility::Collapsed);

        // collapse/show the row that the tabs are in.
        // NaN is the special value XAML uses for "Auto" sizing.
        _tabRow.Height(isVisible ? NAN : 0);
    }

    // Method Description:
    // - Duplicates the current focused tab
    void TerminalPage::_DuplicateFocusedTab()
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            _DuplicateTab(*terminalTab);
        }
    }

    // Method Description:
    // - Duplicates specified tab
    // Arguments:
    // - tab: tab to duplicate
    void TerminalPage::_DuplicateTab(const TerminalTab& tab)
    {
        try
        {
            // TODO: GH#5047 - In the future, we should get the Profile of
            // the focused pane, and use that to build a new instance of the
            // settings so we can duplicate this tab/pane.
            //
            // Currently, if the profile doesn't exist anymore in our
            // settings, we'll silently do nothing.
            //
            // In the future, it will be preferable to just duplicate the
            // current control's settings, but we can't do that currently,
            // because we won't be able to create a new instance of the
            // connection without keeping an instance of the original Profile
            // object around.

            const auto& profileGuid = tab.GetFocusedProfile();
            if (profileGuid.has_value())
            {
                const auto settings{ TerminalSettings::CreateWithProfileByID(_settings, profileGuid.value(), *_bindings) };
                const auto workingDirectory = tab.GetActiveTerminalControl().WorkingDirectory();
                const auto validWorkingDirectory = !workingDirectory.empty();
                if (validWorkingDirectory)
                {
                    settings.StartingDirectory(workingDirectory);
                }

                _CreateNewTabFromSettings(profileGuid.value(), settings);
            }
        }
        CATCH_LOG();
    }

    // Method Description:
    // - Look for the index of the input tabView in the tabs vector,
    //   and call _RemoveTab
    // Arguments:
    // - tabViewItem: the TabViewItem in the TabView that is being removed.
    void TerminalPage::_RemoveTabViewItem(const MUX::Controls::TabViewItem& tabViewItem)
    {
        uint32_t tabIndexFromControl = 0;
        if (_tabView.TabItems().IndexOf(tabViewItem, tabIndexFromControl))
        {
            // If IndexOf returns true, we've actually got an index
            auto tab{ _tabs.GetAt(tabIndexFromControl) };
            _RemoveTab(tab);
        }
    }

    // Method Description:
    // - Removes the tab (both TerminalControl and XAML)
    // Arguments:
    // - tab: the tab to remove
    winrt::Windows::Foundation::IAsyncAction TerminalPage::_RemoveTab(winrt::TerminalApp::TabBase tab)
    {
        if (tab.ReadOnly())
        {
            ContentDialogResult warningResult = co_await _ShowCloseReadOnlyDialog();

            // If the user didn't explicitly click on close tab - leave
            if (warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }
        }

        uint32_t tabIndex{};
        if (!_tabs.IndexOf(tab, tabIndex))
        {
            // The tab is already removed
            co_return;
        }

        // We use _removing flag to suppress _OnTabSelectionChanged events
        // that might get triggered while removing
        _removing = true;
        auto unsetRemoving = wil::scope_exit([&]() noexcept { _removing = false; });

        const auto focusedTabIndex{ _GetFocusedTabIndex() };

        // Removing the tab from the collection should destroy its control and disconnect its connection,
        // but it doesn't always do so. The UI tree may still be holding the control and preventing its destruction.
        tab.Shutdown();

        uint32_t mruIndex{};
        if (_mruTabs.IndexOf(tab, mruIndex))
        {
            _mruTabs.RemoveAt(mruIndex);
        }

        _tabs.RemoveAt(tabIndex);
        _tabView.TabItems().RemoveAt(tabIndex);
        _UpdateTabIndices();

        // To close the window here, we need to close the hosting window.
        if (_tabs.Size() == 0)
        {
            _LastTabClosedHandlers(*this, nullptr);
        }
        else if (focusedTabIndex.has_value() && focusedTabIndex.value() == gsl::narrow_cast<uint32_t>(tabIndex))
        {
            // Manually select the new tab to get focus, rather than relying on TabView since:
            // 1. We want to customize this behavior (e.g., use MRU logic)
            // 2. In fullscreen (GH#5799) and focus (GH#7916) modes the _OnTabItemsChanged is not fired
            // 3. When rearranging tabs (GH#7916) _OnTabItemsChanged is suppressed
            const auto tabSwitchMode = _settings.GlobalSettings().TabSwitcherMode();

            if (tabSwitchMode == TabSwitcherMode::MostRecentlyUsed)
            {
                const auto newSelectedTab = _mruTabs.GetAt(0);

                uint32_t newSelectedIndex;
                if (_tabs.IndexOf(newSelectedTab, newSelectedIndex))
                {
                    _UpdatedSelectedTab(newSelectedIndex);
                    _tabView.SelectedItem(newSelectedTab.TabViewItem());
                }
            }
            else
            {
                // We can't use
                //   auto selectedIndex = _tabView.SelectedIndex();
                // Because this will always return -1 in this scenario unfortunately.
                //
                // So, what we're going to try to do is move the focus to the tab
                // to the left, within the bounds of how many tabs we have.
                //
                // EX: we have 4 tabs: [A, B, C, D]. If we close:
                // * A (tabIndex=0): We'll want to focus tab B (now in index 0)
                // * B (tabIndex=1): We'll want to focus tab A (now in index 0)
                // * C (tabIndex=2): We'll want to focus tab B (now in index 1)
                // * D (tabIndex=3): We'll want to focus tab C (now in index 2)
                const auto newSelectedIndex = std::clamp<int32_t>(tabIndex - 1, 0, _tabs.Size());
                // _UpdatedSelectedTab will do the work of setting up the new tab as
                // the focused one, and unfocusing all the others.
                _UpdatedSelectedTab(newSelectedIndex);

                // Also, we need to _manually_ set the SelectedItem of the tabView
                // here. If we don't, then the TabView will technically not have a
                // selected item at all, which can make things like ClosePane not
                // work correctly.
                auto newSelectedTab{ _tabs.GetAt(newSelectedIndex) };
                _tabView.SelectedItem(newSelectedTab.TabViewItem());
            }
        }

        // GH#5559 - If we were in the middle of a drag/drop, end it by clearing
        // out our state.
        if (_rearranging)
        {
            _rearranging = false;
            _rearrangeFrom = std::nullopt;
            _rearrangeTo = std::nullopt;
        }

        co_return;
    }

    // Method Description:
    // - Connects event handlers to the TermControl for events that we want to
    //   handle. This includes:
    //    * the Copy and Paste events, for setting and retrieving clipboard data
    //      on the right thread
    //    * the TitleChanged event, for changing the text of the tab
    // Arguments:
    // - term: The newly created TermControl to connect the events for
    // - hostingTab: The Tab that's hosting this TermControl instance
    void TerminalPage::_RegisterTerminalEvents(TermControl term, TerminalTab& hostingTab)
    {
        term.RaiseNotice({ this, &TerminalPage::_ControlNoticeRaisedHandler });

        // Add an event handler when the terminal's selection wants to be copied.
        // When the text buffer data is retrieved, we'll copy the data into the Clipboard
        term.CopyToClipboard({ this, &TerminalPage::_CopyToClipboardHandler });

        // Add an event handler when the terminal wants to paste data from the Clipboard.
        term.PasteFromClipboard({ this, &TerminalPage::_PasteFromClipboardHandler });

        term.OpenHyperlink({ this, &TerminalPage::_OpenHyperlinkHandler });

        // Add an event handler for when the terminal wants to set a progress indicator on the taskbar
        term.SetTaskbarProgress({ this, &TerminalPage::_SetTaskbarProgressHandler });

        term.HidePointerCursor({ get_weak(), &TerminalPage::_HidePointerCursorHandler });
        term.RestorePointerCursor({ get_weak(), &TerminalPage::_RestorePointerCursorHandler });

        // Bind Tab events to the TermControl and the Tab's Pane
        hostingTab.Initialize(term);

        auto weakTab{ hostingTab.get_weak() };
        auto weakThis{ get_weak() };
        // PropertyChanged is the generic mechanism by which the Tab
        // communicates changes to any of its observable properties, including
        // the Title
        hostingTab.PropertyChanged([weakTab, weakThis](auto&&, const WUX::Data::PropertyChangedEventArgs& args) {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };
            if (page && tab)
            {
                if (args.PropertyName() == L"Title")
                {
                    page->_UpdateTitle(*tab);
                }
                else if (args.PropertyName() == L"Content")
                {
                    if (*tab == page->_GetFocusedTab())
                    {
                        page->_tabContent.Children().Clear();
                        page->_tabContent.Children().Append(tab->Content());

                        tab->Focus(FocusState::Programmatic);
                    }
                }
            }
        });

        // react on color changed events
        hostingTab.ColorSelected([weakTab, weakThis](auto&& color) {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };

            if (page && tab && (tab->FocusState() != FocusState::Unfocused))
            {
                page->_SetNonClientAreaColors(color);
            }
        });

        hostingTab.ColorCleared([weakTab, weakThis]() {
            auto page{ weakThis.get() };
            auto tab{ weakTab.get() };

            if (page && tab && (tab->FocusState() != FocusState::Unfocused))
            {
                page->_ClearNonClientAreaColors();
            }
        });

        // TODO GH#3327: Once we support colorizing the NewTab button based on
        // the color of the tab, we'll want to make sure to call
        // _ClearNewTabButtonColor here, to reset it to the default (for the
        // newly created tab).
        // remove any colors left by other colored tabs
        // _ClearNewTabButtonColor();
    }

    // Method Description:
    // - Sets focus to the tab to the right or left the currently selected tab.
    void TerminalPage::_SelectNextTab(const bool bMoveRight, const Windows::Foundation::IReference<Microsoft::Terminal::Settings::Model::TabSwitcherMode>& customTabSwitcherMode)
    {
        const auto index{ _GetFocusedTabIndex().value_or(0) };
        const auto tabSwitchMode = customTabSwitcherMode ? customTabSwitcherMode.Value() : _settings.GlobalSettings().TabSwitcherMode();
        if (tabSwitchMode == TabSwitcherMode::Disabled)
        {
            uint32_t tabCount = _tabs.Size();
            // Wraparound math. By adding tabCount and then calculating
            // modulo tabCount, we clamp the values to the range [0,
            // tabCount) while still supporting moving leftward from 0 to
            // tabCount - 1.
            const auto newTabIndex = ((tabCount + index + (bMoveRight ? 1 : -1)) % tabCount);
            _SelectTab(newTabIndex);
        }
        else
        {
            CommandPalette().SetTabs(_tabs, _mruTabs);

            // Otherwise, set up the tab switcher in the selected mode, with
            // the given ordering, and make it visible.
            CommandPalette().EnableTabSwitcherMode(index, tabSwitchMode);
            CommandPalette().Visibility(Visibility::Visible);
            CommandPalette().SelectNextItem(bMoveRight);
        }
    }

    // Method Description:
    // - Sets focus to the desired tab. Returns false if the provided tabIndex
    //   is greater than the number of tabs we have.
    // - During startup, we'll immediately set the selected tab as focused.
    // - After startup, we'll dispatch an async method to set the the selected
    //   item of the TabView, which will then also trigger a
    //   TabView::SelectionChanged, handled in
    //   TerminalPage::_OnTabSelectionChanged
    // Return Value:
    // true iff we were able to select that tab index, false otherwise
    bool TerminalPage::_SelectTab(const uint32_t tabIndex)
    {
        if (tabIndex >= 0 && tabIndex < _tabs.Size())
        {
            if (_startupState == StartupState::InStartup)
            {
                auto tab{ _tabs.GetAt(tabIndex) };
                _tabView.SelectedItem(tab.TabViewItem());
                _UpdatedSelectedTab(tabIndex);
            }
            else
            {
                _SetFocusedTabIndex(tabIndex);
            }

            return true;
        }
        return false;
    }

    // Method Description:
    // - Helper to manually exit "zoom" when certain actions take place.
    //   Anything that modifies the state of the pane tree should probably
    //   un-zoom the focused pane first, so that the user can see the full pane
    //   tree again. These actions include:
    //   * Splitting a new pane
    //   * Closing a pane
    //   * Moving focus between panes
    //   * Resizing a pane
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_UnZoomIfNeeded()
    {
        if (const auto activeTab{ _GetFocusedTabImpl() })
        {
            if (activeTab->IsZoomed())
            {
                // Remove the content from the tab first, so Pane::UnZoom can
                // re-attach the content to the tree w/in the pane
                _tabContent.Children().Clear();
                // In ExitZoom, we'll change the Tab's Content(), triggering the
                // content changed event, which will re-attach the tab's new content
                // root to the tree.
                activeTab->ExitZoom();
            }
        }
    }

    // Method Description:
    // - Attempt to move focus between panes, as to focus the child on
    //   the other side of the separator. See Pane::NavigateFocus for details.
    // - Moves the focus of the currently focused tab.
    // Arguments:
    // - direction: The direction to move the focus in.
    // Return Value:
    // - <none>
    void TerminalPage::_MoveFocus(const FocusDirection& direction)
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            terminalTab->NavigateFocus(direction);
        }
    }

    TermControl TerminalPage::_GetActiveControl()
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            return terminalTab->GetActiveTerminalControl();
        }
        return nullptr;
    }

    // Method Description:
    // - Returns the index in our list of tabs of the currently focused tab. If
    //      no tab is currently selected, returns nullopt.
    // Return Value:
    // - the index of the currently focused tab if there is one, else nullopt
    std::optional<uint32_t> TerminalPage::_GetFocusedTabIndex() const noexcept
    {
        // GH#1117: This is a workaround because _tabView.SelectedIndex()
        //          sometimes return incorrect result after removing some tabs
        uint32_t focusedIndex;
        if (_tabView.TabItems().IndexOf(_tabView.SelectedItem(), focusedIndex))
        {
            return focusedIndex;
        }
        return std::nullopt;
    }

    // Method Description:
    // - returns a com_ptr to the currently focused tab. This might return null,
    //   so make sure to check the result!
    winrt::TerminalApp::TabBase TerminalPage::_GetFocusedTab() const noexcept
    {
        if (auto index{ _GetFocusedTabIndex() })
        {
            return _tabs.GetAt(*index);
        }
        return nullptr;
    }

    // Method Description:
    // - returns a com_ptr to the currently focused tab implementation. This might return null,
    //   so make sure to check the result!
    winrt::com_ptr<TerminalTab> TerminalPage::_GetFocusedTabImpl() const noexcept
    {
        if (auto tab{ _GetFocusedTab() })
        {
            return _GetTerminalTabImpl(tab);
        }
        return nullptr;
    }

    // Method Description:
    // - An async method for changing the focused tab on the UI thread. This
    //   method will _only_ set the selected item of the TabView, which will
    //   then also trigger a TabView::SelectionChanged event, which we'll handle
    //   in TerminalPage::_OnTabSelectionChanged, where we'll mark the new tab
    //   as focused.
    // Arguments:
    // - tabIndex: the index in the list of tabs to focus.
    // Return Value:
    // - <none>
    winrt::fire_and_forget TerminalPage::_SetFocusedTabIndex(const uint32_t tabIndex)
    {
        // GH#1117: This is a workaround because _tabView.SelectedIndex(tabIndex)
        //          sometimes set focus to an incorrect tab after removing some tabs
        auto weakThis{ get_weak() };

        co_await winrt::resume_foreground(_tabView.Dispatcher());

        if (auto page{ weakThis.get() })
        {
            auto tabToFocus = page->_tabs.GetAt(tabIndex);
            _tabView.SelectedItem(tabToFocus.TabViewItem());
        }
    }

    // Method Description:
    // - Close the currently focused tab. Focus will move to the left, if possible.
    void TerminalPage::_CloseFocusedTab()
    {
        if (auto index{ _GetFocusedTabIndex() })
        {
            auto tab{ _tabs.GetAt(*index) };
            _RemoveTab(tab);
        }
    }

    // Method Description:
    // - Close the currently focused pane. If the pane is the last pane in the
    //   tab, the tab will also be closed. This will happen when we handle the
    //   tab's Closed event.
    winrt::fire_and_forget TerminalPage::_CloseFocusedPane()
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();

            auto pane = terminalTab->GetActivePane();

            if (const auto pane{ terminalTab->GetActivePane() })
            {
                if (const auto control{ pane->GetTerminalControl() })
                {
                    if (control.ReadOnly())
                    {
                        ContentDialogResult warningResult = co_await _ShowCloseReadOnlyDialog();

                        // If the user didn't explicitly click on close tab - leave
                        if (warningResult != ContentDialogResult::Primary)
                        {
                            co_return;
                        }

                        // Clean read-only mode to prevent additional prompt if closing the pane triggers closing of a hosting tab
                        if (control.ReadOnly())
                        {
                            control.ToggleReadOnly();
                        }
                    }

                    pane->Close();
                }
            }
        }
        else if (auto index{ _GetFocusedTabIndex() })
        {
            const auto tab{ _tabs.GetAt(*index) };
            if (tab.try_as<TerminalApp::SettingsTab>())
            {
                _RemoveTab(tab);
            }
        }
    }

    // Method Description:
    // - Close the terminal app. If there is more
    //   than one tab opened, show a warning dialog.
    fire_and_forget TerminalPage::CloseWindow()
    {
        if (_tabs.Size() > 1 && _settings.GlobalSettings().ConfirmCloseAllTabs() && !_displayingCloseDialog)
        {
            _displayingCloseDialog = true;
            ContentDialogResult warningResult = co_await _ShowCloseWarningDialog();
            _displayingCloseDialog = false;

            if (warningResult != ContentDialogResult::Primary)
            {
                co_return;
            }
        }

        // Since _RemoveTab is asynchronous, create a snapshot of the  tabs we want to remove
        std::vector<winrt::TerminalApp::TabBase> tabsToRemove;
        std::copy(begin(_tabs), end(_tabs), std::back_inserter(tabsToRemove));
        _RemoveTabs(tabsToRemove);
    }

    // Method Description:
    // - Closes provided tabs one by one
    // Arguments:
    // - tabs - tabs to remove
    winrt::fire_and_forget TerminalPage::_RemoveTabs(const std::vector<winrt::TerminalApp::TabBase> tabs)
    {
        for (auto& tab : tabs)
        {
            co_await _RemoveTab(tab);
        }
    }

    // Method Description:
    // - Move the viewport of the terminal of the currently focused tab up or
    //      down a number of lines.
    // Arguments:
    // - scrollDirection: ScrollUp will move the viewport up, ScrollDown will move the viewport down
    // - rowsToScroll: a number of lines to move the viewport. If not provided we will use a system default.
    void TerminalPage::_Scroll(ScrollDirection scrollDirection, const Windows::Foundation::IReference<uint32_t>& rowsToScroll)
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            uint32_t realRowsToScroll;
            if (rowsToScroll == nullptr)
            {
                // The magic value of WHEEL_PAGESCROLL indicates that we need to scroll the entire page
                realRowsToScroll = _systemRowsToScroll == WHEEL_PAGESCROLL ?
                                       terminalTab->GetActiveTerminalControl().GetViewHeight() :
                                       _systemRowsToScroll;
            }
            else
            {
                // use the custom value specified in the command
                realRowsToScroll = rowsToScroll.Value();
            }
            auto scrollDelta = _ComputeScrollDelta(scrollDirection, realRowsToScroll);
            terminalTab->Scroll(scrollDelta);
        }
    }

    // Method Description:
    // - Split the focused pane either horizontally or vertically, and place the
    //   given TermControl into the newly created pane.
    // - If splitType == SplitState::None, this method does nothing.
    // Arguments:
    // - splitType: one value from the TerminalApp::SplitState enum, indicating how the
    //   new pane should be split from its parent.
    // - splitMode: value from TerminalApp::SplitType enum, indicating the profile to be used in the newly split pane.
    // - newTerminalArgs: An object that may contain a blob of parameters to
    //   control which profile is created and with possible other
    //   configurations. See CascadiaSettings::BuildSettings for more details.
    void TerminalPage::_SplitPane(const SplitState splitType,
                                  const SplitType splitMode,
                                  const float splitSize,
                                  const NewTerminalArgs& newTerminalArgs)
    {
        // Do nothing if we're requesting no split.
        if (splitType == SplitState::None)
        {
            return;
        }

        const auto focusedTab{ _GetFocusedTabImpl() };

        // Do nothing if no TerminalTab is focused
        if (!focusedTab)
        {
            return;
        }

        try
        {
            TerminalSettings controlSettings{ nullptr };
            GUID realGuid;
            bool profileFound = false;

            if (splitMode == SplitType::Duplicate)
            {
                std::optional<GUID> current_guid = focusedTab->GetFocusedProfile();
                if (current_guid)
                {
                    profileFound = true;
                    controlSettings = TerminalSettings::CreateWithProfileByID(_settings, current_guid.value(), *_bindings);
                    const auto workingDirectory = focusedTab->GetActiveTerminalControl().WorkingDirectory();
                    const auto validWorkingDirectory = !workingDirectory.empty();
                    if (validWorkingDirectory)
                    {
                        controlSettings.StartingDirectory(workingDirectory);
                    }
                    realGuid = current_guid.value();
                }
                // TODO: GH#5047 - In the future, we should get the Profile of
                // the focused pane, and use that to build a new instance of the
                // settings so we can duplicate this tab/pane.
                //
                // Currently, if the profile doesn't exist anymore in our
                // settings, we'll silently do nothing.
                //
                // In the future, it will be preferable to just duplicate the
                // current control's settings, but we can't do that currently,
                // because we won't be able to create a new instance of the
                // connection without keeping an instance of the original Profile
                // object around.
            }
            if (!profileFound)
            {
                realGuid = _settings.GetProfileForArgs(newTerminalArgs);
                controlSettings = TerminalSettings::CreateWithNewTerminalArgs(_settings, newTerminalArgs, *_bindings);
            }

            const auto controlConnection = _CreateConnectionFromSettings(realGuid, controlSettings);

            const float contentWidth = ::base::saturated_cast<float>(_tabContent.ActualWidth());
            const float contentHeight = ::base::saturated_cast<float>(_tabContent.ActualHeight());
            const winrt::Windows::Foundation::Size availableSpace{ contentWidth, contentHeight };

            auto realSplitType = splitType;
            if (realSplitType == SplitState::Automatic)
            {
                realSplitType = focusedTab->PreCalculateAutoSplit(availableSpace);
            }

            const auto canSplit = focusedTab->PreCalculateCanSplit(realSplitType, splitSize, availableSpace);
            if (!canSplit)
            {
                return;
            }

            auto newControl = _InitControl(controlSettings, controlConnection);

            // Hookup our event handlers to the new terminal
            _RegisterTerminalEvents(newControl, *focusedTab);

            _UnZoomIfNeeded();

            focusedTab->SplitPane(realSplitType, splitSize, realGuid, newControl);
        }
        CATCH_LOG();
    }

    // Method Description:
    // - Attempt to move a separator between panes, as to resize each child on
    //   either size of the separator. See Pane::ResizePane for details.
    // - Moves a separator on the currently focused tab.
    // Arguments:
    // - direction: The direction to move the separator in.
    // Return Value:
    // - <none>
    void TerminalPage::_ResizePane(const ResizeDirection& direction)
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            _UnZoomIfNeeded();
            terminalTab->ResizePane(direction);
        }
    }

    // Method Description:
    // - Move the viewport of the terminal of the currently focused tab up or
    //      down a page. The page length will be dependent on the terminal view height.
    // Arguments:
    // - scrollDirection: ScrollUp will move the viewport up, ScrollDown will move the viewport down
    void TerminalPage::_ScrollPage(ScrollDirection scrollDirection)
    {
        // Do nothing if for some reason, there's no terminal tab in focus. We don't want to crash.
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            const auto control = _GetActiveControl();
            const auto termHeight = control.GetViewHeight();
            auto scrollDelta = _ComputeScrollDelta(scrollDirection, termHeight);
            terminalTab->Scroll(scrollDelta);
        }
    }

    void TerminalPage::_ScrollToBufferEdge(ScrollDirection scrollDirection)
    {
        if (const auto terminalTab{ _GetFocusedTabImpl() })
        {
            auto scrollDelta = _ComputeScrollDelta(scrollDirection, INT_MAX);
            terminalTab->Scroll(scrollDelta);
        }
    }

    // Method Description:
    // - Gets the title of the currently focused terminal control. If there
    //   isn't a control selected for any reason, returns "Windows Terminal"
    // Arguments:
    // - <none>
    // Return Value:
    // - the title of the focused control if there is one, else "Windows Terminal"
    hstring TerminalPage::Title()
    {
        if (_settings.GlobalSettings().ShowTitleInTitlebar())
        {
            auto selectedIndex = _tabView.SelectedIndex();
            if (selectedIndex >= 0)
            {
                try
                {
                    if (auto focusedControl{ _GetActiveControl() })
                    {
                        return focusedControl.Title();
                    }
                }
                CATCH_LOG();
            }
        }
        return { L"Windows Terminal" };
    }

    // Method Description:
    // - Handles the special case of providing a text override for the UI shortcut due to VK_OEM issue.
    //      Looks at the flags from the KeyChord modifiers and provides a concatenated string value of all
    //      in the same order that XAML would put them as well.
    // Return Value:
    // - a string representation of the key modifiers for the shortcut
    //NOTE: This needs to be localized with https://github.com/microsoft/terminal/issues/794 if XAML framework issue not resolved before then
    static std::wstring _FormatOverrideShortcutText(KeyModifiers modifiers)
    {
        std::wstring buffer{ L"" };

        if (WI_IsFlagSet(modifiers, KeyModifiers::Ctrl))
        {
            buffer += L"Ctrl+";
        }

        if (WI_IsFlagSet(modifiers, KeyModifiers::Shift))
        {
            buffer += L"Shift+";
        }

        if (WI_IsFlagSet(modifiers, KeyModifiers::Alt))
        {
            buffer += L"Alt+";
        }

        return buffer;
    }

    // Method Description:
    // - Takes a MenuFlyoutItem and a corresponding KeyChord value and creates the accelerator for UI display.
    //   Takes into account a special case for an error condition for a comma
    // Arguments:
    // - MenuFlyoutItem that will be displayed, and a KeyChord to map an accelerator
    void TerminalPage::_SetAcceleratorForMenuItem(WUX::Controls::MenuFlyoutItem& menuItem,
                                                  const KeyChord& keyChord)
    {
#ifdef DEP_MICROSOFT_UI_XAML_708_FIXED
        // work around https://github.com/microsoft/microsoft-ui-xaml/issues/708 in case of VK_OEM_COMMA
        if (keyChord.Vkey() != VK_OEM_COMMA)
        {
            // use the XAML shortcut to give us the automatic capabilities
            auto menuShortcut = Windows::UI::Xaml::Input::KeyboardAccelerator{};

            // TODO: Modify this when https://github.com/microsoft/terminal/issues/877 is resolved
            menuShortcut.Key(static_cast<Windows::System::VirtualKey>(keyChord.Vkey()));

            // inspect the modifiers from the KeyChord and set the flags int he XAML value
            auto modifiers = AppKeyBindings::ConvertVKModifiers(keyChord.Modifiers());

            // add the modifiers to the shortcut
            menuShortcut.Modifiers(modifiers);

            // add to the menu
            menuItem.KeyboardAccelerators().Append(menuShortcut);
        }
        else // we've got a comma, so need to just use the alternate method
#endif
        {
            // extract the modifier and key to a nice format
            auto overrideString = _FormatOverrideShortcutText(keyChord.Modifiers());
            auto mappedCh = MapVirtualKeyW(keyChord.Vkey(), MAPVK_VK_TO_CHAR);
            if (mappedCh != 0)
            {
                menuItem.KeyboardAcceleratorTextOverride(overrideString + gsl::narrow_cast<wchar_t>(mappedCh));
            }
        }
    }

    // Method Description:
    // - Calculates the appropriate size to snap to in the given direction, for
    //   the given dimension. If the global setting `snapToGridOnResize` is set
    //   to `false`, this will just immediately return the provided dimension,
    //   effectively disabling snapping.
    // - See Pane::CalcSnappedDimension
    float TerminalPage::CalcSnappedDimension(const bool widthOrHeight, const float dimension) const
    {
        if (_settings && _settings.GlobalSettings().SnapToGridOnResize())
        {
            if (const auto terminalTab{ _GetFocusedTabImpl() })
            {
                return terminalTab->CalcSnappedDimension(widthOrHeight, dimension);
            }
        }
        return dimension;
    }

    // Method Description:
    // - Place `copiedData` into the clipboard as text. Triggered when a
    //   terminal control raises it's CopyToClipboard event.
    // Arguments:
    // - copiedData: the new string content to place on the clipboard.
    winrt::fire_and_forget TerminalPage::_CopyToClipboardHandler(const IInspectable /*sender*/,
                                                                 const CopyToClipboardEventArgs copiedData)
    {
        co_await winrt::resume_foreground(Dispatcher(), CoreDispatcherPriority::High);

        DataPackage dataPack = DataPackage();
        dataPack.RequestedOperation(DataPackageOperation::Copy);

        // The EventArgs.Formats() is an override for the global setting "copyFormatting"
        //   iff it is set
        bool useGlobal = copiedData.Formats() == nullptr;
        auto copyFormats = useGlobal ?
                               _settings.GlobalSettings().CopyFormatting() :
                               copiedData.Formats().Value();

        // copy text to dataPack
        dataPack.SetText(copiedData.Text());

        if (WI_IsFlagSet(copyFormats, CopyFormat::HTML))
        {
            // copy html to dataPack
            const auto htmlData = copiedData.Html();
            if (!htmlData.empty())
            {
                dataPack.SetHtmlFormat(htmlData);
            }
        }

        if (WI_IsFlagSet(copyFormats, CopyFormat::RTF))
        {
            // copy rtf data to dataPack
            const auto rtfData = copiedData.Rtf();
            if (!rtfData.empty())
            {
                dataPack.SetRtf(rtfData);
            }
        }

        try
        {
            Clipboard::SetContent(dataPack);
            Clipboard::Flush();
        }
        CATCH_LOG();
    }

    // Function Description:
    // - This function is called when the `TermControl` requests that we send
    //   it the clipboard's content.
    // - Retrieves the data from the Windows Clipboard and converts it to text.
    // - Shows warnings if the clipboard is too big or contains multiple lines
    //   of text.
    // - Sends the text back to the TermControl through the event's
    //   `HandleClipboardData` member function.
    // - Does some of this in a background thread, as to not hang/crash the UI thread.
    // Arguments:
    // - eventArgs: the PasteFromClipboard event sent from the TermControl
    fire_and_forget TerminalPage::_PasteFromClipboardHandler(const IInspectable /*sender*/,
                                                             const PasteFromClipboardEventArgs eventArgs)
    {
        const DataPackageView data = Clipboard::GetContent();

        // This will switch the execution of the function to a background (not
        // UI) thread. This is IMPORTANT, because the getting the clipboard data
        // will crash on the UI thread, because the main thread is a STA.
        co_await winrt::resume_background();

        try
        {
            hstring text = L"";
            if (data.Contains(StandardDataFormats::Text()))
            {
                text = co_await data.GetTextAsync();
            }
            // Windows Explorer's "Copy address" menu item stores a StorageItem in the clipboard, and no text.
            else if (data.Contains(StandardDataFormats::StorageItems()))
            {
                Windows::Foundation::Collections::IVectorView<Windows::Storage::IStorageItem> items = co_await data.GetStorageItemsAsync();
                if (items.Size() > 0)
                {
                    Windows::Storage::IStorageItem item = items.GetAt(0);
                    text = item.Path();
                }
            }

            const auto isNewLineLambda = [](auto c) { return c == L'\n' || c == L'\r'; };
            const auto hasNewLine = std::find_if(text.cbegin(), text.cend(), isNewLineLambda) != text.cend();
            const auto warnMultiLine = hasNewLine && _settings.GlobalSettings().WarnAboutMultiLinePaste();

            constexpr const std::size_t minimumSizeForWarning = 1024 * 5; // 5 KiB
            const bool warnLargeText = text.size() > minimumSizeForWarning &&
                                       _settings.GlobalSettings().WarnAboutLargePaste();

            if (warnMultiLine || warnLargeText)
            {
                co_await winrt::resume_foreground(Dispatcher());

                // We have to initialize the dialog here to be able to change the text of the text block within it
                FindName(L"MultiLinePasteDialog").try_as<WUX::Controls::ContentDialog>();
                ClipboardText().Text(text);

                // The vertical offset on the scrollbar does not reset automatically, so reset it manually
                ClipboardContentScrollViewer().ScrollToVerticalOffset(0);

                ContentDialogResult warningResult;
                if (warnMultiLine)
                {
                    warningResult = co_await _ShowMultiLinePasteWarningDialog();
                }
                else if (warnLargeText)
                {
                    warningResult = co_await _ShowLargePasteWarningDialog();
                }

                // Clear the clipboard text so it doesn't lie around in memory
                ClipboardText().Text(L"");

                if (warningResult != ContentDialogResult::Primary)
                {
                    // user rejected the paste
                    co_return;
                }
            }

            eventArgs.HandleClipboardData(text);
        }
        CATCH_LOG();
    }

    void TerminalPage::_OpenHyperlinkHandler(const IInspectable /*sender*/, const Microsoft::Terminal::Control::OpenHyperlinkEventArgs eventArgs)
    {
        try
        {
            auto parsed = winrt::Windows::Foundation::Uri(eventArgs.Uri().c_str());
            if (_IsUriSupported(parsed))
            {
                ShellExecute(nullptr, L"open", eventArgs.Uri().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            else
            {
                _ShowCouldNotOpenDialog(RS_(L"UnsupportedSchemeText"), eventArgs.Uri());
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            _ShowCouldNotOpenDialog(RS_(L"InvalidUriText"), eventArgs.Uri());
        }
    }

    // Method Description:
    // - Opens up a dialog box explaining why we could not open a URI
    // Arguments:
    // - The reason (unsupported scheme, invalid uri, potentially more in the future)
    // - The uri
    void TerminalPage::_ShowCouldNotOpenDialog(winrt::hstring reason, winrt::hstring uri)
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            // FindName needs to be called first to actually load the xaml object
            auto unopenedUriDialog = FindName(L"CouldNotOpenUriDialog").try_as<WUX::Controls::ContentDialog>();

            // Insert the reason and the URI
            CouldNotOpenUriReason().Text(reason);
            UnopenedUri().Text(uri);

            // Show the dialog
            presenter.ShowDialog(unopenedUriDialog);
        }
    }

    // Method Description:
    // - Determines if the given URI is currently supported
    // Arguments:
    // - The parsed URI
    // Return value:
    // - True if we support it, false otherwise
    bool TerminalPage::_IsUriSupported(const winrt::Windows::Foundation::Uri& parsedUri)
    {
        if (parsedUri.SchemeName() == L"http" || parsedUri.SchemeName() == L"https")
        {
            return true;
        }
        if (parsedUri.SchemeName() == L"file")
        {
            const auto host = parsedUri.Host();
            // If no hostname was provided or if the hostname was "localhost", Host() will return an empty string
            // and we allow it
            if (host == L"")
            {
                return true;
            }
            // TODO: by the OSC 8 spec, if a hostname (other than localhost) is provided, we _should_ be
            // comparing that value against what is returned by GetComputerNameExW and making sure they match.
            // However, ShellExecute does not seem to be happy with file URIs of the form
            //          file://{hostname}/path/to/file.ext
            // and so while we could do the hostname matching, we do not know how to actually open the URI
            // if its given in that form. So for now we ignore all hostnames other than localhost
        }
        return false;
    }

    void TerminalPage::_ControlNoticeRaisedHandler(const IInspectable /*sender*/, const Microsoft::Terminal::Control::NoticeEventArgs eventArgs)
    {
        winrt::hstring message = eventArgs.Message();

        winrt::hstring title;

        switch (eventArgs.Level())
        {
        case NoticeLevel::Debug:
            title = RS_(L"NoticeDebug"); //\xebe8
            break;
        case NoticeLevel::Info:
            title = RS_(L"NoticeInfo"); // \xe946
            break;
        case NoticeLevel::Warning:
            title = RS_(L"NoticeWarning"); //\xe7ba
            break;
        case NoticeLevel::Error:
            title = RS_(L"NoticeError"); //\xe783
            break;
        }

        _ShowControlNoticeDialog(title, message);
    }

    void TerminalPage::_ShowControlNoticeDialog(const winrt::hstring& title, const winrt::hstring& message)
    {
        if (auto presenter{ _dialogPresenter.get() })
        {
            // FindName needs to be called first to actually load the xaml object
            auto controlNoticeDialog = FindName(L"ControlNoticeDialog").try_as<WUX::Controls::ContentDialog>();

            ControlNoticeDialog().Title(winrt::box_value(title));

            // Insert the message
            NoticeMessage().Text(message);

            // Show the dialog
            presenter.ShowDialog(controlNoticeDialog);
        }
    }

    // Method Description:
    // - Copy text from the focused terminal to the Windows Clipboard
    // Arguments:
    // - singleLine: if enabled, copy contents as a single line of text
    // - formats: dictate which formats need to be copied
    // Return Value:
    // - true iff we we able to copy text (if a selection was active)
    bool TerminalPage::_CopyText(const bool singleLine, const Windows::Foundation::IReference<CopyFormat>& formats)
    {
        const auto control = _GetActiveControl();
        return control.CopySelectionToClipboard(singleLine, formats);
    }

    // Method Description:
    // - Send an event (which will be caught by AppHost) to set the progress indicator on the taskbar
    // Arguments:
    // - sender (not used)
    // - eventArgs: the arguments specifying how to set the progress indicator
    void TerminalPage::_SetTaskbarProgressHandler(const IInspectable /*sender*/, const IInspectable /*eventArgs*/)
    {
        _SetTaskbarProgressHandlers(*this, nullptr);
    }

    // Method Description:
    // - Paste text from the Windows Clipboard to the focused terminal
    void TerminalPage::_PasteText()
    {
        const auto control = _GetActiveControl();
        control.PasteTextFromClipboard();
    }

    // Function Description:
    // - Called when the settings button is clicked. ShellExecutes the settings
    //   file, as to open it in the default editor for .json files. Does this in
    //   a background thread, as to not hang/crash the UI thread.
    fire_and_forget TerminalPage::_LaunchSettings(const SettingsTarget target)
    {
        if (target == SettingsTarget::SettingsUI)
        {
            _OpenSettingsUI();
        }
        else
        {
            // This will switch the execution of the function to a background (not
            // UI) thread. This is IMPORTANT, because the Windows.Storage API's
            // (used for retrieving the path to the file) will crash on the UI
            // thread, because the main thread is a STA.
            co_await winrt::resume_background();

            auto openFile = [](const auto& filePath) {
                HINSTANCE res = ShellExecute(nullptr, nullptr, filePath.c_str(), nullptr, nullptr, SW_SHOW);
                if (static_cast<int>(reinterpret_cast<uintptr_t>(res)) <= 32)
                {
                    ShellExecute(nullptr, nullptr, L"notepad", filePath.c_str(), nullptr, SW_SHOW);
                }
            };

            switch (target)
            {
            case SettingsTarget::DefaultsFile:
                openFile(CascadiaSettings::DefaultSettingsPath());
                break;
            case SettingsTarget::SettingsFile:
                openFile(CascadiaSettings::SettingsPath());
                break;
            case SettingsTarget::AllFiles:
                openFile(CascadiaSettings::DefaultSettingsPath());
                openFile(CascadiaSettings::SettingsPath());
                break;
            }
        }
    }

    // Method Description:
    // - Responds to changes in the TabView's item list by changing the
    //   tabview's visibility.
    // - This method is also invoked when tabs are dragged / dropped as part of
    //   tab reordering and this method hands that case as well in concert with
    //   TabDragStarting and TabDragCompleted handlers that are set up in
    //   TerminalPage::Create()
    // Arguments:
    // - sender: the control that originated this event
    // - eventArgs: the event's constituent arguments
    void TerminalPage::_OnTabItemsChanged(const IInspectable& /*sender*/, const Windows::Foundation::Collections::IVectorChangedEventArgs& eventArgs)
    {
        if (_rearranging)
        {
            if (eventArgs.CollectionChange() == Windows::Foundation::Collections::CollectionChange::ItemRemoved)
            {
                _rearrangeFrom = eventArgs.Index();
            }

            if (eventArgs.CollectionChange() == Windows::Foundation::Collections::CollectionChange::ItemInserted)
            {
                _rearrangeTo = eventArgs.Index();
            }
        }

        CommandPalette().Visibility(Visibility::Collapsed);
        _UpdateTabView();
    }

    // Method Description:
    // - Additional responses to clicking on a TabView's item. Currently, just remove tab with middle click
    // Arguments:
    // - sender: the control that originated this event (TabViewItem)
    // - eventArgs: the event's constituent arguments
    void TerminalPage::_OnTabClick(const IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& eventArgs)
    {
        if (eventArgs.GetCurrentPoint(*this).Properties().IsMiddleButtonPressed())
        {
            _RemoveTabViewItem(sender.as<MUX::Controls::TabViewItem>());
            eventArgs.Handled(true);
        }
        else if (eventArgs.GetCurrentPoint(*this).Properties().IsRightButtonPressed())
        {
            eventArgs.Handled(true);
        }
    }

    void TerminalPage::_UpdatedSelectedTab(const int32_t index)
    {
        // Unfocus all the tabs.
        for (auto tab : _tabs)
        {
            tab.Focus(FocusState::Unfocused);
        }

        if (index >= 0)
        {
            try
            {
                auto tab{ _tabs.GetAt(index) };

                _tabContent.Children().Clear();
                _tabContent.Children().Append(tab.Content());

                // GH#7409: If the tab switcher is open, then we _don't_ want to
                // automatically focus the new tab here. The tab switcher wants
                // to be able to "preview" the selected tab as the user tabs
                // through the menu, but if we toss the focus to the control
                // here, then the user won't be able to navigate the ATS any
                // longer.
                //
                // When the tab switcher is eventually dismissed, the focus will
                // get tossed back to the focused terminal control, so we don't
                // need to worry about focus getting lost.
                if (CommandPalette().Visibility() != Visibility::Visible)
                {
                    tab.Focus(FocusState::Programmatic);
                    _UpdateMRUTab(index);
                }

                tab.TabViewItem().StartBringIntoView();

                // Raise an event that our title changed
                if (_settings.GlobalSettings().ShowTitleInTitlebar())
                {
                    _TitleChangedHandlers(*this, tab.Title());
                }
            }
            CATCH_LOG();
        }
    }

    // Method Description:
    // - Responds to the TabView control's Selection Changed event (to move a
    //      new terminal control into focus) when not in in the middle of a tab rearrangement.
    // Arguments:
    // - sender: the control that originated this event
    // - eventArgs: the event's constituent arguments
    void TerminalPage::_OnTabSelectionChanged(const IInspectable& sender, const WUX::Controls::SelectionChangedEventArgs& /*eventArgs*/)
    {
        if (!_rearranging && !_removing)
        {
            auto tabView = sender.as<MUX::Controls::TabView>();
            auto selectedIndex = tabView.SelectedIndex();
            _UpdatedSelectedTab(selectedIndex);
        }
    }

    // Method Description:
    // - Called when our tab content size changes. This updates each tab with
    //   the new size, so they have a chance to update each of their panes with
    //   the new size.
    // Arguments:
    // - e: the SizeChangedEventArgs with the new size of the tab content area.
    // Return Value:
    // - <none>
    void TerminalPage::_OnContentSizeChanged(const IInspectable& /*sender*/, Windows::UI::Xaml::SizeChangedEventArgs const& e)
    {
        const auto newSize = e.NewSize();
        for (auto tab : _tabs)
        {
            if (auto terminalTab = _GetTerminalTabImpl(tab))
            {
                terminalTab->ResizeContent(newSize);
            }
        }
    }

    // Method Description:
    // - Responds to the TabView control's Tab Closing event by removing
    //      the indicated tab from the set and focusing another one.
    //      The event is cancelled so App maintains control over the
    //      items in the tabview.
    // Arguments:
    // - sender: the control that originated this event
    // - eventArgs: the event's constituent arguments
    void TerminalPage::_OnTabCloseRequested(const IInspectable& /*sender*/, const MUX::Controls::TabViewTabCloseRequestedEventArgs& eventArgs)
    {
        const auto tabViewItem = eventArgs.Tab();
        _RemoveTabViewItem(tabViewItem);
    }

    TermControl TerminalPage::_InitControl(const TerminalSettings& settings, const ITerminalConnection& connection)
    {
        return TermControl{ TerminalSettings::CreateWithParent(settings), connection };
    }

    // Method Description:
    // - Hook up keybindings, and refresh the UI of the terminal.
    //   This includes update the settings of all the tabs according
    //   to their profiles, update the title and icon of each tab, and
    //   finally create the tab flyout
    winrt::fire_and_forget TerminalPage::_RefreshUIForSettingsReload()
    {
        // Re-wire the keybindings to their handlers, as we'll have created a
        // new AppKeyBindings object.
        _HookupKeyBindings(_settings.KeyMap());

        // Refresh UI elements
        auto profiles = _settings.ActiveProfiles();
        for (const auto& profile : profiles)
        {
            const auto profileGuid = profile.Guid();

            try
            {
                // This can throw an exception if the profileGuid does
                // not belong to an actual profile in the list of profiles.
                auto settings{ TerminalSettings::CreateWithProfileByID(_settings, profileGuid, *_bindings) };

                for (auto tab : _tabs)
                {
                    if (auto terminalTab = _GetTerminalTabImpl(tab))
                    {
                        terminalTab->UpdateSettings(settings, profileGuid);
                    }
                }
            }
            CATCH_LOG();
        }

        // GH#2455: If there are any panes with controls that had been
        // initialized with a Profile that no longer exists in our list of
        // profiles, we'll leave it unmodified. The profile doesn't exist
        // anymore, so we can't possibly update its settings.

        // Update the icon of the tab for the currently focused profile in that tab.
        // Only do this for TerminalTabs. Other types of tabs won't have multiple panes
        // and profiles so the Title and Icon will be set once and only once on init.
        for (auto tab : _tabs)
        {
            if (auto terminalTab = _GetTerminalTabImpl(tab))
            {
                _UpdateTabIcon(*terminalTab);

                // Force the TerminalTab to re-grab its currently active control's title.
                terminalTab->UpdateTitle();
            }
            else if (auto settingsTab = tab.try_as<TerminalApp::SettingsTab>())
            {
                settingsTab.UpdateSettings(_settings);
            }

            auto tabImpl{ winrt::get_self<TabBase>(tab) };
            tabImpl->SetKeyMap(_settings.KeyMap());
        }

        auto weakThis{ get_weak() };

        co_await winrt::resume_foreground(Dispatcher());

        // repopulate the new tab button's flyout with entries for each
        // profile, which might have changed
        if (auto page{ weakThis.get() })
        {
            _UpdateTabWidthMode();
            _CreateNewTabFlyout();
        }

        // Reload the current value of alwaysOnTop from the settings file. This
        // will let the user hot-reload this setting, but any runtime changes to
        // the alwaysOnTop setting will be lost.
        _isAlwaysOnTop = _settings.GlobalSettings().AlwaysOnTop();
        _AlwaysOnTopChangedHandlers(*this, nullptr);

        // Settings AllowDependentAnimations will affect whether animations are
        // enabled application-wide, so we don't need to check it each time we
        // want to create an animation.
        WUX::Media::Animation::Timeline::AllowDependentAnimations(!_settings.GlobalSettings().DisableAnimations());
    }

    // This is a helper to aid in sorting commands by their `Name`s, alphabetically.
    static bool _compareSchemeNames(const ColorScheme& lhs, const ColorScheme& rhs)
    {
        std::wstring leftName{ lhs.Name() };
        std::wstring rightName{ rhs.Name() };
        return leftName.compare(rightName) < 0;
    }

    // Method Description:
    // - Takes a mapping of names->commands and expands them
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    IMap<winrt::hstring, Command> TerminalPage::_ExpandCommands(IMapView<winrt::hstring, Command> commandsToExpand,
                                                                IVectorView<Profile> profiles,
                                                                IMapView<winrt::hstring, ColorScheme> schemes)
    {
        IVector<SettingsLoadWarnings> warnings{ winrt::single_threaded_vector<SettingsLoadWarnings>() };

        std::vector<ColorScheme> sortedSchemes;
        sortedSchemes.reserve(schemes.Size());

        for (const auto& nameAndScheme : schemes)
        {
            sortedSchemes.push_back(nameAndScheme.Value());
        }
        std::sort(sortedSchemes.begin(),
                  sortedSchemes.end(),
                  _compareSchemeNames);

        IMap<winrt::hstring, Command> copyOfCommands = winrt::single_threaded_map<winrt::hstring, Command>();
        for (const auto& nameAndCommand : commandsToExpand)
        {
            copyOfCommands.Insert(nameAndCommand.Key(), nameAndCommand.Value());
        }

        Command::ExpandCommands(copyOfCommands,
                                profiles,
                                { sortedSchemes },
                                warnings);

        return copyOfCommands;
    }
    // Method Description:
    // - Repopulates the list of commands in the command palette with the
    //   current commands in the settings. Also updates the keybinding labels to
    //   reflect any matching keybindings.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_UpdateCommandsForPalette()
    {
        IMap<winrt::hstring, Command> copyOfCommands = _ExpandCommands(_settings.GlobalSettings().Commands(),
                                                                       _settings.ActiveProfiles().GetView(),
                                                                       _settings.GlobalSettings().ColorSchemes());

        _recursiveUpdateCommandKeybindingLabels(_settings, copyOfCommands.GetView());

        // Update the command palette when settings reload
        auto commandsCollection = winrt::single_threaded_vector<Command>();
        for (const auto& nameAndCommand : copyOfCommands)
        {
            commandsCollection.Append(nameAndCommand.Value());
        }

        CommandPalette().SetCommands(commandsCollection);
    }

    // Method Description:
    // - Sets the initial actions to process on startup. We'll make a copy of
    //   this list, and process these actions when we're loaded.
    // - This function will have no effective result after Create() is called.
    // Arguments:
    // - actions: a list of Actions to process on startup.
    // Return Value:
    // - <none>
    void TerminalPage::SetStartupActions(std::vector<ActionAndArgs>& actions)
    {
        // The fastest way to copy all the actions out of the std::vector and
        // put them into a winrt::IVector is by making a copy, then moving the
        // copy into the winrt vector ctor.
        auto listCopy = actions;
        _startupActions = winrt::single_threaded_vector<ActionAndArgs>(std::move(listCopy));
    }

    // Routine Description:
    // - Notifies this Terminal Page that it should start the incoming connection
    //   listener for command-line tools attempting to join this Terminal
    //   through the default application channel.
    // Arguments:
    // - <none> - Implicitly sets to true. Default page state is false.
    // Return Value:
    // - <none>
    void TerminalPage::SetInboundListener()
    {
        _shouldStartInboundListener = true;
    }

    winrt::TerminalApp::IDialogPresenter TerminalPage::DialogPresenter() const
    {
        return _dialogPresenter.get();
    }

    void TerminalPage::DialogPresenter(winrt::TerminalApp::IDialogPresenter dialogPresenter)
    {
        _dialogPresenter = dialogPresenter;
    }

    // Method Description:
    // - Gets the taskbar state value from the last active control
    // Return Value:
    // - The taskbar state of the last active control
    size_t TerminalPage::GetLastActiveControlTaskbarState()
    {
        if (auto control{ _GetActiveControl() })
        {
            return gsl::narrow_cast<size_t>(control.TaskbarState());
        }
        return {};
    }

    // Method Description:
    // - Gets the taskbar progress value from the last active control
    // Return Value:
    // - The taskbar progress of the last active control
    size_t TerminalPage::GetLastActiveControlTaskbarProgress()
    {
        if (auto control{ _GetActiveControl() })
        {
            return gsl::narrow_cast<size_t>(control.TaskbarProgress());
        }
        return {};
    }

    // Method Description:
    // - This is the method that App will call when the titlebar
    //   has been clicked. It dismisses any open flyouts.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::TitlebarClicked()
    {
        if (_newTabButton && _newTabButton.Flyout())
        {
            _newTabButton.Flyout().Hide();
        }

        for (const auto& tab : _tabs)
        {
            if (tab.TabViewItem().ContextFlyout())
            {
                tab.TabViewItem().ContextFlyout().Hide();
            }
        }
    }

    // Method Description:
    // - Called when the user tries to do a search using keybindings.
    //   This will tell the current focused terminal control to create
    //   a search box and enable find process.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_Find()
    {
        const auto termControl = _GetActiveControl();
        termControl.CreateSearchBoxControl();
    }

    // Method Description:
    // - Toggles borderless mode. Hides the tab row, and raises our
    //   FocusModeChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleFocusMode()
    {
        _isInFocusMode = !_isInFocusMode;
        _UpdateTabView();
        _FocusModeChangedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Toggles fullscreen mode. Hides the tab row, and raises our
    //   FullscreenChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleFullscreen()
    {
        _isFullscreen = !_isFullscreen;
        _UpdateTabView();
        _FullscreenChangedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Toggles always on top mode. Raises our AlwaysOnTopChanged event.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::ToggleAlwaysOnTop()
    {
        _isAlwaysOnTop = !_isAlwaysOnTop;
        _AlwaysOnTopChangedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Sets the tab split button color when a new tab color is selected
    // Arguments:
    // - color: The color of the newly selected tab, used to properly calculate
    //          the foreground color of the split button (to match the font
    //          color of the tab)
    // - accentColor: the actual color we are going to use to paint the tab row and
    //                split button, so that there is some contrast between the tab
    //                and the non-client are behind it
    // Return Value:
    // - <none>
    void TerminalPage::_SetNewTabButtonColor(const Windows::UI::Color& color, const Windows::UI::Color& accentColor)
    {
        // TODO GH#3327: Look at what to do with the tab button when we have XAML theming
        bool IsBrightColor = ColorHelper::IsBrightColor(color);
        bool isLightAccentColor = ColorHelper::IsBrightColor(accentColor);
        winrt::Windows::UI::Color pressedColor{};
        winrt::Windows::UI::Color hoverColor{};
        winrt::Windows::UI::Color foregroundColor{};
        const float hoverColorAdjustment = 5.f;
        const float pressedColorAdjustment = 7.f;

        if (IsBrightColor)
        {
            foregroundColor = winrt::Windows::UI::Colors::Black();
        }
        else
        {
            foregroundColor = winrt::Windows::UI::Colors::White();
        }

        if (isLightAccentColor)
        {
            hoverColor = ColorHelper::Darken(accentColor, hoverColorAdjustment);
            pressedColor = ColorHelper::Darken(accentColor, pressedColorAdjustment);
        }
        else
        {
            hoverColor = ColorHelper::Lighten(accentColor, hoverColorAdjustment);
            pressedColor = ColorHelper::Lighten(accentColor, pressedColorAdjustment);
        }

        Media::SolidColorBrush backgroundBrush{ accentColor };
        Media::SolidColorBrush backgroundHoverBrush{ hoverColor };
        Media::SolidColorBrush backgroundPressedBrush{ pressedColor };
        Media::SolidColorBrush foregroundBrush{ foregroundColor };

        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackground"), backgroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackgroundPointerOver"), backgroundHoverBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonBackgroundPressed"), backgroundPressedBrush);

        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForeground"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundPointerOver"), foregroundBrush);
        _newTabButton.Resources().Insert(winrt::box_value(L"SplitButtonForegroundPressed"), foregroundBrush);

        _newTabButton.Background(backgroundBrush);
        _newTabButton.Foreground(foregroundBrush);
    }

    // Method Description:
    // - Clears the tab split button color to a system color
    //   (or white if none is found) when the tab's color is cleared
    // - Clears the tab row color to a system color
    //   (or white if none is found) when the tab's color is cleared
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_ClearNewTabButtonColor()
    {
        // TODO GH#3327: Look at what to do with the tab button when we have XAML theming
        winrt::hstring keys[] = {
            L"SplitButtonBackground",
            L"SplitButtonBackgroundPointerOver",
            L"SplitButtonBackgroundPressed",
            L"SplitButtonForeground",
            L"SplitButtonForegroundPointerOver",
            L"SplitButtonForegroundPressed"
        };

        // simply clear any of the colors in the split button's dict
        for (auto keyString : keys)
        {
            auto key = winrt::box_value(keyString);
            if (_newTabButton.Resources().HasKey(key))
            {
                _newTabButton.Resources().Remove(key);
            }
        }

        const auto res = Application::Current().Resources();

        const auto defaultBackgroundKey = winrt::box_value(L"TabViewItemHeaderBackground");
        const auto defaultForegroundKey = winrt::box_value(L"SystemControlForegroundBaseHighBrush");
        winrt::Windows::UI::Xaml::Media::SolidColorBrush backgroundBrush;
        winrt::Windows::UI::Xaml::Media::SolidColorBrush foregroundBrush;

        // TODO: Related to GH#3917 - I think if the system is set to "Dark"
        // theme, but the app is set to light theme, then this lookup still
        // returns to us the dark theme brushes. There's gotta be a way to get
        // the right brushes...
        // See also GH#5741
        if (res.HasKey(defaultBackgroundKey))
        {
            winrt::Windows::Foundation::IInspectable obj = res.Lookup(defaultBackgroundKey);
            backgroundBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            backgroundBrush = winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Colors::Black() };
        }

        if (res.HasKey(defaultForegroundKey))
        {
            winrt::Windows::Foundation::IInspectable obj = res.Lookup(defaultForegroundKey);
            foregroundBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
        }
        else
        {
            foregroundBrush = winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Colors::White() };
        }

        _newTabButton.Background(backgroundBrush);
        _newTabButton.Foreground(foregroundBrush);
    }

    // Method Description:
    // - Sets the tab split button color when a new tab color is selected
    // - This method could also set the color of the title bar and tab row
    // in the future
    // Arguments:
    // - selectedTabColor: The color of the newly selected tab
    // Return Value:
    // - <none>
    void TerminalPage::_SetNonClientAreaColors(const Windows::UI::Color& /*selectedTabColor*/)
    {
        // TODO GH#3327: Look at what to do with the NC area when we have XAML theming
    }

    // Method Description:
    // - Clears the tab split button color when the tab's color is cleared
    // - This method could also clear the color of the title bar and tab row
    // in the future
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_ClearNonClientAreaColors()
    {
        // TODO GH#3327: Look at what to do with the NC area when we have XAML theming
    }

    // Function Description:
    // - This is a helper method to get the commandline out of a
    //   ExecuteCommandline action, break it into subcommands, and attempt to
    //   parse it into actions. This is used by _HandleExecuteCommandline for
    //   processing commandlines in the current WT window.
    // Arguments:
    // - args: the ExecuteCommandlineArgs to synthesize a list of startup actions for.
    // Return Value:
    // - an empty list if we failed to parse, otherwise a list of actions to execute.
    std::vector<ActionAndArgs> TerminalPage::ConvertExecuteCommandlineToActions(const ExecuteCommandlineArgs& args)
    {
        ::TerminalApp::AppCommandlineArgs appArgs;
        if (appArgs.ParseArgs(args) == 0)
        {
            return appArgs.GetStartupActions();
        }

        return {};
    }

    void TerminalPage::_CommandPaletteClosed(const IInspectable& /*sender*/,
                                             const RoutedEventArgs& /*eventArgs*/)
    {
        // We don't want to set focus on the tab if fly-out is open as it will be closed
        // TODO GH#5400: consider checking we are not in the opening state, by hooking both Opening and Open events
        if (!_newTabButton.Flyout().IsOpen())
        {
            // Return focus to the active control
            if (auto index{ _GetFocusedTabIndex() })
            {
                _tabs.GetAt(*index).Focus(FocusState::Programmatic);
                _UpdateMRUTab(index.value());
            }
        }
    }

    bool TerminalPage::FocusMode() const
    {
        return _isInFocusMode;
    }

    bool TerminalPage::Fullscreen() const
    {
        return _isFullscreen;
    }

    // Method Description:
    // - Returns true if we're currently in "Always on top" mode. When we're in
    //   always on top mode, the window should be on top of all other windows.
    //   If multiple windows are all "always on top", they'll maintain their own
    //   z-order, with all the windows on top of all other non-topmost windows.
    // Arguments:
    // - <none>
    // Return Value:
    // - true if we should be in "always on top" mode
    bool TerminalPage::AlwaysOnTop() const
    {
        return _isAlwaysOnTop;
    }

    void TerminalPage::_OnNewConnection(winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection)
    {
        // TODO: GH 9458 will give us more context so we can try to choose a better profile.
        _OpenNewTab(nullptr, connection);
    }

    // Method Description:
    // - Updates all tabs with their current index in _tabs.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_UpdateTabIndices()
    {
        const uint32_t size = _tabs.Size();
        for (uint32_t i = 0; i < size; ++i)
        {
            auto tab{ _tabs.GetAt(i) };
            auto tabImpl{ winrt::get_self<TabBase>(tab) };
            tabImpl->UpdateTabViewIndex(i, size);
        }
    }

    // Method Description:
    // - Creates a settings UI tab and focuses it. If there's already a settings UI tab open,
    //   just focus the existing one.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TerminalPage::_OpenSettingsUI()
    {
        // If we're holding the settings tab's switch command, don't create a new one, switch to the existing one.
        if (!_settingsTab)
        {
            winrt::Microsoft::Terminal::Settings::Editor::MainPage sui{ _settings };
            if (_hostingHwnd)
            {
                sui.SetHostingWindow(reinterpret_cast<uint64_t>(*_hostingHwnd));
            }

            sui.PreviewKeyDown({ this, &TerminalPage::_SUIPreviewKeyDownHandler });

            sui.OpenJson([weakThis{ get_weak() }](auto&& /*s*/, winrt::Microsoft::Terminal::Settings::Model::SettingsTarget e) {
                if (auto page{ weakThis.get() })
                {
                    page->_LaunchSettings(e);
                }
            });

            auto newTabImpl = winrt::make_self<SettingsTab>(sui);

            // Add the new tab to the list of our tabs.
            _tabs.Append(*newTabImpl);
            _mruTabs.Append(*newTabImpl);

            newTabImpl->SetDispatch(*_actionDispatch);
            newTabImpl->SetKeyMap(_settings.KeyMap());

            // Give the tab its index in the _tabs vector so it can manage its own SwitchToTab command.
            _UpdateTabIndices();

            // Don't capture a strong ref to the tab. If the tab is removed as this
            // is called, we don't really care anymore about handling the event.
            auto weakTab = make_weak(newTabImpl);

            auto tabViewItem = newTabImpl->TabViewItem();
            _tabView.TabItems().Append(tabViewItem);

            tabViewItem.PointerPressed({ this, &TerminalPage::_OnTabClick });

            // When the tab is closed, remove it from our list of tabs.
            newTabImpl->Closed([tabViewItem, weakThis{ get_weak() }](auto&& /*s*/, auto&& /*e*/) {
                if (auto page{ weakThis.get() })
                {
                    page->_settingsTab = nullptr;
                    page->_RemoveOnCloseRoutine(tabViewItem, page);
                }
            });

            _settingsTab = *newTabImpl;

            // This kicks off TabView::SelectionChanged, in response to which
            // we'll attach the terminal's Xaml control to the Xaml root.
            _tabView.SelectedItem(tabViewItem);
        }
        else
        {
            _tabView.SelectedItem(_settingsTab.TabViewItem());
        }
    }

    // Method Description:
    // - Returns a com_ptr to the implementation type of the given tab if it's a TerminalTab.
    //   If the tab is not a TerminalTab, returns nullptr.
    // Arguments:
    // - tab: the projected type of a Tab
    // Return Value:
    // - If the tab is a TerminalTab, a com_ptr to the implementation type.
    //   If the tab is not a TerminalTab, nullptr
    winrt::com_ptr<TerminalTab> TerminalPage::_GetTerminalTabImpl(const TerminalApp::TabBase& tab)
    {
        if (auto terminalTab = tab.try_as<TerminalApp::TerminalTab>())
        {
            winrt::com_ptr<TerminalTab> tabImpl;
            tabImpl.copy_from(winrt::get_self<TerminalTab>(terminalTab));
            return tabImpl;
        }
        else
        {
            return nullptr;
        }
    }

    // Method Description:
    // - Computes the delta for scrolling the tab's viewport.
    // Arguments:
    // - scrollDirection - direction (up / down) to scroll
    // - rowsToScroll - the number of rows to scroll
    // Return Value:
    // - delta - Signed delta, where a negative value means scrolling up.
    int TerminalPage::_ComputeScrollDelta(ScrollDirection scrollDirection, const uint32_t rowsToScroll)
    {
        return scrollDirection == ScrollUp ? -1 * rowsToScroll : rowsToScroll;
    }

    // Method Description:
    // - Reads system settings for scrolling (based on the step of the mouse scroll).
    // Upon failure fallbacks to default.
    // Return Value:
    // - The number of rows to scroll or a magic value of WHEEL_PAGESCROLL
    // indicating that we need to scroll an entire view height
    uint32_t TerminalPage::_ReadSystemRowsToScroll()
    {
        uint32_t systemRowsToScroll;
        if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &systemRowsToScroll, 0))
        {
            LOG_LAST_ERROR();

            // If SystemParametersInfoW fails, which it shouldn't, fall back to
            // Windows' default value.
            return DefaultRowsToScroll;
        }

        return systemRowsToScroll;
    }

    // Method Description:
    // - Bumps the tab in its in-order index up to the top of the mru list.
    // Arguments:
    // - index: the in-order index of the tab to bump.
    // Return Value:
    // - <none>
    void TerminalPage::_UpdateMRUTab(const uint32_t index)
    {
        uint32_t mruIndex;
        const auto tab = _tabs.GetAt(index);
        if (_mruTabs.IndexOf(tab, mruIndex))
        {
            if (mruIndex > 0)
            {
                _mruTabs.RemoveAt(mruIndex);
                _mruTabs.InsertAt(0, tab);
            }
        }
    }

    // Method Description:
    // - Moves the tab to another index in the tabs row (if required).
    // Arguments:
    // - currentTabIndex: the current index of the tab to move
    // - suggestedNewTabIndex: the new index of the tab, might get clamped to fit int the tabs row boundaries
    // Return Value:
    // - <none>
    void TerminalPage::_TryMoveTab(const uint32_t currentTabIndex, const int32_t suggestedNewTabIndex)
    {
        auto newTabIndex = gsl::narrow_cast<uint32_t>(std::clamp<int32_t>(suggestedNewTabIndex, 0, _tabs.Size() - 1));
        if (currentTabIndex != newTabIndex)
        {
            auto tab = _tabs.GetAt(currentTabIndex);
            auto tabViewItem = tab.TabViewItem();
            _tabs.RemoveAt(currentTabIndex);
            _tabs.InsertAt(newTabIndex, tab);
            _UpdateTabIndices();

            _tabView.TabItems().RemoveAt(currentTabIndex);
            _tabView.TabItems().InsertAt(newTabIndex, tabViewItem);
            _tabView.SelectedItem(tabViewItem);
        }
    }

    // Method Description:
    // - Displays a dialog stating the "Touch Keyboard and Handwriting Panel
    //   Service" is disabled.
    void TerminalPage::ShowKeyboardServiceWarning()
    {
        if (auto keyboardWarningInfoBar = FindName(L"KeyboardWarningInfoBar").try_as<MUX::Controls::InfoBar>())
        {
            keyboardWarningInfoBar.IsOpen(true);
        }
    }

    // Function Description:
    // - Helper function to get the OS-localized name for the "Touch Keyboard
    //   and Handwriting Panel Service". If we can't open up the service for any
    //   reason, then we'll just return the service's key, "TabletInputService".
    // Return Value:
    // - The OS-localized name for the TabletInputService
    winrt::hstring _getTabletServiceName()
    {
        auto isUwp = false;
        try
        {
            isUwp = ::winrt::Windows::UI::Xaml::Application::Current().as<::winrt::TerminalApp::App>().Logic().IsUwp();
        }
        CATCH_LOG();

        if (isUwp)
        {
            return winrt::hstring{ TabletInputServiceKey };
        }

        wil::unique_schandle hManager{ OpenSCManager(nullptr, nullptr, 0) };

        if (LOG_LAST_ERROR_IF(!hManager.is_valid()))
        {
            return winrt::hstring{ TabletInputServiceKey };
        }

        DWORD cchBuffer = 0;
        GetServiceDisplayName(hManager.get(), TabletInputServiceKey.data(), nullptr, &cchBuffer);
        std::wstring buffer;
        cchBuffer += 1; // Add space for a null
        buffer.resize(cchBuffer);

        if (LOG_LAST_ERROR_IF(!GetServiceDisplayName(hManager.get(),
                                                     TabletInputServiceKey.data(),
                                                     buffer.data(),
                                                     &cchBuffer)))
        {
            return winrt::hstring{ TabletInputServiceKey };
        }
        return winrt::hstring{ buffer };
    }

    // Method Description:
    // - Return the fully-formed warning message for the
    //   "KeyboardServiceDisabled" InfoBar. This InfoBar is used to warn the user
    //   if the keyboard service is disabled, and uses the OS localization for
    //   the service's actual name. It's bound to the bar in XAML.
    // Return Value:
    // - The warning message, including the OS-localized service name.
    winrt::hstring TerminalPage::KeyboardServiceDisabledText()
    {
        const winrt::hstring serviceName{ _getTabletServiceName() };
        const winrt::hstring text{ fmt::format(std::wstring_view(RS_(L"KeyboardServiceWarningText")), serviceName) };
        return text;
    }

    // Method Description:
    // - Hides cursor if required
    // Return Value:
    // - <none>
    void TerminalPage::_HidePointerCursorHandler(const IInspectable& /*sender*/, const IInspectable& /*eventArgs*/)
    {
        if (_shouldMouseVanish && !_isMouseHidden)
        {
            if (auto window{ CoreWindow::GetForCurrentThread() })
            {
                try
                {
                    window.PointerCursor(nullptr);
                    _isMouseHidden = true;
                }
                CATCH_LOG();
            }
        }
    }

    // Method Description:
    // - Restores cursor if required
    // Return Value:
    // - <none>
    void TerminalPage::_RestorePointerCursorHandler(const IInspectable& /*sender*/, const IInspectable& /*eventArgs*/)
    {
        if (_isMouseHidden)
        {
            if (auto window{ CoreWindow::GetForCurrentThread() })
            {
                try
                {
                    window.PointerCursor(_defaultPointerCursor);
                    _isMouseHidden = false;
                }
                CATCH_LOG();
            }
        }
    }
}
