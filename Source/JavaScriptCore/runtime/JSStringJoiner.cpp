/*
 * Copyright (C) 2012-2024 Apple Inc. All rights reserved.
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
#include "JSStringJoiner.h"

#include "JSCJSValueInlines.h"
#include <charconv>
#include <wtf/text/ParsingUtilities.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

JSStringJoiner::~JSStringJoiner() = default;

template<typename CharacterType>
static inline void appendStringToData(std::span<CharacterType>& data, StringView string)
{
    if constexpr (std::is_same_v<CharacterType, LChar>) {
        ASSERT(string.is8Bit());
        string.getCharacters8(data);
    } else
        string.getCharacters(data);
    skip(data, string.length());
}

template<typename OutputCharacterType, typename SeparatorCharacterType>
static inline void appendStringToData(std::span<OutputCharacterType>& data, std::span<const SeparatorCharacterType> separator)
{
    StringImpl::copyCharacters(data, separator);
    skip(data, separator.size());
}

template<typename CharacterType>
static inline void appendStringToData(std::span<CharacterType>& data, int32_t value)
{
    if constexpr (std::is_same_v<CharacterType, LChar>) {
        auto result = std::to_chars(std::bit_cast<char*>(data.data()), std::bit_cast<char*>(data.data() + data.size()), value);
        ASSERT(result.ec != std::errc::value_too_large);
        skip(data, result.ptr - std::bit_cast<char*>(data.data()));
    } else {
        WTF::StringTypeAdapter<int32_t> adapter { value };
        adapter.writeTo(data);
        skip(data, adapter.length());
    }
}

template<typename CharacterType>
static inline void appendStringToDataWithOneCharacterSeparatorRepeatedly(std::span<CharacterType>& data, char16_t separatorCharacter, StringView string, unsigned count)
{
#if OS(DARWIN)
    if constexpr (std::is_same_v<CharacterType, LChar>) {
        ASSERT(string.is8Bit());
        if (count > 4) {
            switch (string.length() + 1) {
            case 16: {
                alignas(16) LChar pattern[16];
                pattern[0] = separatorCharacter;
                string.getCharacters8(std::span { pattern }.subspan(1));
                size_t fillLength = count * 16;
                memset_pattern16(data.data(), pattern, fillLength);
                skip(data, fillLength);
                return;
            }
            case 8: {
                alignas(8) LChar pattern[8];
                pattern[0] = separatorCharacter;
                string.getCharacters8(std::span { pattern }.subspan(1));
                size_t fillLength = count * 8;
                memset_pattern8(data.data(), pattern, fillLength);
                skip(data, fillLength);
                return;
            }
            case 4: {
                alignas(4) LChar pattern[4];
                pattern[0] = separatorCharacter;
                string.getCharacters8(std::span { pattern }.subspan(1));
                size_t fillLength = count * 4;
                memset_pattern4(data.data(), pattern, fillLength);
                skip(data, fillLength);
                return;
            }
            default:
                break;
            }
        }
    }
#endif

    while (count--) {
        consume(data) = separatorCharacter;
        appendStringToData(data, string);
    }
}

template<typename OutputCharacterType, typename SeparatorCharacterType>
static inline String joinStrings(const JSStringJoiner::Entries& strings, std::span<const SeparatorCharacterType> separator, unsigned joinedLength)
{
    ASSERT(joinedLength);

    std::span<OutputCharacterType> data;
    String result = StringImpl::tryCreateUninitialized(joinedLength, data);
    if (result.isNull()) [[unlikely]]
        return result;

    unsigned size = strings.size();

    switch (separator.size()) {
    case 0: {
        for (unsigned i = 0; i < size; ++i) {
            const auto& entry = strings[i];
            unsigned count = entry.m_additional;
            do {
                appendStringToData(data, entry.m_view.view);
            } while (count--);
        }
        break;
    }
    case 1: {
        OutputCharacterType separatorCharacter = separator.data()[0];
        {
            const auto& entry = strings[0];
            unsigned count = entry.m_additional;
            appendStringToData(data, entry.m_view.view);
            appendStringToDataWithOneCharacterSeparatorRepeatedly(data, separatorCharacter, entry.m_view.view, count);
        }
        for (unsigned i = 1; i < size; ++i) {
            const auto& entry = strings[i];
            unsigned count = entry.m_additional;
            appendStringToDataWithOneCharacterSeparatorRepeatedly(data, separatorCharacter, entry.m_view.view, count + 1);
        }
        break;
    }
    default: {
        {
            const auto& entry = strings[0];
            unsigned count = entry.m_additional;
            appendStringToData(data, entry.m_view.view);
            while (count--) {
                appendStringToData(data, separator);
                appendStringToData(data, entry.m_view.view);
            }
        }
        for (unsigned i = 1; i < size; ++i) {
            const auto& entry = strings[i];
            unsigned count = entry.m_additional;
            do {
                appendStringToData(data, separator);
                appendStringToData(data, entry.m_view.view);
            } while (count--);
        }
        break;
    }
    }
    ASSERT(data.data() == result.span<OutputCharacterType>().data() + joinedLength);

    return result;
}

template<typename OutputCharacterType, typename SeparatorCharacterType>
static inline String joinStrings(JSGlobalObject* globalObject, const WriteBarrier<Unknown>* strings, unsigned size, std::span<const SeparatorCharacterType> separator, unsigned joinedLength)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!joinedLength)
        return emptyString();

    std::span<OutputCharacterType> data;
    String result = StringImpl::tryCreateUninitialized(joinedLength, data);
    if (result.isNull()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }

    switch (separator.size()) {
    case 0: {
        for (unsigned i = 0; i < size; ++i) {
            JSValue value = strings[i].get();
            if (value.isString()) {
                auto view = asString(value)->view(globalObject);
                RETURN_IF_EXCEPTION(scope, String());
                appendStringToData(data, view);
            } else {
                ASSERT(value.isInt32());
                appendStringToData(data, value.asInt32());
            }
        }
        break;
    }
    default: {
        JSValue value = strings[0].get();
        if (value.isString()) {
            auto view = asString(value)->view(globalObject);
            RETURN_IF_EXCEPTION(scope, String());
            appendStringToData(data, view);
        } else {
            ASSERT(value.isInt32());
            appendStringToData(data, value.asInt32());
        }

        for (unsigned i = 1; i < size; ++i) {
            JSValue value = strings[i].get();
            if (value.isString()) {
                auto view = asString(value)->view(globalObject);
                RETURN_IF_EXCEPTION(scope, String());
                appendStringToData(data, separator);
                appendStringToData(data, view);
            } else {
                ASSERT(value.isInt32());
                appendStringToData(data, separator);
                appendStringToData(data, value.asInt32());
            }
        }
        break;
    }
    }
    ASSERT(data.data() == result.span<OutputCharacterType>().data() + joinedLength);

    return result;
}

inline unsigned JSStringJoiner::joinedLength(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!m_stringsCount)
        return 0;

    CheckedInt32 separatorLength = m_separator.length();
    CheckedInt32 totalSeparatorsLength = separatorLength * (m_stringsCount - 1);
    CheckedInt32 totalLength = totalSeparatorsLength + m_accumulatedStringsLength;
    if (totalLength.hasOverflowed()) {
        throwOutOfMemoryError(globalObject, scope);
        return 0;
    }
    return totalLength;
}

JSString* JSStringJoiner::joinImpl(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (m_hasOverflowed) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    ASSERT(m_strings.size() <= m_strings.capacity());

    unsigned length = joinedLength(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    if (!length)
        return jsEmptyString(vm);

    String result;
    if (m_isAll8Bit)
        result = joinStrings<LChar>(m_strings, m_separator.span8(), length);
    else {
        if (m_separator.is8Bit())
            result = joinStrings<char16_t>(m_strings, m_separator.span8(), length);
        else
            result = joinStrings<char16_t>(m_strings, m_separator.span16(), length);
    }

    if (result.isNull()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }

    return jsString(vm, WTFMove(result));
}

JSString* JSOnlyStringsAndInt32sJoiner::joinImpl(JSGlobalObject* globalObject, const WriteBarrier<Unknown>* data, unsigned length)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!length)
        return jsEmptyString(vm);

    CheckedInt32 separatorLength = m_separator.length();
    CheckedInt32 totalSeparatorsLength = separatorLength * (CheckedInt32(length) - 1);
    CheckedInt32 totalLength = totalSeparatorsLength + m_accumulatedStringsLength;
    if (totalLength.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }

    String result;
    if (m_isAll8Bit)
        result = joinStrings<LChar>(globalObject, data, length, m_separator.span8(), totalLength);
    else {
        if (m_separator.is8Bit())
            result = joinStrings<char16_t>(globalObject, data, length, m_separator.span8(), totalLength);
        else
            result = joinStrings<char16_t>(globalObject, data, length, m_separator.span16(), totalLength);
    }

    RETURN_IF_EXCEPTION(scope, { });

    return jsString(vm, WTFMove(result));
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
