-- Phase 2 test 02: SHOW ENGINES lists TIDESDB.
SELECT engine, support
  FROM information_schema.engines
 WHERE engine IN ('TIDESDB','InnoDB')
 ORDER BY engine;
