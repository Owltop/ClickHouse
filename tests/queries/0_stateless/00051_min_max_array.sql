-- Tags: stateful
SELECT CounterID, count(), max(GoalsReached), min(GoalsReached), minIf(GoalsReached, notEmpty(GoalsReached)) FROM test.hits GROUP BY CounterID ORDER BY count() DESC LIMIT 20;
SELECT CounterID, count(), max(GoalsReached), min(GoalsReached), minIf(GoalsReached, notEmpty(GoalsReached)) FROM test.hits GROUP BY CounterID ORDER BY count() DESC LIMIT 20 SETTINGS optimize_aggregation_in_order = 1
