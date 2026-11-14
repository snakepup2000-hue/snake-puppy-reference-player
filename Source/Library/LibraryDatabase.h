/*
    Snake Puppy Reference Player
    Shared SQLite database handle. One connection is owned by the app; the
    library scanner, playlist manager and profile engine serialise access
    through the lock provided here (they never hold it while doing disk I/O
    other than the statement itself).
*/
#pragma once

#include "Library/Sqlite.h"
#include <juce_core/juce_core.h>

namespace sp
{

class LibraryDatabase
{
public:
    LibraryDatabase();
    ~LibraryDatabase();

    /** Opens (creating if needed) the SQLite file and enables WAL + busy
        timeout. Returns an error string, empty on success. */
    juce::String open (const juce::File& dbFile);
    void close();
    bool isOpen() const;

    /** Creates all tables and indexes if missing. */
    juce::String initialiseSchema();

    SqliteDb& db();
    const SqliteDb& db() const;

    /** Coarse lock for multi-statement transactions. Use judiciously. */
    juce::CriticalSection& lock();

    /** Begins/commits a transaction (assumes lock held by caller). */
    bool beginTransaction();
    bool commitTransaction();
    void rollbackTransaction();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryDatabase)
};

} // namespace sp
