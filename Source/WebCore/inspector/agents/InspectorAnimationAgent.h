/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
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

#include "ComputedEffectTiming.h"
#include "InspectorWebAgentBase.h"
#include "Timer.h"
#include <JavaScriptCore/InspectorBackendDispatchers.h>
#include <JavaScriptCore/InspectorFrontendDispatchers.h>
#include <JavaScriptCore/InspectorProtocolObjects.h>
#include <wtf/CheckedPtr.h>
#include <wtf/Forward.h>
#include <wtf/RobinHoodHashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakHashMap.h>

namespace WebCore {

class AnimationEffect;
class Element;
class Event;
class KeyframeEffect;
class LocalFrame;
class Page;
class StyleOriginatedAnimation;
class WebAnimation;
class WeakPtrImplWithEventTargetData;

struct Styleable;

class InspectorAnimationAgent final : public InspectorAgentBase, public Inspector::AnimationBackendDispatcherHandler, public CanMakeCheckedPtr<InspectorAnimationAgent> {
    WTF_MAKE_NONCOPYABLE(InspectorAnimationAgent);
    WTF_MAKE_TZONE_ALLOCATED(InspectorAnimationAgent);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(InspectorAnimationAgent);
public:
    InspectorAnimationAgent(PageAgentContext&);
    ~InspectorAnimationAgent();

    // InspectorAgentBase
    void didCreateFrontendAndBackend();
    void willDestroyFrontendAndBackend(Inspector::DisconnectReason);

    // AnimationBackendDispatcherHandler
    Inspector::Protocol::ErrorStringOr<void> enable();
    Inspector::Protocol::ErrorStringOr<void> disable();
    Inspector::Protocol::ErrorStringOr<RefPtr<Inspector::Protocol::Animation::Effect>> requestEffect(const Inspector::Protocol::Animation::AnimationId&);
    Inspector::Protocol::ErrorStringOr<Ref<Inspector::Protocol::DOM::Styleable>> requestEffectTarget(const Inspector::Protocol::Animation::AnimationId&);
    Inspector::Protocol::ErrorStringOr<Ref<Inspector::Protocol::Runtime::RemoteObject>> resolveAnimation(const Inspector::Protocol::Animation::AnimationId&, const String& objectGroup);
    Inspector::Protocol::ErrorStringOr<void> startTracking();
    Inspector::Protocol::ErrorStringOr<void> stopTracking();

    // InspectorInstrumentation
    void willApplyKeyframeEffect(const Styleable&, KeyframeEffect&, const ComputedEffectTiming&);
    void didChangeWebAnimationName(WebAnimation&);
    void didSetWebAnimationEffect(WebAnimation&);
    void didChangeWebAnimationEffectTiming(WebAnimation&);
    void didChangeWebAnimationEffectTarget(WebAnimation&);
    void didCreateWebAnimation(WebAnimation&);
    void willDestroyWebAnimation(WebAnimation&);
    void frameNavigated(LocalFrame&);

private:
    String findAnimationId(WebAnimation&);
    WebAnimation* assertAnimation(Inspector::Protocol::ErrorString&, const String& animationId);
    void bindAnimation(WebAnimation&, RefPtr<Inspector::Protocol::Console::StackTrace> backtrace);
    void animationBindingTimerFired();
    void unbindAnimation(const String& animationId);
    void animationDestroyedTimerFired();
    void reset();

    void stopTrackingStyleOriginatedAnimation(StyleOriginatedAnimation&);

    const UniqueRef<Inspector::AnimationFrontendDispatcher> m_frontendDispatcher;
    const Ref<Inspector::AnimationBackendDispatcher> m_backendDispatcher;

    Inspector::InjectedScriptManager& m_injectedScriptManager;
    WeakRef<Page> m_inspectedPage;

    MemoryCompactRobinHoodHashMap<Inspector::Protocol::Animation::AnimationId, WebAnimation*> m_animationIdMap;

    WeakHashMap<WebAnimation, Ref<Inspector::Protocol::Console::StackTrace>, WeakPtrImplWithEventTargetData> m_animationsPendingBinding;
    Timer m_animationBindingTimer;

    Vector<Inspector::Protocol::Animation::AnimationId> m_removedAnimationIds;
    Timer m_animationDestroyedTimer;

    struct TrackedStyleOriginatedAnimationData {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(TrackedStyleOriginatedAnimationData);
        Inspector::Protocol::Animation::AnimationId trackingAnimationId;
        ComputedEffectTiming lastComputedTiming;
    };
    HashMap<StyleOriginatedAnimation*, UniqueRef<TrackedStyleOriginatedAnimationData>> m_trackedStyleOriginatedAnimationData;

    WeakHashSet<WebAnimation, WeakPtrImplWithEventTargetData> m_animationsIgnoringEffectChanges;
    WeakHashSet<WebAnimation, WeakPtrImplWithEventTargetData> m_animationsIgnoringTargetChanges;
};

} // namespace WebCore
