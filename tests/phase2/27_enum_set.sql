-- Phase 4 test 27: ENUM and SET.
CREATE TABLE t1 (
    id INT PRIMARY KEY,
    color ENUM('red','green','blue'),
    flags SET('read','write','exec')
) ENGINE=TIDESDB;

INSERT INTO t1 VALUES
    (1, 'red',   'read'),
    (2, 'green', 'read,write'),
    (3, 'blue',  'read,write,exec'),
    (4, NULL,    '');

SELECT id, color, flags FROM t1 ORDER BY id;
SELECT id, color FROM t1 WHERE color = 'green';
SELECT id, flags FROM t1 WHERE FIND_IN_SET('exec', flags) > 0;

DROP TABLE t1;
