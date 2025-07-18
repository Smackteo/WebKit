/*
 * Copyright (C) 2004-2016 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#import "DOMHTMLOptionElementInternal.h"

#import "DOMHTMLFormElementInternal.h"
#import "DOMNodeInternal.h"
#import "ExceptionHandlers.h"
#import <WebCore/ElementInlines.h>
#import <WebCore/HTMLFormElement.h>
#import <WebCore/HTMLNames.h>
#import <WebCore/HTMLOptionElement.h>
#import <WebCore/JSExecState.h>
#import <WebCore/ThreadCheck.h>
#import <WebCore/WebScriptObjectPrivate.h>
#import <wtf/GetPtr.h>
#import <wtf/URL.h>

#define IMPL static_cast<WebCore::HTMLOptionElement*>(reinterpret_cast<WebCore::Node*>(_internal))

@implementation DOMHTMLOptionElement

- (BOOL)disabled
{
    WebCore::JSMainThreadNullState state;
    return IMPL->hasAttributeWithoutSynchronization(WebCore::HTMLNames::disabledAttr);
}

- (void)setDisabled:(BOOL)newDisabled
{
    WebCore::JSMainThreadNullState state;
    IMPL->setBooleanAttribute(WebCore::HTMLNames::disabledAttr, newDisabled);
}

- (DOMHTMLFormElement *)form
{
    WebCore::JSMainThreadNullState state;
    return kit(WTF::getPtr(IMPL->form()));
}

- (NSString *)label
{
    WebCore::JSMainThreadNullState state;
    return IMPL->label().createNSString().autorelease();
}

- (void)setLabel:(NSString *)newLabel
{
    WebCore::JSMainThreadNullState state;
    IMPL->setAttributeWithoutSynchronization(WebCore::HTMLNames::labelAttr, newLabel);
}

- (BOOL)defaultSelected
{
    WebCore::JSMainThreadNullState state;
    return IMPL->hasAttributeWithoutSynchronization(WebCore::HTMLNames::selectedAttr);
}

- (void)setDefaultSelected:(BOOL)newDefaultSelected
{
    WebCore::JSMainThreadNullState state;
    IMPL->setBooleanAttribute(WebCore::HTMLNames::selectedAttr, newDefaultSelected);
}

- (BOOL)selected
{
    WebCore::JSMainThreadNullState state;
    return IMPL->selected();
}

- (void)setSelected:(BOOL)newSelected
{
    WebCore::JSMainThreadNullState state;
    IMPL->setSelected(newSelected);
}

- (NSString *)value
{
    WebCore::JSMainThreadNullState state;
    return IMPL->value().createNSString().autorelease();
}

- (void)setValue:(NSString *)newValue
{
    WebCore::JSMainThreadNullState state;
    IMPL->setAttributeWithoutSynchronization(WebCore::HTMLNames::valueAttr, newValue);
}

- (NSString *)text
{
    WebCore::JSMainThreadNullState state;
    return IMPL->text().createNSString().autorelease();
}

- (int)index
{
    WebCore::JSMainThreadNullState state;
    return IMPL->index();
}

@end

WebCore::HTMLOptionElement* core(DOMHTMLOptionElement *wrapper)
{
    return wrapper ? reinterpret_cast<WebCore::HTMLOptionElement*>(wrapper->_internal) : 0;
}

DOMHTMLOptionElement *kit(WebCore::HTMLOptionElement* value)
{
    WebCoreThreadViolationCheckRoundOne();
    return static_cast<DOMHTMLOptionElement*>(kit(static_cast<WebCore::Node*>(value)));
}

#undef IMPL
