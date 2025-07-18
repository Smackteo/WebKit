/*
 * Copyright (C) 2016 Canon Inc.
 * Copyright (C) 2020-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted, provided that the following conditions
 * are required to be met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Canon Inc. nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY CANON INC. AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CANON INC. AND ITS CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FetchResponse.h"

#include "FetchRequest.h"
#include "FetchResponseBodyLoader.h"
#include "HTTPParsers.h"
#include "InspectorInstrumentation.h"
#include "JSBlob.h"
#include "MIMETypeRegistry.h"
#include "ReadableStreamSink.h"
#include "ResourceError.h"
#include "ScriptExecutionContext.h"
#include <JavaScriptCore/JSONObject.h>
#include <wtf/text/MakeString.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FetchResponseBodyLoader);

// https://fetch.spec.whatwg.org/#null-body-status
static inline bool isNullBodyStatus(int status)
{
    return status == 101 || status == 204 || status == 205 || status == 304;
}

FetchResponse::~FetchResponse() = default;

Ref<FetchResponse> FetchResponse::create(ScriptExecutionContext* context, std::optional<FetchBody>&& body, FetchHeaders::Guard guard, ResourceResponse&& response)
{
    bool isOpaque = response.tainting() == ResourceResponse::Tainting::Opaque;
    Ref headers = isOpaque ? FetchHeaders::create(guard) : FetchHeaders::create(guard, HTTPHeaderMap { response.httpHeaderFields() });
    return FetchResponse::create(context, WTFMove(body), WTFMove(headers), WTFMove(response));
}

Ref<FetchResponse> FetchResponse::create(ScriptExecutionContext* context, std::optional<FetchBody>&& body, Ref<FetchHeaders>&& headers, ResourceResponse&& response)
{
    bool isSynthetic = response.type() == ResourceResponse::Type::Default || response.type() == ResourceResponse::Type::Error;
    bool isOpaque = response.tainting() == ResourceResponse::Tainting::Opaque;

    Ref fetchResponse = adoptRef(*new FetchResponse(context, WTFMove(body), WTFMove(headers), WTFMove(response)));
    fetchResponse->suspendIfNeeded();
    if (!isSynthetic)
        fetchResponse->m_filteredResponse = ResourceResponseBase::filter(fetchResponse->m_internalResponse, ResourceResponse::PerformExposeAllHeadersCheck::Yes);
    if (isOpaque)
        fetchResponse->setBodyAsOpaque();
    return fetchResponse;
}

ExceptionOr<Ref<FetchResponse>> FetchResponse::create(ScriptExecutionContext& context, std::optional<FetchBodyWithType>&& bodyWithType, Init&& init)
{
    // https://fetch.spec.whatwg.org/#initialize-a-response
    // 1. If init["status"] is not in the range 200 to 599, inclusive, then throw a RangeError.
    if (init.status < 200 || init.status > 599)
        return Exception { ExceptionCode::RangeError, "Status must be between 200 and 599"_s };

    // 2. If init["statusText"] does not match the reason-phrase token production, then throw a TypeError.
    if (!isValidReasonPhrase(init.statusText))
        return Exception { ExceptionCode::TypeError, "Status text must be a valid reason-phrase."_s };

    // Both uses of "initialize a response" (the Response constructor and Response.json) create the
    // Response object with the "response" header guard.
    auto headers = FetchHeaders::create(FetchHeaders::Guard::Response);

    // 5. If init["headers"] exists, then fill response’s headers with init["headers"].
    if (init.headers) {
        auto result = headers->fill(*init.headers);
        if (result.hasException())
            return result.releaseException();
    }

    std::optional<FetchBody> body;

    // 6. If body was given, then:
    if (bodyWithType) {
        // 6.1 If response’s status is a null body status, then throw a TypeError.
        //     (NOTE: 101 and 103 are included in null body status due to their use elsewhere. It does not affect this step.)
        if (isNullBodyStatus(init.status))
            return Exception { ExceptionCode::TypeError, "Response cannot have a body with the given status."_s };

        // 6.2 Set response’s body to body’s body.
        body = WTFMove(bodyWithType->body);

        // 6.3 If body’s type is non-null and response’s header list does not contain `Content-Type`, then append
        //     (`Content-Type`, body’s type) to response’s header list.
        if (!bodyWithType->type.isNull() && !headers->fastHas(HTTPHeaderName::ContentType))
            headers->fastSet(HTTPHeaderName::ContentType, bodyWithType->type);
    }

    auto contentType = headers->fastGet(HTTPHeaderName::ContentType);

    auto r = adoptRef(*new FetchResponse(&context, WTFMove(body), WTFMove(headers), { }));
    r->suspendIfNeeded();

    auto mimeType = extractMIMETypeFromMediaType(contentType);
    r->m_internalResponse.setMimeType(mimeType.isEmpty() ? String { defaultMIMEType() } : WTFMove(mimeType));
    r->m_internalResponse.setTextEncodingName(extractCharsetFromMediaType(contentType).toString());

    if (auto expectedContentLength = parseContentLength(r->m_headers->fastGet(HTTPHeaderName::ContentLength)))
        r->m_internalResponse.setExpectedContentLength(*expectedContentLength);

    // 3. Set response’s response’s status to init["status"].
    r->m_internalResponse.setHTTPStatusCode(init.status);
    // 4. Set response’s response’s status message to init["statusText"].
    r->m_internalResponse.setHTTPStatusText(init.statusText.releaseString());

    return r;
}

ExceptionOr<Ref<FetchResponse>> FetchResponse::create(ScriptExecutionContext& context, std::optional<FetchBody::Init>&& body, Init&& init)
{
    std::optional<FetchBodyWithType> bodyWithType;
    if (body) {
        String type;
        auto result = FetchBody::extract(WTFMove(*body), type);
        if (result.hasException())
            return result.releaseException();
        bodyWithType = { result.releaseReturnValue(), WTFMove(type) };
    }

    return FetchResponse::create(context, WTFMove(bodyWithType), WTFMove(init));
}

Ref<FetchResponse> FetchResponse::error(ScriptExecutionContext& context)
{
    auto response = adoptRef(*new FetchResponse(&context, { }, FetchHeaders::create(FetchHeaders::Guard::Immutable), { }));
    response->suspendIfNeeded();
    response->m_internalResponse.setType(Type::Error);
    return response;
}

ExceptionOr<Ref<FetchResponse>> FetchResponse::redirect(ScriptExecutionContext& context, const String& url, int status)
{
    URL requestURL = context.completeURL(url, ScriptExecutionContext::ForceUTF8::Yes);
    if (!requestURL.isValid())
        return Exception { ExceptionCode::TypeError, makeString("Redirection URL '"_s, requestURL.string(), "' is invalid"_s) };
    if (requestURL.hasCredentials())
        return Exception { ExceptionCode::TypeError, "Redirection URL contains credentials"_s };
    if (!ResourceResponse::isRedirectionStatusCode(status))
        return Exception { ExceptionCode::RangeError, makeString(status, " is not a redirection status code"_s) };
    auto redirectResponse = adoptRef(*new FetchResponse(&context, { }, FetchHeaders::create(FetchHeaders::Guard::Immutable), { }));
    redirectResponse->suspendIfNeeded();
    redirectResponse->m_internalResponse.setHTTPStatusCode(status);
    redirectResponse->m_internalResponse.setHTTPHeaderField(HTTPHeaderName::Location, requestURL.string());
    redirectResponse->m_headers->fastSet(HTTPHeaderName::Location, requestURL.string());
    return redirectResponse;
}

ExceptionOr<Ref<FetchResponse>> FetchResponse::jsonForBindings(ScriptExecutionContext& context, JSC::JSValue data, Init&& init)
{
    auto* globalObject = context.globalObject();
    if (!globalObject)
        return Exception { ExceptionCode::InvalidStateError, "Context is stopped"_s };

    String jsonString = JSC::JSONStringify(globalObject, data, 0);
    if (jsonString.isNull())
        return Exception { ExceptionCode::TypeError, "Value doesn't have a JSON representation"_s };

    FetchBodyWithType body { FetchBody(WTFMove(jsonString)), "application/json"_s };
    return FetchResponse::create(context, WTFMove(body), WTFMove(init));
}

FetchResponse::FetchResponse(ScriptExecutionContext* context, std::optional<FetchBody>&& body, Ref<FetchHeaders>&& headers, ResourceResponse&& response)
    : FetchBodyOwner(context, WTFMove(body), WTFMove(headers))
    , m_internalResponse(WTFMove(response))
{
}

ExceptionOr<Ref<FetchResponse>> FetchResponse::clone()
{
    if (isDisturbedOrLocked())
        return Exception { ExceptionCode::TypeError, "Body is disturbed or locked"_s };

    // If loading, let's create a stream so that data is teed on both clones.
    RefPtr context = scriptExecutionContext();
    if (isLoading() && !m_readableStreamSource) {
        auto* globalObject = context ? context->globalObject() : nullptr;
        if (!globalObject)
            return Exception { ExceptionCode::InvalidStateError, "Context is stopped"_s };

        auto voidOrException = createReadableStream(*globalObject);
        if (voidOrException.hasException()) [[unlikely]]
            return voidOrException.releaseException();
    }

    // Synthetic responses do not store headers in m_internalResponse.
    if (m_internalResponse.type() == ResourceResponse::Type::Default)
        m_internalResponse.setHTTPHeaderFields(HTTPHeaderMap { headers().internalHeaders() });

    Ref headers = FetchHeaders::create(this->headers());
    auto clone = FetchResponse::create(context.get(), std::nullopt, WTFMove(headers), ResourceResponse { m_internalResponse });
    clone->cloneBody(*this);
    clone->m_opaqueLoadIdentifier = m_opaqueLoadIdentifier;
    clone->m_bodySizeWithPadding = m_bodySizeWithPadding;
    return clone;
}

void FetchResponse::addAbortSteps(Ref<AbortSignal>&& signal)
{
    m_abortSignal = signal.copyRef();
    signal->addAlgorithm([weakThis = WeakPtr { *this }](JSC::JSValue) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;

        protectedThis->m_abortSignal = nullptr;

        protectedThis->setLoadingError(Exception { ExceptionCode::AbortError, "Fetch is aborted"_s });

        if (CheckedPtr loader = protectedThis->m_loader.get()) {
            if (auto callback = loader->takeNotificationCallback())
                callback(Exception { ExceptionCode::AbortError, "Fetch is aborted"_s });

            if (auto callback = loader->takeConsumeDataCallback())
                callback(Exception { ExceptionCode::AbortError, "Fetch is aborted"_s });
        }

        if (RefPtr readableStreamSource = protectedThis->m_readableStreamSource) {
            if (!readableStreamSource->isCancelling())
                readableStreamSource->error(*protectedThis->loadingException());
            protectedThis->m_readableStreamSource = nullptr;
        }
        if (protectedThis->m_body)
            protectedThis->m_body->loadingFailed(*protectedThis->loadingException());

        if (auto loader = WTFMove(protectedThis->m_loader))
            loader->stop();
        if (auto bodyLoader = WTFMove(protectedThis->m_bodyLoader))
            bodyLoader->stop();
    });
}

Ref<FetchResponse> FetchResponse::createFetchResponse(ScriptExecutionContext& context, FetchRequest& request, NotificationCallback&& responseCallback)
{
    auto response = adoptRef(*new FetchResponse(&context, FetchBody { }, FetchHeaders::create(FetchHeaders::Guard::Immutable), { }));
    response->suspendIfNeeded();

    response->body().checkedConsumer()->setAsLoading();

    response->addAbortSteps(request.signal());

    response->m_loader = makeUnique<Loader>(response.get(), WTFMove(responseCallback));
    return response;
}

void FetchResponse::fetch(ScriptExecutionContext& context, FetchRequest& request, NotificationCallback&& responseCallback, const String& initiator)
{
    if (request.isReadableStreamBody()) {
        responseCallback(Exception { ExceptionCode::NotSupportedError, "ReadableStream uploading is not supported"_s });
        return;
    }

    if (request.hasReadableStreamBody()) {
        request.body().convertReadableStreamToArrayBuffer(request, [context = Ref { context }, weakRequest = WeakPtr { request }, responseCallback = WTFMove(responseCallback), initiator](auto&& exception) mutable {
            if (!!exception) {
                responseCallback(WTFMove(*exception));
                return;
            }

            Ref protectedRequest = *weakRequest;
            Ref response = createFetchResponse(context.get(), protectedRequest.get(), WTFMove(responseCallback));
            response->startLoader(context.get(), protectedRequest.get(), initiator);
        });
        return;
    }

    Ref response = createFetchResponse(context, request, WTFMove(responseCallback));
    response->startLoader(context, request, initiator);
}

void FetchResponse::startLoader(ScriptExecutionContext& context, FetchRequest& request, const String& initiator)
{
    InspectorInstrumentation::willFetch(context, request.url().string());

    if (CheckedPtr loader = m_loader.get(); loader && loader->start(context, request, initiator))
        return;
    m_loader = nullptr;
}

const String& FetchResponse::url() const
{
    if (m_responseURL.isNull()) {
        URL url = filteredResponse().url();
        url.removeFragmentIdentifier();
        m_responseURL = url.string();
    }
    return m_responseURL;
}

const ResourceResponse& FetchResponse::filteredResponse() const
{
    if (m_filteredResponse)
        return m_filteredResponse.value();
    return m_internalResponse;
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(FetchResponse::Loader);

void FetchResponse::Loader::didSucceed(const NetworkLoadMetrics& metrics)
{
    Ref response = m_response.get();
    ASSERT(response->hasPendingActivity());

    response->didSucceed(metrics);

    if (m_loader->isStarted())
        response->m_loader = nullptr;
}

void FetchResponse::Loader::didFail(const ResourceError& error)
{
    Ref response = m_response.get();
    ASSERT(response->hasPendingActivity());

    response->setLoadingError(ResourceError { error });
    response->processReceivedError();

    // Check whether didFail is called as part of FetchLoader::start.
    if (m_loader && m_loader->isStarted())
        response->m_loader = nullptr;
}

static std::atomic<uint64_t> nextOpaqueLoadIdentifier;
void FetchResponse::setReceivedInternalResponse(const ResourceResponse& resourceResponse, FetchOptions::Credentials credentials)
{
    if (m_hasInitializedInternalResponse)
        return;

    m_hasInitializedInternalResponse = true;
    auto performCheck = credentials == FetchOptions::Credentials::Include ? ResourceResponse::PerformExposeAllHeadersCheck::No : ResourceResponse::PerformExposeAllHeadersCheck::Yes;
    m_filteredResponse = ResourceResponseBase::filter(resourceResponse, performCheck);
    m_internalResponse = resourceResponse;
    m_internalResponse.setType(m_filteredResponse->type());
    if (resourceResponse.tainting() == ResourceResponse::Tainting::Opaque) {
        m_opaqueLoadIdentifier = ++nextOpaqueLoadIdentifier;
        setBodyAsOpaque();
    }

    m_headers->filterAndFill(m_filteredResponse->httpHeaderFields(), FetchHeaders::Guard::Response);
}

FetchResponse::Loader::Loader(FetchResponse& response, NotificationCallback&& responseCallback)
    : m_response(response)
    , m_responseCallback(WTFMove(responseCallback))
    , m_pendingActivity(response.makePendingActivity(response))
{
}

FetchResponse::Loader::~Loader() = default;

void FetchResponse::Loader::didReceiveResponse(const ResourceResponse& resourceResponse)
{
    Ref response = m_response.get();
    response->setReceivedInternalResponse(resourceResponse, m_credentials);

    if (auto responseCallback = WTFMove(m_responseCallback))
        responseCallback(WTFMove(response));
}

void FetchResponse::Loader::didReceiveData(const SharedBuffer& buffer)
{
    Ref response = m_response.get();
    ASSERT(response->m_readableStreamSource || m_consumeDataCallback);

    if (m_consumeDataCallback) {
        auto chunk = buffer.span();
        m_consumeDataCallback(&chunk);
        return;
    }

    Ref source = *response->m_readableStreamSource;

    CheckedRef consumer = response->body().consumer();
    if (!source->isPulling()) {
        consumer->append(buffer);
        return;
    }

    if (consumer->hasData() && !source->enqueue(consumer->takeAsArrayBuffer())) {
        stop();
        return;
    }
    if (!source->enqueue(buffer.tryCreateArrayBuffer())) {
        stop();
        return;
    }
    source->resolvePullPromise();
}

bool FetchResponse::Loader::start(ScriptExecutionContext& context, const FetchRequest& request, const String& initiator)
{
    m_credentials = request.fetchOptions().credentials;
    m_loader = makeUnique<FetchLoader>(*this, &m_response->m_body->consumer());
    CheckedRef loader = *m_loader;
    loader->start(context, request, initiator);

    if (!loader->isStarted())
        return false;

    if (m_shouldStartStreaming) {
        auto data = loader->startStreaming();
        ASSERT_UNUSED(data, !data);
    }

    return true;
}

void FetchResponse::Loader::stop()
{
    m_responseCallback = { };
    if (CheckedPtr loader = m_loader.get())
        loader->stop();
}

void FetchResponse::Loader::consumeDataByChunk(ConsumeDataByChunkCallback&& consumeDataCallback)
{
    ASSERT(!m_consumeDataCallback);
    m_consumeDataCallback = WTFMove(consumeDataCallback);
    auto data = startStreaming();
    if (!data)
        return;

    auto contiguousBuffer = data->makeContiguous();
    auto chunk = contiguousBuffer->span();
    m_consumeDataCallback(&chunk);
}

FetchResponse::ResponseData FetchResponse::consumeBody()
{
    ASSERT(!isBodyReceivedByChunk());

    if (isBodyNull())
        return nullptr;

    ASSERT(!m_isDisturbed);
    m_isDisturbed = true;

    return body().take();
}

void FetchResponse::markAsUsedForPreload()
{
    ASSERT(!m_isDisturbed);
    m_isDisturbed = true;
    m_isUsedForPreload = true;
}

void FetchResponse::consumeBodyReceivedByChunk(ConsumeDataByChunkCallback&& callback)
{
    ASSERT(isBodyReceivedByChunk());
    ASSERT(!isDisturbed());
    m_isDisturbed = true;

    if (hasReadableStreamBody()) {
        m_body->checkedConsumer()->extract(*m_body->protectedReadableStream(), WTFMove(callback));
        return;
    }

    ASSERT(isLoading());
    checkedLoader()->consumeDataByChunk(WTFMove(callback));
}

void FetchResponse::setBodyData(ResponseData&& data, uint64_t bodySizeWithPadding)
{
    m_bodySizeWithPadding = bodySizeWithPadding;
    WTF::switchOn(data,
        [this](Ref<FormData>& formData) {
            if (isBodyNull())
                setBody({ });
            body().setAsFormData(WTFMove(formData));
        },
        [this](Ref<SharedBuffer>& buffer) {
            if (isBodyNull())
                setBody({ });
            body().checkedConsumer()->setData(WTFMove(buffer));
        },
        [](std::nullptr_t&) {
        }
    );
}

void FetchResponse::consumeChunk(Ref<JSC::Uint8Array>&& chunk)
{
    body().checkedConsumer()->append(SharedBuffer::create(chunk->span()));
}

void FetchResponse::consumeBodyAsStream()
{
    ASSERT(m_readableStreamSource);
    if (!isLoading()) {
        FetchBodyOwner::consumeBodyAsStream();
        return;
    }

    ASSERT(m_loader);
    auto data = checkedLoader()->startStreaming();
    if (data) {
        Ref readableStreamSource = *m_readableStreamSource;
        if (!readableStreamSource->enqueue(data->tryCreateArrayBuffer())) {
            stop();
            return;
        }
        readableStreamSource->resolvePullPromise();
    }
}

void FetchResponse::closeStream()
{
    ASSERT(m_readableStreamSource);
    Ref { *m_readableStreamSource }->close();
    m_readableStreamSource = nullptr;
}

void FetchResponse::cancelStream()
{
    if (isAllowedToRunScript() && hasReadableStreamBody()) {
        body().protectedReadableStream()->cancel(Exception { ExceptionCode::AbortError, "load is cancelled"_s });
        return;
    }
    cancel();
}

void FetchResponse::feedStream()
{
    ASSERT(m_readableStreamSource);
    bool shouldCloseStream = !m_loader;

    CheckedRef consumer = body().consumer();
    if (consumer->hasData()) {
        Ref readableStreamSource = *m_readableStreamSource;
        if (!readableStreamSource->enqueue(consumer->takeAsArrayBuffer())) {
            stop();
            return;
        }
        if (!shouldCloseStream) {
            readableStreamSource->resolvePullPromise();
            return;
        }
    } else if (!shouldCloseStream)
        return;

    closeStream();
}

RefPtr<FragmentedSharedBuffer> FetchResponse::Loader::startStreaming()
{
    if (CheckedPtr loader = m_loader.get())
        return loader->startStreaming();

    m_shouldStartStreaming = true;
    return nullptr;
}

void FetchResponse::cancel()
{
    m_isDisturbed = true;
    stop();
}

void FetchResponse::stop()
{
    RefPtr<FetchResponse> protectedThis(this);
    FetchBodyOwner::stop();
    if (auto loader = WTFMove(m_loader))
        loader->stop();
    if (auto bodyLoader = WTFMove(m_bodyLoader))
        bodyLoader->stop();
}

void FetchResponse::loadBody()
{
    if (m_bodyLoader)
        m_bodyLoader->start();
}

void FetchResponse::setBodyLoader(UniqueRef<FetchResponseBodyLoader>&& bodyLoader)
{
    ASSERT(!m_loader);
    ASSERT(isBodyNull());

    setBody({ });
    body().consumer().setAsLoading();
    m_bodyLoader = bodyLoader.moveToUniquePtr();
}

void FetchResponse::receivedError(Exception&& exception)
{
    setLoadingError(WTFMove(exception));
    processReceivedError();
}

void FetchResponse::receivedError(ResourceError&& error)
{
    setLoadingError(WTFMove(error));
    processReceivedError();
}

void FetchResponse::processReceivedError()
{
    if (CheckedPtr loader = m_loader.get()) {
        if (auto callback = loader->takeNotificationCallback())
            callback(*loadingException());
        else if (auto callback = loader->takeConsumeDataCallback())
            callback(*loadingException());
    }

    if (RefPtr readableStreamSource = m_readableStreamSource) {
        if (!readableStreamSource->isCancelling())
            readableStreamSource->error(*loadingException());
        m_readableStreamSource = nullptr;
    }

    if (m_body)
        m_body->loadingFailed(*loadingException());
}

void FetchResponse::didSucceed(const NetworkLoadMetrics& metrics)
{
    setNetworkLoadMetrics(metrics);

    if (CheckedPtr loader = m_loader.get()) {
        if (auto consumeDataCallback = loader->takeConsumeDataCallback())
            consumeDataCallback(nullptr);
    }

    if (RefPtr readableStreamSource = m_readableStreamSource) {
        CheckedRef consumer = body().consumer();
        if (consumer->hasData())
            readableStreamSource->enqueue(consumer->takeAsArrayBuffer());

        closeStream();
    }

    if (m_body)
        m_body->loadingSucceeded(contentType());
}

void FetchResponse::receivedData(Ref<SharedBuffer>&& buffer)
{
    body().checkedConsumer()->append(buffer.get());
}

ResourceResponse FetchResponse::resourceResponse() const
{
    auto response = m_internalResponse;

    if (headers().guard() != FetchHeaders::Guard::Immutable) {
        // FIXME: Add a setHTTPHeaderFields on ResourceResponseBase.
        for (auto& header : headers().internalHeaders()) {
            if (header.keyAsHTTPHeaderName)
                response.setHTTPHeaderField(*header.keyAsHTTPHeaderName, header.value);
            else
                response.setUncommonHTTPHeaderField(header.key, header.value);
        }
    }

    return response;
}

bool FetchResponse::isCORSSameOrigin() const
{
    // https://html.spec.whatwg.org/#cors-same-origin
    // A response whose type is "basic", "cors", or "default" is CORS-same-origin.
    switch (type()) {
    case ResourceResponse::Type::Basic:
    case ResourceResponse::Type::Cors:
    case ResourceResponse::Type::Default:
        return true;
    default:
        return false;
    }
}

bool FetchResponse::hasWasmMIMEType() const
{
    return MIMETypeRegistry::isSupportedWebAssemblyMIMEType(m_headers->fastGet(HTTPHeaderName::ContentType));
}

} // namespace WebCore
