CREATE TABLE accounts (
    id BIGINT PRIMARY KEY,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    balance DECIMAL(12, 2) CHECK (balance >= 0),
    owner_id INTEGER REFERENCES users(id)
);
GRANT SELECT, UPDATE ON accounts TO app_user;
COMMIT;
