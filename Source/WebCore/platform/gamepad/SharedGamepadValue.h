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

#if ENABLE(GAMEPAD)

#include <wtf/FastMalloc.h>
#include <wtf/Nonmovable.h>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>

namespace WebCore {

class SharedGamepadValue {
public:
    SharedGamepadValue()
        : m_data(adoptRef(*new Data(0.0)))
    {
    }

    explicit SharedGamepadValue(double value)
        : m_data(adoptRef(*new Data(value)))
    {
    }

    void setValue(double value) { m_data->value = value; }
    double value() const { return m_data->value; }

private:
    struct Data : RefCounted<Data> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Data);

        Data(double theValue)
            : value(theValue)
        {
        }

        double value;
    };

    Ref<Data> m_data;
};

} // namespace WebCore

#endif // ENABLE(GAMEPAD)
