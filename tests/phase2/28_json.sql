-- Phase 4 test 28: JSON type.
CREATE TABLE t1 (id INT PRIMARY KEY, doc JSON) ENGINE=TIDESDB;

INSERT INTO t1 VALUES
    (1, '{"name":"alice","age":30}'),
    (2, '{"name":"bob","tags":["x","y","z"]}'),
    (3, '[1,2,3,4,5]'),
    (4, 'null'),
    (5, '"plain string"');

SELECT id, doc FROM t1 ORDER BY id;
SELECT id, JSON_EXTRACT(doc, '$.name') AS name FROM t1 WHERE JSON_TYPE(doc) = 'OBJECT' ORDER BY id;
SELECT id, JSON_LENGTH(doc) AS len FROM t1 ORDER BY id;

DROP TABLE t1;
