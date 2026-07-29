#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <list>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

enum class Policy { LRU, LFU };

struct Entry {
    string value;
    bool hasExpiry = false;
    chrono::steady_clock::time_point expiryTime;
    
    // LRU List iterator
    list<string>::iterator lruIt;
    
    // LFU Frequency and iterator inside its frequency list
    int freq = 1;
    list<string>::iterator lfuIt;

    Entry() = default;

    Entry(const string& val)
        : value(val), hasExpiry(false), freq(1) {}

    Entry(const string& val, long long ttlSeconds)
        : value(val),
          hasExpiry(true),
          expiryTime(chrono::steady_clock::now() + chrono::seconds(ttlSeconds)),
          freq(1) {}
};

struct CacheStats {
    long long cacheHits = 0;
    long long cacheMisses = 0;
    long long setOperations = 0;
    long long getOperations = 0;
    long long deleteOperations = 0;
    long long expiredKeys = 0;
    long long evictedKeys = 0;
};

struct SaveRecord {
    string key;
    string value;
    bool hasExpiry = false;
    long long expiryEpochSeconds = 0;
};

class CacheLite {
private:
    unordered_map<string, Entry> store;
    
    // Data Structure for LRU
    list<string> lruList;

    // Data Structures for LFU: Map frequency count -> List of keys
    unordered_map<int, list<string>> freqMap;
    int minFreq = 1;

    // Expiry Index
    std::set<pair<chrono::steady_clock::time_point, string>> expiryIndex;

    size_t capacity;
    Policy currentPolicy;
    CacheStats stats;

    using StoreIterator = unordered_map<string, Entry>::iterator;

    bool isExpired(const Entry& entry, chrono::steady_clock::time_point now) const {
        if (!entry.hasExpiry) {
            return false;
        }
        return now >= entry.expiryTime;
    }

    void removeFromExpiryIndex(StoreIterator it) {
        if (it == store.end() || !it->second.hasExpiry) {
            return;
        }
        expiryIndex.erase({it->second.expiryTime, it->first});
    }

    void addToExpiryIndex(StoreIterator it) {
        if (it == store.end() || !it->second.hasExpiry) {
            return;
        }
        expiryIndex.insert({it->second.expiryTime, it->first});
    }

    void eraseEntry(StoreIterator it) {
        if (it == store.end()) {
            return;
        }

        removeFromExpiryIndex(it);
        
        // Remove from LRU list
        lruList.erase(it->second.lruIt);

        // Remove from LFU frequency list
        int f = it->second.freq;
        freqMap[f].erase(it->second.lfuIt);
        if (freqMap[f].empty()) {
            freqMap.erase(f);
        }

        store.erase(it);
    }

    bool removeKey(const string& key) {
        auto it = store.find(key);
        if (it == store.end()) {
            return false;
        }
        eraseEntry(it);
        return true;
    }

    bool eraseIfExpired(StoreIterator it, chrono::steady_clock::time_point now) {
        if (it == store.end()) {
            return false;
        }

        if (!isExpired(it->second, now)) {
            return false;
        }

        eraseEntry(it);
        stats.expiredKeys++;
        return true;
    }

    void cleanupExpiredKeys() {
        auto now = chrono::steady_clock::now();

        while (!expiryIndex.empty()) {
            auto earliest = expiryIndex.begin();

            if (earliest->first > now) {
                break;
            }

            auto expiryTime = earliest->first;
            string key = earliest->second;
            expiryIndex.erase(earliest);

            auto it = store.find(key);

            if (it == store.end()) {
                continue;
            }

            if (!it->second.hasExpiry || it->second.expiryTime != expiryTime) {
                continue;
            }

            if (!isExpired(it->second, now)) {
                expiryIndex.insert({it->second.expiryTime, it->first});
                continue;
            }

            eraseEntry(it);
            stats.expiredKeys++;
        }
    }

    void touchKey(StoreIterator it) {
        if (it == store.end()) {
            return;
        }

        string key = it->first;

        // 1. Update LRU tracking
        lruList.erase(it->second.lruIt);
        lruList.push_front(key);
        it->second.lruIt = lruList.begin();

        // 2. Update LFU tracking
        int oldFreq = it->second.freq;
        freqMap[oldFreq].erase(it->second.lfuIt);

        if (freqMap[oldFreq].empty()) {
            freqMap.erase(oldFreq);
            if (minFreq == oldFreq) {
                minFreq++;
            }
        }

        it->second.freq++;
        int newFreq = it->second.freq;
        freqMap[newFreq].push_front(key);
        it->second.lfuIt = freqMap[newFreq].begin();
    }

    void insertNewEntry(const string& key, Entry entry) {
        // Insert into LRU list
        lruList.push_front(key);
        entry.lruIt = lruList.begin();

        // Insert into LFU tracking (frequency defaults to 1)
        entry.freq = 1;
        freqMap[1].push_front(key);
        entry.lfuIt = freqMap[1].begin();
        minFreq = 1;

        auto result = store.emplace(key, entry);
        addToExpiryIndex(result.first);
    }

    void evictKey() {
        if (store.empty()) {
            return;
        }

        string keyToEvict;

        if (currentPolicy == Policy::LRU) {
            keyToEvict = lruList.back();
        } else {
            // LFU Eviction: Pick from the back of the minimum frequency list
            keyToEvict = freqMap[minFreq].back();
        }

        removeKey(keyToEvict);
        stats.evictedKeys++;
    }

    void ensureCapacityForNewKey() {
        if (store.size() < capacity) {
            return;
        }

        cleanupExpiredKeys();

        while (store.size() >= capacity && !store.empty()) {
            evictKey();
        }
    }

public:
    explicit CacheLite(size_t maxCapacity, Policy initialPolicy = Policy::LRU)
        : capacity(max<size_t>(1, maxCapacity)), currentPolicy(initialPolicy) {}

    void setPolicy(Policy p) {
        currentPolicy = p;
    }

    Policy getPolicy() const {
        return currentPolicy;
    }

    void set(const string& key, const string& value) {
        stats.setOperations++;

        auto it = store.find(key);
        auto now = chrono::steady_clock::now();

        if (it != store.end() && eraseIfExpired(it, now)) {
            it = store.end();
        }

        if (it != store.end()) {
            removeFromExpiryIndex(it);
            it->second.value = value;
            it->second.hasExpiry = false;
            touchKey(it);
            return;
        }

        ensureCapacityForNewKey();
        insertNewEntry(key, Entry(value));
    }

    bool setex(const string& key, const string& value, long long seconds) {
        if (seconds <= 0) {
            return false;
        }

        stats.setOperations++;

        auto it = store.find(key);
        auto now = chrono::steady_clock::now();

        if (it != store.end() && eraseIfExpired(it, now)) {
            it = store.end();
        }

        if (it != store.end()) {
            removeFromExpiryIndex(it);
            it->second.value = value;
            it->second.hasExpiry = true;
            it->second.expiryTime = now + chrono::seconds(seconds);
            addToExpiryIndex(it);
            touchKey(it);
            return true;
        }

        ensureCapacityForNewKey();
        insertNewEntry(key, Entry(value, seconds));

        return true;
    }

    optional<string> get(const string& key) {
        stats.getOperations++;

        auto it = store.find(key);

        if (it == store.end()) {
            stats.cacheMisses++;
            return nullopt;
        }

        auto now = chrono::steady_clock::now();

        if (eraseIfExpired(it, now)) {
            stats.cacheMisses++;
            return nullopt;
        }

        stats.cacheHits++;
        touchKey(it);

        return it->second.value;
    }

    bool del(const string& key) {
        stats.deleteOperations++;

        auto it = store.find(key);

        if (it == store.end()) {
            return false;
        }

        auto now = chrono::steady_clock::now();

        if (eraseIfExpired(it, now)) {
            return false;
        }

        eraseEntry(it);
        return true;
    }

    bool exists(const string& key) {
        auto it = store.find(key);

        if (it == store.end()) {
            return false;
        }

        auto now = chrono::steady_clock::now();

        if (eraseIfExpired(it, now)) {
            return false;
        }

        return true;
    }

    long long ttl(const string& key) {
        auto it = store.find(key);

        if (it == store.end()) {
            return -2;
        }

        auto now = chrono::steady_clock::now();

        if (eraseIfExpired(it, now)) {
            return -2;
        }

        if (!it->second.hasExpiry) {
            return -1;
        }

        auto remaining = chrono::duration_cast<chrono::seconds>(
            it->second.expiryTime - now
        ).count();

        return remaining;
    }

    bool expire(const string& key, long long seconds) {
        if (seconds <= 0) {
            return false;
        }

        auto it = store.find(key);

        if (it == store.end()) {
            return false;
        }

        auto now = chrono::steady_clock::now();

        if (eraseIfExpired(it, now)) {
            return false;
        }

        removeFromExpiryIndex(it);
        it->second.hasExpiry = true;
        it->second.expiryTime = now + chrono::seconds(seconds);
        addToExpiryIndex(it);
        touchKey(it);

        return true;
    }

    bool save(const string& filename) {
        cleanupExpiredKeys();

        vector<SaveRecord> records;

        auto steadyNow = chrono::steady_clock::now();
        auto systemNow = chrono::system_clock::now();

        for (auto rit = lruList.rbegin(); rit != lruList.rend(); ++rit) {
            const string& key = *rit;
            auto it = store.find(key);

            if (it == store.end()) {
                continue;
            }

            SaveRecord record;
            record.key = key;
            record.value = it->second.value;
            record.hasExpiry = it->second.hasExpiry;

            if (it->second.hasExpiry) {
                auto remainingSeconds = chrono::duration_cast<chrono::seconds>(
                    it->second.expiryTime - steadyNow
                ).count();

                if (remainingSeconds <= 0) {
                    continue;
                }

                auto expirySystemTime = systemNow + chrono::seconds(remainingSeconds);

                record.expiryEpochSeconds = chrono::duration_cast<chrono::seconds>(
                    expirySystemTime.time_since_epoch()
                ).count();
            }

            records.push_back(record);
        }

        string tempFilename = filename + ".tmp";
        string backupFilename = filename + ".bak";

        try {
            fs::path finalPath(filename);

            if (finalPath.has_parent_path()) {
                fs::create_directories(finalPath.parent_path());
            }
        } catch (...) {
            return false;
        }

        ofstream out(tempFilename, ios::trunc);

        if (!out) {
            return false;
        }

        out << "CACHELITE_V1\n";
        out << records.size() << "\n";

        for (const SaveRecord& record : records) {
            out << record.key.size() << " "
                << record.value.size() << " "
                << (record.hasExpiry ? 1 : 0) << " "
                << record.expiryEpochSeconds << "\n";

            out << record.key << "\n";
            out << record.value << "\n";
        }

        out.close();

        if (!out) {
            try {
                fs::remove(tempFilename);
            } catch (...) {}

            return false;
        }

        try {
            if (fs::exists(backupFilename)) {
                fs::remove(backupFilename);
            }

            if (fs::exists(filename)) {
                fs::rename(filename, backupFilename);
            }

            fs::rename(tempFilename, filename);

            if (fs::exists(backupFilename)) {
                fs::remove(backupFilename);
            }
        } catch (...) {
            try {
                if (fs::exists(filename)) {
                    fs::remove(filename);
                }

                if (fs::exists(backupFilename)) {
                    fs::rename(backupFilename, filename);
                }

                if (fs::exists(tempFilename)) {
                    fs::remove(tempFilename);
                }
            } catch (...) {}

            return false;
        }

        return true;
    }

    bool load(const string& filename) {
        ifstream in(filename);

        if (!in) {
            return false;
        }

        string header;
        getline(in, header);

        if (header != "CACHELITE_V1") {
            return false;
        }

        size_t recordCount = 0;
        in >> recordCount;

        if (!in) {
            return false;
        }

        in.ignore(numeric_limits<streamsize>::max(), '\n');

        CacheLite tempCache(capacity, currentPolicy);

        auto systemNow = chrono::system_clock::now();
        long long currentEpochSeconds = chrono::duration_cast<chrono::seconds>(
            systemNow.time_since_epoch()
        ).count();

        for (size_t i = 0; i < recordCount; i++) {
            size_t keyLength = 0;
            size_t valueLength = 0;
            int hasExpiryInt = 0;
            long long expiryEpochSeconds = 0;

            in >> keyLength >> valueLength >> hasExpiryInt >> expiryEpochSeconds;

            if (!in) {
                return false;
            }

            if (hasExpiryInt != 0 && hasExpiryInt != 1) {
                return false;
            }

            in.ignore(numeric_limits<streamsize>::max(), '\n');

            string key;
            string value;

            getline(in, key);
            getline(in, value);

            if (!in) {
                return false;
            }

            if (key.size() != keyLength || value.size() != valueLength) {
                return false;
            }

            bool hasExpiry = (hasExpiryInt == 1);

            auto existing = tempCache.store.find(key);
            if (existing != tempCache.store.end()) {
                tempCache.eraseEntry(existing);
            }

            if (hasExpiry) {
                long long remainingSeconds = expiryEpochSeconds - currentEpochSeconds;

                if (remainingSeconds <= 0) {
                    continue;
                }

                tempCache.ensureCapacityForNewKey();
                tempCache.insertNewEntry(key, Entry(value, remainingSeconds));
            } else {
                tempCache.ensureCapacityForNewKey();
                tempCache.insertNewEntry(key, Entry(value));
            }
        }

        store.swap(tempCache.store);
        lruList.swap(tempCache.lruList);
        freqMap.swap(tempCache.freqMap);
        expiryIndex.swap(tempCache.expiryIndex);

        return true;
    }

    void keys() {
        cleanupExpiredKeys();

        if (store.empty()) {
            cout << "(empty)\n";
            return;
        }

        for (const auto& pair : store) {
            cout << pair.first << "\n";
        }
    }

    void printLRU() {
        cleanupExpiredKeys();

        if (lruList.empty()) {
            cout << "(empty)\n";
            return;
        }

        cout << "Most recent -> least recent:\n";

        for (const string& key : lruList) {
            cout << key << " ";
        }

        cout << "\n";
    }

    void printStats() {
        cleanupExpiredKeys();

        cout << "Active Eviction Policy: " << (currentPolicy == Policy::LRU ? "LRU" : "LFU") << "\n";
        cout << "Capacity: " << capacity << "\n";
        cout << "Total keys: " << store.size() << "\n";
        cout << "Cache hits: " << stats.cacheHits << "\n";
        cout << "Cache misses: " << stats.cacheMisses << "\n";
        cout << "SET operations: " << stats.setOperations << "\n";
        cout << "GET operations: " << stats.getOperations << "\n";
        cout << "DELETE operations: " << stats.deleteOperations << "\n";
        cout << "Expired keys: " << stats.expiredKeys << "\n";
        cout << "Evicted keys: " << stats.evictedKeys << "\n";
    }

    void inspect() {
        cleanupExpiredKeys();

        if (store.empty()) {
            cout << "(empty)\n";
            return;
        }

        cout << "Active Policy: " << (currentPolicy == Policy::LRU ? "LRU" : "LFU") << "\n";
        cout << "--- Memory Inspection ---\n";
        for (const auto& pair : store) {
            cout << "Key: " << pair.first 
                 << " | Value: " << pair.second.value 
                 << " | Frequency: " << pair.second.freq << "\n";
        }
    }

    size_t clear() {
        cleanupExpiredKeys();

        size_t removedKeys = store.size();

        store.clear();
        lruList.clear();
        freqMap.clear();
        expiryIndex.clear();

        return removedKeys;
    }

    void printCapacity() {
        cleanupExpiredKeys();

        cout << "Capacity: " << capacity << "\n";
        cout << "Current keys: " << store.size() << "\n";
        cout << "Available slots: " << (capacity - store.size()) << "\n";
    }
};

string toUpperCase(string command) {
    transform(command.begin(), command.end(), command.begin(),
              [](unsigned char c) {
                  return toupper(c);
              });

    return command;
}

string trimLeft(const string& text) {
    size_t pos = text.find_first_not_of(" ");

    if (pos == string::npos) {
        return "";
    }

    return text.substr(pos);
}

size_t readInitialCapacity() {
    const size_t defaultCapacity = 3;

    while (true) {
        cout << "Enter cache capacity (positive integer, default 3): ";

        string input;

        if (!getline(cin, input)) {
            cout << "\nInput stream closed while reading capacity. Using default capacity 3.\n";
            return defaultCapacity;
        }

        if (input.empty()) {
            return defaultCapacity;
        }

        stringstream ss(input);
        long long userCapacity;
        char extra;

        if ((ss >> userCapacity) && !(ss >> extra) && userCapacity > 0) {
            return static_cast<size_t>(userCapacity);
        }

        cout << "Invalid capacity. Please enter a positive integer.\n";
    }
}

void printHelp() {
    cout << "CacheLite Commands:\n\n";

    cout << "Basic key-value commands:\n";
    cout << "  SET key value              Store or update a permanent key\n";
    cout << "  GET key                    Get value for a key\n";
    cout << "  DELETE key / DEL key       Delete a key\n";
    cout << "  EXISTS key                 Check whether a key exists\n";
    cout << "  KEYS                       Print all current keys\n\n";

    cout << "TTL commands:\n";
    cout << "  SETEX key value seconds    Store key with expiry time\n";
    cout << "  TTL key                    Show remaining TTL in seconds\n";
    cout << "                             -2 = key does not exist\n";
    cout << "                             -1 = key exists without expiry\n";
    cout << "  EXPIRE key seconds         Add or update expiry for existing key\n\n";

    cout << "Cache inspection & policy commands:\n";
    cout << "  POLICY [LRU|LFU]           Switch eviction policy dynamically\n";
    cout << "  INSPECT                    Show key-values, frequency, and policy status\n";
    cout << "  STATS                      Show cache statistics\n";
    cout << "  LRU                        Show most recent to least recent order\n";
    cout << "  CAPACITY                   Show fixed capacity and current usage\n\n";

    cout << "Persistence commands:\n";
    cout << "  SAVE filename              Save cache to file\n";
    cout << "  LOAD filename              Load cache from file\n\n";

    cout << "Utility commands:\n";
    cout << "  CLEAR                      Remove all keys from cache\n";
    cout << "  HELP                       Show this help message\n";
    cout << "  EXIT / QUIT                Exit CacheLite\n\n";
}

int main() {
    cout << "CacheLite v13.0 started.\n";

    size_t initialCapacity = readInitialCapacity();

    CacheLite cache(initialCapacity);
    string line;

    cout << "Cache capacity set to " << initialCapacity << " keys. Default Policy: LRU.\n";
    cout << "Type HELP to see all available commands.\n";

    while (true) {
        cout << "> ";

        if (!getline(cin, line)) {
            cout << "\nInput stream closed. Exiting CacheLite.\n";
            break;
        }

        if (line.empty()) {
            continue;
        }

        stringstream ss(line);

        string command;
        ss >> command;

        if (command.empty()) {
            continue;
        }

        command = toUpperCase(command);

        if (command == "SET") {
            string key;
            ss >> key;

            string value;
            getline(ss, value);
            value = trimLeft(value);

            if (key.empty() || value.empty()) {
                cout << "Usage: SET key value\n";
                continue;
            }

            cache.set(key, value);
            cout << "OK\n";
        }
        else if (command == "SETEX") {
            string key;
            string value;
            long long seconds;

            ss >> key >> value >> seconds;

            if (key.empty() || value.empty() || !ss || seconds <= 0) {
                cout << "Usage: SETEX key value seconds\n";
                continue;
            }

            bool success = cache.setex(key, value, seconds);

            if (success) {
                cout << "OK\n";
            } else {
                cout << "Invalid TTL\n";
            }
        }
        else if (command == "GET") {
            string key;
            ss >> key;

            if (key.empty()) {
                cout << "Usage: GET key\n";
                continue;
            }

            optional<string> value = cache.get(key);

            if (value.has_value()) {
                cout << value.value() << "\n";
            } else {
                cout << "(nil)\n";
            }
        }
        else if (command == "DELETE" || command == "DEL") {
            string key;
            ss >> key;

            if (key.empty()) {
                cout << "Usage: DELETE key\n";
                continue;
            }

            bool removed = cache.del(key);

            if (removed) {
                cout << "Deleted\n";
            } else {
                cout << "Key not found\n";
            }
        }
        else if (command == "EXISTS") {
            string key;
            ss >> key;

            if (key.empty()) {
                cout << "Usage: EXISTS key\n";
                continue;
            }

            cout << (cache.exists(key) ? "true" : "false") << "\n";
        }
        else if (command == "TTL") {
            string key;
            ss >> key;

            if (key.empty()) {
                cout << "Usage: TTL key\n";
                continue;
            }

            cout << cache.ttl(key) << "\n";
        }
        else if (command == "EXPIRE") {
            string key;
            long long seconds;

            ss >> key >> seconds;

            if (key.empty() || !ss || seconds <= 0) {
                cout << "Usage: EXPIRE key seconds\n";
                continue;
            }

            cout << (cache.expire(key, seconds) ? "true" : "false") << "\n";
        }
        else if (command == "POLICY") {
            string pol;
            ss >> pol;
            pol = toUpperCase(pol);

            if (pol == "LRU") {
                cache.setPolicy(Policy::LRU);
                cout << "Policy switched to LRU\n";
            } else if (pol == "LFU") {
                cache.setPolicy(Policy::LFU);
                cout << "Policy switched to LFU\n";
            } else {
                cout << "Current Policy: " << (cache.getPolicy() == Policy::LRU ? "LRU" : "LFU") << "\n";
                cout << "Usage: POLICY LRU or POLICY LFU\n";
            }
        }
        else if (command == "INSPECT") {
            cache.inspect();
        }
        else if (command == "SAVE") {
            string filename;
            ss >> filename;

            if (filename.empty()) {
                cout << "Usage: SAVE filename\n";
                continue;
            }

            cout << (cache.save(filename) ? "Saved\n" : "Save failed\n");
        }
        else if (command == "LOAD") {
            string filename;
            ss >> filename;

            if (filename.empty()) {
                cout << "Usage: LOAD filename\n";
                continue;
            }

            cout << (cache.load(filename) ? "Loaded\n" : "Load failed\n");
        }
        else if (command == "KEYS") {
            cache.keys();
        }
        else if (command == "LRU") {
            cache.printLRU();
        }
        else if (command == "STATS") {
            cache.printStats();
        }
        else if (command == "CAPACITY") {
            string extra;
            ss >> extra;

            if (!extra.empty()) {
                cout << "Error: capacity is fixed after startup and cannot be changed while running.\n";
                cout << "Use CAPACITY without arguments to view current usage.\n";
                continue;
            }

            cache.printCapacity();
        }
        else if (command == "SETCAPACITY" || command == "RESIZE") {
            cout << "Error: capacity is fixed after startup and cannot be changed while running.\n";
            cout << "Restart CacheLite and choose a new capacity at startup.\n";
        }
        else if (command == "CLEAR") {
            size_t removedKeys = cache.clear();
            cout << "Cleared " << removedKeys << " keys\n";
        }
        else if (command == "HELP") {
            printHelp();
        }
        else if (command == "EXIT" || command == "QUIT") {
            cout << "Bye!\n";
            break;
        }
        else {
            cout << "Unknown command. Type HELP to see available commands.\n";
        }
    }

    return 0;
}