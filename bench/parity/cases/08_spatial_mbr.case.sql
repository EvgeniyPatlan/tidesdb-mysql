-- @case: spatial_mbr_contains
-- @axis: spatial
CREATE TABLE g (id INT PRIMARY KEY AUTO_INCREMENT, pt POINT NOT NULL SRID 0,
  SPATIAL INDEX sp (pt)) ENGINE=TidesDB;
INSERT INTO g (pt) VALUES
 (ST_GeomFromText('POINT(1 1)',0)),
 (ST_GeomFromText('POINT(5 5)',0)),
 (ST_GeomFromText('POINT(9 9)',0));
SELECT id FROM g
WHERE MBRContains(ST_GeomFromText('POLYGON((0 0,6 0,6 6,0 6,0 0))',0), pt)
ORDER BY id;
-- @expect:
-- 1
-- 2
-- @endexpect
