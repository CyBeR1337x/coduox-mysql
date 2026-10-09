# coduox-mysql

[![build](https://github.com/CyBeR1337x/coduox-mysql/actions/workflows/build.yml/badge.svg)](https://github.com/CyBeR1337x/coduox-mysql/actions/workflows/build.yml)
[![license: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)
[![ported with AI](https://img.shields.io/badge/ported%20with-AI-8A2BE2)](#about-this-port)

MySQL plugin for [coduox](https://github.com/CyBeR1337x/coduox) *Call of
Duty: United Offensive* servers. It lets game scripts (GSC) query a MySQL or
MariaDB database over up to 4 connections.

This is a port of the [CoD4X MySQL plugin](https://github.com/callofduty4x/mysql)
by Sharpienero, MichaelHillcox and T-Max to the coduox server plugin
interface. It is distributed under the same license, the GNU Affero General
Public License version 3 (see `LICENSE`). The script functions and their
behaviour are the same unless listed under "Differences from the CoD4X
plugin".

## About this port

This plugin was ported using AI. The port from the CoD4X plugin, its fixes,
the tests, and this documentation were written with an AI coding assistant
and reviewed by the maintainer. Every script function is exercised against a
real database by the tests on each change; if you find behaviour that
differs from what is documented here, please open an issue.

## Requirements

- A coduox server, which provides the `loadplugin` command, on 64-bit Linux.
- On the machine that runs the server: the MariaDB or MySQL client library,
  for example `apt install libmariadb3` on Debian and Ubuntu.

## Install

Download `mysql.so` from the
[releases page](https://github.com/CyBeR1337x/coduox-mysql/releases), or
build it yourself, and put it in the server's `plugins` folder. Then load it
before the first map starts, on the server command line or in the server
config:

```text
loadplugin mysql
```

If the server reports `libmariadb.so.3: cannot open shared object file`, the
client library is not installed on that machine.

## Build

Building needs `g++`, `make`, and the client development package, for
example `apt install libmariadb-dev`.

```sh
make
make install PLUGIN_DIR=/path/to/server/plugins
```

The result is `mysql.so`. The server's plugin interface header is included
in `include/`; to build against a newer copy from the server source, pass
`CODUOMP_SDK=/path/to/coduox/src/plugin`.

## Test

`make test` loads the built plugin into a stand-in for the game server and
runs every script function against a real database. It needs a MySQL or
MariaDB server and an account that may create and drop the database
`coduox_mysql_test`:

```sh
MYSQL_TEST_HOST=127.0.0.1 MYSQL_TEST_PORT=3306 \
MYSQL_TEST_USER=root MYSQL_TEST_PASSWORD=secret make test
```

The same build and tests run on GitHub Actions for every push and pull
request.

## Settings

| Cvar | Default | Meaning |
|---|---|---|
| `mysql_connect_timeout` | 5 | Seconds to wait for a connection |
| `mysql_read_timeout` | 10 | Seconds to wait for a query result |
| `mysql_write_timeout` | 10 | Seconds to wait while sending a query |

Values outside 1 to 300 use the default. The values are read when
`mysql_real_connect` is called.

Queries run on the server's main thread: the game is paused for every
player until a call returns. Keep the database close to the game server,
keep queries small, and keep the timeouts short.

## Script functions

### `mysql_real_connect(<host>, <user>, <password>, <database>, [port])`

Connects and returns a connection handle (0 to 3). Pass `""` as the database
to connect without selecting one. The port defaults to 3306. `"localhost"`
is treated as `"127.0.0.1"`. A failed connection is a script error.

```c
handle = mysql_real_connect("localhost", "gameserver", "secret", "stats");
```

### `mysql_close(<handle>)`

Closes a connection and frees its result.

### `mysql_query(<handle>, <query>)`

Runs one SQL statement and keeps its result for the functions below. A
failed statement is a script error.

Text from players, such as names and chat, must go through
`mysql_real_escape_string` before it is put into a query.

### `mysql_real_escape_string(<string>)` or `mysql_real_escape_string(<handle>, <string>)`

Returns the string with the characters that could end or alter a quoted SQL
string escaped: quotes, backslash, newline, carriage return, NUL and Ctrl-Z.
Put the result between quotes in the query:

```c
name = mysql_real_escape_string(handle, player.name);
mysql_query(handle, "UPDATE players SET name = '" + name + "' WHERE guid = " + player getGuid());
```

With a handle, that connection's character set and SQL mode decide the
escaping. Without one, the first open connection is used. If no connection
is open, the escaping assumes a character set such as utf8 or latin1.

Escaping protects text placed between quotes. A value used without quotes
must be a number the script produced itself, such as the result of
`getGuid()`; put anything else between quotes and escape it.

### `mysql_num_rows(<handle>)`

Number of rows in the last result; 0 after a statement that returns no rows,
such as `INSERT`.

### `mysql_num_fields(<handle>)`

Number of columns in the last result; 0 after a statement that returns none.

### `mysql_affected_rows(<handle>)`

Number of rows changed by the last `INSERT`, `UPDATE` or `DELETE`.

### `mysql_fetch_row(<handle>)`

Returns the next row as an array keyed by column name, or undefined when no
row is left. Every value is a string; a SQL `NULL` is undefined.

```c
mysql_query(handle, "SELECT name, xp FROM players WHERE guid = " + guid);
row = mysql_fetch_row(handle);
if (isDefined(row))
    iprintln(row["name"] + " has " + row["xp"] + " xp");
```

### `mysql_fetch_rows(<handle>)`

Returns all remaining rows as an array of such rows; an empty array when no
row is left.

```c
rows = mysql_fetch_rows(handle);
for (i = 0; i < rows.size; i++)
    iprintln(rows[i]["name"]);
```

## Connections and map changes

Every connection is closed when a map ends or restarts, because the scripts
that held the handles are gone. Connect again when the next map starts.

## Differences from the CoD4X plugin

- All 4 connections are usable. The original refused the fourth and could
  write past its connection table when all were in use.
- `mysql_num_rows`, `mysql_num_fields`, `mysql_fetch_row` and
  `mysql_fetch_rows` are safe after a statement without a result set; the
  original crashed the server.
- A failed connection attempt no longer leaks its client handle.
- Connect, read and write timeouts are applied (see Settings).
- `mysql_real_escape_string` is added.
- `mysql_fetch_row` returns undefined at the end of a result, as the
  original code does; the original README describes an empty array.
