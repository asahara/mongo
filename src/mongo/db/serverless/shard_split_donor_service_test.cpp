/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include <memory>

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/client/streamable_replica_set_monitor_for_testing.h"
#include "mongo/db/catalog/database_holder_mock.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/serverless/shard_split_donor_op_observer.h"
#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/db/serverless/shard_split_state_machine_gen.h"
#include "mongo/db/serverless/shard_split_test_utils.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"


namespace mongo {

namespace {
sdam::TopologyDescriptionPtr makeRecipientTopologyDescription(const MockReplicaSet& set) {
    std::shared_ptr<TopologyDescription> topologyDescription =
        std::make_shared<sdam::TopologyDescription>(sdam::SdamConfiguration(
            set.getHosts(), sdam::TopologyType::kReplicaSetNoPrimary, set.getSetName()));

    for (auto& server : set.getHosts()) {
        auto serverDescription = sdam::ServerDescriptionBuilder()
                                     .withAddress(server)
                                     .withSetName(set.getSetName())
                                     .instance();
        topologyDescription->installServerDescription(serverDescription);
    }

    return topologyDescription;
}

}  // namespace

std::ostringstream& operator<<(std::ostringstream& builder,
                               const mongo::ShardSplitDonorStateEnum state) {
    switch (state) {
        case mongo::ShardSplitDonorStateEnum::kUninitialized:
            builder << "kUninitialized";
            break;
        case mongo::ShardSplitDonorStateEnum::kAborted:
            builder << "kAborted";
            break;
        case mongo::ShardSplitDonorStateEnum::kBlocking:
            builder << "kBlocking";
            break;
        case mongo::ShardSplitDonorStateEnum::kCommitted:
            builder << "kCommitted";
            break;
        case mongo::ShardSplitDonorStateEnum::kDataSync:
            builder << "kDataSync";
            break;
    }

    return builder;
}

class ShardSplitDonorServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        // The database needs to be open before using shard split donor service.
        {
            auto opCtx = cc().makeOperationContext();
            AutoGetDb autoDb(
                opCtx.get(), NamespaceString::kTenantSplitDonorsNamespace.db(), MODE_X);
            auto db = autoDb.ensureDbExists(opCtx.get());
            ASSERT_TRUE(db);
        }

        // Timestamps of "0 seconds" are not allowed, so we must advance our clock mock to the first
        // real second. Don't save an instance, since this just internally modified the global
        // immortal ClockSourceMockImpl.
        ClockSourceMock clockSource;
        clockSource.advance(Milliseconds(1000));

        // Fake replSet just for creating consistent URI for monitor
        _rsmMonitor.setup(_replSet.getURI());
    }

protected:
    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ShardSplitDonorService>(serviceContext);
    }

    void setUpOpObserverRegistry(OpObserverRegistry* opObserverRegistry) override {
        opObserverRegistry->addObserver(std::make_unique<ShardSplitDonorOpObserver>());
    }

    ShardSplitDonorDocument defaultStateDocument() const {
        return ShardSplitDonorDocument::parse(
            {"donor.document"},
            BSON("_id" << _uuid << "tenantIds" << _tenantIds << "recipientTagName"
                       << _recipientTagName << "recipientSetName" << _recipientSetName));
    }


    UUID _uuid = UUID::gen();
    MockReplicaSet _replSet{
        "donorSetForTest", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    const NamespaceString _nss{"testDB2", "testColl2"};
    std::vector<std::string> _tenantIds = {"tenant1", "tenantAB"};
    StreamableReplicaSetMonitorForTesting _rsmMonitor;
    std::string _recipientTagName{"$recipientNode"};
    std::string _recipientSetName{_replSet.getURI().getSetName()};
};

TEST_F(ShardSplitDonorServiceTest, BasicShardSplitDonorServiceInstanceCreation) {
    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts());

    // Create and start the instance.
    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, defaultStateDocument().toBSON());
    ASSERT(serviceInstance.get());
    ASSERT_EQ(_uuid, serviceInstance->getId());

    auto completionFuture = serviceInstance->completionFuture();

    std::shared_ptr<TopologyDescription> topologyDescriptionOld =
        std::make_shared<sdam::TopologyDescription>(sdam::SdamConfiguration());
    std::shared_ptr<TopologyDescription> topologyDescriptionNew =
        makeRecipientTopologyDescription(_replSet);

    // Wait until the RSM has been created by the instance.
    auto replicaSetMonitorCreatedFuture = serviceInstance->replicaSetMonitorCreatedFuture();
    replicaSetMonitorCreatedFuture.wait(opCtx.get());

    // Retrieve monitor installed by _rsmMonitor.setup(...)
    auto monitor = std::dynamic_pointer_cast<StreamableReplicaSetMonitor>(
        ReplicaSetMonitor::createIfNeeded(_replSet.getURI()));
    invariant(monitor);
    auto publisher = monitor->getEventsPublisher();

    publisher->onTopologyDescriptionChangedEvent(topologyDescriptionOld, topologyDescriptionNew);

    completionFuture.wait();

    auto result = completionFuture.get();
    ASSERT(!result.abortReason);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kCommitted);
}

TEST_F(ShardSplitDonorServiceTest, ShardSplitDonorServiceTimeout) {
    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts());

    auto stateDocument = defaultStateDocument();

    // Set a timeout of 200 ms, and make sure we reset after this test is run
    ON_BLOCK_EXIT([splitTimout = repl::shardSplitTimeoutMS.load()] {
        repl::shardSplitTimeoutMS.store(splitTimout);
    });

    repl::shardSplitTimeoutMS.store(200);

    // Create and start the instance.
    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, stateDocument.toBSON());
    ASSERT(serviceInstance.get());
    ASSERT_EQ(_uuid, serviceInstance->getId());

    auto completionFuture = serviceInstance->completionFuture();

    auto result = completionFuture.get();

    ASSERT(result.abortReason);
    ASSERT_EQ(result.abortReason->code(), ErrorCodes::ExceededTimeLimit);
}

// Abort scenario : abortSplit called before startSplit.
TEST_F(ShardSplitDonorServiceTest, CreateInstanceInAbortState) {
    auto opCtx = makeOperationContext();

    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());

    auto stateDocument = defaultStateDocument();
    stateDocument.setState(ShardSplitDonorStateEnum::kAborted);

    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, stateDocument.toBSON());
    ASSERT(serviceInstance.get());

    auto result = serviceInstance->completionFuture().get(opCtx.get());

    ASSERT(!!result.abortReason);
    ASSERT_EQ(result.abortReason->code(), ErrorCodes::TenantMigrationAborted);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kAborted);
}

// Abort scenario : instance created through startSplit then calling abortSplit.
TEST_F(ShardSplitDonorServiceTest, CreateInstanceThenAbort) {
    auto opCtx = makeOperationContext();

    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts());

    std::shared_ptr<ShardSplitDonorService::DonorStateMachine> serviceInstance;
    {
        FailPointEnableBlock fp("pauseShardSplitAfterInitialSync");
        auto initialTimesEntered = fp.initialTimesEntered();

        serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
            opCtx.get(), _service, defaultStateDocument().toBSON());
        ASSERT(serviceInstance.get());

        fp->waitForTimesEntered(initialTimesEntered + 1);

        serviceInstance->tryAbort();
    }
    auto result = serviceInstance->completionFuture().get(opCtx.get());

    ASSERT(!!result.abortReason);
    ASSERT_EQ(result.abortReason->code(), ErrorCodes::TenantMigrationAborted);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kAborted);
}

TEST_F(ShardSplitDonorServiceTest, StepDownTest) {
    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts());

    std::shared_ptr<ShardSplitDonorService::DonorStateMachine> serviceInstance;

    {
        FailPointEnableBlock fp("pauseShardSplitAfterInitialSync");
        auto initialTimesEntered = fp.initialTimesEntered();

        serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
            opCtx.get(), _service, defaultStateDocument().toBSON());
        ASSERT(serviceInstance.get());

        fp->waitForTimesEntered(initialTimesEntered + 1);

        stepDown();
    }

    auto result = serviceInstance->completionFuture().getNoThrow();
    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(ErrorCodes::InterruptedDueToReplStateChange, result.getStatus());
}

class SplitReplicaSetObserverTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        // we need a mock replication coordinator in order to identify recipient nodes
        auto serviceContext = getServiceContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
        repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

        _rsmMonitor.setup(_validRepl.getURI());
        _otherRsmMonitor.setup(_invalidRepl.getURI());

        _executor = repl::makeTestExecutor();

        // Retrieve monitor installed by _rsmMonitor.setup(...)
        auto monitor = checked_pointer_cast<StreamableReplicaSetMonitor>(
            ReplicaSetMonitor::createIfNeeded(_validRepl.getURI()));
        invariant(monitor);
        _publisher = monitor->getEventsPublisher();
    }

protected:
    MockReplicaSet _validRepl{
        "replInScope", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    MockReplicaSet _invalidRepl{
        "replNotInScope", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};

    StreamableReplicaSetMonitorForTesting _rsmMonitor;
    StreamableReplicaSetMonitorForTesting _otherRsmMonitor;
    std::shared_ptr<executor::TaskExecutor> _executor;
    std::shared_ptr<sdam::TopologyEventsPublisher> _publisher;
    std::string _recipientTagName{"$recipientNode"};
    std::string _recipientSetName{_validRepl.getURI().getSetName()};
};

TEST_F(SplitReplicaSetObserverTest, SupportsCancellation) {
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _validRepl.getHosts());

    CancellationSource source;
    auto future = detail::makeRecipientAcceptSplitFuture(
        _executor, source.token(), _recipientTagName, _recipientSetName);

    ASSERT_FALSE(future.isReady());
    source.cancel();

    ASSERT_EQ(future.getNoThrow().code(), ErrorCodes::CallbackCanceled);
}

TEST_F(SplitReplicaSetObserverTest, GetRecipientAcceptSplitFutureTest) {
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _validRepl.getHosts());

    CancellationSource source;
    auto future = detail::makeRecipientAcceptSplitFuture(
        _executor, source.token(), _recipientTagName, _recipientSetName);

    std::shared_ptr<TopologyDescription> topologyDescriptionOld =
        std::make_shared<sdam::TopologyDescription>(sdam::SdamConfiguration());
    std::shared_ptr<TopologyDescription> topologyDescriptionNew =
        makeRecipientTopologyDescription(_validRepl);

    _publisher->onTopologyDescriptionChangedEvent(topologyDescriptionOld, topologyDescriptionNew);

    future.wait();
}

TEST_F(SplitReplicaSetObserverTest, FutureNotReadyMissingNodes) {
    auto predicate =
        detail::makeRecipientAcceptSplitPredicate(_validRepl.getURI().connectionString());

    std::shared_ptr<TopologyDescription> topologyDescriptionNew =
        makeRecipientTopologyDescription(_validRepl);
    topologyDescriptionNew->removeServerDescription(_validRepl.getHosts()[0]);

    ASSERT_FALSE(predicate(topologyDescriptionNew->getServers()));
}

TEST_F(SplitReplicaSetObserverTest, FutureNotReadyWrongSet) {
    auto predicate =
        detail::makeRecipientAcceptSplitPredicate(_validRepl.getURI().connectionString());

    std::shared_ptr<TopologyDescription> topologyDescriptionNew =
        makeRecipientTopologyDescription(_invalidRepl);

    ASSERT_FALSE(predicate(topologyDescriptionNew->getServers()));
}

TEST_F(SplitReplicaSetObserverTest, ExecutorCanceled) {
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _validRepl.getHosts());

    CancellationSource source;
    auto future = detail::makeRecipientAcceptSplitFuture(
        _executor, source.token(), _recipientTagName, _recipientSetName);

    _executor->shutdown();
    _executor->join();

    ASSERT_FALSE(future.isReady());

    // Ensure the test does not hang.
    source.cancel();
    ASSERT_EQ(future.getNoThrow().code(), ErrorCodes::ShutdownInProgress);
}

}  // namespace mongo
