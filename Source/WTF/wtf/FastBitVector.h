/*
 * Copyright (C) 2012, 2013, 2016 Apple Inc. All rights reserved.
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

#include <string.h>
#include <wtf/Atomics.h>
#include <wtf/FastMalloc.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

namespace WTF {

class PrintStream;

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(FastBitVector);

inline constexpr size_t fastBitVectorArrayLength(size_t numBits) { return (numBits + 31) / 32; }

class FastBitVectorWordView {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FastBitVectorWordView);
public:
    typedef FastBitVectorWordView ViewType;
    
    FastBitVectorWordView() { }
    
    FastBitVectorWordView(const uint32_t* array, size_t numBits)
        : m_words(array)
        , m_numBits(numBits)
    {
    }
    
    size_t numBits() const { return m_numBits; }
    
    uint32_t word(size_t index) const { return words()[index]; }
    
private:
    std::span<const uint32_t> words() const { return unsafeMakeSpan(m_words, fastBitVectorArrayLength(m_numBits)); }

    const uint32_t* m_words { nullptr };
    size_t m_numBits { 0 };
};

class FastBitVectorWordOwner {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FastBitVectorWordOwner);
public:
    typedef FastBitVectorWordView ViewType;
    
    FastBitVectorWordOwner() = default;
    
    FastBitVectorWordOwner(FastBitVectorWordOwner&& other)
        : m_words(std::exchange(other.m_words, nullptr))
        , m_numBits(std::exchange(other.m_numBits, 0))
    {
    }

    FastBitVectorWordOwner(const FastBitVectorWordOwner& other)
    {
        *this = other;
    }
    
    ~FastBitVectorWordOwner()
    {
        if (m_words)
            FastBitVectorMalloc::free(m_words);
    }
    
    FastBitVectorWordView view() const { return FastBitVectorWordView(m_words, m_numBits); }
    
    FastBitVectorWordOwner& operator=(const FastBitVectorWordOwner& other)
    {
        if (arrayLength() != other.arrayLength())
            setEqualsSlow(other);
        else {
            memcpySpan(words(), other.words());
            m_numBits = other.m_numBits;
        }
        return *this;
    }
    
    FastBitVectorWordOwner& operator=(FastBitVectorWordOwner&& other)
    {
        std::swap(m_words, other.m_words);
        std::swap(m_numBits, other.m_numBits);
        return *this;
    }
    
    void setAll()
    {
        memsetSpan(words(), 255);
    }
    
    void clearAll()
    {
        zeroSpan(words());
    }
    
    void set(const FastBitVectorWordOwner& other)
    {
        ASSERT_WITH_SECURITY_IMPLICATION(m_numBits == other.m_numBits);
        memcpySpan(words(), other.words());
    }
    
    size_t numBits() const { return m_numBits; }
    
    size_t arrayLength() const { return fastBitVectorArrayLength(numBits()); }
    
    void resize(size_t numBits)
    {
        if (arrayLength() != fastBitVectorArrayLength(numBits))
            resizeSlow(numBits);
        m_numBits = numBits;
    }
    
    uint32_t word(size_t index) const { return words()[index]; }
    uint32_t& word(size_t index) { return words()[index]; }

    std::span<uint32_t> words() { return unsafeMakeSpan(m_words, arrayLength()); }
    std::span<const uint32_t> words() const { return unsafeMakeSpan(m_words, arrayLength()); }

private:
    WTF_EXPORT_PRIVATE void setEqualsSlow(const FastBitVectorWordOwner& other);
    WTF_EXPORT_PRIVATE void resizeSlow(size_t numBits);
    
    uint32_t* m_words { nullptr };
    size_t m_numBits { 0 };
};

template<typename Left, typename Right>
class FastBitVectorAndWords {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FastBitVectorAndWords);
public:
    typedef FastBitVectorAndWords ViewType;
    
    FastBitVectorAndWords(const Left& left, const Right& right)
        : m_left(left)
        , m_right(right)
    {
        ASSERT_WITH_SECURITY_IMPLICATION(m_left.numBits() == m_right.numBits());
    }
    
    FastBitVectorAndWords view() const { return *this; }
    
    size_t numBits() const
    {
        return m_left.numBits();
    }
    
    uint32_t word(size_t index) const
    {
        return m_left.word(index) & m_right.word(index);
    }
    
private:
    Left m_left;
    Right m_right;
};
    
template<typename Left, typename Right>
class FastBitVectorOrWords {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FastBitVectorOrWords);
public:
    typedef FastBitVectorOrWords ViewType;
    
    FastBitVectorOrWords(const Left& left, const Right& right)
        : m_left(left)
        , m_right(right)
    {
        ASSERT_WITH_SECURITY_IMPLICATION(m_left.numBits() == m_right.numBits());
    }
    
    FastBitVectorOrWords view() const { return *this; }
    
    size_t numBits() const
    {
        return m_left.numBits();
    }
    
    uint32_t word(size_t index) const
    {
        return m_left.word(index) | m_right.word(index);
    }
    
private:
    Left m_left;
    Right m_right;
};
    
template<typename View>
class FastBitVectorNotWords {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FastBitVectorNotWords);
public:
    typedef FastBitVectorNotWords ViewType;
    
    FastBitVectorNotWords(const View& view)
        : m_view(view)
    {
    }
    
    FastBitVectorNotWords view() const { return *this; }
    
    size_t numBits() const
    {
        return m_view.numBits();
    }
    
    uint32_t word(size_t index) const
    {
        return ~m_view.word(index);
    }
    
private:
    View m_view;
};

class FastBitVector;

template<typename Words>
class FastBitVectorImpl {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FastBitVectorImpl);
public:
    FastBitVectorImpl()
        : m_words()
    {
    }
    
    FastBitVectorImpl(const Words& words)
        : m_words(words)
    {
    }
    
    FastBitVectorImpl(Words&& words)
        : m_words(WTFMove(words))
    {
    }

    size_t numBits() const { return m_words.numBits(); }
    size_t size() const { return numBits(); }
    
    size_t arrayLength() const { return fastBitVectorArrayLength(numBits()); }
    
    bool operator==(const FastBitVectorImpl& other) const
    {
        if (numBits() != other.numBits())
            return false;
        for (size_t i = arrayLength(); i--;) {
            if (m_words.word(i) != other.m_words.word(i))
                return false;
        }
        return true;
    }

    bool at(size_t index) const
    {
        return atImpl(index);
    }
    
    bool operator[](size_t index) const
    {
        return atImpl(index);
    }
    
    size_t bitCount() const
    {
        size_t result = 0;
        for (size_t index = arrayLength(); index--;)
            result += WTF::bitCount(m_words.word(index));
        return result;
    }
    
    bool isEmpty() const
    {
        for (size_t index = arrayLength(); index--;) {
            if (m_words.word(index))
                return false;
        }
        return true;
    }
    
    template<typename OtherWords>
    FastBitVectorImpl<FastBitVectorAndWords<typename Words::ViewType, typename OtherWords::ViewType>> operator&(const FastBitVectorImpl<OtherWords>& other) const
    {
        return FastBitVectorImpl<FastBitVectorAndWords<typename Words::ViewType, typename OtherWords::ViewType>>(FastBitVectorAndWords<typename Words::ViewType, typename OtherWords::ViewType>(wordView(), other.wordView()));
    }
    
    template<typename OtherWords>
    FastBitVectorImpl<FastBitVectorOrWords<typename Words::ViewType, typename OtherWords::ViewType>> operator|(const FastBitVectorImpl<OtherWords>& other) const
    {
        return FastBitVectorImpl<FastBitVectorOrWords<typename Words::ViewType, typename OtherWords::ViewType>>(FastBitVectorOrWords<typename Words::ViewType, typename OtherWords::ViewType>(wordView(), other.wordView()));
    }
    
    FastBitVectorImpl<FastBitVectorNotWords<typename Words::ViewType>> operator~() const
    {
        return FastBitVectorImpl<FastBitVectorNotWords<typename Words::ViewType>>(FastBitVectorNotWords<typename Words::ViewType>(wordView()));
    }
    
    template<typename Func>
    ALWAYS_INLINE void forEachSetBit(const Func& func) const
    {
        size_t n = arrayLength();
        for (size_t i = 0; i < n; ++i) {
            uint32_t word = m_words.word(i);
            size_t j = i * 32;
            while (word) {
                if (word & 1)
                    func(j);
                word >>= 1;
                j++;
            }
        }
    }
    
    template<typename Func>
    ALWAYS_INLINE void forEachClearBit(const Func& func) const
    {
        (~*this).forEachSetBit(func);
    }
    
    template<typename Func>
    void forEachBit(bool value, const Func& func) const
    {
        if (value)
            forEachSetBit(func);
        else
            forEachClearBit(func);
    }
    
    // Starts looking for bits at the index you pass. If that index contains the value you want,
    // then it will return that index. Returns numBits when we get to the end. For example, you
    // can write a loop to iterate over all set bits like this:
    //
    // for (size_t i = 0; i < bits.numBits(); i = bits.findBit(i + 1, true))
    //     ...
    ALWAYS_INLINE size_t findBit(size_t startIndex, bool value) const
    {
        // If value is true, this produces 0. If value is false, this produces UINT_MAX. It's
        // written this way so that it performs well regardless of whether value is a constant.
        uint32_t skipValue = -(static_cast<uint32_t>(value) ^ 1);
        
        size_t numWords = fastBitVectorArrayLength(m_words.numBits());
        
        size_t wordIndex = startIndex / 32;
        size_t startIndexInWord = startIndex - wordIndex * 32;
        
        while (wordIndex < numWords) {
            uint32_t word = m_words.word(wordIndex);
            if (word != skipValue) {
                size_t index = startIndexInWord;
                if (findBitInWord(word, index, 32, value))
                    return wordIndex * 32 + index;
            }
            
            wordIndex++;
            startIndexInWord = 0;
        }
        
        return numBits();
    }
    
    ALWAYS_INLINE size_t findSetBit(size_t index) const
    {
        return findBit(index, true);
    }
    
    ALWAYS_INLINE size_t findClearBit(size_t index) const
    {
        return findBit(index, false);
    }
    
    void dump(PrintStream& out) const
    {
        for (size_t i = 0; i < numBits(); ++i)
            out.print((*this)[i] ? "1" : "-");
    }
    
    typename Words::ViewType wordView() const { return m_words.view(); }

    Words& unsafeWords() { return m_words; }
    const Words& unsafeWords() const { return m_words; }
    
private:
    // You'd think that we could remove this friend if we used protected, but you'd be wrong,
    // because templates.
    friend class FastBitVector;
    
    bool atImpl(size_t index) const
    {
        ASSERT_WITH_SECURITY_IMPLICATION(index < numBits());
        return !!(m_words.word(index >> 5) & (1 << (index & 31)));
    }
    
    Words m_words;
};

class FastBitReference {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FastBitReference);
public:
    FastBitReference() = default;

    FastBitReference(uint32_t* word, uint32_t mask)
        : m_word(word)
        , m_mask(mask)
    {
    }

    operator bool() const
    {
        return !!(*m_word & m_mask);
    }

    FastBitReference& operator=(bool value)
    {
        if (value)
            *m_word |= m_mask;
        else
            *m_word &= ~m_mask;
        return *this;
    }

    FastBitReference& operator|=(bool value) { return value ? *this = value : *this; }
    FastBitReference& operator&=(bool value) { return value ? *this : *this = value; }

private:
    uint32_t* m_word { nullptr };
    uint32_t m_mask { 0 };
};



class FastBitVector : public FastBitVectorImpl<FastBitVectorWordOwner> {
public:
    FastBitVector() { }
    explicit FastBitVector(size_t numBits)
    {
        grow(numBits);
    }

    FastBitVector(size_t numBits, bool value)
    {
        grow(numBits);
        fill(value);
    }
    
    FastBitVector(const FastBitVector&) = default;
    FastBitVector& operator=(const FastBitVector&) = default;
    
    template<typename OtherWords>
    FastBitVector(const FastBitVectorImpl<OtherWords>& other)
    {
        *this = other;
    }
    
    template<typename OtherWords>
    FastBitVector& operator=(const FastBitVectorImpl<OtherWords>& other)
    {
        if (numBits() != other.numBits()) [[unlikely]]
            resize(other.numBits());
        
        for (unsigned i = arrayLength(); i--;)
            m_words.word(i) = other.m_words.word(i);
        return *this;
    }
    
    void resize(size_t numBits)
    {
        m_words.resize(numBits);
    }
    
    void setAll()
    {
        m_words.setAll();
    }
    
    void clearAll()
    {
        m_words.clearAll();
    }

    // For templating as Vector<bool>
    void fill(bool value) { value ? setAll() : clearAll(); }
    void grow(size_t newSize) { resize(newSize); }

    WTF_EXPORT_PRIVATE void clearRange(size_t begin, size_t end);

    // Returns true if the contents of this bitvector changed.
    template<typename OtherWords>
    bool setAndCheck(const FastBitVectorImpl<OtherWords>& other)
    {
        bool changed = false;
        ASSERT_WITH_SECURITY_IMPLICATION(numBits() == other.numBits());
        for (unsigned i = arrayLength(); i--;) {
            changed |= m_words.word(i) != other.m_words.word(i);
            m_words.word(i) = other.m_words.word(i);
        }
        return changed;
    }
    
    template<typename OtherWords>
    FastBitVector& operator|=(const FastBitVectorImpl<OtherWords>& other)
    {
        ASSERT_WITH_SECURITY_IMPLICATION(numBits() == other.numBits());
        for (unsigned i = arrayLength(); i--;)
            m_words.word(i) |= other.m_words.word(i);
        return *this;
    }
    
    template<typename OtherWords>
    FastBitVector& operator&=(const FastBitVectorImpl<OtherWords>& other)
    {
        ASSERT_WITH_SECURITY_IMPLICATION(numBits() == other.numBits());
        for (unsigned i = arrayLength(); i--;)
            m_words.word(i) &= other.m_words.word(i);
        return *this;
    }
    
    bool at(size_t index) const
    {
        return atImpl(index);
    }
    
    bool operator[](size_t index) const
    {
        return atImpl(index);
    }
    
    FastBitReference at(size_t index)
    {
        RELEASE_ASSERT(index < numBits());
        return FastBitReference(&m_words.word(index >> 5), 1 << (index & 31));
    }
    
    FastBitReference operator[](size_t index)
    {
        return at(index);
    }
    
    // Returns true if the contents changed.
    ALWAYS_INLINE bool atomicSetAndCheck(size_t index, bool value)
    {
        uint32_t* pointer = &m_words.word(index >> 5);
        uint32_t mask = 1 << (index & 31);
        for (;;) {
            uint32_t oldValue = *pointer;
            uint32_t newValue;
            if (value) {
                if (oldValue & mask)
                    return false;
                newValue = oldValue | mask;
            } else {
                if (!(oldValue & mask))
                    return false;
                newValue = oldValue & ~mask;
            }
            if (atomicCompareExchangeWeakRelaxed(pointer, oldValue, newValue))
                return true;
        }
    }
};

} // namespace WTF

using WTF::FastBitReference;
using WTF::FastBitVector;
