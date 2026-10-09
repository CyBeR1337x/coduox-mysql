/*
 * End-to-end test for mysql.so.
 *
 * Stands in for the game server: it loads the plugin, hands it a table of
 * server functions that implement a small script stack, calls the script
 * functions the plugin registers, and checks what they return. A script
 * error is a C++ exception thrown through the plugin's frames, which is how
 * the real server unwinds out of a failing script function.
 *
 * It needs a reachable MySQL or MariaDB server and an account that may
 * create and drop the database `coduox_mysql_test`:
 *
 *   MYSQL_TEST_HOST      default 127.0.0.1
 *   MYSQL_TEST_PORT      default 3306
 *   MYSQL_TEST_USER      default root
 *   MYSQL_TEST_PASSWORD  default empty
 *
 * Exit status is the number of failed checks.
 */

#include "coduomp_plugin.h"

#include <dlfcn.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ScriptError {
    std::string text;
};

struct Value {
    enum Kind { Undefined, Int, Float, String, Array };

    Kind kind = Undefined;
    long long integer = 0;
    double number = 0;
    std::string text;
    /* Array elements; an empty key marks an integer-indexed element. */
    std::vector<std::pair<std::string, Value>> items;

    std::string describe() const
    {
        switch (kind) {
        case Undefined:
            return "undefined";
        case Int:
            return std::to_string(integer);
        case Float:
            return std::to_string(number);
        case String:
            return "\"" + text + "\"";
        case Array:
            break;
        }

        std::string out = "[";
        for (size_t i = 0; i < items.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            if (!items[i].first.empty()) {
                out += items[i].first + ": ";
            }
            out += items[i].second.describe();
        }
        return out + "]";
    }
};

Value Int(long long value)
{
    Value v;
    v.kind = Value::Int;
    v.integer = value;
    return v;
}

Value Str(const std::string &value)
{
    Value v;
    v.kind = Value::String;
    v.text = value;
    return v;
}

std::vector<Value> g_arguments;
std::vector<Value> g_stack;
std::vector<std::string> g_scriptStrings;
std::map<std::string, coduomp_plugin_function_t> g_functions;
std::map<std::string, int> g_cvars;
int g_failures;

/* ---- server function table ---------------------------------------- */

void apiPrintf(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    printf("    [server] ");
    vprintf(format, arguments);
    va_end(arguments);
}

void apiDPrintf(const char *, ...) {}

void apiPrintError(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    printf("    [server error] ");
    vprintf(format, arguments);
    printf("\n");
    va_end(arguments);
}

void *apiMalloc(size_t size) { return malloc(size); }
void apiFree(void *memory) { free(memory); }

void apiEmptyString(const char *, char *buffer, int32_t size)
{
    if (size > 0) {
        buffer[0] = '\0';
    }
}

int32_t apiCvarGetInt(const char *name)
{
    return g_cvars.count(name) != 0 ? g_cvars[name] : 0;
}

float apiCvarGetFloat(const char *) { return 0.0f; }
void apiCvarSet(const char *, const char *) {}
int32_t apiAddCommand(const char *, coduomp_plugin_function_t) { return 1; }
int32_t apiCmdArgc() { return 0; }
const char *apiCmdArgv(int32_t) { return ""; }
void apiExecuteCommand(const char *) {}

int32_t apiScrAddFunction(const char *name, coduomp_plugin_function_t function)
{
    g_functions[name] = function;
    return 1;
}

uint32_t apiScrGetNumParam()
{
    return static_cast<uint32_t>(g_arguments.size());
}

const Value &argument(uint32_t index)
{
    if (index >= g_arguments.size()) {
        throw ScriptError{"parameter does not exist"};
    }
    return g_arguments[index];
}

int32_t apiScrGetInt(uint32_t index)
{
    if (argument(index).kind != Value::Int) {
        throw ScriptError{"parameter is not an int"};
    }
    return static_cast<int32_t>(argument(index).integer);
}

float apiScrGetFloat(uint32_t index)
{
    return static_cast<float>(argument(index).number);
}

/* One buffer reused by every call, so the test catches a plugin that keeps
 * a returned string across another server call. */
const char *apiScrGetString(uint32_t index)
{
    static char buffer[8192];

    if (argument(index).kind != Value::String) {
        throw ScriptError{"parameter is not a string"};
    }
    snprintf(buffer, sizeof(buffer), "%s", argument(index).text.c_str());
    return buffer;
}

void apiScrAddInt(int32_t value) { g_stack.push_back(Int(value)); }

void apiScrAddFloat(float value)
{
    Value v;
    v.kind = Value::Float;
    v.number = value;
    g_stack.push_back(v);
}

void apiScrAddString(const char *value) { g_stack.push_back(Str(value)); }
void apiScrAddUndefined() { g_stack.push_back(Value()); }

void apiScrMakeArray()
{
    Value v;
    v.kind = Value::Array;
    g_stack.push_back(v);
}

/* Moves the top value into the array below it. */
void appendToArray(const std::string &key)
{
    if (g_stack.size() < 2 ||
        g_stack[g_stack.size() - 2].kind != Value::Array) {
        printf("TEST HARNESS: array element added without an array\n");
        exit(100);
    }
    Value element = g_stack.back();
    g_stack.pop_back();
    g_stack.back().items.push_back(std::make_pair(key, element));
}

void apiScrAddArray() { appendToArray(""); }

void apiScrAddArrayKey(int32_t stringIndex)
{
    if (stringIndex <= 0 ||
        stringIndex > static_cast<int32_t>(g_scriptStrings.size())) {
        printf("TEST HARNESS: unknown script string %d\n", stringIndex);
        exit(100);
    }
    appendToArray(g_scriptStrings[stringIndex - 1]);
}

int32_t apiScrAllocString(const char *text)
{
    g_scriptStrings.push_back(text);
    return static_cast<int32_t>(g_scriptStrings.size());
}

void apiScrError(const char *message) { throw ScriptError{message}; }

void apiScrParamError(int32_t index, const char *message)
{
    throw ScriptError{"parameter " + std::to_string(index + 1) + ": " +
                      message};
}

int32_t apiZero() { return 0; }
int32_t apiZeroForClient(int32_t) { return 0; }

void apiEmptyClientString(int32_t, char *buffer, int32_t size)
{
    if (size > 0) {
        buffer[0] = '\0';
    }
}

void apiDropClient(int32_t, const char *) {}

const coduomp_plugin_api_t g_api = {
    CODUOMP_PLUGIN_API_VERSION,
    static_cast<uint32_t>(sizeof(coduomp_plugin_api_t)),
    apiPrintf,
    apiDPrintf,
    apiPrintError,
    apiMalloc,
    apiFree,
    apiEmptyString,
    apiCvarGetInt,
    apiCvarGetFloat,
    apiCvarSet,
    apiAddCommand,
    apiCmdArgc,
    apiCmdArgv,
    apiExecuteCommand,
    apiScrAddFunction,
    apiScrGetNumParam,
    apiScrGetInt,
    apiScrGetFloat,
    apiScrGetString,
    apiScrAddInt,
    apiScrAddFloat,
    apiScrAddString,
    apiScrAddUndefined,
    apiScrMakeArray,
    apiScrAddArray,
    apiScrAddArrayKey,
    apiScrAllocString,
    apiScrError,
    apiScrParamError,
    apiZero,
    apiZeroForClient,
    apiEmptyClientString,
    apiZeroForClient,
    apiEmptyClientString,
    apiDropClient
};

/* ---- test helpers -------------------------------------------------- */

/* Calls a registered script function. Returns a description of its return
 * value, or "ERROR: <text>" when it raised a script error. */
std::string call(const char *name, const std::vector<Value> &arguments)
{
    g_arguments = arguments;
    g_stack.clear();
    if (g_functions.count(name) == 0) {
        return "NOT REGISTERED";
    }
    try {
        g_functions[name]();
    } catch (const ScriptError &error) {
        return "ERROR: " + error.text;
    }
    if (g_stack.size() > 1) {
        return "TEST HARNESS: " + std::to_string(g_stack.size()) +
               " values returned";
    }
    return g_stack.empty() ? "undefined" : g_stack.back().describe();
}

void check(const char *what, bool passed, const std::string &detail)
{
    printf("%-50s %s  %s\n", what, passed ? "ok  " : "FAIL",
           detail.substr(0, 140).c_str());
    if (!passed) {
        ++g_failures;
    }
}

/* Passes when the result starts with expected. */
void expect(const char *what, const std::string &result, const char *expected)
{
    check(what, result.compare(0, strlen(expected), expected) == 0, result);
}

std::string query(const std::string &sql)
{
    return call("mysql_query", {Int(0), Str(sql)});
}

const char *environment(const char *name, const char *fallback)
{
    const char *value = getenv(name);

    return value != NULL && value[0] != '\0' ? value : fallback;
}

/* Fetches an exported function; POSIX dlsym returns void * for functions
 * too, so the address is copied rather than cast. */
template <typename Function>
Function symbol(void *library, const char *name)
{
    void *address = dlsym(library, name);
    Function function = NULL;

    memcpy(&function, &address, sizeof(function));
    return function;
}

} // namespace

int main(int argc, char **argv)
{
    const char *const pluginPath = argc > 1 ? argv[1] : "./mysql.so";
    const std::string host = environment("MYSQL_TEST_HOST", "127.0.0.1");
    const std::string user = environment("MYSQL_TEST_USER", "root");
    const std::string password = environment("MYSQL_TEST_PASSWORD", "");
    const int port = atoi(environment("MYSQL_TEST_PORT", "3306"));

    void *const library = dlopen(pluginPath, RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        printf("cannot load %s: %s\n", pluginPath, dlerror());
        return 100;
    }

    typedef int32_t (*AttachFunction)(const coduomp_plugin_api_t *);
    typedef void (*InfoFunction)(pluginInfo_t *);
    typedef int (*InitFunction)();
    typedef void (*EventFunction)();
    const AttachFunction attach =
        symbol<AttachFunction>(library, "coduomp_plugin_attach");
    const InfoFunction onInfoRequest =
        symbol<InfoFunction>(library, "OnInfoRequest");
    const InitFunction onInit = symbol<InitFunction>(library, "OnInit");
    const EventFunction onExitLevel =
        symbol<EventFunction>(library, "OnExitLevel");
    const EventFunction onTerminate =
        symbol<EventFunction>(library, "OnTerminate");
    if (attach == NULL || onInfoRequest == NULL || onInit == NULL ||
        onExitLevel == NULL || onTerminate == NULL) {
        printf("%s does not export the plugin entry points\n", pluginPath);
        return 100;
    }

    check("built for this plugin interface",
          attach(&g_api) == CODUOMP_PLUGIN_API_VERSION, "");
    pluginInfo_t info;
    memset(&info, 0, sizeof(info));
    onInfoRequest(&info);
    printf("    %s v%d.%d\n    %s\n", info.fullName,
           static_cast<int>(info.pluginVersion.major),
           static_cast<int>(info.pluginVersion.minor), info.longDescription);
    check("OnInit succeeds", onInit() == 0, "");
    check("registers 9 script functions", g_functions.size() == 9,
          std::to_string(g_functions.size()));

    const std::vector<Value> login = {Str(host), Str(user), Str(password),
                                      Str(""), Int(port)};

    printf("\n-- before any connection\n");
    expect("num_rows without a connection",
           call("mysql_num_rows", {Int(0)}),
           "ERROR: 'mysql_real_connect' must be called");
    expect("escape without a connection",
           call("mysql_real_escape_string", {Str("it's")}), "\"it\\'s\"");
    expect("connect: wrong argument count",
           call("mysql_real_connect", {Str("x")}), "ERROR: Usage");
    expect("connect: port out of range",
           call("mysql_real_connect",
                {Str("h"), Str("u"), Str("p"), Str(""), Int(70000)}),
           "ERROR: parameter 5");
    expect("connect: closed port",
           call("mysql_real_connect",
                {Str("127.0.0.1"), Str("u"), Str("p"), Str(""), Int(1)}),
           "ERROR: MySQL connect error");

    printf("\n-- connections\n");
    expect("connect #1", call("mysql_real_connect", login), "0");
    expect("connect #2", call("mysql_real_connect", login), "1");
    expect("connect #3", call("mysql_real_connect", login), "2");
    expect("connect #4", call("mysql_real_connect", login), "3");
    expect("connect #5 exceeds the limit",
           call("mysql_real_connect", login),
           "ERROR: MySQL connect error: max connections");
    expect("close #4", call("mysql_close", {Int(3)}), "undefined");
    expect("close #4 twice", call("mysql_close", {Int(3)}),
           "ERROR: 'mysql_real_connect' must be called");
    expect("handle out of range",
           call("mysql_query", {Int(7), Str("SELECT 1")}),
           "ERROR: parameter 1: Incorrect connection handle");
    expect("num_rows before any query", call("mysql_num_rows", {Int(0)}),
           "ERROR: 'mysql_query' must be called");

    printf("\n-- statements without a result set\n");
    expect("drop old test database",
           query("DROP DATABASE IF EXISTS coduox_mysql_test"), "undefined");
    expect("create database", query("CREATE DATABASE coduox_mysql_test"),
           "undefined");
    expect("use database", query("USE coduox_mysql_test"), "undefined");
    expect("create table",
           query("CREATE TABLE players (id INT PRIMARY KEY, "
                 "name VARCHAR(64), xp INT, note VARCHAR(32))"),
           "undefined");
    expect("insert 3 rows",
           query("INSERT INTO players VALUES (1,'SubZero',1500,NULL),"
                 "(2,'LoL',20,'new'),(3,'Acti Bot',0,'bot')"),
           "undefined");
    expect("affected_rows", call("mysql_affected_rows", {Int(0)}), "3");
    expect("num_rows is 0", call("mysql_num_rows", {Int(0)}), "0");
    expect("num_fields is 0", call("mysql_num_fields", {Int(0)}), "0");
    expect("fetch_row is undefined", call("mysql_fetch_row", {Int(0)}),
           "undefined");
    expect("fetch_rows is empty", call("mysql_fetch_rows", {Int(0)}), "[]");

    printf("\n-- result sets\n");
    expect("select",
           query("SELECT id, name, xp, note FROM players ORDER BY id"),
           "undefined");
    expect("num_rows", call("mysql_num_rows", {Int(0)}), "3");
    expect("num_fields", call("mysql_num_fields", {Int(0)}), "4");
    expect("fetch_row: NULL column is undefined",
           call("mysql_fetch_row", {Int(0)}),
           "[id: \"1\", name: \"SubZero\", xp: \"1500\", note: undefined]");
    expect("fetch_rows: the remaining rows",
           call("mysql_fetch_rows", {Int(0)}),
           "[[id: \"2\", name: \"LoL\", xp: \"20\", note: \"new\"], "
           "[id: \"3\", name: \"Acti Bot\", xp: \"0\", note: \"bot\"]]");
    expect("fetch_row at the end", call("mysql_fetch_row", {Int(0)}),
           "undefined");
    expect("fetch_rows at the end", call("mysql_fetch_rows", {Int(0)}),
           "[]");
    expect("update", query("UPDATE players SET xp = xp + 10 WHERE id < 3"),
           "undefined");
    expect("affected_rows after update",
           call("mysql_affected_rows", {Int(0)}), "2");
    expect("second connection reads the change",
           call("mysql_query",
                {Int(1), Str("SELECT xp FROM coduox_mysql_test.players "
                             "WHERE id = 1")}),
           "undefined");
    expect("  its row", call("mysql_fetch_row", {Int(1)}),
           "[xp: \"1510\"]");
    expect("select without a match",
           query("SELECT * FROM players WHERE id = 99"), "undefined");
    expect("  num_rows", call("mysql_num_rows", {Int(0)}), "0");
    expect("  fetch_rows", call("mysql_fetch_rows", {Int(0)}), "[]");

    printf("\n-- errors\n");
    expect("invalid SQL", query("SELEC nonsense"),
           "ERROR: MySQL query error: (1064)");
    expect("num_rows after a failed query",
           call("mysql_num_rows", {Int(0)}),
           "ERROR: 'mysql_query' must be called");
    expect("connection works after an error",
           query("SELECT COUNT(*) AS n FROM players"), "undefined");
    expect("  its row", call("mysql_fetch_row", {Int(0)}), "[n: \"3\"]");

    printf("\n-- escaping\n");
    expect("escape, string only",
           call("mysql_real_escape_string", {Str("O'Brien")}),
           "\"O\\'Brien\"");
    expect("escape, handle and string",
           call("mysql_real_escape_string", {Int(0), Str("x' OR '1'='1")}),
           "\"x\\' OR \\'1\\'=\\'1\"");
    expect("escape leaves plain text alone",
           call("mysql_real_escape_string", {Int(0), Str("SubZero_99")}),
           "\"SubZero_99\"");
    expect("escape, empty string",
           call("mysql_real_escape_string", {Str("")}), "\"\"");
    expect("escape, handle out of range",
           call("mysql_real_escape_string", {Int(7), Str("x")}),
           "ERROR: parameter 1: Incorrect connection handle");
    expect("escape, closed handle",
           call("mysql_real_escape_string", {Int(3), Str("x")}),
           "ERROR: 'mysql_real_connect' must be called");
    expect("escape, no arguments", call("mysql_real_escape_string", {}),
           "ERROR: Usage");
    {
        const std::string hostile = "x'); DROP TABLE players;-- \\\"q\"";

        expect("injection, unescaped: breaks the query",
               query("INSERT INTO players VALUES (4,'" + hostile +
                     "',1,'x')"),
               "ERROR: MySQL query error");
        call("mysql_real_escape_string", {Int(0), Str(hostile)});
        const std::string escaped =
            g_stack.empty() ? "" : g_stack.back().text;
        expect("injection, escaped: stored as text",
               query("INSERT INTO players VALUES (4,'" + escaped +
                     "',1,'x')"),
               "undefined");
        query("SELECT name FROM players WHERE id = 4");
        call("mysql_fetch_row", {Int(0)});
        const std::string stored =
            g_stack.empty() || g_stack.back().items.empty()
                ? "<no row>"
                : g_stack.back().items[0].second.text;
        check("  read back unchanged", stored == hostile, stored);
        expect("  table still exists",
               query("SELECT COUNT(*) AS n FROM players"), "undefined");
        expect("  with 4 rows", call("mysql_fetch_row", {Int(0)}),
               "[n: \"4\"]");
    }

    printf("\n-- map change\n");
    onExitLevel();
    expect("handles are closed after OnExitLevel", query("SELECT 1"),
           "ERROR: 'mysql_real_connect' must be called");
    expect("reconnect takes slot 0", call("mysql_real_connect", login),
           "0");
    expect("drop test database", query("DROP DATABASE coduox_mysql_test"),
           "undefined");

    printf("\n-- timeout\n");
    g_cvars["mysql_connect_timeout"] = 2;
    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    const std::string unreachable = call(
        "mysql_real_connect",
        {Str("10.255.255.1"), Str("u"), Str("p"), Str("")});
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    expect("unreachable host is an error", unreachable,
           "ERROR: MySQL connect error");
    check("  within mysql_connect_timeout (2 s)", seconds < 4.0,
          std::to_string(seconds) + " s");

    onTerminate();
    printf("\n%s\n", g_failures == 0 ? "all checks passed" : "CHECKS FAILED");
    return g_failures;
}
