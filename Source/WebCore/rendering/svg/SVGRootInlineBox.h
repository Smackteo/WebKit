/*
 * Copyright (C) 2006 Oliver Hunt <ojh16@student.canterbury.ac.nz>
 * Copyright (C) 2006 Apple Inc. All rights reserved.
 * Copyright (C) 2007 Nikolas Zimmermann <zimmermann@kde.org>
 * Copyright (C) Research In Motion Limited 2010. All rights reserved.
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

#pragma once

#include "LegacyRootInlineBox.h"
#include "SVGTextLayoutEngine.h"

namespace WebCore {

class RenderSVGText;
class SVGInlineTextBox;

class SVGRootInlineBox final : public LegacyRootInlineBox {
    WTF_MAKE_TZONE_OR_ISO_ALLOCATED(SVGRootInlineBox);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(SVGRootInlineBox);
public:
    explicit SVGRootInlineBox(RenderSVGText&);

    float virtualLogicalHeight() const override { return m_logicalHeight; }
    void setLogicalHeight(float height) { m_logicalHeight = height; }

private:
    RenderSVGText& renderSVGText() const;

    bool isSVGRootInlineBox() const override { return true; }

    float m_logicalHeight;
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_INLINE_BOX(SVGRootInlineBox, isSVGRootInlineBox())
