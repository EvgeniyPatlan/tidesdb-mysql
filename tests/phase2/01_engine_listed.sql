-- Phase 2 test 01: TIDESDB engine appears in information_schema.engines.
-- Pre-condition: plugin already INSTALL'd by the runner.
SELECT engine, support, transactions
  FROM information_schema.engines
 WHERE engine = 'TIDESDB';
