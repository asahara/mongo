/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <memory>

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class OperationContext;
class ScopedSession;
class ServiceContext;

/**
 * Provides facilities for extracting oplog entries of writes in a particular namespace that needs
 * to be migrated.
 *
 * This also ensures that oplog returned are majority committed. This is achieved by calling
 * waitForWriteConcern. However, waitForWriteConcern does not support waiting for opTimes of
 * previous terms. To get around this, the waitForWriteConcern is performed in two phases:
 *
 * During init() call phase:
 * 1. Scan the entire config.transactions and extract all the lastWriteOpTime.
 * 2. Insert a no-op oplog entry and wait for it to be majority committed.
 * 3. At this point any writes before should be majority committed (including all the oplog
 *    entries that the collected lastWriteOpTime points to). If the particular oplog with the
 *    opTime cannot be found: it either means that the oplog was truncated or rolled back.
 *
 * New writes/xfer mods phase oplog entries:
 * In this case, caller is responsible for calling waitForWriteConcern. If getLastFetchedOplog
 * returns shouldWaitForMajority == true, it should wait for the highest opTime it has got from
 * getLastFetchedOplog. It should also error if it detects a change of term within a batch since
 * it would be wrong to wait for the highest opTime in this case.
 */
class SessionCatalogMigrationSource {
    SessionCatalogMigrationSource(const SessionCatalogMigrationSource&) = delete;
    SessionCatalogMigrationSource& operator=(const SessionCatalogMigrationSource&) = delete;

public:
    enum class EntryAtOpTimeType { kTransaction, kRetryableWrite };

    struct OplogResult {
        OplogResult(boost::optional<repl::OplogEntry> _oplog, bool _shouldWaitForMajority)
            : oplog(std::move(_oplog)), shouldWaitForMajority(_shouldWaitForMajority) {}

        // The oplog fetched.
        boost::optional<repl::OplogEntry> oplog;

        // If this is set to true, oplog returned is not confirmed to be majority committed,
        // so the caller has to explicitly wait for it to be committed to majority.
        bool shouldWaitForMajority = false;
    };

    SessionCatalogMigrationSource(OperationContext* opCtx,
                                  NamespaceString ns,
                                  ChunkRange chunk,
                                  KeyPattern shardKey);

    /**
     * Returns true if there are more oplog entries to fetch at this moment. Note that new writes
     * can still continue to come in after this has returned false, so it can become true again.
     * Once this has returned false, this means that it has depleted the existing buffer so it
     * is a good time to enter the critical section.
     */
    bool hasMoreOplog();

    /**
     * Returns true if the majority committed oplog entries are drained and false otherwise.
     */
    bool inCatchupPhase();

    /**
     * Returns the estimated bytes of data left to transfer in _newWriteOpTimeList.
     */
    int64_t untransferredCatchUpDataSize();

    /**
     * Attempts to fetch the next oplog entry. Returns true if it was able to fetch anything.
     */
    bool fetchNextOplog(OperationContext* opCtx);

    /**
     * Returns a notification that can be used to wait for new oplog entries to fetch. Note
     * that this should only be called if hasMoreOplog/fetchNextOplog returned false at
     * least once.
     *
     * If the notification is set to true, then that means that there is no longer a need to
     * fetch more oplog because the data migration has entered the critical section and
     * the buffer for oplog to fetch is empty or the data migration has aborted.
     */
    std::shared_ptr<Notification<bool>> getNotificationForNewOplog();

    /**
     * Returns the oplog document that was last fetched by the fetchNextOplog call.
     * Returns an empty object if there are no oplog to fetch.
     */
    OplogResult getLastFetchedOplog();

    /**
     * Remembers the oplog timestamp of a new write that just occurred.
     */
    void notifyNewWriteOpTime(repl::OpTime opTimestamp, EntryAtOpTimeType entryAtOpTimeType);

    /**
     * Returns the rollback ID recorded at the beginning of session migration.
     */
    int getRollbackIdAtInit() const {
        return _rollbackIdAtInit;
    }

    /**
     * Inform this session migration machinery that the data migration just entered the critical
     * section.
     */
    void onCommitCloneStarted();

    /**
     * Inform this session migration machinery that the data migration just terminated and
     * entering the cleanup phase (can be aborted or committed).
     */
    void onCloneCleanup();

    /**
     * This function will utilize the shardKeyPattern and chunkRange to evaluate whether or not
     * the oplogEntry is relevant to the migration. If not, the chunk should be skipped and the
     * function will return true. Otherwise the function will return false.
     *
     * If the oplogEntry is of type no-op and it has been rewritten by another migration and it's
     * outside of the chunk range, then it should be skipped. Or if the oplog is a crud operation
     * and it's outside of the chunk range then it should be skipped.
     */
    static bool shouldSkipOplogEntry(const mongo::repl::OplogEntry& oplogEntry,
                                     const ShardKeyPattern& shardKeyPattern,
                                     const ChunkRange& chunkRange);

private:
    /**
     * An iterator for extracting session write oplogs that need to be cloned during migration.
     */
    class SessionOplogIterator {
    public:
        SessionOplogIterator(SessionTxnRecord txnRecord, int expectedRollbackId);

        /**
         * Returns the next oplog write that happened in this session, or boost::none if there
         * are no remaining entries for this session.
         *
         * If either:
         *     a) the oplog is lost because the oplog rolled over, or
         *     b) if the oplog entry is a prepare or commitTransaction entry,
         * this will return a sentinel oplog entry instead with type 'n' and o2 field set to
         * Session::kDeadEndSentinel.  This will also mean that next subsequent calls to getNext
         * will return boost::none.
         */
        boost::optional<repl::OplogEntry> getNext(OperationContext* opCtx);

        BSONObj toBSON() const {
            return _record.toBSON();
        }

    private:
        const SessionTxnRecord _record;
        const int _initialRollbackId;
        std::unique_ptr<TransactionHistoryIterator> _writeHistoryIterator;
    };

    enum class State { kActive, kCommitStarted, kCleanup };

    ///////////////////////////////////////////////////////////////////////////
    // Methods for extracting the oplog entries from session information.

    /**
     * If this returns false, it just means that there are no more oplog entry in the buffer that
     * needs to be moved over. However, there can still be new incoming operations that can add
     * new entries. Also see hasNewWrites.
     */
    bool _hasMoreOplogFromSessionCatalog();

    /**
     * Attempts to extract the next oplog document by following the oplog chain from the sessions
     * catalog. Returns true if a document was actually fetched.
     */
    bool _fetchNextOplogFromSessionCatalog(OperationContext* opCtx);

    /**
     * Extracts oplog information from the current writeHistoryIterator to _lastFetchedOplog. This
     * handles insert/update/delete/findAndModify oplog entries.
     *
     * Returns true if current writeHistoryIterator has any oplog entry.
     */
    bool _handleWriteHistory(WithLock, OperationContext* opCtx);

    ///////////////////////////////////////////////////////////////////////////
    // Methods for capturing and extracting oplog entries for new writes.

    /**
     * Returns true if there are oplog generated by new writes that needs to be fetched.
     */
    bool _hasNewWrites(WithLock);

    /**
     * Attempts to fetch the next oplog entry from the new writes that was saved by saveNewWriteTS.
     * Returns true if there were documents that were retrieved.
     */
    bool _fetchNextNewWriteOplog(OperationContext* opCtx);

    // Namespace for which the migration is happening
    const NamespaceString _ns;

    // The rollback id just before migration started. This value is needed so that step-down
    // followed by step-up situations can be discovered.
    const int _rollbackIdAtInit;

    const ChunkRange _chunkRange;
    const ShardKeyPattern _keyPattern;

    // Protects _sessionCatalogCursor, _sessionOplogIterators, _currentOplogIterator,
    // _lastFetchedOplogBuffer, _lastFetchedOplog
    Mutex _sessionCloneMutex =
        MONGO_MAKE_LATCH("SessionCatalogMigrationSource::_sessionCloneMutex");

    // List of remaining session records that needs to be cloned.
    std::vector<std::unique_ptr<SessionOplogIterator>> _sessionOplogIterators;

    // Points to the current session record eing cloned.
    std::unique_ptr<SessionOplogIterator> _currentOplogIterator;

    // Used for temporarily storng oplog entries for operations that has more than one entry.
    // For example, findAndModify generates one for the actual operation and another for the
    // pre/post image.
    std::vector<repl::OplogEntry> _lastFetchedOplogBuffer;

    // Used to store the last fetched oplog. This enables calling get multiple times.
    boost::optional<repl::OplogEntry> _lastFetchedOplog;

    // Protects _newWriteTsList, _lastFetchedNewWriteOplog, _state, _newOplogNotification
    Mutex _newOplogMutex = MONGO_MAKE_LATCH("SessionCatalogMigrationSource::_newOplogMutex");

    // The average size of documents in config.transactions.
    uint64_t _averageSessionDocSize;

    // Stores oplog opTime of new writes that are coming in.
    std::list<std::pair<repl::OpTime, EntryAtOpTimeType>> _newWriteOpTimeList;

    // Used to store the last fetched oplog from _newWriteTsList.
    boost::optional<repl::OplogEntry> _lastFetchedNewWriteOplog;

    // Used to store an image when `_lastFetchedNewWriteOplog` has a `needsRetryImage` field.
    boost::optional<repl::OplogEntry> _lastFetchedNewWriteOplogImage;

    // Stores the current state.
    State _state{State::kActive};

    // Holds the latest request for notification of new oplog entries that needs to be fetched.
    // Sets to true if there is no need to fetch an oplog anymore (for example, because migration
    // aborted).
    std::shared_ptr<Notification<bool>> _newOplogNotification;
};

}  // namespace mongo
