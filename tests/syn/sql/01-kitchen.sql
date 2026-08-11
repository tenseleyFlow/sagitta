-- ANSI kitchen sink
WITH recent AS (
  SELECT DISTINCT "user""name", amount, TRUE, CURRENT_DATE
  FROM "order"
  WHERE amount >= 10.5e2 AND deleted IS NULL
)
INSERT INTO audit(id, note) VALUES (:id, 'it''s done');
UPDATE audit SET note = N'nat''ional' WHERE id <> ?;
