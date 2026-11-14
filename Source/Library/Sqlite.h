/*
    Snake Puppy Reference Player
    Thin, safe wrapper around the system SQLite3 library (JUCE 8 no longer
    bundles SQLite). Used by the library, playlists and profile stores.

    Usage:
        SqliteDb db;
        db.open (file, error);
        auto st = db.prepare ("SELECT id, name FROM tracks WHERE genre = ?");
        st->bind (1, genre);
        while (st->step()) { auto id = st->getInt64 (0); ... }
*/
#pragma once

#include <juce_core/juce_core.h>
#include <memory>

struct sqlite3;
struct sqlite3_stmt;

namespace sp
{

class SqliteStatement
{
public:
    SqliteStatement (sqlite3* db, const juce::String& sql);
    ~SqliteStatement();

    bool isValid() const;
    const juce::String& getError() const { return error; }

    SqliteStatement& bind (int index, const juce::String& value);
    SqliteStatement& bind (int index, const char* value);
    SqliteStatement& bind (int index, std::int64_t value);
    SqliteStatement& bind (int index, int value) { return bind (index, (std::int64_t) value); }
    SqliteStatement& bind (int index, double value);
    SqliteStatement& bind (int index, bool value) { return bind (index, (std::int64_t) (value ? 1 : 0)); }
    SqliteStatement& bindNull (int index);

    /** Executes and returns true while a row is available. */
    bool step();

    /** Executes a statement that returns no rows. */
    bool exec();

    void reset();

    // Column accessors (valid after step() returned true).
    juce::String getString (int column) const;
    std::int64_t getInt64 (int column) const;
    int getInt (int column) const { return (int) getInt64 (column); }
    double getDouble (int column) const;
    bool isNull (int column) const;
    int numColumns() const;
    juce::String getColumnName (int column) const;

    sqlite3_stmt* getHandle() const { return stmt; }

private:
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    juce::String error;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SqliteStatement)
};

class SqliteDb
{
public:
    SqliteDb();
    ~SqliteDb();

    bool open (const juce::File& file, juce::String& errorOut);
    void close();
    bool isOpen() const;

    bool execute (const juce::String& sql, juce::String* errorOut = nullptr);
    std::unique_ptr<SqliteStatement> prepare (const juce::String& sql);

    std::int64_t lastInsertRowid() const;
    int lastChanges() const;
    juce::String lastError() const;

    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

    sqlite3* getHandle() const { return db; }

private:
    sqlite3* db = nullptr;
    juce::String openPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SqliteDb)
};

} // namespace sp
