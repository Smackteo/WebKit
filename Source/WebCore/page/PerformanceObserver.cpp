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
#include "PerformanceObserver.h"

#include "Document.h"
#include "InspectorInstrumentation.h"
#include "LocalDOMWindow.h"
#include "Performance.h"
#include "PerformanceObserverEntryList.h"
#include "WorkerGlobalScope.h"

namespace WebCore {

PerformanceObserver::PerformanceObserver(ScriptExecutionContext& scriptExecutionContext, Ref<PerformanceObserverCallback>&& callback)
    : m_callback(WTFMove(callback))
{
    if (RefPtr document = dynamicDowncast<Document>(scriptExecutionContext)) {
        if (auto* window = document->window())
            m_performance = window->performance();
    } else if (RefPtr workerGlobalScope = dynamicDowncast<WorkerGlobalScope>(scriptExecutionContext))
        m_performance = workerGlobalScope->performance();
    else
        ASSERT_NOT_REACHED();
}

RefPtr<Performance> PerformanceObserver::protectedPerformance() const
{
    return m_performance;
}

void PerformanceObserver::disassociate()
{
    m_performance = nullptr;
    m_registered = false;
}

ExceptionOr<void> PerformanceObserver::observe(Init&& init)
{
    if (!m_performance)
        return Exception { ExceptionCode::TypeError };

    bool isBuffered = false;
    OptionSet<PerformanceEntry::Type> filter;
    if (init.entryTypes) {
        if (init.type)
            return Exception { ExceptionCode::TypeError, "either entryTypes or type must be provided"_s };
        if (m_registered && m_isTypeObserver)
            return Exception { ExceptionCode::InvalidModificationError, "observer type can't be changed once registered"_s };
        for (auto& entryType : *init.entryTypes) {
            if (auto type = PerformanceEntry::parseEntryTypeString(entryType))
                filter.add(*type);
        }
        if (filter.isEmpty())
            return { };
        m_typeFilter = filter;
    } else {
        if (!init.type)
            return Exception { ExceptionCode::TypeError, "no type or entryTypes were provided"_s };
        if (m_registered && !m_isTypeObserver)
            return Exception { ExceptionCode::InvalidModificationError, "observer type can't be changed once registered"_s };
        m_isTypeObserver = true;
        if (auto type = PerformanceEntry::parseEntryTypeString(*init.type))
            filter.add(*type);
        else
            return { };
        if (init.buffered) {
            isBuffered = true;
            auto oldSize = m_entriesToDeliver.size();
            protectedPerformance()->appendBufferedEntriesByType(*init.type, m_entriesToDeliver, *this);
            auto entriesToDeliver = m_entriesToDeliver.mutableSpan();
            auto begin = entriesToDeliver.begin();
            auto oldEnd = entriesToDeliver.subspan(oldSize).begin();
            auto end = entriesToDeliver.end();
            std::stable_sort(oldEnd, end, PerformanceEntry::startTimeCompareLessThan);
            std::inplace_merge(begin, oldEnd, end, PerformanceEntry::startTimeCompareLessThan);
        }
        m_typeFilter.add(filter);
    }

    if (!m_registered) {
        protectedPerformance()->registerPerformanceObserver(*this);
        m_registered = true;
    }
    if (isBuffered)
        deliver();

    return { };
}

Vector<Ref<PerformanceEntry>> PerformanceObserver::takeRecords()
{
    return std::exchange(m_entriesToDeliver, { });
}

void PerformanceObserver::disconnect()
{
    if (RefPtr performance = m_performance)
        performance->unregisterPerformanceObserver(*this);

    m_registered = false;
    m_entriesToDeliver.clear();
    m_typeFilter = { };
}

void PerformanceObserver::queueEntry(PerformanceEntry& entry)
{
    m_entriesToDeliver.append(entry);
}

void PerformanceObserver::deliver()
{
    if (m_entriesToDeliver.isEmpty())
        return;

    auto* context = m_callback->scriptExecutionContext();
    if (!context)
        return;

    Vector<Ref<PerformanceEntry>> entries = std::exchange(m_entriesToDeliver, { });
    auto list = PerformanceObserverEntryList::create(WTFMove(entries));

    InspectorInstrumentation::willFireObserverCallback(*context, "PerformanceObserver"_s);
    m_callback->invoke(*this, list, *this);
    InspectorInstrumentation::didFireObserverCallback(*context);
}

Vector<String> PerformanceObserver::supportedEntryTypes(ScriptExecutionContext& context)
{
    Vector<String> entryTypes = {
        "mark"_s,
        "measure"_s,
        "navigation"_s,
    };

    if (RefPtr document = dynamicDowncast<Document>(context); document && document->supportsPaintTiming())
        entryTypes.append("paint"_s);

    entryTypes.append("resource"_s);

    return entryTypes;
}

} // namespace WebCore
