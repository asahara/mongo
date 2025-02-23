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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_record_store.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_kv_engine.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_radix_store.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace ephemeral_for_test {
namespace {

class RecordStoreHarnessHelper final : public ::mongo::RecordStoreHarnessHelper {
    KVEngine _kvEngine{};
    VisibilityManager _visibilityManager;

public:
    RecordStoreHarnessHelper() {}

    virtual std::unique_ptr<mongo::RecordStore> newRecordStore() {
        return newRecordStore("a.b", CollectionOptions());
    }

    virtual std::unique_ptr<mongo::RecordStore> newRecordStore(
        const std::string& ns,
        const CollectionOptions& collOptions,
        KeyFormat keyFormat = KeyFormat::Long) {
        if (collOptions.clusteredIndex) {
            // A clustered collection requires both CollectionOptions.clusteredIndex and
            // KeyFormat::String. For a clustered record store that is not associated with a
            // clustered collection KeyFormat::String is sufficient.
            uassert(6144102,
                    "RecordStore with CollectionOptions.clusteredIndex requires KeyFormat::String",
                    keyFormat == KeyFormat::String);
        }

        return std::make_unique<RecordStore>(ns,
                                             "ident"_sd /* ident */,
                                             collOptions.clusteredIndex ? KeyFormat::String
                                                                        : KeyFormat::Long,
                                             false /* isCapped */,
                                             nullptr /* cappedCallback */,
                                             nullptr /* visibilityManager */);
    }

    virtual std::unique_ptr<mongo::RecordStore> newOplogRecordStore() final {
        return std::make_unique<RecordStore>(NamespaceString::kRsOplogNamespace.toString(),
                                             "ident"_sd,
                                             KeyFormat::Long,
                                             /*isCapped*/ true,
                                             /*cappedCallback*/ nullptr,
                                             &_visibilityManager);
    }

    std::unique_ptr<mongo::RecoveryUnit> newRecoveryUnit() final {
        return std::make_unique<RecoveryUnit>(&_kvEngine);
    }

    KVEngine* getEngine() override final {
        return &_kvEngine;
    }
};

std::unique_ptr<mongo::RecordStoreHarnessHelper> makeRecordStoreHarnessHelper() {
    return std::make_unique<RecordStoreHarnessHelper>();
}

MONGO_INITIALIZER(RegisterRecordStoreHarnessFactory)(InitializerContext*) {
    mongo::registerRecordStoreHarnessHelperFactory(makeRecordStoreHarnessHelper);
}
}  // namespace
}  // namespace ephemeral_for_test
}  // namespace mongo
