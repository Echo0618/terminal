// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TSFInputControl.h"
#include "TSFInputControl.g.cpp"

#include <Utils.h>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Text::Core;
using namespace winrt::Windows::UI::Xaml;

namespace winrt::Microsoft::Terminal::TerminalControl::implementation
{
    TSFInputControl::TSFInputControl() :
        _editContext{ nullptr },
        _inComposition{ false }
    {
        _Create();
    }

    // Method Description:
    // - Creates XAML controls for displaying user input and hooks up CoreTextEditContext handlers
    //   for handling text input from the Text Services Framework.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TSFInputControl::_Create()
    {
        // TextBlock for user input form TSF
        _textBlock = Controls::TextBlock();
        _textBlock.Visibility(Visibility::Collapsed);
        _textBlock.IsTextSelectionEnabled(false);
        _textBlock.TextDecorations(TextDecorations::Underline);

        // Canvas for controlling exact position of the TextBlock
        _canvas = Windows::UI::Xaml::Controls::Canvas();
        _canvas.Visibility(Visibility::Collapsed);

        // add the Textblock to the Canvas
        _canvas.Children().Append(_textBlock);

        // set the content of this control to be the Canvas
        this->Content(_canvas);

        // Create a CoreTextEditingContext for since we are acting like a custom edit control
        auto manager = Core::CoreTextServicesManager::GetForCurrentView();
        _editContext = manager.CreateEditContext();

        // sets the Input Pane display policy to Manual for now so that it can manually show the
        // software keyboard when the control gains focus and dismiss it when the control loses focus.
        // TODO GitHub #3639: Should Input Pane display policy be Automatic
        _editContext.InputPaneDisplayPolicy(Core::CoreTextInputPaneDisplayPolicy::Manual);

        // set the input scope to Text because this control is for any text.
        _editContext.InputScope(Core::CoreTextInputScope::Text);

        _textRequestedRevoker = _editContext.TextRequested(winrt::auto_revoke, { this, &TSFInputControl::_textRequestedHandler });

        _selectionRequestedRevoker = _editContext.SelectionRequested(winrt::auto_revoke, { this, &TSFInputControl::_selectionRequestedHandler });

        _focusRemovedRevoker = _editContext.FocusRemoved(winrt::auto_revoke, { this, &TSFInputControl::_focusRemovedHandler });

        _textUpdatingRevoker = _editContext.TextUpdating(winrt::auto_revoke, { this, &TSFInputControl::_textUpdatingHandler });

        _selectionUpdatingRevoker = _editContext.SelectionUpdating(winrt::auto_revoke, { this, &TSFInputControl::_selectionUpdatingHandler });

        _formatUpdatingRevoker = _editContext.FormatUpdating(winrt::auto_revoke, { this, &TSFInputControl::_formatUpdatingHandler });

        _layoutRequestedRevoker = _editContext.LayoutRequested(winrt::auto_revoke, { this, &TSFInputControl::_layoutRequestedHandler });

        _compositionStartedRevoker = _editContext.CompositionStarted(winrt::auto_revoke, { this, &TSFInputControl::_compositionStartedHandler });

        _compositionCompletedRevoker = _editContext.CompositionCompleted(winrt::auto_revoke, { this, &TSFInputControl::_compositionCompletedHandler });
    }

    // Method Description:
    // - Prepares this TSFInputControl to be removed from the UI hierarchy.
    void TSFInputControl::Close()
    {
        // Explicitly disconnect the LayoutRequested handler -- it can cause problems during application teardown.
        // See GH#4159 for more info.
        _layoutRequestedRevoker.revoke();
    }

    // Method Description:
    // - NotifyFocusEnter handler for notifying CoreEditTextContext of focus enter
    //   when TerminalControl receives focus.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TSFInputControl::NotifyFocusEnter()
    {
        if (_editContext != nullptr)
        {
            _editContext.NotifyFocusEnter();
        }
    }

    // Method Description:
    // - NotifyFocusEnter handler for notifying CoreEditTextContext of focus leaving.
    //   when TerminalControl no longer has focus.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TSFInputControl::NotifyFocusLeave()
    {
        if (_editContext != nullptr)
        {
            _editContext.NotifyFocusLeave();
        }
    }

    // Method Description:
    // - Handler for LayoutRequested event by CoreEditContext responsible
    //   for returning the current position the IME should be placed
    //   in screen coordinates on the screen.  TSFInputControls internal
    //   XAML controls (TextBlock/Canvas) are also positioned and updated.
    //   NOTE: documentation says application should handle this event
    // Arguments:
    // - sender: CoreTextEditContext sending the request.
    // - args: CoreTextLayoutRequestedEventArgs to be updated with position information.
    // Return Value:
    // - <none>
    void TSFInputControl::_layoutRequestedHandler(CoreTextEditContext sender, CoreTextLayoutRequestedEventArgs const& args)
    {
        auto request = args.Request();

        // Get window in screen coordinates, this is the entire window including tabs
        const auto windowBounds = CoreWindow::GetForCurrentThread().Bounds();

        // Get the cursor position in text buffer position
        auto cursorArgs = winrt::make_self<CursorPositionEventArgs>();
        _CurrentCursorPositionHandlers(*this, *cursorArgs);
        const COORD cursorPos = { ::base::ClampedNumeric<short>(cursorArgs->CurrentPosition().X), ::base::ClampedNumeric<short>(cursorArgs->CurrentPosition().Y) };

        // Get Font Info as we use this is the pixel size for characters in the display
        auto fontArgs = winrt::make_self<FontInfoEventArgs>();
        _CurrentFontInfoHandlers(*this, *fontArgs);

        const auto fontWidth = fontArgs->FontSize().Width;
        const auto fontHeight = fontArgs->FontSize().Height;

        // Convert text buffer cursor position to client coordinate position within the window
        COORD clientCursorPos;
        clientCursorPos.X = ::base::ClampMul(cursorPos.X, ::base::ClampedNumeric<short>(fontWidth));
        clientCursorPos.Y = ::base::ClampMul(cursorPos.Y, ::base::ClampedNumeric<short>(fontHeight));

        // Convert from client coordinate to screen coordinate by adding window position
        COORD screenCursorPos;
        screenCursorPos.X = ::base::ClampAdd(clientCursorPos.X, ::base::ClampedNumeric<short>(windowBounds.X));
        screenCursorPos.Y = ::base::ClampAdd(clientCursorPos.Y, ::base::ClampedNumeric<short>(windowBounds.Y));

        // get any offset (margin + tabs, etc..) of the control within the window
        const auto offsetPoint = this->TransformToVisual(nullptr).TransformPoint(winrt::Windows::Foundation::Point(0, 0));

        // add the margin offsets if any
        screenCursorPos.X = ::base::ClampAdd(screenCursorPos.X, ::base::ClampedNumeric<short>(offsetPoint.X));
        screenCursorPos.Y = ::base::ClampAdd(screenCursorPos.Y, ::base::ClampedNumeric<short>(offsetPoint.Y));

        // Get scale factor for view
        const double scaleFactor = DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();

        // Set the selection layout bounds
        Rect selectionRect = Rect(screenCursorPos.X, screenCursorPos.Y, 0, fontHeight);
        request.LayoutBounds().TextBounds(ScaleRect(selectionRect, scaleFactor));

        // Set the control bounds of the whole control
        Rect controlRect = Rect(screenCursorPos.X, screenCursorPos.Y, 0, fontHeight);
        request.LayoutBounds().ControlBounds(ScaleRect(controlRect, scaleFactor));

        // position textblock to cursor position
        _canvas.SetLeft(_textBlock, clientCursorPos.X);
        _canvas.SetTop(_textBlock, ::base::ClampedNumeric<double>(clientCursorPos.Y));

        _textBlock.Height(fontHeight);
        // calculate FontSize in pixels from DIPs
        const double fontSizePx = (fontHeight * 72) / USER_DEFAULT_SCREEN_DPI;
        _textBlock.FontSize(fontSizePx);

        _textBlock.FontFamily(Media::FontFamily(fontArgs->FontFace()));
    }

    // Method Description:
    // - Handler for CompositionStarted event by CoreEditContext responsible
    //   for making internal TSFInputControl controls visible.
    // Arguments:
    // - sender: CoreTextEditContext sending the request. Not used in method.
    // - args: CoreTextCompositionStartedEventArgs. Not used in method.
    // Return Value:
    // - <none>
    void TSFInputControl::_compositionStartedHandler(CoreTextEditContext sender, CoreTextCompositionStartedEventArgs const& /*args*/)
    {
        _inComposition = true;
    }

    // Method Description:
    // - Handler for CompositionCompleted event by CoreEditContext responsible
    //   for making internal TSFInputControl controls visible.
    // Arguments:
    // - sender: CoreTextEditContext sending the request. Not used in method.
    // - args: CoreTextCompositionCompletedEventArgs. Not used in method.
    // Return Value:
    // - <none>
    void TSFInputControl::_compositionCompletedHandler(CoreTextEditContext sender, CoreTextCompositionCompletedEventArgs const& /*args*/)
    {
        _inComposition = false;

        // only need to do work if the current buffer has text
        if (!_inputBuffer.empty())
        {
            _SendAndClearText();
        }
    }

    // Method Description:
    // - Handler for FocusRemoved event by CoreEditContext responsible
    //   for removing focus for the TSFInputControl control accordingly
    //   when focus was forcibly removed from text input control.
    //   NOTE: Documentation says application should handle this event
    // Arguments:
    // - sender: CoreTextEditContext sending the request. Not used in method.
    // - object: CoreTextCompositionStartedEventArgs. Not used in method.
    // Return Value:
    // - <none>
    void TSFInputControl::_focusRemovedHandler(CoreTextEditContext sender, winrt::Windows::Foundation::IInspectable const& /*object*/)
    {
    }

    // Method Description:
    // - Handler for TextRequested event by CoreEditContext responsible
    //   for returning the range of text requested.
    //   NOTE: Documentation says application should handle this event
    // Arguments:
    // - sender: CoreTextEditContext sending the request. Not used in method.
    // - args: CoreTextTextRequestedEventArgs to be updated with requested range text.
    // Return Value:
    // - <none>
    void TSFInputControl::_textRequestedHandler(CoreTextEditContext sender, CoreTextTextRequestedEventArgs const& args)
    {
        // the range the TSF wants to know about
        const auto range = args.Request().Range();

        try
        {
            const auto textEnd = ::base::ClampMin<size_t>(range.EndCaretPosition, _inputBuffer.length());
            const auto length = ::base::ClampSub<size_t>(textEnd, range.StartCaretPosition);
            const auto textRequested = _inputBuffer.substr(range.StartCaretPosition, length);

            args.Request().Text(textRequested);
        }
        CATCH_LOG();
    }

    // Method Description:
    // - Handler for SelectionRequested event by CoreEditContext responsible
    //   for returning the currently selected text.
    //   TSFInputControl currently doesn't allow selection, so nothing happens.
    //   NOTE: Documentation says application should handle this event
    // Arguments:
    // - sender: CoreTextEditContext sending the request. Not used in method.
    // - args: CoreTextSelectionRequestedEventArgs for providing data for the SelectionRequested event. Not used in method.
    // Return Value:
    // - <none>
    void TSFInputControl::_selectionRequestedHandler(CoreTextEditContext sender, CoreTextSelectionRequestedEventArgs const& /*args*/)
    {
    }

    // Method Description:
    // - Handler for SelectionUpdating event by CoreEditContext responsible
    //   for handling modifications to the range of text currently selected.
    //   TSFInputControl doesn't currently allow selection, so nothing happens.
    //   NOTE: Documentation says application should set its selection range accordingly
    // Arguments:
    // - sender: CoreTextEditContext sending the request. Not used in method.
    // - args: CoreTextSelectionUpdatingEventArgs for providing data for the SelectionUpdating event. Not used in method.
    // Return Value:
    // - <none>
    void TSFInputControl::_selectionUpdatingHandler(CoreTextEditContext sender, CoreTextSelectionUpdatingEventArgs const& /*args*/)
    {
    }

    // Method Description:
    // - Handler for TextUpdating event by CoreEditContext responsible
    //   for handling text updates.
    // Arguments:
    // - sender: CoreTextEditContext sending the request. Not used in method.
    // - args: CoreTextTextUpdatingEventArgs contains new text to update buffer with.
    // Return Value:
    // - <none>
    void TSFInputControl::_textUpdatingHandler(CoreTextEditContext sender, CoreTextTextUpdatingEventArgs const& args)
    {
        const auto text = args.Text();
        const auto range = args.Range();

        try
        {
            _canvas.Visibility(Visibility::Visible);
            _textBlock.Visibility(Visibility::Visible);

            const auto length = ::base::ClampSub<size_t>(range.EndCaretPosition, range.StartCaretPosition);
            _inputBuffer = _inputBuffer.replace(
                range.StartCaretPosition,
                length,
                text);

            _textBlock.Text(_inputBuffer);

            // If we receive tabbed IME input like emoji, kaomojis, and symbols, send it to the terminal immediately.
            // They aren't composition, so we don't want to wait for the user to start and finish a composition to send the text.
            if (!_inComposition)
            {
                _SendAndClearText();
            }

            // Notify the TSF that the update succeeded
            args.Result(CoreTextTextUpdatingResult::Succeeded);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();

            // indicate updating failed.
            args.Result(CoreTextTextUpdatingResult::Failed);
        }
    }

    // Method Description:
    // - Sends the currently held text in the input buffer to the parent and
    //   clears the input buffer and text block for the next round of input.
    //   Then hides the text block control until the next time text received.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TSFInputControl::_SendAndClearText()
    {
        // call event handler with data handled by parent
        _compositionCompletedHandlers(_inputBuffer);

        // clear the buffer for next round
        const auto bufferLength = ::base::ClampedNumeric<int32_t>(_inputBuffer.length());
        _inputBuffer.clear();
        _textBlock.Text(L"");

        // Leaving focus before NotifyTextChanged seems to guarantee that the next
        // composition will send us a CompositionStarted event.
        _editContext.NotifyFocusLeave();
        _editContext.NotifyTextChanged({ 0, bufferLength }, 0, { 0, 0 });
        _editContext.NotifyFocusEnter();

        // hide the controls until text input starts again
        _canvas.Visibility(Visibility::Collapsed);
        _textBlock.Visibility(Visibility::Collapsed);
    }

    // Method Description:
    // - Handler for FormatUpdating event by CoreEditContext responsible
    //   for handling different format updates for a particular range of text.
    //   TSFInputControl doesn't do anything with this event.
    // Arguments:
    // - sender: CoreTextEditContext sending the request. Not used in method.
    // - args: CoreTextFormatUpdatingEventArgs Provides data for the FormatUpdating event. Not used in method.
    // Return Value:
    // - <none>
    void TSFInputControl::_formatUpdatingHandler(CoreTextEditContext sender, CoreTextFormatUpdatingEventArgs const& /*args*/)
    {
    }

    DEFINE_EVENT(TSFInputControl, CompositionCompleted, _compositionCompletedHandlers, TerminalControl::CompositionCompletedEventArgs);
}
