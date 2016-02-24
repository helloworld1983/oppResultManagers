#include <cSQLiteOutputManager.h>

Register_PerRunConfigOption(CFGID_SQLITEOUTMGR_FILE, "sqliteoutputmanager-file", CFG_FILENAME,
        "${resultdir}/${configname}-${runnumber}.sqlite3", "Object name of database connection parameters");
Register_PerRunConfigOption(CFGID_SQLITEMGR_COMMIT_FREQ, "sqliteoutputmanager-commit-freq", CFG_INT, "10000",
        "COMMIT every n INSERTs, default=10");

#define SQL_SELECT_MODULE "SELECT * FROM module;"
#define SQL_INSERT_MODULE "INSERT INTO module(name) VALUES(?);"
#define SQL_SELECT_NAME "SELECT * FROM name;"
#define SQL_INSERT_NAME "INSERT INTO name(name) VALUES(?);"
#define SQL_INSERT_RUN "INSERT INTO run(runid) VALUES(?);"
#define SQL_SELECT_RUN "SELECT id FROM run WHERE runid=?;"
#define SQL_INSERT_RUN_ATTR "INSERT INTO runattr(runid,nameid,value) VALUES(?,?,?);"

sqlite3* cSQLiteOutputManager::connection = nullptr;
bool cSQLiteOutputManager::hasTransaction = false;
size_t cSQLiteOutputManager::users = 0;

std::unordered_map<std::string, size_t> cSQLiteOutputManager::moduleIDMap;
std::unordered_map<std::string, size_t> cSQLiteOutputManager::nameIDMap;

cSQLiteOutputManager::cSQLiteOutputManager()
{
    runid = 0;
    users++;
}

cSQLiteOutputManager::~cSQLiteOutputManager()
{
    users--;
    if (connection && users == 0)
    {
        sqlite3_close(connection);
    }
}

void cSQLiteOutputManager::startRun()
{
    if (!connection)
    {
        std::string cfgobj = omnetpp::getEnvir()->getConfig()->getAsFilename(CFGID_SQLITEOUTMGR_FILE);
        int rc = sqlite3_open(cfgobj.c_str(), &connection);
        if (rc)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't open database: %s", sqlite3_errmsg(connection));
        }
        char * zErrMsg = nullptr;
        rc = sqlite3_exec(connection, "PRAGMA synchronous = OFF;", nullptr, nullptr, &zErrMsg);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't set PRAGMA synchronous = OFF: %s", zErrMsg);
        }
        rc = sqlite3_exec(connection, "PRAGMA journal_mode = MEMORY;", nullptr, nullptr, &zErrMsg);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't set PRAGMA journal_mode = MEMORY: %s", zErrMsg);
        }
        rc = sqlite3_exec(connection, "PRAGMA cache_size = -16768;", nullptr, nullptr, &zErrMsg);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't set PRAGMA cache_size: %s", zErrMsg);
        }
        //We don't need foreign_keys check (performance reasons)
        rc = sqlite3_exec(connection, "PRAGMA foreign_keys = OFF;", nullptr, nullptr, &zErrMsg);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't set PRAGMA foreign_keys: %s", zErrMsg);
        }
        //We trust our integrity and turn off the constraint checks
        rc = sqlite3_exec(connection, "PRAGMA ignore_check_constraints = ON;", nullptr, nullptr, &zErrMsg);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't set PRAGMA ignore_check_constraints: %s",
                    zErrMsg);
        }
    }

    commitFreq = omnetpp::getEnvir()->getConfig()->getAsInt(CFGID_SQLITEMGR_COMMIT_FREQ);

    char * zErrMsg = nullptr;

    if (!hasTransaction)
    {
        int rc = sqlite3_exec(connection, "BEGIN;", nullptr, nullptr, &zErrMsg);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't begin transaction: %s", zErrMsg);
        }
        hasTransaction = true;
    }

    //Create Tables
    int rc =
            sqlite3_exec(connection,
                    "CREATE TABLE IF NOT EXISTS run (\
         id INTEGER PRIMARY KEY,\
         runid TEXT NOT NULL UNIQUE\
       );",
                    nullptr, nullptr, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't create table 'run': %s", zErrMsg);
    }
    rc =
            sqlite3_exec(connection,
                    "CREATE TABLE IF NOT EXISTS module(\
                 id INTEGER PRIMARY KEY,\
                 name TEXT NOT NULL UNIQUE\
               );",
                    nullptr, nullptr, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't create table 'module': %s", zErrMsg);
    }
    rc =
            sqlite3_exec(connection,
                    "CREATE TABLE IF NOT EXISTS name(\
                 id INTEGER PRIMARY KEY,\
                 name TEXT NOT NULL UNIQUE\
              );",
                    nullptr, nullptr, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't create table 'name': %s", zErrMsg);
    }
    rc =
            sqlite3_exec(connection,
                    "CREATE TABLE IF NOT EXISTS runattr(\
                                         id INTEGER PRIMARY KEY,\
                                         runid INT NOT NULL,\
                                         nameid INT NOT NULL,\
                                         value TEXT NOT NULL,\
                                         FOREIGN KEY (runid) REFERENCES run(id) ON DELETE CASCADE,\
                                         FOREIGN KEY (nameid) REFERENCES name(id) ON DELETE CASCADE\
                                      );",
                    nullptr, nullptr, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't create table 'runattr': %s", zErrMsg);
    }
    rc =
            sqlite3_exec(connection,
                    "CREATE VIEW IF NOT EXISTS runattr_names AS \
                                         SELECT runattr.id AS id, runattr.runid AS runid, \
                                         name.name AS name, runattr.value AS value FROM runattr \
                                         JOIN name ON name.id = runattr.nameid;",
                    nullptr, nullptr, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't create view 'scalarattr_names': %s", zErrMsg);
    }

    flush();

    std::string runid_var = omnetpp::getEnvir()->getConfigEx()->getVariable(CFGVAR_RUNID);

    sqlite3_stmt *stmt;

    //Find already existing modules and names
    rc = sqlite3_exec(connection, SQL_SELECT_MODULE,
            [] (void *data, int argc, char **argv,__attribute__((__unused__)) char **azColName) -> int
            {
                cSQLiteOutputManager *thisManager = (cSQLiteOutputManager*)data;
                if(argc!=2)
                {
                    throw omnetpp::cRuntimeError("wrong number of columns returned in select!");
                }
                thisManager->moduleIDMap[argv[1]] = atoi(argv[0]);
                return SQLITE_OK;
            }, (void*) this, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        throw omnetpp::cRuntimeError("SQLiteOutputManager:: Error in select (SQL_SELECT_MODULE): %s", zErrMsg);
    }

    rc = sqlite3_exec(connection, SQL_SELECT_NAME,
            [] (void *data, int argc, char **argv,__attribute__((__unused__)) char **azColName) -> int
            {
                cSQLiteOutputManager *thisManager = (cSQLiteOutputManager*)data;
                if(argc!=2)
                {
                    throw omnetpp::cRuntimeError("wrong number of columns returned in select!");
                }
                thisManager->nameIDMap[argv[1]] = atoi(argv[0]);
                return SQLITE_OK;
            }, (void*) this, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        throw omnetpp::cRuntimeError("SQLiteOutputManager:: Error in select (SQL_SELECT_NAME): %s", zErrMsg);
    }

    //Try find already existing run
    rc = sqlite3_prepare(connection, SQL_SELECT_RUN, -1, &stmt, 0);
    if (rc != SQLITE_OK)
    {
        throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not prepare statement: %s",
                sqlite3_errmsg(connection));
    }
    rc = sqlite3_bind_text(stmt, 1, runid_var.c_str(), -1, SQLITE_STATIC);
    if (rc != SQLITE_OK)
    {
        throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not bind runid");
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        runid = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (!runid)
    {
        rc = sqlite3_prepare(connection, SQL_INSERT_RUN, -1, &stmt, 0);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not prepare statement: %s",
                    sqlite3_errmsg(connection));
        }
        rc = sqlite3_bind_text(stmt, 1, runid_var.c_str(), -1, SQLITE_STATIC);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not bind runid");
        }

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not execute statement (SQL_INSERT_RUN): %s",
                    sqlite3_errmsg(connection));
        }
        runid = sqlite3_last_insert_rowid(connection);
        sqlite3_reset(stmt);

        sqlite3_stmt *insertRunAttrStmt;
        rc = sqlite3_prepare_v2(connection, SQL_INSERT_RUN_ATTR, strlen(SQL_INSERT_RUN_ATTR), &insertRunAttrStmt, 0);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not prepare statement (SQL_INSERT_RUN_ATTR): %s",
                    sqlite3_errmsg(connection));
        }
        //INSERT runattr
        std::vector<const char *> keys1 = omnetpp::getEnvir()->getConfigEx()->getPredefinedVariableNames();
        std::vector<const char *> keys2 = omnetpp::getEnvir()->getConfigEx()->getIterationVariableNames();
        keys1.insert(keys1.end(), keys2.begin(), keys2.end());
        for (size_t i = 0; i < keys1.size(); i++)
        {
            rc = sqlite3_bind_int64(insertRunAttrStmt, 1, static_cast<sqlite3_int64>(runid));
            if (rc != SQLITE_OK)
            {
                throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not bind runid.");
            }
            rc = sqlite3_bind_int64(insertRunAttrStmt, 2, static_cast<sqlite3_int64>(getNameID(keys1[i])));
            if (rc != SQLITE_OK)
            {
                throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not bind nameid.");
            }
            rc = sqlite3_bind_text(insertRunAttrStmt, 3, omnetpp::getEnvir()->getConfigEx()->getVariable(keys1[i]), -1,
            SQLITE_STATIC);
            if (rc != SQLITE_OK)
            {
                throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not bind value.");
            }

            rc = sqlite3_step(insertRunAttrStmt);
            if (rc != SQLITE_DONE)
            {
                throw omnetpp::cRuntimeError(
                        "cSQLiteOutputVectorManager:: Could not execute statement (SQL_INSERT_VECTOR_ATTR): %s",
                        sqlite3_errmsg(connection));
            }
            sqlite3_clear_bindings(insertRunAttrStmt);
            sqlite3_reset(insertRunAttrStmt);
        }
        sqlite3_finalize(insertRunAttrStmt);
    }
}

void cSQLiteOutputManager::endRun()
{
    if (connection && hasTransaction)
    {
        char * zErrMsg = nullptr;
        int rc = sqlite3_exec(connection, "COMMIT;", nullptr, nullptr, &zErrMsg);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't commit: %s", zErrMsg);
        }
        hasTransaction = false;
    }
}

void cSQLiteOutputManager::flush()
{
    if (connection)
    {
        char * zErrMsg = nullptr;
        int rc = sqlite3_exec(connection, "COMMIT; BEGIN;", nullptr, nullptr, &zErrMsg);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Can't commit: %s", zErrMsg);
        }
    }
}

size_t cSQLiteOutputManager::getModuleID(std::string module)
{
    std::unordered_map<std::string, size_t>::const_iterator found = moduleIDMap.find(module);
    if (found != moduleIDMap.end())
    {
        return (*found).second;
    }
    else
    {
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare(connection, SQL_INSERT_MODULE, -1, &stmt, 0);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not prepare statement: %s",
                    sqlite3_errmsg(connection));
        }
        rc = sqlite3_bind_text(stmt, 1, module.c_str(), -1, SQLITE_STATIC);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not bind module: %s", sqlite3_errmsg(connection));
        }
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not execute statement (SQL_INSERT_MODULE): %s",
                    sqlite3_errmsg(connection));
        }
        size_t id = sqlite3_last_insert_rowid(connection);
        moduleIDMap[module] = id;
        sqlite3_reset(stmt);

        flush();
        return id;
    }
}

size_t cSQLiteOutputManager::getNameID(std::string name)
{
    std::unordered_map<std::string, size_t>::const_iterator found = nameIDMap.find(name);
    if (found != nameIDMap.end())
    {
        return (*found).second;
    }
    else
    {
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare(connection, SQL_INSERT_NAME, -1, &stmt, 0);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not prepare statement: %s",
                    sqlite3_errmsg(connection));
        }
        rc = sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
        if (rc != SQLITE_OK)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not bind name: %s", sqlite3_errmsg(connection));
        }
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE)
        {
            throw omnetpp::cRuntimeError("SQLiteOutputManager:: Could not execute statement (SQL_INSERT_NAME): %s",
                    sqlite3_errmsg(connection));
        }
        size_t id = sqlite3_last_insert_rowid(connection);
        nameIDMap[name] = id;
        sqlite3_reset(stmt);

        flush();
        return id;
    }
}
