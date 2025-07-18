/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
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
#include "CryptoAlgorithmECDH.h"

#include "CryptoAlgorithmEcKeyParams.h"
#include "CryptoAlgorithmEcdhKeyDeriveParams.h"
#include "CryptoKeyEC.h"
#include "ExceptionOr.h"
#include "ScriptExecutionContext.h"
#if HAVE(SWIFT_CPP_INTEROP)
#include <pal/PALSwift.h>
#endif
namespace WebCore {

Ref<CryptoAlgorithm> CryptoAlgorithmECDH::create()
{
    return adoptRef(*new CryptoAlgorithmECDH);
}

CryptoAlgorithmIdentifier CryptoAlgorithmECDH::identifier() const
{
    return s_identifier;
}

void CryptoAlgorithmECDH::generateKey(const CryptoAlgorithmParameters& parameters, bool extractable, CryptoKeyUsageBitmap usages, KeyOrKeyPairCallback&& callback, ExceptionCallback&& exceptionCallback, ScriptExecutionContext&)
{
    const auto& ecParameters = downcast<CryptoAlgorithmEcKeyParams>(parameters);

    if (usages & (CryptoKeyUsageEncrypt | CryptoKeyUsageDecrypt | CryptoKeyUsageSign | CryptoKeyUsageVerify | CryptoKeyUsageWrapKey | CryptoKeyUsageUnwrapKey)) {
        exceptionCallback(ExceptionCode::SyntaxError);
        return;
    }
    auto result = CryptoKeyEC::generatePair(CryptoAlgorithmIdentifier::ECDH, ecParameters.namedCurve, extractable, usages);
    if (result.hasException()) {
        exceptionCallback(result.releaseException().code());
        return;
    }

    auto pair = result.releaseReturnValue();
    pair.publicKey->setUsagesBitmap(0);
    pair.privateKey->setUsagesBitmap(pair.privateKey->usagesBitmap() & (CryptoKeyUsageDeriveKey | CryptoKeyUsageDeriveBits));
    callback(WTFMove(pair));
}

void CryptoAlgorithmECDH::deriveBits(const CryptoAlgorithmParameters& parameters, Ref<CryptoKey>&& baseKey, std::optional<size_t> length, VectorCallback&& callback, ExceptionCallback&& exceptionCallback, ScriptExecutionContext& context, WorkQueue& workQueue)
{
    auto& ecParameters = downcast<CryptoAlgorithmEcdhKeyDeriveParams>(parameters);

    if (baseKey->type() != CryptoKey::Type::Private) {
        exceptionCallback(ExceptionCode::InvalidAccessError);
        return;
    }
    ASSERT(ecParameters.publicKey);
    if (ecParameters.publicKey->type() != CryptoKey::Type::Public) {
        exceptionCallback(ExceptionCode::InvalidAccessError);
        return;
    }
    if (baseKey->algorithmIdentifier() != ecParameters.publicKey->algorithmIdentifier()) {
        exceptionCallback(ExceptionCode::InvalidAccessError);
        return;
    }
    auto& ecBaseKey = downcast<CryptoKeyEC>(baseKey.get());
    auto& ecPublicKey = downcast<CryptoKeyEC>(*(ecParameters.publicKey.get()));
    if (ecBaseKey.namedCurve() != ecPublicKey.namedCurve()) {
        exceptionCallback(ExceptionCode::InvalidAccessError);
        return;
    }

    // Return an empty string doesn't make much sense, but truncating either at all.
    // https://github.com/w3c/webcrypto/issues/369
    if (length && !(*length)) {
        // Avoid executing the key-derivation, since we are going to return an empty string.
        callback({ });
        return;
    }

    auto unifiedCallback = [callback = WTFMove(callback), exceptionCallback = WTFMove(exceptionCallback)](std::optional<Vector<uint8_t>>&& derivedKey, std::optional<size_t> length) mutable {
        if (!derivedKey) {
            exceptionCallback(ExceptionCode::OperationError);
            return;
        }
        if (!length) {
            callback(WTFMove(*derivedKey));
            return;
        }
        auto lengthInBytes = std::ceil(*length / 8.);
        if (lengthInBytes > (*derivedKey).size()) {
            exceptionCallback(ExceptionCode::OperationError);
            return;
        }
        (*derivedKey).shrink(lengthInBytes);
        callback(WTFMove(*derivedKey));
    };
    // This is a special case that can't use dispatchOperation() because it bundles
    // the result validation and callback dispatch into unifiedCallback.
    workQueue.dispatch(
        [baseKey = WTFMove(baseKey), publicKey = ecParameters.publicKey, length, unifiedCallback = WTFMove(unifiedCallback), contextIdentifier = context.identifier()]() mutable {
            auto derivedKey = platformDeriveBits(downcast<CryptoKeyEC>(baseKey.get()), downcast<CryptoKeyEC>(*publicKey));
            ScriptExecutionContext::postTaskTo(contextIdentifier, [derivedKey = WTFMove(derivedKey), length, unifiedCallback = WTFMove(unifiedCallback)](auto&) mutable {
                unifiedCallback(WTFMove(derivedKey), length);
            });
        });
}

void CryptoAlgorithmECDH::importKey(CryptoKeyFormat format, KeyData&& data, const CryptoAlgorithmParameters& parameters, bool extractable, CryptoKeyUsageBitmap usages, KeyCallback&& callback, ExceptionCallback&& exceptionCallback)
{
    const auto& ecParameters = downcast<CryptoAlgorithmEcKeyParams>(parameters);
    RefPtr<CryptoKeyEC> result;
    switch (format) {
    case CryptoKeyFormat::Jwk: {
        JsonWebKey key = WTFMove(std::get<JsonWebKey>(data));

        bool isUsagesAllowed = false;
        if (!key.d.isNull()) {
            isUsagesAllowed = isUsagesAllowed || !(usages ^ CryptoKeyUsageDeriveKey);
            isUsagesAllowed = isUsagesAllowed || !(usages ^ CryptoKeyUsageDeriveBits);
            isUsagesAllowed = isUsagesAllowed || !(usages ^ (CryptoKeyUsageDeriveKey | CryptoKeyUsageDeriveBits));
        }
        isUsagesAllowed = isUsagesAllowed || !usages;
        if (!isUsagesAllowed) {
            exceptionCallback(ExceptionCode::SyntaxError);
            return;
        }

        if (usages && !key.use.isNull() && key.use != "enc"_s) {
            exceptionCallback(ExceptionCode::DataError);
            return;
        }

        result = CryptoKeyEC::importJwk(ecParameters.identifier, ecParameters.namedCurve, WTFMove(key), extractable, usages);
        break;
    }
    case CryptoKeyFormat::Raw:
        if (usages) {
            exceptionCallback(ExceptionCode::SyntaxError);
            return;
        }
        result = CryptoKeyEC::importRaw(ecParameters.identifier, ecParameters.namedCurve, WTFMove(std::get<Vector<uint8_t>>(data)), extractable, usages);
        break;
    case CryptoKeyFormat::Spki:
        if (usages) {
            exceptionCallback(ExceptionCode::SyntaxError);
            return;
        }
        result = CryptoKeyEC::importSpki(ecParameters.identifier, ecParameters.namedCurve, WTFMove(std::get<Vector<uint8_t>>(data)), extractable, usages);
        break;
    case CryptoKeyFormat::Pkcs8:
        if (usages && (usages ^ CryptoKeyUsageDeriveKey) && (usages ^ CryptoKeyUsageDeriveBits) && (usages ^ (CryptoKeyUsageDeriveKey | CryptoKeyUsageDeriveBits))) {
            exceptionCallback(ExceptionCode::SyntaxError);
            return;
        }
        result = CryptoKeyEC::importPkcs8(ecParameters.identifier, ecParameters.namedCurve, WTFMove(std::get<Vector<uint8_t>>(data)), extractable, usages);
        break;
    }
    if (!result) {
        exceptionCallback(ExceptionCode::DataError);
        return;
    }

    callback(*result);
}

void CryptoAlgorithmECDH::exportKey(CryptoKeyFormat format, Ref<CryptoKey>&& key, KeyDataCallback&& callback, ExceptionCallback&& exceptionCallback)
{
    const auto& ecKey = downcast<CryptoKeyEC>(key.get());
    if (!ecKey.keySizeInBits()) {
        exceptionCallback(ExceptionCode::OperationError);
        return;
    }

    KeyData result;
    switch (format) {
    case CryptoKeyFormat::Jwk: {
        auto jwk = ecKey.exportJwk();
        if (jwk.hasException()) {
            exceptionCallback(jwk.releaseException().code());
            return;
        }
        result = jwk.releaseReturnValue();
        break;
    }
    case CryptoKeyFormat::Raw: {
        auto raw = ecKey.exportRaw();
        if (raw.hasException()) {
            exceptionCallback(raw.releaseException().code());
            return;
        }
        result = raw.releaseReturnValue();
        break;
    }
    case CryptoKeyFormat::Spki: {
        auto spki = ecKey.exportSpki();
        if (spki.hasException()) {
            exceptionCallback(spki.releaseException().code());
            return;
        }
        result = spki.releaseReturnValue();
        break;
    }
    case CryptoKeyFormat::Pkcs8: {
        auto pkcs8 = ecKey.exportPkcs8();
        if (pkcs8.hasException()) {
            exceptionCallback(pkcs8.releaseException().code());
            return;
        }
        result = pkcs8.releaseReturnValue();
        break;
    }
    }

    callback(format, WTFMove(result));
}

} // namespace WebCore
