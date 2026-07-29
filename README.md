# CacheLite

CacheLite is a CLI-based in-memory key-value store built in C++.

It supports basic key-value operations, TTL expiry, fixed-capacity LRU eviction, safe file persistence, cache statistics, and optimized expired-key cleanup.

---

## Features

- Basic key-value commands: `SET`, `GET`, `DELETE`, `EXISTS`, `KEYS`
- Fixed cache capacity selected at startup
- LRU eviction when cache capacity is full
- TTL support with lazy expiry
- Optimized TTL cleanup using an expiry index
- Save and load cache data from files
- Cache statistics
- CLI helper commands like `HELP`, `CLEAR`, `CAPACITY`, `LRU`
- EOF-safe input handling for piped input and automated testing

---

## Tech Used

- C++17
- STL containers:
  - `unordered_map`
  - `list`
  - `set`
  - `optional`
- File handling using `fstream`
- Filesystem handling using `std::filesystem`
- Time handling using `std::chrono`

---

## Main Data Structures

### 1. Hash Map

```cpp
unordered_map<string, Entry> store;
```

The hash map stores the actual key-value data.

It gives average `O(1)` lookup, insert, update, and delete.

---

### 2. Doubly Linked List

```cpp
list<string> lruList;
```

The list maintains the LRU order.

The most recently used key is kept at the front.

The least recently used key is kept at the back.

When capacity is full, the key at the back is evicted.

---

### 3. Expiry Index

```cpp
set<pair<chrono::steady_clock::time_point, string>> expiryIndex;
```

The expiry index stores only TTL keys.

It keeps keys sorted by expiry time.

This allows CacheLite to clean expired keys without scanning the full hash map every time.

---

## Commands

### Basic Commands

```text
SET key value
GET key
DELETE key
DEL key
EXISTS key
KEYS
```

`SET` supports values with spaces.

Example:

```text
SET name Hemant Gattani
GET name
```

---

### TTL Commands

```text
SETEX key value seconds
TTL key
EXPIRE key seconds
```

`TTL key` returns:

```text
-2  -> key does not exist
-1  -> key exists but has no expiry
>=0 -> remaining time in seconds
```

Example:

```text
SETEX temp 100 10
TTL temp
EXPIRE temp 20
```

---

### Cache Inspection Commands

```text
STATS
LRU
CAPACITY
```

`STATS` prints cache statistics.

`LRU` prints keys from most recently used to least recently used.

`CAPACITY` shows fixed capacity, current key count, and available slots.

---

### Persistence Commands

```text
SAVE filename
LOAD filename
```

Example:

```text
SAVE data/cache.txt
LOAD data/cache.txt
```

---

### Utility Commands

```text
HELP
CLEAR
EXIT
QUIT
```

`HELP` shows all available commands.

`CLEAR` removes all current keys from the cache.

`EXIT` and `QUIT` stop the program.

---

## Build and Run

Compile:

```bash
g++ -std=c++17 -Wall -Wextra src/main.cpp -o cachelite
```

Run on Linux/macOS:

```bash
./cachelite
```

Run on Windows PowerShell:

```powershell
.\cachelite.exe
```

---

## Example Usage

```text
CacheLite v12.1 started.
Enter cache capacity (positive integer, default 3): 3
Cache capacity set to 3 keys.
Type HELP to see all available commands.

> SET a 100
OK

> SET b 200
OK

> GET a
100

> SETEX temp 500 10
OK

> TTL temp
8

> LRU
Most recent -> least recent:
temp a b

> CAPACITY
Capacity: 3
Current keys: 3
Available slots: 0

> SAVE data/cache.txt
Saved

> CLEAR
Cleared 3 keys

> LOAD data/cache.txt
Loaded

> KEYS
a
b
temp

> EXIT
Bye!
```

---

## Design Decisions

### Fixed Capacity

Cache capacity is selected once at startup.

It cannot be changed while the program is running.

This keeps the design simple and avoids unclear behavior when reducing capacity midway.

Example:

```text
CAPACITY 10
```

will not change the cache capacity.

The user must restart CacheLite and choose a new capacity at startup.

---

### LRU Eviction

CacheLite uses LRU eviction when the cache is full.

LRU means Least Recently Used.

If the cache is full and a new key is inserted, the least recently used key is removed.

This is implemented using:

```text
unordered_map + list
```

The hash map gives fast lookup.

The list maintains usage order.

---

### TTL Expiry

TTL means Time To Live.

A TTL key expires after a fixed number of seconds.

Example:

```text
SETEX temp 100 5
```

Here, `temp` will expire after 5 seconds.

CacheLite uses lazy expiry.

This means expired keys are removed when cache operations happen, not by a background thread.

---

### Optimized TTL Cleanup

Earlier, expired-key cleanup required scanning all keys in the hash map.

That approach was `O(n)`.

CacheLite now uses an expiry index:

```text
set<pair<expiryTime, key>>
```

This keeps TTL keys sorted by expiry time.

During cleanup, CacheLite checks only the earliest expiring keys first.

This avoids scanning the full cache when only a few keys have expired.

---

### Persistence

CacheLite supports saving and loading data using:

```text
SAVE filename
LOAD filename
```

The save operation uses a temporary file and backup file approach.

This reduces the chance of corrupting existing saved data if saving fails.

TTL keys are saved using real-world expiry time.

Because of this, TTL expiry still works even after restarting the program.

---

### EOF-Safe Input Handling

The CLI checks whether input reading succeeds.

This prevents infinite loops when the program is used with piped input or automated tests.

Example:

```powershell
@"
2
SET a 1
GET a
"@ | .\cachelite.exe
```

The program exits cleanly after input ends.

---

## Current Limitations

- This is a single-threaded CLI project.
- `SET` supports values with spaces.
- `SETEX` currently supports one-word values.
- Values are stored as strings.
- Multi-line values are not supported.
- The project is currently kept in a single source file for simplicity.

---

## Project Status

Core implementation is complete.

Completed features:

- Basic key-value store
- TTL support
- LRU eviction
- Fixed startup capacity
- Save/load persistence
- Cache statistics
- CLI polish
- Expiry index optimization
- EOF-safe input handling
- Reduced redundant hash map lookups

---

## Possible Future Improvements

- Split code into multiple files
- Add unit tests
- Add support for multi-word values in `SETEX`
- Add mutex-based thread safety
- Add benchmarking for GET/SET throughput
- Add support for more data types