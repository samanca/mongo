/**
 * @tags: [requires_replication]
 */
(function() {
'use strict';

const kOpCount = 1000;
const kDbName = "op_journey_test";
const kCollName = "test";

const rst =
    new ReplSetTest({nodes: 3, nodeOptions: {setParameter: {"enableTrackingOpJourney": true}}});
rst.startSet();
rst.initiateWithHighElectionTimeout();

jsTestLog(`Applying ${kOpCount} operations against the primary`);
assert.commandWorked(
    rst.getPrimary().getDB(kDbName).runCommand({"insert": kCollName, documents: [{_id: 1, x: 0}]}));
for (var i = 0; i < kOpCount - 1; i++) {
    assert.commandWorked(rst.getPrimary().getDB(kDbName).runCommand(
        {findAndModify: kCollName, query: {}, update: {'$inc': {x: 1}}}));
}

jsTestLog("Verifying the operation journey tracking");
const summary = rst.getPrimary().getDB("admin").runCommand({"opJourney": 1});
jsTestLog(summary);
assert.gt(summary.operations, kOpCount);
assert.eq(summary.stable, true);
assert.eq(summary.ok, 1);

rst.stopSet();
})();
