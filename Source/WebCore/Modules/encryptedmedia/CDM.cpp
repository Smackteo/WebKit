/*
 * Copyright (C) 2016 Apple Inc. All rights reserved.
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
#include "CDM.h"

#if ENABLE(ENCRYPTED_MEDIA)

#include "CDMFactory.h"
#include "CDMPrivate.h"
#include "ContextDestructionObserverInlines.h"
#include "DocumentInlines.h"
#include "FrameInlines.h"
#include "InitDataRegistry.h"
#include "MediaKeysRequirement.h"
#include "MediaPlayer.h"
#include "NotImplemented.h"
#include "Page.h"
#include "ParsedContentType.h"
#include "ScriptExecutionContext.h"
#include "SecurityOrigin.h"
#include "SecurityOriginData.h"
#include "Settings.h"
#include "SharedBuffer.h"
#include <wtf/FileSystem.h>
#include <wtf/Logger.h>
#include <wtf/LoggerHelper.h>
#include <wtf/NeverDestroyed.h>

namespace WebCore {

bool CDM::supportsKeySystem(const String& keySystem)
{
    for (auto* factory : CDMFactory::registeredFactories()) {
        if (factory->supportsKeySystem(keySystem))
            return true;
    }
    return false;
}

Ref<CDM> CDM::create(Document& document, const String& keySystem, const String& mediaKeysHashSalt)
{
    return adoptRef(*new CDM(document, keySystem, mediaKeysHashSalt));
}

CDM::CDM(Document& document, const String& keySystem, const String& mediaKeysHashSalt)
    : ContextDestructionObserver(&document)
#if !RELEASE_LOG_DISABLED
    , m_logger(document.logger())
    , m_logIdentifier(LoggerHelper::uniqueLogIdentifier())
#endif
    , m_keySystem(keySystem)
    , m_mediaKeysHashSalt { mediaKeysHashSalt }
{
    ASSERT(supportsKeySystem(keySystem));
    for (auto* factory : CDMFactory::registeredFactories()) {
        if (factory->supportsKeySystem(keySystem)) {
            m_private = factory->createCDM(keySystem, m_mediaKeysHashSalt, *this);
#if !RELEASE_LOG_DISABLED
            m_private->setLogIdentifier(m_logIdentifier);
#endif
            break;
        }
    }
}

CDM::~CDM() = default;

void CDM::getSupportedConfiguration(MediaKeySystemConfiguration&& candidateConfiguration, SupportedConfigurationCallback&& callback)
{
    // https://w3c.github.io/encrypted-media/#get-supported-configuration
    // W3C Editor's Draft 09 November 2016
    // Implemented in CDMPrivate::getSupportedConfiguration()

    RefPtr document = downcast<Document>(scriptExecutionContext());
    if (!document || !m_private) {
        callback(std::nullopt);
        return;
    }

    RefPtr page = document->page();
    auto access = CDMPrivate::LocalStorageAccess::Allowed;
    bool isEphemeral = !page || page->sessionID().isEphemeral();
    if (isEphemeral || document->canAccessResource(ScriptExecutionContext::ResourceType::LocalStorage) == ScriptExecutionContext::HasResourceAccess::No)
        access = CDMPrivate::LocalStorageAccess::NotAllowed;
    m_private->getSupportedConfiguration(WTFMove(candidateConfiguration), access, WTFMove(callback));
}

void CDM::loadAndInitialize()
{
    if (m_private)
        m_private->loadAndInitialize();
}

RefPtr<CDMInstance> CDM::createInstance()
{
    if (!m_private)
        return nullptr;
    auto instance = m_private->createInstance();
    if (instance)
        instance->setStorageDirectory(storageDirectory());
    return instance;
}

bool CDM::supportsServerCertificates() const
{
    return m_private && m_private->supportsServerCertificates();
}

bool CDM::supportsSessions() const
{
    return m_private && m_private->supportsSessions();
}

bool CDM::supportsInitDataType(const AtomString& initDataType) const
{
    return m_private && m_private->supportedInitDataTypes().contains(initDataType);
}

RefPtr<SharedBuffer> CDM::sanitizeInitData(const AtomString& initDataType, const SharedBuffer& initData)
{
    return InitDataRegistry::shared().sanitizeInitData(initDataType, initData);
}

bool CDM::supportsInitData(const AtomString& initDataType, const SharedBuffer& initData)
{
    return m_private && m_private->supportsInitData(initDataType, initData);
}

RefPtr<SharedBuffer> CDM::sanitizeResponse(const SharedBuffer& response)
{
    if (!m_private)
        return nullptr;
    return m_private->sanitizeResponse(response);
}

std::optional<String> CDM::sanitizeSessionId(const String& sessionId)
{
    if (!m_private)
        return std::nullopt;
    return m_private->sanitizeSessionId(sessionId);
}

String CDM::storageDirectory() const
{
    RefPtr document = downcast<Document>(scriptExecutionContext());
    return document ? document->mediaKeysStorageDirectory() : emptyString();
}

}

#endif
