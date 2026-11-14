#include "Library/Sqlite.h"

#include <sqlite3.h>

namespace sp
{

//==============================================================================
SqliteStatement::SqliteStatement (sqlite3* database, const juce::String& sql) : db (database)
{
    if (db == nullptr)
    {
        error = "Database is not open";
        return;
    }
    if (sqlite3_prepare_v2 (db, sql.toRawUTF8(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        error = sqlite3_errmsg (db);
        stmt = nullptr;
    }
}

SqliteStatement::~SqliteStatement()
{
    if (stmt != nullptr)
        sqlite3_finalize (stmt);
}

bool SqliteStatement::isValid() const { return stmt != nullptr; }

SqliteStatement& SqliteStatement::bind (int index, const juce::String& value)
{
    if (stmt != nullptr)
        sqlite3_bind_text (stmt, index, value.toRawUTF8(), (int) value.getNumBytesAsUTF8(), SQLITE_TRANSIENT);
    return *this;
}

SqliteStatement& SqliteStatement::bind (int index, const char* value)
{
    return bind (index, juce::String (value != nullptr ? value : ""));
}

SqliteStatement& SqliteStatement::bind (int index, std::int64_t value)
{
    if (stmt != nullptr)
        sqlite3_bind_int64 (stmt, index, (sqlite3_int64) value);
    return *this;
}

SqliteStatement& SqliteStatement::bind (int index, double value)
{
    if (stmt != nullptr)
        sqlite3_bind_double (stmt, index, value);
    return *this;
}

SqliteStatement& SqliteStatement::bindNull (int index)
{
    if (stmt != nullptr)
        sqlite3_bind_null (stmt, index);
    return *this;
}

bool SqliteStatement::step()
{
    if (stmt == nullptr)
        return false;
    const int rc = sqlite3_step (stmt);
    if (rc == SQLITE_ROW)
        return true;
    if (rc != SQLITE_DONE)
        error = sqlite3_errmsg (db);
    return false;
}

bool SqliteStatement::exec()
{
    if (stmt == nullptr)
        return false;
    const int rc = sqlite3_step (stmt);
    if (rc == SQLITE_DONE || rc == SQLITE_ROW)
        return true;
    error = sqlite3_errmsg (db);
    return false;
}

void SqliteStatement::reset()
{
    if (stmt != nullptr)
        sqlite3_reset (stmt);
}

juce::String SqliteStatement::getString (int column) const
{
    if (stmt == nullptr)
        return {};
    auto* text = sqlite3_column_text (stmt, column);
    if (text == nullptr)
        return {};
    return juce::String::fromUTF8 ((const char*) text, sqlite3_column_bytes (stmt, column));
}

std::int64_t SqliteStatement::getInt64 (int column) const
{
    return stmt != nullptr ? (std::int64_t) sqlite3_column_int64 (stmt, column) : 0;
}

double SqliteStatement::getDouble (int column) const
{
    return stmt != nullptr ? sqlite3_column_double (stmt, column) : 0.0;
}

bool SqliteStatement::isNull (int column) const
{
    return stmt == nullptr || sqlite3_column_type (stmt, column) == SQLITE_NULL;
}

int SqliteStatement::numColumns() const
{
    return stmt != nullptr ? sqlite3_column_count (stmt) : 0;
}

juce::String SqliteStatement::getColumnName (int column) const
{
    if (stmt == nullptr)
        return {};
    auto* name = sqlite3_column_name (stmt, column);
    return name != nullptr ? juce::String (name) : juce::String();
}

//==============================================================================
SqliteDb::SqliteDb() = default;

SqliteDb::~SqliteDb() { close(); }

bool SqliteDb::open (const juce::File& file, juce::String& errorOut)
{
    close();
    file.getParentDirectory().createDirectory();
    openPath = file.getFullPathName();

    const int rc = sqlite3_open_v2 (openPath.toRawUTF8(), &db,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                    nullptr);
    if (rc != SQLITE_OK)
    {
        errorOut = db != nullptr ? sqlite3_errmsg (db) : "could not create database";
        if (db != nullptr)
        {
            sqlite3_close (db);
            db = nullptr;
        }
        return false;
    }

    sqlite3_busy_timeout (db, 5000);
    execute ("PRAGMA journal_mode=WAL");
    execute ("PRAGMA synchronous=NORMAL");
    execute ("PRAGMA foreign_keys=ON");
    return true;
}

void SqliteDb::close()
{
    if (db != nullptr)
    {
        sqlite3_close (db);
        db = nullptr;
    }
}

bool SqliteDb::isOpen() const { return db != nullptr; }

bool SqliteDb::execute (const juce::String& sql, juce::String* errorOut)
{
    if (db == nullptr)
    {
        if (errorOut != nullptr) *errorOut = "Database is not open";
        return false;
    }
    char* err = nullptr;
    const int rc = sqlite3_exec (db, sql.toRawUTF8(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        if (errorOut != nullptr)
            *errorOut = err != nullptr ? juce::String (err) : juce::String (sqlite3_errmsg (db));
        if (err != nullptr)
            sqlite3_free (err);
        return false;
    }
    if (err != nullptr)
        sqlite3_free (err);
    return true;
}

std::unique_ptr<SqliteStatement> SqliteDb::prepare (const juce::String& sql)
{
    auto st = std::make_unique<SqliteStatement> (db, sql);
    if (! st->isValid())
        return nullptr;
    return st;
}

std::int64_t SqliteDb::lastInsertRowid() const
{
    return db != nullptr ? (std::int64_t) sqlite3_last_insert_rowid (db) : 0;
}

int SqliteDb::lastChanges() const
{
    return db != nullptr ? sqlite3_changes (db) : 0;
}

juce::String SqliteDb::lastError() const
{
    return db != nullptr ? juce::String (sqlite3_errmsg (db)) : juce::String ("Database is not open");
}

bool SqliteDb::beginTransaction() { return execute ("BEGIN IMMEDIATE"); }
bool SqliteDb::commitTransaction() { return execute ("COMMIT"); }
bool SqliteDb::rollbackTransaction() { return execute ("ROLLBACK"); }

} // namespace sp
