/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
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

#pragma once

#include "APIObject.h"
#include "APIWebsitePolicies.h"
#include "FrameInfoData.h"
#include "NavigationActionData.h"
#include "ProcessThrottler.h"
#include "WebBackForwardListItem.h"
#include "WebContentMode.h"
#include <WebCore/AdvancedPrivacyProtections.h>
#include <WebCore/NavigationIdentifier.h>
#include <WebCore/PrivateClickMeasurement.h>
#include <WebCore/ProcessIdentifier.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/SubstituteData.h>
#include <wtf/ListHashSet.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Ref.h>

namespace WebCore {
enum class FrameLoadType : uint8_t;
class ResourceResponse;
}

namespace WebKit {
class WebBackForwardListFrameItem;
class BrowsingWarning;
}

namespace API {

struct SubstituteData {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(SubstituteData);

    SubstituteData(Vector<uint8_t>&& content, const WTF::String& MIMEType, const WTF::String& encoding, const WTF::String& baseURL, API::Object* userData, WebCore::SubstituteData::SessionHistoryVisibility sessionHistoryVisibility = WebCore::SubstituteData::SessionHistoryVisibility::Hidden)
        : content(WTFMove(content))
        , MIMEType(MIMEType)
        , encoding(encoding)
        , baseURL(baseURL)
        , userData(userData)
        , sessionHistoryVisibility(sessionHistoryVisibility)
    { }

    SubstituteData(Vector<uint8_t>&& content, const WebCore::ResourceResponse&, WebCore::SubstituteData::SessionHistoryVisibility);

    Vector<uint8_t> content;
    WTF::String MIMEType;
    WTF::String encoding;
    WTF::String baseURL;
    RefPtr<API::Object> userData;
    WebCore::SubstituteData::SessionHistoryVisibility sessionHistoryVisibility { WebCore::SubstituteData::SessionHistoryVisibility::Hidden };
};

class Navigation : public ObjectImpl<Object::Type::Navigation> {
    WTF_MAKE_NONCOPYABLE(Navigation);
public:
    static Ref<Navigation> create(WebCore::ProcessIdentifier processID, RefPtr<WebKit::WebBackForwardListItem>&& currentAndTargetItem)
    {
        return adoptRef(*new Navigation(processID, WTFMove(currentAndTargetItem)));
    }

    static Ref<Navigation> create(WebCore::ProcessIdentifier processID, Ref<WebKit::WebBackForwardListFrameItem>&& targetFrameItem, RefPtr<WebKit::WebBackForwardListItem>&& fromItem, WebCore::FrameLoadType backForwardFrameLoadType)
    {
        return adoptRef(*new Navigation(processID, WTFMove(targetFrameItem), WTFMove(fromItem), backForwardFrameLoadType));
    }

    static Ref<Navigation> create(WebCore::ProcessIdentifier processID, WebCore::ResourceRequest&& request, RefPtr<WebKit::WebBackForwardListItem>&& fromItem)
    {
        return adoptRef(*new Navigation(processID, WTFMove(request), WTFMove(fromItem)));
    }

    static Ref<Navigation> create(WebCore::ProcessIdentifier processID, std::unique_ptr<SubstituteData>&& substituteData)
    {
        return adoptRef(*new Navigation(processID, WTFMove(substituteData)));
    }

    static Ref<Navigation> create(WebCore::ProcessIdentifier processID, WebCore::ResourceRequest&& simulatedRequest, std::unique_ptr<SubstituteData>&& substituteData, RefPtr<WebKit::WebBackForwardListItem>&& fromItem)
    {
        return adoptRef(*new Navigation(processID, WTFMove(simulatedRequest), WTFMove(substituteData), WTFMove(fromItem)));
    }

    virtual ~Navigation();

    WebCore::NavigationIdentifier navigationID() const { return m_navigationID; }

    const WebCore::ResourceRequest& originalRequest() const { return m_originalRequest; }
    void setCurrentRequest(WebCore::ResourceRequest&&, WebCore::ProcessIdentifier);
    const WebCore::ResourceRequest& currentRequest() const { return m_currentRequest; }
    std::optional<WebCore::ProcessIdentifier> currentRequestProcessIdentifier() const { return m_currentRequestProcessIdentifier; }

    bool currentRequestIsRedirect() const { return m_lastNavigationAction && !m_lastNavigationAction->redirectResponse.isNull(); }
    bool currentRequestIsCrossSiteRedirect() const;

    WebKit::WebBackForwardListItem* targetItem() const;
    RefPtr<WebKit::WebBackForwardListItem> protectedTargetItem() const { return targetItem(); }
    WebKit::WebBackForwardListFrameItem* targetFrameItem() const { return m_targetFrameItem.get(); }
    WebKit::WebBackForwardListItem* fromItem() const { return m_fromItem.get(); }
    std::optional<WebCore::FrameLoadType> backForwardFrameLoadType() const { return m_backForwardFrameLoadType; }
    WebKit::WebBackForwardListItem* reloadItem() const { return m_reloadItem.get(); }

    void appendRedirectionURL(const WTF::URL&);
    Vector<WTF::URL> takeRedirectChain() { return WTFMove(m_redirectChain); }
    size_t redirectChainIndex(const WTF::URL&);

    bool wasUserInitiated() const { return m_lastNavigationAction && !!m_lastNavigationAction->userGestureTokenIdentifier; }
    bool isRequestFromClientOrUserInput() const;
    void markRequestAsFromClientInput();
    void markAsFromLoadData() { m_isFromLoadData = true; }
    bool isFromLoadData() const { return m_isFromLoadData; }

    bool shouldPerformDownload() const { return m_lastNavigationAction && !m_lastNavigationAction->downloadAttribute.isNull(); }

    bool treatAsSameOriginNavigation() const { return m_lastNavigationAction && m_lastNavigationAction->treatAsSameOriginNavigation; }
    bool hasOpenedFrames() const { return m_lastNavigationAction && m_lastNavigationAction->hasOpenedFrames; }
    bool openedByDOMWithOpener() const { return m_lastNavigationAction && m_lastNavigationAction->openedByDOMWithOpener; }
    bool isInitialFrameSrcLoad() const { return m_lastNavigationAction && m_lastNavigationAction->isInitialFrameSrcLoad; }
    WebCore::SecurityOriginData requesterOrigin() const { return m_lastNavigationAction ? m_lastNavigationAction->requesterOrigin : WebCore::SecurityOriginData { }; }
    WebCore::ShouldOpenExternalURLsPolicy shouldOpenExternalURLsPolicy() const { return m_lastNavigationAction ? m_lastNavigationAction->shouldOpenExternalURLsPolicy : WebCore::ShouldOpenExternalURLsPolicy::ShouldNotAllow; }

    void setUserContentExtensionsEnabled(bool enabled) { m_userContentExtensionsEnabled = enabled; }
    bool userContentExtensionsEnabled() const { return m_userContentExtensionsEnabled; }

    WebCore::LockHistory lockHistory() const { return m_lastNavigationAction ? m_lastNavigationAction->lockHistory : WebCore::LockHistory::No; }
    WebCore::LockBackForwardList lockBackForwardList() const { return m_lastNavigationAction ? m_lastNavigationAction->lockBackForwardList : WebCore::LockBackForwardList::No; }

    WTF::String clientRedirectSourceForHistory() const { return m_lastNavigationAction ? m_lastNavigationAction->clientRedirectSourceForHistory : WTF::String(); }
    std::optional<WebCore::OwnerPermissionsPolicyData> ownerPermissionsPolicy() const { return m_lastNavigationAction ? m_lastNavigationAction->ownerPermissionsPolicy : std::nullopt; }

    void setLastNavigationAction(const WebKit::NavigationActionData& navigationAction) { m_lastNavigationAction = navigationAction; }
    const std::optional<WebKit::NavigationActionData>& lastNavigationAction() const { return m_lastNavigationAction; }

    void setOriginatingFrameInfo(const WebKit::FrameInfoData& frameInfo) { m_originatingFrameInfo = frameInfo; }
    const std::optional<WebKit::FrameInfoData>& originatingFrameInfo() const { return m_originatingFrameInfo; }

    void setDestinationFrameSecurityOrigin(const WebCore::SecurityOriginData& origin) { m_destinationFrameSecurityOrigin = origin; }
    const WebCore::SecurityOriginData& destinationFrameSecurityOrigin() const { return m_destinationFrameSecurityOrigin; }

    void setEffectiveContentMode(WebKit::WebContentMode mode) { m_effectiveContentMode = mode; }
    WebKit::WebContentMode effectiveContentMode() const { return m_effectiveContentMode; }

#if !LOG_DISABLED
    WTF::String loggingString() const;
#endif

    const std::unique_ptr<SubstituteData>& substituteData() const { return m_substituteData; }

    const WebCore::PrivateClickMeasurement* privateClickMeasurement() const { return m_lastNavigationAction && m_lastNavigationAction->privateClickMeasurement ? &*m_lastNavigationAction->privateClickMeasurement : nullptr; }

    void setClientNavigationActivity(RefPtr<WebKit::ProcessThrottler::Activity>&& activity) { Ref { m_clientNavigationActivity }->setActivity(WTFMove(activity)); }

    void setIsLoadedWithNavigationShared(bool value) { m_isLoadedWithNavigationShared = value; }
    bool isLoadedWithNavigationShared() const { return m_isLoadedWithNavigationShared; }

    void setWebsitePolicies(RefPtr<API::WebsitePolicies>&& policies) { m_websitePolicies = WTFMove(policies); }
    API::WebsitePolicies* websitePolicies() { return m_websitePolicies.get(); }
    RefPtr<API::WebsitePolicies> protectedWebsitePolicies() const { return m_websitePolicies; }

    void setOriginatorAdvancedPrivacyProtections(OptionSet<WebCore::AdvancedPrivacyProtections> advancedPrivacyProtections) { m_originatorAdvancedPrivacyProtections = advancedPrivacyProtections; }
    std::optional<OptionSet<WebCore::AdvancedPrivacyProtections>> originatorAdvancedPrivacyProtections() const { return m_originatorAdvancedPrivacyProtections; }
    void setSafeBrowsingCheckOngoing(size_t, bool);
    bool safeBrowsingCheckOngoing(size_t);
    bool safeBrowsingCheckOngoing();
    void setSafeBrowsingWarning(RefPtr<WebKit::BrowsingWarning>&&);
    RefPtr<WebKit::BrowsingWarning> safeBrowsingWarning();
    void setSafeBrowsingCheckTimedOut() { m_safeBrowsingCheckTimedOut = true; }
    bool safeBrowsingCheckTimedOut() { return m_safeBrowsingCheckTimedOut; }
    MonotonicTime requestStart() const { return m_requestStart; }
    void resetRequestStart();

    WebCore::ProcessIdentifier processID() const { return m_processID; }
    void setProcessID(WebCore::ProcessIdentifier processID) { m_processID = processID; }

private:
    Navigation(WebCore::ProcessIdentifier);
    Navigation(WebCore::ProcessIdentifier, RefPtr<WebKit::WebBackForwardListItem>&&);
    Navigation(WebCore::ProcessIdentifier, WebCore::ResourceRequest&&, RefPtr<WebKit::WebBackForwardListItem>&& fromItem);
    Navigation(WebCore::ProcessIdentifier, Ref<WebKit::WebBackForwardListFrameItem>&& targetItem, RefPtr<WebKit::WebBackForwardListItem>&& fromItem, WebCore::FrameLoadType);
    Navigation(WebCore::ProcessIdentifier, std::unique_ptr<SubstituteData>&&);
    Navigation(WebCore::ProcessIdentifier, WebCore::ResourceRequest&&, std::unique_ptr<SubstituteData>&&, RefPtr<WebKit::WebBackForwardListItem>&& fromItem);

    WebCore::NavigationIdentifier m_navigationID;
    WebCore::ProcessIdentifier m_processID;
    WebCore::ResourceRequest m_originalRequest;
    WebCore::ResourceRequest m_currentRequest;
    std::optional<WebCore::ProcessIdentifier> m_currentRequestProcessIdentifier;
    Vector<WTF::URL> m_redirectChain;

    const RefPtr<WebKit::WebBackForwardListFrameItem> m_targetFrameItem;
    RefPtr<WebKit::WebBackForwardListItem> m_fromItem;
    RefPtr<WebKit::WebBackForwardListItem> m_reloadItem;
    std::optional<WebCore::FrameLoadType> m_backForwardFrameLoadType;
    std::unique_ptr<SubstituteData> m_substituteData;
    std::optional<WebKit::NavigationActionData> m_lastNavigationAction;
    std::optional<WebKit::FrameInfoData> m_originatingFrameInfo;
    WebCore::SecurityOriginData m_destinationFrameSecurityOrigin;
    WebKit::WebContentMode m_effectiveContentMode { WebKit::WebContentMode::Recommended };
    Ref<WebKit::ProcessThrottler::TimedActivity> m_clientNavigationActivity;
    bool m_userContentExtensionsEnabled : 1 { true };
    bool m_isLoadedWithNavigationShared : 1 { false };
    bool m_requestIsFromClientInput : 1 { false };
    bool m_isFromLoadData : 1 { false };
    bool m_safeBrowsingCheckTimedOut : 1 { false };
    RefPtr<API::WebsitePolicies> m_websitePolicies;
    std::optional<OptionSet<WebCore::AdvancedPrivacyProtections>> m_originatorAdvancedPrivacyProtections;
    MonotonicTime m_requestStart { MonotonicTime::now() };
    RefPtr<WebKit::BrowsingWarning> m_safeBrowsingWarning;
    ListHashSet<size_t> m_ongoingSafeBrowsingChecks;
};

} // namespace API

SPECIALIZE_TYPE_TRAITS_API_OBJECT(Navigation);
