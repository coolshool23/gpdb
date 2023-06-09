0:CREATE RESOURCE QUEUE rq_concurrency_test WITH (active_statements = 1);
0:CREATE role role_concurrency_test RESOURCE QUEUE rq_concurrency_test;

1:SET role role_concurrency_test;
1:BEGIN;
1:DECLARE c1 CURSOR FOR SELECT 1;

2:SET deadlock_timeout TO '500ms';
2:SET log_lock_waits TO on;
2:SET role role_concurrency_test;
2:PREPARE fooplan AS SELECT 1;
2&:EXECUTE fooplan;

-- EXECUTE statement(cached plan) will be blocked when the concurrency limit of the resource queue is reached.
-- At the same time ensure that spurious deadlock will not complete this query after activation of deadlock detector when log_lock_waits GUC is on.
0:SELECT pg_sleep(1);
0:SELECT rsqcountvalue FROM gp_toolkit.gp_resqueue_status WHERE rsqname='rq_concurrency_test';
0:SELECT waiting_reason from pg_stat_activity where query = 'EXECUTE fooplan;';

1:END;

2<:
2:END;

-- Sanity check: Ensure that the resource queue is now empty.
0: SELECT rsqcountlimit, rsqcountvalue from pg_resqueue_status WHERE rsqname = 'rq_concurrency_test';

0:DROP role role_concurrency_test;
0:DROP RESOURCE QUEUE rq_concurrency_test;
