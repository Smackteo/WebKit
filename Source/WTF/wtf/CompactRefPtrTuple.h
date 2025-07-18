/*
 * Copyright (C) 2020 Apple Inc. All rights reserved.
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

#include <wtf/CompactPointerTuple.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>

namespace WTF {

template<typename T, typename Type>
class CompactRefPtrTuple final {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CompactRefPtrTuple);

    static_assert(::allowCompactPointers<T>());
public:
    CompactRefPtrTuple() = default;
    CompactRefPtrTuple(T* pointer, Type type)
    {
        setPointer(pointer);
        setType(type);
    }

    CompactRefPtrTuple(const CompactRefPtrTuple& other)
    {
        setPointer(other.pointer());
        setType(other.type());
    }

    CompactRefPtrTuple(CompactRefPtrTuple&& other)
    {
        m_data.setPointer(other.pointer());
        m_data.setType(other.type());
        other.m_data.setPointer(nullptr);
        other.m_data.setType({ });
    }

    CompactRefPtrTuple& operator=(const CompactRefPtrTuple& other)
    {
        CompactRefPtrTuple copied(other);
        swap(copied);
        return *this;
    }

    CompactRefPtrTuple& operator=(CompactRefPtrTuple&& other)
    {
        CompactRefPtrTuple moved(WTFMove(other));
        swap(moved);
        return *this;
    }

    ~CompactRefPtrTuple()
    {
        WTF::DefaultRefDerefTraits<T>::derefIfNotNull(m_data.pointer());
    }

    T* pointer() const
    {
        return m_data.pointer();
    }

    void setPointer(T* pointer)
    {
        auto* old = m_data.pointer();
        m_data.setPointer(WTF::DefaultRefDerefTraits<T>::refIfNotNull(pointer));
        WTF::DefaultRefDerefTraits<T>::derefIfNotNull(old);
    }

    void setPointer(RefPtr<T>&& pointer)
    {
        auto willRelease = WTFMove(pointer);
        auto* old = m_data.pointer();
        m_data.setPointer(willRelease.leakRef());
        WTF::DefaultRefDerefTraits<T>::derefIfNotNull(old);
    }

    void setPointer(Ref<T>&& pointer)
    {
        auto willRelease = WTFMove(pointer);
        auto* old = m_data.pointer();
        m_data.setPointer(&willRelease.leakRef());
        WTF::DefaultRefDerefTraits<T>::derefIfNotNull(old);
    }

    Type type() const { return m_data.type(); }
    void setType(Type type)
    {
        m_data.setType(type);
    }

    void swap(CompactRefPtrTuple<T, Type>& other)
    {
        m_data.swap(other.m_data);
    }

private:
    CompactPointerTuple<T*, Type> m_data;
};

} // namespace WTF

using WTF::CompactRefPtrTuple;
