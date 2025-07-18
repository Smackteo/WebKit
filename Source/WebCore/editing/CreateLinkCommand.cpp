/*
 * Copyright (C) 2006-2025 Apple Inc. All rights reserved.
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

#include "config.h"
#include "CreateLinkCommand.h"

#include "Editing.h"
#include "HTMLAnchorElement.h"
#include "Text.h"

namespace WebCore {

CreateLinkCommand::CreateLinkCommand(Ref<Document>&& document, const String& url)
    : CompositeEditCommand(WTFMove(document))
    , m_url(url)
{
}

void CreateLinkCommand::doApply()
{
    if (endingSelection().isNoneOrOrphaned())
        return;

    Ref document = this->document();
    Ref anchorElement = HTMLAnchorElement::create(document);
    anchorElement->setAttributeWithoutSynchronization(HTMLNames::hrefAttr, AtomString { m_url });

    if (endingSelection().isRange())
        applyStyledElement(WTFMove(anchorElement));
    else {
        insertNodeAt(anchorElement.copyRef(), endingSelection().start());
        appendNode(Text::create(document, String { m_url }), anchorElement.copyRef());
        setEndingSelection(VisibleSelection(positionInParentBeforeNode(anchorElement.ptr()), positionInParentAfterNode(anchorElement.ptr()), Affinity::Downstream, endingSelection().directionality()));
    }
}

}
