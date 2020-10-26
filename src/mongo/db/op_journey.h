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

#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/duration.h"

namespace mongo {

#define OpJourneyStage(opCtx, stage)                  \
    {                                                 \
        if (OpJourney::isTrackingEnabled())           \
            OpJourney::get(opCtx)->enterStage(stage); \
    }

#define makeScopedOpJourney(opCtx, stage)                                                   \
    OpJourney::isTrackingEnabled() ? std::make_unique<OpJourney::ScopedStage>(opCtx, stage) \
                                   : std::unique_ptr<OpJourney::ScopedStage>(nullptr)

/**
 * TODO
 */
class OpJourney final {
public:
    class ScopedStage;
    class Observer;

    OpJourney(OpJourney&&) = delete;
    OpJourney(const OpJourney&) = delete;

    enum Stage {
        kRunning = 0,  // This must always be set to zero.
        kNetworkSync,
        kReleased,
        kDestroyed,  // This must be the last stage.
    };

    OpJourney(const OperationContext* opCtx, Stage stage);
    ~OpJourney();

    static bool isTrackingEnabled();

    static void enable(OperationContext* opCtx);
    static OpJourney* get(OperationContext* opCtx);

    void enterStage(Stage stage);

    void append(BSONObjBuilder& bob) const;

    static constexpr auto kDiagnosticLogLevel = 0;

private:
    const OperationContext* _opCtx;

    const Nanoseconds _created;
    boost::optional<Nanoseconds> _killed;
    boost::optional<Nanoseconds> _destroyed;

    // The current stage of the operation, and the process time at the time of entering the stage.
    struct State {
        State(Stage stage, Nanoseconds entered) : stage(stage), entered(entered) {}

        Stage stage;
        Nanoseconds entered;
    };
    State _current;

    // Holds the elapsed time in each stage for the associated operation.
    std::vector<Nanoseconds> _profile;
};

class OpJourney::ScopedStage final {
public:
    ScopedStage(ScopedStage&&) = delete;
    ScopedStage(const ScopedStage&) = delete;

    explicit ScopedStage(OperationContext* opCtx, Stage stage)
        : _opCtx(opCtx), _oldStage(OpJourney::get(opCtx)->_current.stage) {
        OpJourney::get(_opCtx)->enterStage(stage);
    }

    ~ScopedStage() {
        OpJourney::get(_opCtx)->enterStage(_oldStage);
    }

private:
    OperationContext* _opCtx;
    const Stage _oldStage;
};

}  // namespace mongo
