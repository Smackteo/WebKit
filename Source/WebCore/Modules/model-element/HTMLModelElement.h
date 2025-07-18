/*
 * Copyright (C) 2020-2023 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(MODEL_ELEMENT)

#include "ActiveDOMObject.h"
#include "CachedRawResource.h"
#include "CachedRawResourceClient.h"
#include "CachedResourceHandle.h"
#include "EventLoop.h"
#include "HTMLElement.h"
#include "HTMLModelElementCamera.h"
#include "IDLTypes.h"
#include "LayerHostingContextIdentifier.h"
#include "ModelPlayer.h"
#include "ModelPlayerClient.h"
#include "PlatformLayer.h"
#include "PlatformLayerIdentifier.h"
#include "SharedBuffer.h"
#include "VisibilityChangeClient.h"
#include <wtf/UniqueRef.h>

#if ENABLE(MODEL_PROCESS)
#include "StageModeOperations.h"
#endif

namespace WebCore {

class CachedResourceRequest;
class DOMMatrixReadOnly;
class DOMPointReadOnly;
class Event;
class GraphicsLayer;
class LayoutPoint;
class LayoutSize;
class Model;
class ModelPlayerProvider;
class MouseEvent;

template<typename IDLType> class DOMPromiseDeferred;
template<typename IDLType> class DOMPromiseProxyWithResolveCallback;
template<typename> class ExceptionOr;

#if ENABLE(MODEL_PROCESS)
template<typename IDLType> class DOMPromiseProxy;
class ModelContext;
#endif

class HTMLModelElement final : public HTMLElement, private CachedRawResourceClient, public ModelPlayerClient, public ActiveDOMObject, public VisibilityChangeClient {
    WTF_MAKE_TZONE_OR_ISO_ALLOCATED(HTMLModelElement);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(HTMLModelElement);
public:
    USING_CAN_MAKE_WEAKPTR(HTMLElement);

    static Ref<HTMLModelElement> create(const QualifiedName&, Document&);
    virtual ~HTMLModelElement();

    // ActiveDOMObject.
    void ref() const final { HTMLElement::ref(); }
    void deref() const final { HTMLElement::deref(); }

    // VisibilityChangeClient.
    void visibilityStateChanged() final;

    void sourcesChanged();
    const URL& currentSrc() const { return m_sourceURL; }
    bool complete() const { return m_dataComplete; }

    // MARK: DOM Functions and Attributes

    using ReadyPromise = DOMPromiseProxyWithResolveCallback<IDLInterface<HTMLModelElement>>;
    ReadyPromise& ready() { return m_readyPromise.get(); }

    WEBCORE_EXPORT RefPtr<Model> model() const;

    bool usesPlatformLayer() const;
    PlatformLayer* platformLayer() const;

    std::optional<LayerHostingContextIdentifier> layerHostingContextIdentifier() const;

    std::optional<PlatformLayerIdentifier> layerID() const;

#if ENABLE(MODEL_PROCESS)
    RefPtr<ModelContext> modelContext() const;

    const DOMMatrixReadOnly& entityTransform() const;
    ExceptionOr<void> setEntityTransform(const DOMMatrixReadOnly&);

    const DOMPointReadOnly& boundingBoxCenter() const;
    const DOMPointReadOnly& boundingBoxExtents() const;

    using EnvironmentMapPromise = DOMPromiseProxy<IDLUndefined>;
    EnvironmentMapPromise& environmentMapReady() { return m_environmentMapReadyPromise.get(); }
#endif

    void enterFullscreen();

    using CameraPromise = DOMPromiseDeferred<IDLDictionary<HTMLModelElementCamera>>;
    void getCamera(CameraPromise&&);
    void setCamera(HTMLModelElementCamera, DOMPromiseDeferred<void>&&);

    using IsPlayingAnimationPromise = DOMPromiseDeferred<IDLBoolean>;
    void isPlayingAnimation(IsPlayingAnimationPromise&&);
    void playAnimation(DOMPromiseDeferred<void>&&);
    void pauseAnimation(DOMPromiseDeferred<void>&&);

    using IsLoopingAnimationPromise = DOMPromiseDeferred<IDLBoolean>;
    void isLoopingAnimation(IsLoopingAnimationPromise&&);
    void setIsLoopingAnimation(bool, DOMPromiseDeferred<void>&&);

    using DurationPromise = DOMPromiseDeferred<IDLDouble>;
    void animationDuration(DurationPromise&&);
    using CurrentTimePromise = DOMPromiseDeferred<IDLDouble>;
    void animationCurrentTime(CurrentTimePromise&&);
    void setAnimationCurrentTime(double, DOMPromiseDeferred<void>&&);

    using HasAudioPromise = DOMPromiseDeferred<IDLBoolean>;
    void hasAudio(HasAudioPromise&&);
    using IsMutedPromise = DOMPromiseDeferred<IDLBoolean>;
    void isMuted(IsMutedPromise&&);
    void setIsMuted(bool, DOMPromiseDeferred<void>&&);

    bool supportsDragging() const;
    bool isDraggableIgnoringAttributes() const final;

    bool isInteractive() const;

#if ENABLE(MODEL_PROCESS)
    double playbackRate() const { return m_playbackRate; }
    void setPlaybackRate(double);
    double duration() const;
    bool paused() const;
    void play(DOMPromiseDeferred<void>&&);
    void pause(DOMPromiseDeferred<void>&&);
    void setPaused(bool, DOMPromiseDeferred<void>&&);
    double currentTime() const;
    void setCurrentTime(double);
    const URL& environmentMap() const;
    void setEnvironmentMap(const URL&);
    WEBCORE_EXPORT bool supportsStageModeInteraction() const;
    WEBCORE_EXPORT void beginStageModeTransform(const TransformationMatrix&);
    WEBCORE_EXPORT void updateStageModeTransform(const TransformationMatrix&);
    WEBCORE_EXPORT void endStageModeInteraction();
    WEBCORE_EXPORT void tryAnimateModelToFitPortal(bool handledDrag, CompletionHandler<void(bool)>&&);
    WEBCORE_EXPORT void resetModelTransformAfterDrag();
#endif

#if PLATFORM(COCOA)
    Vector<RetainPtr<id>> accessibilityChildren();
#endif

    void sizeMayHaveChanged();

#if ENABLE(ARKIT_INLINE_PREVIEW_MAC)
    WEBCORE_EXPORT String inlinePreviewUUIDForTesting() const;
#endif

    size_t memoryCost() const;
#if ENABLE(RESOURCE_USAGE)
    size_t externalMemoryCost() const;
#endif

    void viewportIntersectionChanged(bool isIntersecting);
    bool isIntersectingViewport() const final { return m_isIntersectingViewport; }

    WEBCORE_EXPORT String modelElementStateForTesting() const;

private:
    HTMLModelElement(const QualifiedName&, Document&);

    URL selectModelSource() const;
    void setSourceURL(const URL&);
    void modelDidChange();
    void createModelPlayer();
    void deleteModelPlayer();
    void unloadModelPlayer(bool onSuspend);
    void reloadModelPlayer();
    void startLoadModelTimer();
    void loadModelTimerFired();

    RefPtr<GraphicsLayer> graphicsLayer() const;

    HTMLModelElement& readyPromiseResolve();

    CachedResourceRequest createResourceRequest(const URL&, FetchOptions::Destination);

    // ActiveDOMObject.
    bool virtualHasPendingActivity() const final;
    void resume() final;
    void suspend(ReasonForSuspension) final;
    void stop() final;

    // DOM overrides.
    void didMoveToNewDocument(Document& oldDocument, Document& newDocument) final;
    bool isURLAttribute(const Attribute&) const final;
    void attributeChanged(const QualifiedName&, const AtomString& oldValue, const AtomString& newValue, AttributeModificationReason) final;

    // StyledElement
    bool hasPresentationalHintsForAttribute(const QualifiedName&) const final;
    void collectPresentationalHintsForAttribute(const QualifiedName&, const AtomString&, MutableStyleProperties&) final;

    // Rendering overrides.
    RenderPtr<RenderElement> createElementRenderer(RenderStyle&&, const RenderTreePosition&) final;
    bool isReplaced(const RenderStyle&) const final { return true; }
    void didAttachRenderers() final;

    // CachedRawResourceClient overrides.
    void dataReceived(CachedResource&, const SharedBuffer&) final;
    void notifyFinished(CachedResource&, const NetworkLoadMetrics&, LoadWillContinueInAnotherProcess) final;

    // ModelPlayerClient overrides.
    void didUpdateLayerHostingContextIdentifier(ModelPlayer&, LayerHostingContextIdentifier) final;
    void didFinishLoading(ModelPlayer&) final;
    void didFailLoading(ModelPlayer&, const ResourceError&) final;
#if ENABLE(MODEL_PROCESS)
    void didUpdateEntityTransform(ModelPlayer&, const TransformationMatrix&) final;
    void didUpdateBoundingBox(ModelPlayer&, const FloatPoint3D&, const FloatPoint3D&) final;
    void didFinishEnvironmentMapLoading(bool succeeded) final;
    void didUnload(ModelPlayer&) final;
#endif
    std::optional<PlatformLayerIdentifier> modelContentsLayerID() const final;
    bool isVisible() const final;
    void logWarning(ModelPlayer&, const String&) final;

    Node::InsertedIntoAncestorResult insertedIntoAncestor(InsertionType , ContainerNode& parentOfInsertedTree) override;
    void removedFromAncestor(RemovalType, ContainerNode& oldParentOfRemovedTree) override;

    void defaultEventHandler(Event&) final;
    void dragDidStart(MouseEvent&);
    void dragDidChange(MouseEvent&);
    void dragDidEnd(MouseEvent&);

    LayoutPoint flippedLocationInElementForMouseEvent(MouseEvent&);

    void setAnimationIsPlaying(bool, DOMPromiseDeferred<void>&&);

    LayoutSize contentSize() const;

    void reportExtraMemoryCost();

#if ENABLE(MODEL_PROCESS)
    bool autoplay() const;
    void updateAutoplay();
    bool loop() const;
    void updateLoop();
    void updateEnvironmentMap();
    URL selectEnvironmentMapURL() const;
    void environmentMapRequestResource();
    void environmentMapResetAndReject(Exception&&);
    void environmentMapResourceFinished();
    bool hasPortal() const;
    void updateHasPortal();
    WebCore::StageModeOperation stageMode() const;
    void updateStageMode();
#endif
    void modelResourceFinished();
    void sourceRequestResource();
    bool shouldDeferLoading() const;
    bool isModelDeferred() const;
    bool isModelLoading() const;
    bool isModelLoaded() const;
    bool isModelUnloading() const;
    bool isModelUnloaded() const;

    URL m_sourceURL;
    CachedResourceHandle<CachedRawResource> m_resource;
    SharedBufferBuilder m_data;
    mutable std::atomic<size_t> m_dataMemoryCost { 0 };
    size_t m_reportedDataMemoryCost { 0 };
    WeakPtr<ModelPlayerProvider> m_modelPlayerProvider;
    RefPtr<Model> m_model;
    UniqueRef<ReadyPromise> m_readyPromise;
    bool m_dataComplete { false };
    bool m_isDragging { false };
    bool m_shouldCreateModelPlayerUponRendererAttachment { false };
    bool m_isIntersectingViewport { false };

    RefPtr<ModelPlayer> m_modelPlayer;
    EventLoopTimerHandle m_loadModelTimer;
#if ENABLE(MODEL_PROCESS)
    Ref<DOMMatrixReadOnly> m_entityTransform;
    Ref<DOMPointReadOnly> m_boundingBoxCenter;
    Ref<DOMPointReadOnly> m_boundingBoxExtents;
    double m_playbackRate { 1.0 };
    URL m_environmentMapURL;
    SharedBufferBuilder m_environmentMapData;
    mutable std::atomic<size_t> m_environmentMapDataMemoryCost { 0 };
    CachedResourceHandle<CachedRawResource> m_environmentMapResource;
    UniqueRef<EnvironmentMapPromise> m_environmentMapReadyPromise;
#endif
};

} // namespace WebCore

#endif // ENABLE(MODEL_ELEMENT)
