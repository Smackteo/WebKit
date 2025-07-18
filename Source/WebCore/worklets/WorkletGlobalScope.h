/*
 * Copyright (C) 2018-2025 Apple Inc. All rights reserved.
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
 *
 */

#pragma once

#include "Document.h"
#include "EventTargetInterfaces.h"
#include "FetchRequestCredentials.h"
#include "ScriptExecutionContext.h"
#include "ScriptSourceCode.h"
#include "Settings.h"
#include "WorkerOrWorkletGlobalScope.h"
#include "WorkerOrWorkletScriptController.h"
#include <JavaScriptCore/ConsoleMessage.h>
#include <JavaScriptCore/RuntimeFlags.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Deque.h>
#include <wtf/URL.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

class MessagePortChannelProvider;
class WorkerMessagePortChannelProvider;
class WorkerScriptLoader;

struct WorkletParameters;

enum class WorkletGlobalScopeIdentifierType { };
using WorkletGlobalScopeIdentifier = ObjectIdentifier<WorkletGlobalScopeIdentifierType>;

class WorkletGlobalScope : public WorkerOrWorkletGlobalScope {
    WTF_MAKE_TZONE_OR_ISO_ALLOCATED(WorkletGlobalScope);
public:
    virtual ~WorkletGlobalScope();

    virtual bool isPaintWorkletGlobalScope() const { return false; }
#if ENABLE(WEB_AUDIO)
    virtual bool isAudioWorkletGlobalScope() const { return false; }
#endif

    WEBCORE_EXPORT static unsigned numberOfWorkletGlobalScopes();

    MessagePortChannelProvider& messagePortChannelProvider();

    const URL& url() const final { return m_url; }
    const URL& cookieURL() const final { return url(); }

    void evaluate();

    void addConsoleMessage(std::unique_ptr<Inspector::ConsoleMessage>&&) final;

    SecurityOrigin& topOrigin() const final { return m_topOrigin.get(); }

    SocketProvider* socketProvider() final { return nullptr; }

    bool isSecureContext() const final { return false; }

    JSC::RuntimeFlags jsRuntimeFlags() const { return m_jsRuntimeFlags; }

    void prepareForDestruction() override;

    void fetchAndInvokeScript(const URL&, FetchRequestCredentials, CompletionHandler<void(std::optional<Exception>&&)>&&);

    Document* responsibleDocument() { return m_document.get(); }
    const Document* responsibleDocument() const { return m_document.get(); }

protected:
    WorkletGlobalScope(WorkerOrWorkletThread&, Ref<JSC::VM>&&, const WorkletParameters&);
    WorkletGlobalScope(Document&, Ref<JSC::VM>&&, ScriptSourceCode&&);

private:
    IDBClient::IDBConnectionProxy* idbConnectionProxy() final { ASSERT_NOT_REACHED(); return nullptr; }

    // EventTarget.
    enum EventTargetInterfaceType eventTargetInterface() const final { return EventTargetInterfaceType::WorkletGlobalScope; }

    bool isWorkletGlobalScope() const final { return true; }

    void logExceptionToConsole(const String& errorMessage, const String&, int, int, RefPtr<Inspector::ScriptCallStack>&&) final;
    void addMessage(MessageSource, MessageLevel, const String&, const String&, unsigned, unsigned, RefPtr<Inspector::ScriptCallStack>&&, JSC::JSGlobalObject*, unsigned long) final;
    void addConsoleMessage(MessageSource, MessageLevel, const String&, unsigned long) final;

    EventTarget* errorEventTarget() final { return this; }

    std::optional<Vector<uint8_t>> serializeAndWrapCryptoKey(CryptoKeyData&&) final { RELEASE_ASSERT_NOT_REACHED(); return std::nullopt; }
    std::optional<Vector<uint8_t>> unwrapCryptoKey(const Vector<uint8_t>&) final { RELEASE_ASSERT_NOT_REACHED(); return std::nullopt; }
    URL completeURL(const String&, ForceUTF8 = ForceUTF8::No) const final;
    String userAgent(const URL&) const final;
    const SettingsValues& settingsValues() const final { return m_settingsValues; }

    WeakPtr<Document, WeakPtrImplWithEventTargetData> m_document;

    const Ref<SecurityOrigin> m_topOrigin;

    URL m_url;
    JSC::RuntimeFlags m_jsRuntimeFlags;
    std::optional<ScriptSourceCode> m_code;

    std::unique_ptr<WorkerMessagePortChannelProvider> m_messagePortChannelProvider;

    SettingsValues m_settingsValues;
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::WorkletGlobalScope)
static bool isType(const WebCore::ScriptExecutionContext& context) { return context.isWorkletGlobalScope(); }
SPECIALIZE_TYPE_TRAITS_END()
