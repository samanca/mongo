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
const cmd = {
    findAndModify: kCollName,
    query: {},
    update: {'$inc': {x: 1}}
};
for (var i = 0; i < kOpCount; i++) {
    assert.commandWorked(rst.getPrimary().getDB(kDbName).runCommand(cmd));
}

jsTestLog("Verifying the operation journey tracking");
const summary = rst.getPrimary().getDB("admin").runCommand({"opJourney": 1});
assert.gt(summary.operations, kOpCount);
assert.eq(summary.stable, true);
assert.eq(summary.ok, 1);
jsTestLog(summary);

rst.stopSet();
})();
