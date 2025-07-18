/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
 * Copyright (C) 2025 Samuel Weinig <sam@webkit.org>
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

#include "CSSResolvedColor.h"
#include "Color.h"
#include <wtf/Forward.h>

namespace WebCore {
namespace Style {

struct Color;
struct ColorResolutionState;

struct ResolvedColor {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ResolvedColor);

    WebCore::Color color;

    bool operator==(const ResolvedColor&) const = default;
};

Color toStyleColor(const CSS::ResolvedColor&, ColorResolutionState&);

inline WebCore::Color resolveColor(const ResolvedColor& absoluteColor, const WebCore::Color&)
{
    return absoluteColor.color;
}

constexpr bool containsCurrentColor(const ResolvedColor&)
{
    return false;
}

WTF::TextStream& operator<<(WTF::TextStream&, const ResolvedColor&);

} // namespace Style
} // namespace WebCore
