-- @case: fulltext_natural_boolean
-- @axis: fulltext
CREATE TABLE d (id INT PRIMARY KEY, body TEXT, FULLTEXT KEY ft (body)) ENGINE=TidesDB;
INSERT INTO d VALUES
 (1,'quick brown fox'),
 (2,'lazy dog sleeps'),
 (3,'quick quick rabbit');
SELECT id FROM d WHERE MATCH(body) AGAINST('quick')
  ORDER BY MATCH(body) AGAINST('quick') DESC, id;
SELECT id FROM d WHERE MATCH(body) AGAINST('+quick -rabbit' IN BOOLEAN MODE) ORDER BY id;
-- @expect:
-- 3
-- 1
-- 1
-- @endexpect
