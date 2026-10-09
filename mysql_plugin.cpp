/*
 * MySQL support for game scripts, as a coduox server plugin.
 *
 * Ported from the CoD4X MySQL plugin, https://github.com/callofduty4x/mysql
 * (commit 9bd45f8), by Sharpienero, MichaelHillcox and T-Max. This file is a
 * modified version of that work and is distributed under the same license,
 * the GNU Affero General Public License version 3; see LICENSE.
 *
 * Changes from the original:
 *  - Built against the coduox plugin interface (coduomp_plugin.h) and the
 *    MariaDB or MySQL C client library on 64-bit targets.
 *  - All MYSQL_CONNECTION_COUNT connections are usable; the original refused
 *    the last free slot and wrote past the connection table when all were
 *    in use.
 *  - mysql_num_rows, mysql_num_fields, mysql_fetch_row and mysql_fetch_rows
 *    no longer dereference a missing result set after a statement that
 *    returns none, such as INSERT or UPDATE.
 *  - A failed connection attempt releases its client handle.
 *  - Connect, read and write timeouts are applied, from the cvars
 *    mysql_connect_timeout, mysql_read_timeout and mysql_write_timeout
 *    (seconds), so an unreachable database cannot stall the server
 *    indefinitely.
 *  - mysql_real_escape_string is added, so scripts can put player-supplied
 *    text into a query safely.
 */

#include "coduomp_plugin.h"

#include <mysql.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

CODUOMP_PLUGIN_ENTRY()

namespace {

const int kConnectionCount = 4;
const int kPluginVersionMajor = 2;
const int kPluginVersionMinor = 2;
const int kDefaultPort = 3306;
const int kDefaultConnectTimeout = 5;
const int kDefaultReadWriteTimeout = 10;
const int kMaxTimeout = 300;
/* Value of lastErrno before any query ran on a connection. */
const unsigned int kNoQueryYet = ~0u;

/* MySQL 8 dropped my_bool in favour of bool; MariaDB keeps it. */
#if defined(MARIADB_BASE_VERSION) || !defined(MYSQL_VERSION_ID)
typedef my_bool ReconnectFlag;
#elif MYSQL_VERSION_ID < 80000
typedef my_bool ReconnectFlag;
#else
typedef bool ReconnectFlag;
#endif

struct Connection {
    MYSQL handle;
    MYSQL_RES *result;
    bool inUse;
    unsigned int lastErrno;
};

Connection g_connections[kConnectionCount];

/* Raises a script error; does not return. Nothing owned by the caller may
 * still need releasing when this is called. */
void scriptError(const char *format, ...)
{
    char message[1024];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    Plugin_Scr_Error(message);
}

void releaseResult(Connection &connection)
{
    if (connection.result != NULL) {
        mysql_free_result(connection.result);
        connection.result = NULL;
    }
}

void closeConnection(Connection &connection)
{
    releaseResult(connection);
    mysql_close(&connection.handle);
    connection.inUse = false;
    connection.lastErrno = kNoQueryYet;
}

void closeAllConnections()
{
    for (int i = 0; i < kConnectionCount; ++i) {
        if (g_connections[i].inUse) {
            closeConnection(g_connections[i]);
        }
    }
}

/* Seconds from a cvar, or fallback when the cvar is unset or out of
 * 1..kMaxTimeout. */
unsigned int timeoutFromCvar(const char *name, int fallback)
{
    const int seconds = Plugin_Cvar_GetInt(name);

    return static_cast<unsigned int>(
        seconds >= 1 && seconds <= kMaxTimeout ? seconds : fallback);
}

/* The open connection named by script argument argumentIndex. Raises a
 * script error for an out-of-range handle or a closed connection. */
Connection &connectionFromArgument(unsigned int argumentIndex)
{
    const int index = Plugin_Scr_GetInt(argumentIndex);

    if (index < 0 || index >= kConnectionCount) {
        Plugin_Scr_ParamError(static_cast<int32_t>(argumentIndex),
                              "Incorrect connection handle");
        return g_connections[0];
    }
    if (!g_connections[index].inUse) {
        Plugin_Scr_Error("'mysql_real_connect' must be called before.");
    }
    return g_connections[index];
}

/* Raises a script error unless a query has completed on the connection. */
void requireQuery(const Connection &connection)
{
    if (connection.result == NULL && connection.lastErrno != 0) {
        Plugin_Scr_Error("'mysql_query' must be called before.");
    }
}

/* Pushes one result row as an array keyed by column name. keys holds one
 * script string per column. */
void addRow(MYSQL_ROW row, const int32_t *keys, unsigned int columnCount)
{
    Plugin_Scr_MakeArray();
    for (unsigned int column = 0; column < columnCount; ++column) {
        if (row[column] == NULL) {
            Plugin_Scr_AddUndefined();
        } else {
            Plugin_Scr_AddString(row[column]);
        }
        Plugin_Scr_AddArrayKey(keys[column]);
    }
}

/* Allocates one script string per column name of result. Returns NULL, with
 * nothing allocated, when memory runs out or a column has no name; the
 * caller frees a non-NULL result with Plugin_Free. */
int32_t *columnKeys(MYSQL_RES *result, unsigned int columnCount)
{
    int32_t *keys = static_cast<int32_t *>(
        Plugin_Malloc(columnCount * sizeof(int32_t)));

    if (keys == NULL) {
        return NULL;
    }
    mysql_field_seek(result, 0);
    for (unsigned int column = 0; column < columnCount; ++column) {
        const MYSQL_FIELD *field = mysql_fetch_field(result);

        if (field == NULL || field->name == NULL) {
            Plugin_Free(keys);
            return NULL;
        }
        keys[column] = Plugin_Scr_AllocString(field->name);
    }
    return keys;
}

/* handle = mysql_real_connect(host, user, passwd, db, [port]) */
void Scr_MySQL_Real_Connect_f()
{
    const unsigned int argumentCount = Plugin_Scr_GetNumParam();

    if (argumentCount != 4 && argumentCount != 5) {
        Plugin_Scr_Error("Usage: handle = mysql_real_connect(<str host>, "
                         "<str user>, <str passwd>, <str db or \"\">, "
                         "[int port=3306]);");
    }

    int port = kDefaultPort;
    if (argumentCount == 5) {
        port = Plugin_Scr_GetInt(4);
        if (port < 0 || port > 65535) {
            Plugin_Scr_ParamError(4, "Incorrect port: must be any integer "
                                     "from 0 to 65535");
        }
    }

    int slot = 0;
    while (slot < kConnectionCount && g_connections[slot].inUse) {
        ++slot;
    }
    if (slot == kConnectionCount) {
        scriptError("MySQL connect error: max connections exceeded (%d)",
                    kConnectionCount);
    }

    /* The server reuses its string buffer between calls, so each argument
     * is copied before the next one is read. */
    char host[256];
    char user[128];
    char password[256];
    char database[128];
    snprintf(host, sizeof(host), "%s", Plugin_Scr_GetString(0));
    snprintf(user, sizeof(user), "%s", Plugin_Scr_GetString(1));
    snprintf(password, sizeof(password), "%s", Plugin_Scr_GetString(2));
    snprintf(database, sizeof(database), "%s", Plugin_Scr_GetString(3));
    /* "localhost" selects the Unix socket in the client library; the
     * original plugin always connects over TCP. */
    if (strcmp(host, "localhost") == 0) {
        snprintf(host, sizeof(host), "127.0.0.1");
    }

    Connection &connection = g_connections[slot];
    MYSQL *const handle = &connection.handle;
    if (mysql_init(handle) == NULL) {
        scriptError("MySQL init[%d] failed: out of memory", slot);
    }

    const unsigned int connectTimeout =
        timeoutFromCvar("mysql_connect_timeout", kDefaultConnectTimeout);
    const unsigned int readTimeout =
        timeoutFromCvar("mysql_read_timeout", kDefaultReadWriteTimeout);
    const unsigned int writeTimeout =
        timeoutFromCvar("mysql_write_timeout", kDefaultReadWriteTimeout);
    const ReconnectFlag reconnect = 1;
    mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
    mysql_options(handle, MYSQL_OPT_READ_TIMEOUT, &readTimeout);
    mysql_options(handle, MYSQL_OPT_WRITE_TIMEOUT, &writeTimeout);
    mysql_options(handle, MYSQL_OPT_RECONNECT, &reconnect);

    if (mysql_real_connect(handle, host, user, password,
                           database[0] != '\0' ? database : NULL,
                           static_cast<unsigned int>(port), NULL,
                           0) == NULL) {
        char reason[512];
        const unsigned int code = mysql_errno(handle);

        snprintf(reason, sizeof(reason), "%s", mysql_error(handle));
        mysql_close(handle);
        scriptError("MySQL connect error: (%u) %s", code, reason);
    }

    connection.result = NULL;
    connection.inUse = true;
    connection.lastErrno = kNoQueryYet;
    Plugin_Scr_AddInt(slot);
}

/* mysql_close(handle) */
void Scr_MySQL_Close_f()
{
    if (Plugin_Scr_GetNumParam() != 1) {
        Plugin_Scr_Error("Usage: mysql_close(<handle>);");
    }

    Connection &connection = connectionFromArgument(0);
    Plugin_DPrintf("Closing MySQL connection: %d\n",
                   static_cast<int>(&connection - g_connections));
    closeConnection(connection);
}

/* mysql_query(handle, query) */
void Scr_MySQL_Query_f()
{
    if (Plugin_Scr_GetNumParam() != 2) {
        Plugin_Scr_Error("Usage: mysql_query(<handle>, <string query>);");
    }

    Connection &connection = connectionFromArgument(0);
    MYSQL *const handle = &connection.handle;
    releaseResult(connection);

    if (mysql_query(handle, Plugin_Scr_GetString(1)) != 0) {
        connection.lastErrno = mysql_errno(handle);
        scriptError("MySQL query error: (%u) %s", connection.lastErrno,
                    mysql_error(handle));
    }

    /* A statement without a result set, such as INSERT, stores no result
     * and leaves the error number at 0. */
    connection.result = mysql_store_result(handle);
    connection.lastErrno = mysql_errno(handle);
    if (connection.result == NULL && connection.lastErrno != 0) {
        scriptError("MySQL store error: (%u) %s", connection.lastErrno,
                    mysql_error(handle));
    }
}

/* rows = mysql_affected_rows(handle) */
void Scr_MySQL_Affected_Rows_f()
{
    if (Plugin_Scr_GetNumParam() != 1) {
        Plugin_Scr_Error("Usage: rows = mysql_affected_rows(<handle>);");
    }

    Connection &connection = connectionFromArgument(0);
    requireQuery(connection);
    Plugin_Scr_AddInt(
        static_cast<int32_t>(mysql_affected_rows(&connection.handle)));
}

/* rows = mysql_num_rows(handle) */
void Scr_MySQL_Num_Rows_f()
{
    if (Plugin_Scr_GetNumParam() != 1) {
        Plugin_Scr_Error("Usage: mysql_num_rows(<handle>);");
    }

    Connection &connection = connectionFromArgument(0);
    requireQuery(connection);
    Plugin_Scr_AddInt(connection.result != NULL
                          ? static_cast<int32_t>(
                                mysql_num_rows(connection.result))
                          : 0);
}

/* fields = mysql_num_fields(handle) */
void Scr_MySQL_Num_Fields_f()
{
    if (Plugin_Scr_GetNumParam() != 1) {
        Plugin_Scr_Error("Usage: mysql_num_fields(<handle>);");
    }

    Connection &connection = connectionFromArgument(0);
    requireQuery(connection);
    Plugin_Scr_AddInt(connection.result != NULL
                          ? static_cast<int32_t>(
                                mysql_num_fields(connection.result))
                          : 0);
}

/* row = mysql_fetch_row(handle): the next row as an array keyed by column
 * name, or undefined when no row is left. */
void Scr_MySQL_Fetch_Row_f()
{
    if (Plugin_Scr_GetNumParam() != 1) {
        Plugin_Scr_Error("Usage: mysql_fetch_row(<handle>);");
    }

    Connection &connection = connectionFromArgument(0);
    requireQuery(connection);
    if (connection.result == NULL) {
        return;
    }

    const MYSQL_ROW row = mysql_fetch_row(connection.result);
    if (row == NULL) {
        return;
    }

    const unsigned int columnCount = mysql_num_fields(connection.result);
    int32_t *const keys = columnKeys(connection.result, columnCount);
    if (keys == NULL) {
        Plugin_Scr_Error("MySQL fetch error: could not read column names");
    }
    addRow(row, keys, columnCount);
    Plugin_Free(keys);
}

/* rows = mysql_fetch_rows(handle): every remaining row, as an array of
 * arrays keyed by column name; empty when no row is left. */
void Scr_MySQL_Fetch_Rows_f()
{
    if (Plugin_Scr_GetNumParam() != 1) {
        Plugin_Scr_Error("Usage: mysql_fetch_rows(<handle>);");
    }

    Connection &connection = connectionFromArgument(0);
    requireQuery(connection);

    int32_t *keys = NULL;
    unsigned int columnCount = 0;
    if (connection.result != NULL) {
        columnCount = mysql_num_fields(connection.result);
        keys = columnKeys(connection.result, columnCount);
        if (keys == NULL) {
            Plugin_Scr_Error(
                "MySQL fetch error: could not read column names");
        }
    }

    Plugin_Scr_MakeArray();
    if (connection.result != NULL) {
        MYSQL_ROW row;

        while ((row = mysql_fetch_row(connection.result)) != NULL) {
            addRow(row, keys, columnCount);
            Plugin_Scr_AddArray();
        }
    }
    Plugin_Free(keys);
}

/* escaped = mysql_real_escape_string(text)
 * escaped = mysql_real_escape_string(handle, text)
 *
 * Returns text with the characters that end or alter a quoted SQL string
 * (NUL, newline, carriage return, backslash, both quotes and Ctrl-Z)
 * escaped, for use between quotes in a query.
 *
 * With a handle, the connection's character set and SQL mode decide the
 * escaping. Without one, the first open connection is used; when none is
 * open, the escaping assumes a character set in which those bytes never
 * occur inside a multi-byte character, such as utf8 or latin1. */
void Scr_MySQL_Real_Escape_String_f()
{
    const unsigned int argumentCount = Plugin_Scr_GetNumParam();

    if (argumentCount != 1 && argumentCount != 2) {
        Plugin_Scr_Error("Usage: mysql_real_escape_string([handle], "
                         "<string>);");
    }

    MYSQL *handle = NULL;
    if (argumentCount == 2) {
        handle = &connectionFromArgument(0).handle;
    } else {
        for (int i = 0; i < kConnectionCount && handle == NULL; ++i) {
            if (g_connections[i].inUse) {
                handle = &g_connections[i].handle;
            }
        }
    }

    /* The escaped form is at most twice as long, plus the terminator. The
     * input is copied because the server may reuse its string buffer. */
    const std::string text = Plugin_Scr_GetString(argumentCount - 1);
    std::string escaped(text.size() * 2 + 1, '\0');
    const unsigned long length =
        handle != NULL
            ? mysql_real_escape_string(handle, &escaped[0], text.c_str(),
                                       static_cast<unsigned long>(
                                           text.size()))
            : mysql_escape_string(&escaped[0], text.c_str(),
                                  static_cast<unsigned long>(text.size()));

    if (handle != NULL && length == static_cast<unsigned long>(-1)) {
        scriptError("MySQL escape error: (%u) %s", mysql_errno(handle),
                    mysql_error(handle));
    }
    escaped.resize(length);
    Plugin_Scr_AddString(escaped.c_str());
}

} // namespace

PCL void OnInfoRequest(pluginInfo_t *info)
{
    info->pluginVersion.major = kPluginVersionMajor;
    info->pluginVersion.minor = kPluginVersionMinor;
    snprintf(info->fullName, sizeof(info->fullName), "MySQL Plugin");
    snprintf(info->shortDescription, sizeof(info->shortDescription),
             "MySQL for game scripts; port of the CoD4X MySQL plugin by "
             "Sharpienero, MichaelHillcox, T-Max");
    snprintf(info->longDescription, sizeof(info->longDescription),
             "Lets game scripts query a MySQL or MariaDB database over up "
             "to %d connections. Client library version: %lu",
             kConnectionCount, mysql_get_client_version());
}

PCL int OnInit()
{
    static const struct {
        const char *name;
        coduomp_plugin_function_t function;
    } functions[] = {
        { "mysql_real_connect", Scr_MySQL_Real_Connect_f },
        { "mysql_close", Scr_MySQL_Close_f },
        { "mysql_affected_rows", Scr_MySQL_Affected_Rows_f },
        { "mysql_query", Scr_MySQL_Query_f },
        { "mysql_num_rows", Scr_MySQL_Num_Rows_f },
        { "mysql_num_fields", Scr_MySQL_Num_Fields_f },
        { "mysql_fetch_row", Scr_MySQL_Fetch_Row_f },
        { "mysql_fetch_rows", Scr_MySQL_Fetch_Rows_f },
        { "mysql_real_escape_string", Scr_MySQL_Real_Escape_String_f },
    };

    if (coduomp_plugin_api == NULL) {
        return 1;
    }
    for (int i = 0; i < kConnectionCount; ++i) {
        g_connections[i].result = NULL;
        g_connections[i].inUse = false;
        g_connections[i].lastErrno = kNoQueryYet;
    }
    if (mysql_library_init(0, NULL, NULL) != 0) {
        Plugin_PrintError("MySQL client library failed to initialize");
        return 1;
    }
    for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
        if (!Plugin_ScrAddFunction(functions[i].name,
                                   functions[i].function)) {
            Plugin_PrintError("could not register script function %s",
                              functions[i].name);
            mysql_library_end();
            return 1;
        }
    }
    return 0;
}

/* The scripts that held the handles are gone, so every connection is
 * closed; the next map connects again. */
PCL void OnExitLevel()
{
    closeAllConnections();
}

PCL void OnTerminate()
{
    closeAllConnections();
    mysql_library_end();
}
