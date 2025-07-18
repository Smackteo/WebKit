/*
 * Copyright (C) 2017-2024 Apple Inc. All rights reserved.
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

#include <wtf/Compiler.h>

#include <wtf/dtoa.h>
#include <wtf/text/IntegerToStringConversion.h>
#include <wtf/text/StringConcatenate.h>

namespace WTF {

template<typename Integer>
class StringTypeAdapter<Integer, typename std::enable_if_t<std::is_integral_v<Integer>>> {
public:
    StringTypeAdapter(Integer number)
        : m_number { number }
    {
    }

    unsigned length() const { return lengthOfIntegerAsString(m_number); }
    bool is8Bit() const { return true; }
    template<typename CharacterType>
    void writeTo(std::span<CharacterType> destination) const { writeIntegerToBuffer(m_number, destination); }

private:
    Integer m_number;
};

template<typename Enum>
class StringTypeAdapter<Enum, typename std::enable_if_t<std::is_enum_v<Enum>>> {
using UnderlyingType = typename std::underlying_type_t<Enum>;
public:
    StringTypeAdapter(Enum enumValue)
        : m_enum { enumValue }
    {
    }

    unsigned length() const { return lengthOfIntegerAsString(static_cast<UnderlyingType>(m_enum)); }
    bool is8Bit() const { return true; }
    template<typename CharacterType>
    void writeTo(std::span<CharacterType> destination) const { writeIntegerToBuffer(static_cast<UnderlyingType>(m_enum), destination); }

private:
    Enum m_enum;
};

template<typename FloatingPoint>
class StringTypeAdapter<FloatingPoint, typename std::enable_if_t<std::is_floating_point<FloatingPoint>::value>> {
public:
    StringTypeAdapter(FloatingPoint number)
    {
        m_length = numberToStringAndSize(number, m_buffer).size();
    }

    unsigned length() const { return m_length; }
    bool is8Bit() const { return true; }
    template<typename CharacterType> void writeTo(std::span<CharacterType> destination) const { StringImpl::copyCharacters(destination, span()); }

private:
    std::span<const LChar> span() const LIFETIME_BOUND { return byteCast<LChar>(std::span { m_buffer }).first(m_length); }

    NumberToStringBuffer m_buffer;
    unsigned m_length;
};

class FormattedNumber {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FormattedNumber);
public:
    static FormattedNumber fixedPrecision(double number, unsigned significantFigures = 6, TrailingZerosPolicy trailingZerosTruncatingPolicy = TrailingZerosPolicy::Truncate)
    {
        FormattedNumber numberFormatter;
        numberFormatter.m_length = numberToFixedPrecisionString(number, significantFigures, numberFormatter.m_buffer, trailingZerosTruncatingPolicy == TrailingZerosPolicy::Truncate).size();
        return numberFormatter;
    }

    static FormattedNumber fixedWidth(double number, unsigned decimalPlaces)
    {
        FormattedNumber numberFormatter;
        numberFormatter.m_length = numberToFixedWidthString(number, decimalPlaces, numberFormatter.m_buffer).size();
        return numberFormatter;
    }

    unsigned length() const { return m_length; }
    const LChar* buffer() const LIFETIME_BOUND { return byteCast<LChar>(&m_buffer[0]); }
    std::span<const LChar> span() const LIFETIME_BOUND { return byteCast<LChar>(std::span { m_buffer }).first(m_length); }

private:
    NumberToStringBuffer m_buffer;
    unsigned m_length;
};

template<> class StringTypeAdapter<FormattedNumber> {
public:
    StringTypeAdapter(const FormattedNumber& number)
        : m_number { number }
    {
    }

    unsigned length() const { return m_number.length(); }
    bool is8Bit() const { return true; }
    template<typename CharacterType> void writeTo(std::span<CharacterType> destination) const { StringImpl::copyCharacters(destination, m_number.span()); }

private:
    const FormattedNumber& m_number;
};

class FormattedCSSNumber {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FormattedCSSNumber);
public:
    static FormattedCSSNumber create(double number)
    {
        FormattedCSSNumber numberFormatter;
        numberFormatter.m_length = numberToCSSString(number, numberFormatter.m_buffer).size();
        return numberFormatter;
    } 

    unsigned length() const { return m_length; }
    const LChar* buffer() const LIFETIME_BOUND { return byteCast<LChar>(&m_buffer[0]); }
    std::span<const LChar> span() const LIFETIME_BOUND { return byteCast<LChar>(std::span { m_buffer }).first(m_length); }

private:
    NumberToCSSStringBuffer m_buffer;
    unsigned m_length;
};

template<> class StringTypeAdapter<FormattedCSSNumber> {
public:
    StringTypeAdapter(const FormattedCSSNumber& number)
        : m_number { number }
    {
    }

    unsigned length() const { return m_number.length(); }
    bool is8Bit() const { return true; }
    template<typename CharacterType> void writeTo(std::span<CharacterType> destination) const { StringImpl::copyCharacters(destination, m_number.span()); }

private:
    const FormattedCSSNumber& m_number;
};

}

using WTF::FormattedNumber;
using WTF::FormattedCSSNumber;
