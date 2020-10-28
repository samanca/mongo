/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include <string>

#if defined(__linux__)
#include <time.h>
#endif  // __linux__

#include "mongo/db/op_journey.h"
#include "mongo/db/op_journey_gen.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"

namespace mongo {

namespace {

Nanoseconds getElapsedTime() {
#if defined(__linux__)
    struct timespec t;
    if (auto ret = clock_gettime(CLOCK_MONOTONIC, &t); ret != 0) {
        int ec = errno;
        LOGV2_FATAL(7777701, "Unable to get the time", "error"_attr = errnoWithDescription(ec));
    }
    return Seconds(t.tv_sec) + Nanoseconds(t.tv_nsec);
#else
    return Nanoseconds(0);
#endif  // __linux__
}

bool runsOnClientThread(const OperationContext* opCtx) {
    const auto client = opCtx->getClient();
    return !client || client == Client::getCurrent();
}

auto stageToString(OpJourney::Stage stage) {
    using Stage = OpJourney::Stage;
    switch (stage) {
        case Stage::kRunning:
            return "running";
        case Stage::kWaitForReadConcern:
            return "waitForReadConcern";
        case Stage::kWaitForWriteConcern:
            return "waitForWriteConcern";
        case Stage::kMirroring:
            return "readMirroring";
        case Stage::kCheckAuth:
            return "checkAuthorization";
        case Stage::kExtractReadConcern:
            return "extractReadConcern";
        case Stage::kAcquireDbLock:
            return "acquireDbLock";
        case Stage::kComputeAndGossipOpTime:
            return "computeAndGossipOpTime";
        case Stage::kNetworkSync:
            return "egress";
        case Stage::kReleased:
            return "released";
        case Stage::kDestroyed:
            return "destroyed";
        default:
            MONGO_UNREACHABLE;
    }
}

static auto getOpJourney = OperationContext::declareDecoration<boost::optional<OpJourney>>();

}  // namespace

OpJourney::Observer::Observer()
    : _totalOps(0),
      _max(static_cast<size_t>(kDestroyed)),
      _min(static_cast<size_t>(kDestroyed)),
      _summary(static_cast<size_t>(kDestroyed)) {
    for (size_t stage = 0; stage < static_cast<size_t>(kDestroyed); stage++) {
        _max[stage].store(Nanoseconds::min().count());
        _min[stage].store(Nanoseconds::max().count());
    }
}

static auto getOpJourneyObserver =
    ServiceContext::declareDecoration<boost::optional<OpJourney::Observer>>();

OpJourney::Observer* OpJourney::Observer::get(ServiceContext* svc) {
    return getOpJourneyObserver(svc).get_ptr();
}

void OpJourney::Observer::capture(const OpJourney* journey) {
    invariant(journey->_current.stage == kDestroyed);

    for (size_t stage = 0; stage < _summary.size(); stage++) {
        const auto durNanos = journey->_profile[stage].count();
        if (durNanos == 0)
            continue;

        _summary[stage].ops.fetchAndAdd(1);
        _summary[stage].duration.fetchAndAdd(durNanos);

        Nanoseconds::rep max;
        do {
            max = _max[stage].load();
        } while (durNanos > max && !_max[stage].compareAndSwap(&max, durNanos));

        Nanoseconds::rep min;
        do {
            min = _min[stage].load();
        } while (durNanos < min && !_min[stage].compareAndSwap(&min, durNanos));
    }

    _totalOps.fetchAndAdd(1);
}

void OpJourney::Observer::append(BSONObjBuilder& bob) {
    const auto ops = _totalOps.load();
    for (size_t stage = 0; stage < _summary.size(); stage++) {
        if (_summary[stage].ops.loadRelaxed() == 0)
            continue;

        const auto min = Nanoseconds(_min[stage].loadRelaxed());
        const auto max = Nanoseconds(_max[stage].loadRelaxed());
        const auto avg =
            Nanoseconds(_summary[stage].duration.loadRelaxed() / _summary[stage].ops.loadRelaxed());
        bob.append(stageToString(static_cast<Stage>(stage)),
                   BSON("min" << min.toBSON() << "max" << max.toBSON() << "avg" << avg.toBSON()));
    }
    bob.append("operations", ops);
    bob.append("stable", ops == _totalOps.load());
}

OpJourney::OpJourney(const OperationContext* opCtx, Stage stage)
    : _opCtx(opCtx),
      _created(getElapsedTime()),
      _current(stage, _created),
      _profile(static_cast<size_t>(kDestroyed), Nanoseconds(0)) {}

OpJourney::~OpJourney() {
    enterStage(kDestroyed);
    Observer::get(_opCtx->getServiceContext())->capture(this);

    BSONObjBuilder bob;
    append(bob);
    LOGV2_DEBUG(7777702,
                kDiagnosticLogLevel,
                "Operation reached the end of its journey",
                "summary"_attr = bob.obj());
}

bool OpJourney::isTrackingEnabled() {
    return gEnableTrackingOpJourney;
}

void OpJourney::enable(OperationContext* opCtx) {
    if (!isTrackingEnabled())
        return;
    invariant(runsOnClientThread(opCtx));
    invariant(!getOpJourney(opCtx).has_value());
    getOpJourney(opCtx).emplace(opCtx, kRunning);
}

OpJourney* OpJourney::get(OperationContext* opCtx) {
    invariant(runsOnClientThread(opCtx));
    return getOpJourney(opCtx).get_ptr();
}

void OpJourney::enterStage(Stage stage) {
    const auto oldStage = std::exchange(_current.stage, stage);
    if (oldStage == stage)
        return;

    auto now = getElapsedTime();
    _profile[oldStage] += now - _current.entered;
    _current.entered = now;
}

void OpJourney::append(BSONObjBuilder& bob) const {
    Nanoseconds sum{0};
    for (size_t stage = 0; stage < _profile.size(); stage++) {
        if (_profile[stage] == Nanoseconds(0))
            continue;
        bob.append(stageToString(static_cast<Stage>(stage)), _profile[stage].toBSON());
        sum += _profile[stage];
    }

    const auto total = getElapsedTime() - _created;
    bob.append("other", (total - sum).toBSON());
}

ServiceContext::ConstructorActionRegisterer registerOpJourneyObserver(
    "OpJourneyObserver", [](ServiceContext* svc) {
        if (!OpJourney::isTrackingEnabled())
            return;
        getOpJourneyObserver(svc).emplace();
        LOGV2_DEBUG(7777703, OpJourney::kDiagnosticLogLevel, "Started operation journey observer");
    });

}  // namespace mongo
