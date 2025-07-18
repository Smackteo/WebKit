/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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

#import "config.h"
#import "ParentalControlsURLFilter.h"

#if HAVE(WEBCONTENTRESTRICTIONS)

#import "Logging.h"
#import <wtf/CompletionHandler.h>
#import <wtf/MainThread.h>
#import <wtf/URL.h>
#import <wtf/cocoa/VectorCocoa.h>

#import <pal/cocoa/WebContentRestrictionsSoftLink.h>

namespace WebCore {

#if HAVE(WEBCONTENTRESTRICTIONS_PATH_SPI)

static bool wcrBrowserEngineClientEnabled(const String& path)
{
    if (!path.isEmpty())
        return [PAL::getWCRBrowserEngineClientClass() shouldEvaluateURLsForConfigurationAtPath:path.createNSString().get()];

    return [PAL::getWCRBrowserEngineClientClass() shouldEvaluateURLs];
}

static HashMap<String, UniqueRef<ParentalControlsURLFilter>>& allFiltersWithConfigurationPath()
{
    static MainThreadNeverDestroyed<HashMap<String, UniqueRef<ParentalControlsURLFilter>>> map;
    return map;
}

ParentalControlsURLFilter& ParentalControlsURLFilter::filterWithConfigurationPath(const String& path)
{
    // Coalesce null string into the empty string.
    String key = path.isEmpty() ? emptyString() : path;

    auto& map = allFiltersWithConfigurationPath();
    auto iterator = map.find(key);
    if (iterator != map.end())
        return iterator->value;

    return map.set(key, UniqueRef(*new ParentalControlsURLFilter(key))).iterator->value;
}

ParentalControlsURLFilter::ParentalControlsURLFilter(const String& configurationPath)
    : m_configurationPath(configurationPath)
{
}

#else

static bool wcrBrowserEngineClientEnabled()
{
    return [PAL::getWCRBrowserEngineClientClass() shouldEvaluateURLs];
}

ParentalControlsURLFilter& ParentalControlsURLFilter::singleton()
{
    static MainThreadNeverDestroyed<UniqueRef<ParentalControlsURLFilter>> filter = UniqueRef(*new ParentalControlsURLFilter);
    return filter.get();
}

ParentalControlsURLFilter::ParentalControlsURLFilter() = default;

#endif

static void webContentFilterTypeDidChange(CFNotificationCenterRef, void*, CFStringRef, const void*, CFDictionaryRef)
{
#if HAVE(WEBCONTENTRESTRICTIONS_PATH_SPI)
    for (auto& filter : allFiltersWithConfigurationPath().values())
        filter->resetIsEnabled();
#else
    ParentalControlsURLFilter::singleton().resetIsEnabled();
#endif
}

static void registerNotificationForWebContentFilterTypeChange()
{
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), nullptr, &webContentFilterTypeDidChange, CFSTR("com.apple.ManagedConfiguration.webContentFilterTypeChanged"), nullptr, CFNotificationSuspensionBehaviorCoalesce);
    });
}

void ParentalControlsURLFilter::resetIsEnabled()
{
    m_isEnabled = std::nullopt;
}

bool ParentalControlsURLFilter::isEnabled() const
{
    if (!m_isEnabled) {
#if HAVE(WEBCONTENTRESTRICTIONS_PATH_SPI)
        m_isEnabled = wcrBrowserEngineClientEnabled(m_configurationPath);
#else
        m_isEnabled = wcrBrowserEngineClientEnabled();
#endif
        RELEASE_LOG(ContentFiltering, "%p - ParentalControlsURLFilter::isEnabled %d", this, *m_isEnabled);
    }

    registerNotificationForWebContentFilterTypeChange();

    return *m_isEnabled;
}

void ParentalControlsURLFilter::isURLAllowedWithQueue(const URL& url, CompletionHandler<void(bool, NSData *)>&& completionHandler, WTF::WorkQueue& completionHandlerQueue)
{
    ASSERT(isMainThread());

    RetainPtr wcrBrowserEngineClient = effectiveWCRBrowserEngineClient();
    if (!wcrBrowserEngineClient) {
        completionHandlerQueue.dispatch([completionHandler = WTFMove(completionHandler)]() mutable {
            completionHandler(true, nullptr);
        });
        return;
    }

    [wcrBrowserEngineClient evaluateURL:url.createNSURL().get() withCompletion:makeBlockPtr([url = url.isolatedCopy(), completionHandler = WTFMove(completionHandler)](BOOL shouldBlock, NSData *replacementData) mutable {
        completionHandler(!shouldBlock, replacementData);
    }).get() onCompletionQueue:completionHandlerQueue.dispatchQueue()];
}

void ParentalControlsURLFilter::allowURL(const URL& url, CompletionHandler<void(bool)>&& completionHandler)
{
    ASSERT(isMainThread());

    RetainPtr wcrBrowserEngineClient = effectiveWCRBrowserEngineClient();
    if (!wcrBrowserEngineClient)
        return completionHandler(true);

    [wcrBrowserEngineClient allowURL:url.createNSURL().get() withCompletion:makeBlockPtr([completionHandler = WTFMove(completionHandler)](BOOL didAllow, NSError *) mutable {
        ASSERT(isMainThread());
        completionHandler(didAllow);
    }).get()];
}

WCRBrowserEngineClient* ParentalControlsURLFilter::effectiveWCRBrowserEngineClient()
{
    if (!isEnabled())
        return nullptr;

#if HAVE(WEBCONTENTRESTRICTIONS_PATH_SPI)
    if (!m_wcrBrowserEngineClient && !m_configurationPath.isEmpty())
        lazyInitialize(m_wcrBrowserEngineClient, adoptNS([PAL::allocWCRBrowserEngineClientInstance() initWithConfigurationAtPath:m_configurationPath.createNSString().get()]));
#endif

    if (!m_wcrBrowserEngineClient)
        lazyInitialize(m_wcrBrowserEngineClient, adoptNS([PAL::allocWCRBrowserEngineClientInstance() init]));

    return m_wcrBrowserEngineClient.get();
}

} // namespace WebCore

#endif // HAVE(WEBCONTENTRESTRICTIONS)
