/*
 * Copyright (C) 2006-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2008 Nokia Corporation and/or its subsidiary(-ies)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer. 
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution. 
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#import "config.h"
#import "WebEditorClient.h"

#if PLATFORM(MAC)

#import "MessageSenderInlines.h"
#import "TextCheckerState.h"
#import "WebPage.h"
#import "WebPageProxyMessages.h"
#import "WebProcess.h"
#import <WebCore/Editor.h>
#import <WebCore/FocusController.h>
#import <WebCore/KeyboardEvent.h>
#import <WebCore/LocalFrame.h>
#import <WebCore/NotImplemented.h>
#import <WebCore/Page.h>
#import <wtf/cocoa/NSURLExtras.h>

namespace WebKit {
using namespace WebCore;

void WebEditorClient::handleKeyboardEvent(KeyboardEvent& event)
{
    if (m_page->handleEditingKeyboardEvent(event))
        event.setDefaultHandled();
}

void WebEditorClient::handleInputMethodKeydown(KeyboardEvent& event)
{
    if (event.handledByInputMethod())
        event.setDefaultHandled();
}

void WebEditorClient::didDispatchInputMethodKeydown(KeyboardEvent& event)
{
    m_page->handleEditingKeyboardEvent(event);
}

void WebEditorClient::setInsertionPasteboard(const String&)
{
    // This is used only by Mail, no need to implement it now.
    notImplemented();
}

static void changeWordCase(WebPage* page, NSString *(*changeCase)(NSString *))
{
    RefPtr frame = page->corePage()->focusController().focusedOrMainFrame();
    if (!frame)
        return;
    if (!frame->editor().canEdit())
        return;

    frame->editor().command("selectWord"_s).execute();

    RetainPtr selectedString = frame->displayStringModifiedByEncoding(frame->editor().selectedText()).createNSString();
    page->replaceSelectionWithText(frame.get(), changeCase(selectedString.get()));
}

void WebEditorClient::uppercaseWord()
{
    changeWordCase(RefPtr { m_page.get() }.get(), [] (NSString *string) {
        return [string uppercaseString];
    });
}

void WebEditorClient::lowercaseWord()
{
    changeWordCase(RefPtr { m_page.get() }.get(), [] (NSString *string) {
        return [string lowercaseString];
    });
}

void WebEditorClient::capitalizeWord()
{
    changeWordCase(RefPtr { m_page.get() }.get(), [] (NSString *string) {
        return [string capitalizedString];
    });
}

#if USE(AUTOMATIC_TEXT_REPLACEMENT)

void WebEditorClient::showSubstitutionsPanel(bool)
{
    notImplemented();
}

bool WebEditorClient::substitutionsPanelIsShowing()
{
    auto sendResult = Ref { *m_page }->sendSync(Messages::WebPageProxy::SubstitutionsPanelIsShowing());
    auto [isShowing] = sendResult.takeReplyOr(false);
    return isShowing;
}

void WebEditorClient::toggleSmartInsertDelete()
{
    Ref { *m_page }->send(Messages::WebPageProxy::toggleSmartInsertDelete());
}

bool WebEditorClient::isAutomaticQuoteSubstitutionEnabled()
{
    if (Ref { *m_page }->isControlledByAutomation())
        return false;

    return WebProcess::singleton().textCheckerState().contains(TextCheckerState::AutomaticQuoteSubstitutionEnabled);
}

void WebEditorClient::toggleAutomaticQuoteSubstitution()
{
    Ref { *m_page }->send(Messages::WebPageProxy::toggleAutomaticQuoteSubstitution());
}

bool WebEditorClient::isAutomaticLinkDetectionEnabled()
{
    return WebProcess::singleton().textCheckerState().contains(TextCheckerState::AutomaticLinkDetectionEnabled);
}

void WebEditorClient::toggleAutomaticLinkDetection()
{
    Ref { *m_page }->send(Messages::WebPageProxy::toggleAutomaticLinkDetection());
}

bool WebEditorClient::isAutomaticDashSubstitutionEnabled()
{
    if (m_page->isControlledByAutomation())
        return false;

    return WebProcess::singleton().textCheckerState().contains(TextCheckerState::AutomaticDashSubstitutionEnabled);
}

void WebEditorClient::toggleAutomaticDashSubstitution()
{
    Ref { *m_page }->send(Messages::WebPageProxy::toggleAutomaticDashSubstitution());
}

bool WebEditorClient::isAutomaticTextReplacementEnabled()
{
    if (m_page->isControlledByAutomation())
        return false;

    return WebProcess::singleton().textCheckerState().contains(TextCheckerState::AutomaticTextReplacementEnabled);
}

void WebEditorClient::toggleAutomaticTextReplacement()
{
    Ref { *m_page }->send(Messages::WebPageProxy::toggleAutomaticTextReplacement());
}

bool WebEditorClient::isAutomaticSpellingCorrectionEnabled()
{
    if (m_page->isControlledByAutomation())
        return false;

    return WebProcess::singleton().textCheckerState().contains(TextCheckerState::AutomaticSpellingCorrectionEnabled);
}

void WebEditorClient::toggleAutomaticSpellingCorrection()
{
    notImplemented();
}

#endif // USE(AUTOMATIC_TEXT_REPLACEMENT)

} // namespace WebKit

#endif // PLATFORM(MAC)
