/*
 * Copyright (C) 2014 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef MenuUtilities_h
#define MenuUtilities_h

#import <WebCore/ContextMenuItem.h>
#import <WebCore/IntRect.h>
#import <wtf/text/WTFString.h>

namespace WebKit {

#if PLATFORM(MAC)

#if ENABLE(TELEPHONE_NUMBER_DETECTION)
NSMenuItem *menuItemForTelephoneNumber(const String& telephoneNumber);
RetainPtr<NSMenu> menuForTelephoneNumber(const String& telephoneNumber, NSView *webView, const WebCore::IntRect&);
NSString *menuItemTitleForTelephoneNumberGroup();
#endif

#if ENABLE(CONTEXT_MENU_IMAGES_ON_MAC)
void addImageToMenuItem(NSMenuItem*, const WebCore::ContextMenuAction, bool);
#endif

#endif

} // namespace WebKit

#endif // MenuUtilities_h
