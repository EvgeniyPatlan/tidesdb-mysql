-- @case: fulltext_add_backpopulate_existing_rows
-- @axis: fulltext
-- @note: HEADLINE: SUT A back-populates existing rows on ADD FULLTEXT (v0.2.1 + F-1); upstream B does not.
-- Headline divergence: ADD FULLTEXT on a table that ALREADY has rows.
-- SUT A (tidesdb-mysql v0.2.1) back-populates + F-1 meta fix; upstream
-- behaviour characterized by this case.
CREATE TABLE d (id INT PRIMARY KEY, body TEXT) ENGINE=TidesDB;
INSERT INTO d VALUES
 (1,'alpha widget engine'),
 (2,'beta widget gear'),
 (3,'gamma sprocket only'),
 (4,'widget widget widget heavy');
ALTER TABLE d ADD FULLTEXT INDEX ft (body);
SELECT id FROM d WHERE MATCH(body) AGAINST('widget')
  ORDER BY MATCH(body) AGAINST('widget') DESC, id;
ALTER TABLE d DROP INDEX ft;
ALTER TABLE d ADD FULLTEXT INDEX ft (body);
SELECT id FROM d WHERE MATCH(body) AGAINST('widget')
  ORDER BY MATCH(body) AGAINST('widget') DESC, id;
-- @expect:
-- 4
-- 1
-- 2
-- 4
-- 1
-- 2
-- @endexpect
