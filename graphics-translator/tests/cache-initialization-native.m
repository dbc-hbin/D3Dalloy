// SPDX-License-Identifier: LGPL-2.1-or-later
// clang -std=gnu11 -O2 -fno-objc-arc -fobjc-exceptions -fblocks \
//   -framework Foundation -framework Metal -lsqlite3 -Isrc/winemetal/unix \
//   graphics-translator/tests/cache-initialization-native.m -o <output>
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

void require(bool value, const char* reason) {
    if (!value) {
        fprintf(stderr, "CACHE_INITIALIZATION_NATIVE_FAIL reason=%s\n", reason);
        exit(1);
    }
}
struct Audit {
    sqlite3* database;
    sqlite3_stmt* statement;
    int descriptor;
    bool locked;
    unsigned opens, failedOpens, closes, prepares, finalizes;
    unsigned lockOpens, unlocks, lockCloses;
} cacheAudit = {.descriptor = -1};
unsigned readerDeallocations = 0, writerDeallocations = 0;

// Observe ownership while executing the real SQLite and file-lock operations.
int trackedOpen(const char* path, sqlite3** database, int flags, const char* vfs) {
    ++cacheAudit.opens;
    const int result = sqlite3_open_v2(path, database, flags, vfs);
    if (result != SQLITE_OK) {
        ++cacheAudit.failedOpens;
        require(*database != NULL, "sqlite-open-failure-supplies-real-error-handle");
    }
    require(cacheAudit.database == NULL, "one-live-database");
    cacheAudit.database = *database;
    return result;
}
int trackedClose(sqlite3* database) {
    require(database && cacheAudit.database == database, "database-closed-once");
    cacheAudit.database = NULL;
    ++cacheAudit.closes;
    const int result = sqlite3_close(database);
    require(result == SQLITE_OK, "database-close-has-no-live-statements");
    return result;
}
int trackedPrepare(sqlite3* database, const char* sql, int bytes, sqlite3_stmt** statement, const char** remaining) {
    ++cacheAudit.prepares;
    const int result = sqlite3_prepare_v2(database, sql, bytes, statement, remaining);
    require(cacheAudit.statement == NULL, "one-live-statement");
    cacheAudit.statement = *statement;
    return result;
}
int trackedFinalize(sqlite3_stmt* statement) {
    require(statement && cacheAudit.statement == statement, "statement-finalized-once");
    cacheAudit.statement = NULL;
    ++cacheAudit.finalizes;
    return sqlite3_finalize(statement);
}
int trackedLockOpen(const char* path, int flags, mode_t mode) {
    const int descriptor = open(path, flags, mode);
    if (descriptor >= 0) {
        require(cacheAudit.descriptor == -1, "one-live-lock-descriptor");
        cacheAudit.descriptor = descriptor;
        ++cacheAudit.lockOpens;
    }
    return descriptor;
}
int trackedFlock(int descriptor, int operation) {
    require(cacheAudit.descriptor == descriptor, "locking-owned-descriptor");
    const int result = flock(descriptor, operation);
    require(result == 0, "real-file-lock-operation");
    if (operation == LOCK_EX) {
        require(!cacheAudit.locked, "lock-acquired-once");
        cacheAudit.locked = true;
    }
    if (operation == LOCK_UN) {
        require(cacheAudit.locked, "lock-released-once");
        cacheAudit.locked = false;
        ++cacheAudit.unlocks;
    }
    return result;
}
int trackedLockClose(int descriptor) {
    require(!cacheAudit.locked, "unlock-before-close");
    require(cacheAudit.descriptor == descriptor, "lock-descriptor-closed-once");
    cacheAudit.descriptor = -1;
    ++cacheAudit.lockCloses;
    return close(descriptor);
}

#define sqlite3_open_v2 trackedOpen
#define sqlite3_close trackedClose
#define sqlite3_prepare_v2 trackedPrepare
#define sqlite3_finalize trackedFinalize
#define open trackedLockOpen
#define flock trackedFlock
#define close trackedLockClose
#ifndef PRODUCTION_CACHE
#define PRODUCTION_CACHE "../../src/winemetal/unix/cache.c"
#endif
#include PRODUCTION_CACHE
#undef close
#undef flock
#undef open
#undef sqlite3_finalize
#undef sqlite3_prepare_v2
#undef sqlite3_close
#undef sqlite3_open_v2

@interface ObservedCacheReader : CacheReader
@end
@implementation ObservedCacheReader
- (void)dealloc {
    ++readerDeallocations;
    [super dealloc];
}
@end
@interface ObservedCacheWriter : CacheWriter
@end
@implementation ObservedCacheWriter
- (void)dealloc {
    ++writerDeallocations;
    [super dealloc];
}
@end

void drained() {
    require(!cacheAudit.database && !cacheAudit.statement, "sqlite-resources-drained");
    require(cacheAudit.descriptor == -1 && !cacheAudit.locked, "lock-resources-drained");
    require(cacheAudit.opens == cacheAudit.closes, "each-sqlite-handle-closed");
    require(cacheAudit.lockOpens == cacheAudit.lockCloses && cacheAudit.lockOpens == cacheAudit.unlocks, "each-lock-fd-unlocked-and-closed");
}
void schema(NSString* path, const char* sql) {
    sqlite3* database = NULL;
    require(sqlite3_open_v2(path.fileSystemRepresentation, &database,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK, "fixture-database-open");
    require(sqlite3_exec(database, sql, NULL, NULL, NULL) == SQLITE_OK, "fixture-schema");
    require(sqlite3_close(database) == SQLITE_OK, "fixture-database-close");
}
void failure(bool writer, NSString* path, const char* label, unsigned opens, unsigned failedOpens, unsigned prepares) {
    cacheAudit = (struct Audit){.descriptor = -1};
    const unsigned before = writer ? writerDeallocations : readerDeallocations;
    id cache = writer ? [[ObservedCacheWriter alloc] initWithPath:path version:1]
                      : [[ObservedCacheReader alloc] initWithPath:path version:1];
    require(cache == nil, "failed-cache-initialization-returns-nil");
    require((writer ? writerDeallocations : readerDeallocations) == before + 1, "failed-initialization-relinquishes-self");
    require(cacheAudit.opens == opens && cacheAudit.failedOpens == failedOpens && cacheAudit.prepares == prepares, "real-initialization-failure-stage");
    drained();
    printf("CACHE_INITIALIZATION_NATIVE_CASE_PASS case=%s sqlite_opens=%u sqlite_closes=%u lock_opens=%u lock_closes=%u\n",
                label, cacheAudit.opens, cacheAudit.closes, cacheAudit.lockOpens, cacheAudit.lockCloses);
}
void roundTrip(NSString* path) {
    cacheAudit = (struct Audit){.descriptor = -1};
    const unsigned readersBefore = readerDeallocations, writersBefore = writerDeallocations;
    CacheWriter* writer = [[ObservedCacheWriter alloc] initWithPath:path version:1];
    require(writer != nil, "writer-success");
    const char keyBytes[] = "key", valueBytes[] = "native-sqlite-round-trip";
    NSData* key = [NSData dataWithBytes:keyBytes length:sizeof(keyBytes)];
    dispatch_data_t data = dispatch_data_create(valueBytes, sizeof(valueBytes), NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    [writer set:key value:data];
    dispatch_release(data);
    [writer release];
    CacheReader* reader = [[ObservedCacheReader alloc] initWithPath:path version:1];
    require(reader != nil, "reader-success");
    dispatch_data_t value = [reader get:key];
    require(value != nil, "read-stored-value");
    const void* bytes = NULL;
    size_t length = 0;
    dispatch_data_t mapped = dispatch_data_create_map(value, &bytes, &length);
    require(length == sizeof(valueBytes) && memcmp(bytes, valueBytes, length) == 0, "stored-value-exact");
    dispatch_release(mapped);
    dispatch_release(value);
    [reader release];
    require(readerDeallocations == readersBefore + 1 && writerDeallocations == writersBefore + 1, "normal-deallocation-once");
    require(cacheAudit.opens == 2 && cacheAudit.prepares == 2 && cacheAudit.finalizes == 2, "normal-statements-finalized-once");
    drained();
    puts("CACHE_INITIALIZATION_NATIVE_CASE_PASS case=round-trip sqlite_opens=2 sqlite_closes=2 finalized=2");
}

int main() { @autoreleasepool {
    char directory[PATH_MAX];
    require(snprintf(directory, sizeof(directory), "%s/d3dalloy-cache-init-XXXXXX", NSTemporaryDirectory().fileSystemRepresentation)
            < (int)sizeof(directory), "temporary-path-fits");
    require(mkdtemp(directory) != NULL, "temporary-directory");
    NSString* root = [NSString stringWithUTF8String:directory];
    NSString* (^path)(NSString*) = ^(NSString* name) { return [root stringByAppendingPathComponent:name]; };
    failure(false, path(@"missing.sqlite"), "reader-open", 1, 1, 0);
    schema(path(@"reader-schema.sqlite"), "CREATE TABLE unrelated (key BLOB);");
    failure(false, path(@"reader-schema.sqlite"), "reader-prepare", 1, 0, 1);
    require([[NSData data] writeToFile:path(@"not-a-directory") atomically:NO], "lock-failure-fixture");
    failure(true, [path(@"not-a-directory") stringByAppendingPathComponent:@"cache.sqlite"], "writer-lock-open", 0, 0, 0);
    require([[NSFileManager defaultManager] createDirectoryAtPath:path(@"directory.sqlite")
            withIntermediateDirectories:NO attributes:nil error:nil], "sqlite-open-failure-fixture");
    failure(true, path(@"directory.sqlite"), "writer-open", 1, 1, 0);
    schema(path(@"writer-schema.sqlite"), "CREATE TABLE cache_1 (key BLOB PRIMARY KEY);");
    failure(true, path(@"writer-schema.sqlite"), "writer-prepare", 1, 0, 1);
    roundTrip(path(@"round-trip.sqlite"));
    require([[NSFileManager defaultManager] removeItemAtPath:root error:nil], "remove-temporary-directory");
    puts("CACHE_INITIALIZATION_NATIVE_PASS cases=6");
} }
