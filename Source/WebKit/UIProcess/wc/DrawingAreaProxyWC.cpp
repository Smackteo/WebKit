/*
 * Copyright (C) 2011-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2013 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2016-2019 Igalia S.L.
 * Copyright (C) 2021 Sony Interactive Entertainment Inc.
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

#include "config.h"
#include "DrawingAreaProxyWC.h"

#if USE(GRAPHICS_LAYER_WC)

#include "DrawingAreaMessages.h"
#include "MessageSenderInlines.h"
#include "UpdateInfo.h"
#include "WebPageProxy.h"
#include <WebCore/Region.h>

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DrawingAreaProxyWC);

Ref<DrawingAreaProxyWC> DrawingAreaProxyWC::create(WebPageProxy& page, WebProcessProxy& webProcessProxy)
{
    return adoptRef(*new DrawingAreaProxyWC(page, webProcessProxy));
}

DrawingAreaProxyWC::DrawingAreaProxyWC(WebPageProxy& webPageProxy, WebProcessProxy& webProcessProxy)
    : DrawingAreaProxy(webPageProxy, webProcessProxy)
{
}

void DrawingAreaProxyWC::paint(PlatformPaintContextPtr context, const WebCore::IntRect& rect, WebCore::Region& unpaintedRegion)
{
    unpaintedRegion = rect;

    if (!m_backingStore)
        return;
    m_backingStore->paint(context, rect);
    unpaintedRegion.subtract(WebCore::IntRect({ }, m_backingStore->size()));
}

void DrawingAreaProxyWC::deviceScaleFactorDidChange(CompletionHandler<void()>&& completionHandler)
{
    sizeDidChange();
    completionHandler();
}

void DrawingAreaProxyWC::sizeDidChange()
{
    discardBackingStore();
    m_currentBackingStoreStateID++;
    if (page())
        send(Messages::DrawingArea::UpdateGeometryWC(m_currentBackingStoreStateID, size(), page()->deviceScaleFactor(), page()->intrinsicDeviceScaleFactor()));
}

void DrawingAreaProxyWC::update(uint64_t backingStoreStateID, UpdateInfo&& updateInfo)
{
    if (backingStoreStateID == m_currentBackingStoreStateID)
        incorporateUpdate(WTFMove(updateInfo));
    send(Messages::DrawingArea::DisplayDidRefresh());
}

void DrawingAreaProxyWC::enterAcceleratedCompositingMode(uint64_t backingStoreStateID, const LayerTreeContext&)
{
    discardBackingStore();
}

void DrawingAreaProxyWC::incorporateUpdate(UpdateInfo&& updateInfo)
{
    if (updateInfo.updateRectBounds.isEmpty())
        return;

    if (!m_backingStore)
        m_backingStore.emplace(updateInfo.viewSize, updateInfo.deviceScaleFactor);

    RefPtr page = this->page();
    if (!page)
        return;

    WebCore::Region damageRegion;
    if (updateInfo.scrollRect.isEmpty()) {
        for (const auto& rect : updateInfo.updateRects)
            damageRegion.unite(rect);
    } else
        damageRegion = WebCore::IntRect({ }, page->viewSize());

    m_backingStore->incorporateUpdate(WTFMove(updateInfo));

    page->setViewNeedsDisplay(damageRegion);
}

void DrawingAreaProxyWC::discardBackingStore()
{
    m_backingStore = std::nullopt;
}

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)
