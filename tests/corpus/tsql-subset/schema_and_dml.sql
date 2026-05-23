-- T-SQL subset corpus: schema definition + DML over a tiny order-book.
-- Exercises CREATE TABLE, INSERT, SELECT (with bracket identifiers,
-- qualified names, aliases, comparison ops), UPDATE, DELETE.
-- Also exercises `--` line comments and /* ... */ block comments.

/* ── schema ────────────────────────────────────────────────────────── */

CREATE TABLE Users (
    Id INT,
    Name VARCHAR,
    Email VARCHAR,
    Active BIT
);

CREATE TABLE Orders (
    Id INT,
    UserId INT,
    Amount INT,
    Status VARCHAR
);

CREATE TABLE [Order Items] (
    [Order Id] INT,
    [Product Id] INT,
    Quantity INT
);

-- ── seed data ──────────────────────────────────────────────────────────

INSERT INTO Users VALUES (1, 'Alice', 'alice@example.com', 1);

INSERT INTO Users VALUES (2, 'Bob''s Tools', 'bob@example.com', 1);

INSERT INTO Users VALUES (3, N'héllo', N'h@example.com', 0);

INSERT INTO Orders VALUES (100, 1, 250, 'pending');

INSERT INTO Orders VALUES (101, 2, 75, 'shipped');

INSERT INTO Orders VALUES (102, 1, 999, 'pending');

INSERT INTO [Order Items] VALUES (100, 5001, 3);

INSERT INTO [Order Items] VALUES (101, 5002, 1);

/* ── queries ─────────────────────────────────────────────────────────── */

SELECT * FROM Users;

SELECT Id, Name FROM Users WHERE Active = 1;

SELECT Id AS OrderId, Amount FROM Orders WHERE Status = 'pending';

SELECT u.Name AS Customer, o.Amount FROM Users u WHERE u.Active = 1;

SELECT * FROM company.dbo.Users;

-- comparison ops smoke
SELECT Id FROM Orders WHERE Amount > 100;
SELECT Id FROM Orders WHERE Amount >= 100;
SELECT Id FROM Orders WHERE Amount < 1000;
SELECT Id FROM Orders WHERE Amount <= 500;
SELECT Id FROM Orders WHERE Status != 'pending';

SELECT [Order Id], [Product Id], Quantity FROM [Order Items];

-- ── mutations ──────────────────────────────────────────────────────────

UPDATE Users SET Active = 0 WHERE Id = 3;

UPDATE Orders SET Status = 'shipped' WHERE Id = 100;

UPDATE Orders SET Amount = Amount + 100 WHERE UserId = 1;

UPDATE [Order Items] SET Quantity = Quantity + 1 WHERE [Order Id] = 100;

DELETE FROM Orders WHERE Status = 'cancelled';

DELETE FROM Users WHERE Active = 0;

DELETE FROM [Order Items] WHERE Quantity = 0;
