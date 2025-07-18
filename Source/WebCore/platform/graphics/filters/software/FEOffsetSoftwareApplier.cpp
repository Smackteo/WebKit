/*
 * Copyright (C) 2004, 2005, 2006, 2007 Nikolas Zimmermann <zimmermann@kde.org>
 * Copyright (C) 2004, 2005 Rob Buis <buis@kde.org>
 * Copyright (C) 2005 Eric Seidel <eric@webkit.org>
 * Copyright (C) 2009 Dirk Schulze <krit@webkit.org>
 * Copyright (C) Research In Motion Limited 2010. All rights reserved.
 * Copyright (C) 2021 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "FEOffsetSoftwareApplier.h"

#include "FEOffset.h"
#include "Filter.h"
#include "GraphicsContext.h"
#include "ImageBuffer.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FEOffsetSoftwareApplier);

bool FEOffsetSoftwareApplier::apply(const Filter& filter, std::span<const Ref<FilterImage>> inputs, FilterImage& result) const
{
    Ref input = inputs[0];

    RefPtr resultImage = result.imageBuffer();
    RefPtr inputImage = input->imageBuffer();
    if (!resultImage || !inputImage)
        return false;

    FloatRect inputImageRect = input->absoluteImageRectRelativeTo(result);

    auto offset = filter.resolvedSize({ m_effect->dx(), m_effect->dy() });
    auto absoluteOffset = filter.scaledByFilterScale(offset);

    inputImageRect.move(absoluteOffset);

    resultImage->context().drawImageBuffer(*inputImage, inputImageRect);
    return true;
}

} // namespace WebCore
