#include <Storages/MergeTree/ReplicatedMergeTreeAttachThread.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Common/ZooKeeper/IKeeper.h>

namespace DB
{

ReplicatedMergeTreeAttachThread::ReplicatedMergeTreeAttachThread(StorageReplicatedMergeTree & storage_)
    : storage(storage_)
    , log_name(storage.getStorageID().getFullTableName() + " (ReplicatedMergeTreeAttachThread)")
    , log(&Poco::Logger::get(log_name))
{
    task = storage.getContext()->getSchedulePool().createTask(log_name, [this] { run(); });
    const auto storage_settings = storage.getSettings();
    retry_period = storage_settings->initialization_retry_period.totalSeconds();
}

ReplicatedMergeTreeAttachThread::~ReplicatedMergeTreeAttachThread()
{
    shutdown();
}

void ReplicatedMergeTreeAttachThread::shutdown()
{
    if (!shutdown_called.exchange(true))
    {
        task->deactivate();
        LOG_INFO(log, "Attach thread finished");
    }
}

void ReplicatedMergeTreeAttachThread::run()
{
    bool needs_retry{false};
    try
    {
        // we delay the first reconnect if the storage failed to connect to ZK initially
        if (!first_try_done && !storage.current_zookeeper)
        {
            needs_retry = true;
        }
        else
        {
            runImpl();
            finalizeInitialization();
        }
    }
    catch (const Exception & e)
    {
        if (const auto * coordination_exception = dynamic_cast<const Coordination::Exception *>(&e))
            needs_retry = Coordination::isHardwareError(coordination_exception->code);

        if (needs_retry)
        {
            LOG_ERROR(log, "Initialization failed. Error: {}", e.message());
        }
        else
        {
            LOG_ERROR(log, "Initialization failed, table will remain readonly. Error: {}", e.message());
            storage.initialization_done = true;
        }
    }

    if (!first_try_done.exchange(true))
        first_try_done.notify_one();

    if (shutdown_called)
    {
        LOG_WARNING(log, "Shutdown called, cancelling initialization");
        return;
    }

    if (needs_retry)
    {
        LOG_INFO(log, "Will retry initialization in {}s", retry_period);
        task->scheduleAfter(retry_period * 1000);
    }
}

void ReplicatedMergeTreeAttachThread::runImpl()
{
    storage.setZooKeeper();

    auto zookeeper = storage.getZooKeeper();
    const auto & zookeeper_path = storage.zookeeper_path;
    bool metadata_exists = zookeeper->exists(zookeeper_path + "/metadata");
    if (!metadata_exists)
    {
        LOG_WARNING(log, "No metadata in ZooKeeper for {}: table will stay in readonly mode.", zookeeper_path);
        storage.has_metadata_in_zookeeper = false;
        return;
    }

    auto metadata_snapshot = storage.getInMemoryMetadataPtr();

    const auto & replica_path = storage.replica_path;
    /// May it be ZK lost not the whole root, so the upper check passed, but only the /replicas/replica
    /// folder.
    bool replica_path_exists = zookeeper->exists(replica_path);
    if (!replica_path_exists)
    {
        LOG_WARNING(log, "No metadata in ZooKeeper for {}: table will stay in readonly mode", replica_path);
        storage.has_metadata_in_zookeeper = false;
        return;
    }

    storage.has_metadata_in_zookeeper = true;

    /// In old tables this node may missing or be empty
    String replica_metadata;
    const bool replica_metadata_exists = zookeeper->tryGet(replica_path + "/metadata", replica_metadata);

    if (!replica_metadata_exists || replica_metadata.empty())
    {
        /// We have to check shared node granularity before we create ours.
        storage.other_replicas_fixed_granularity = storage.checkFixedGranularityInZookeeper();

        ReplicatedMergeTreeTableMetadata current_metadata(storage, metadata_snapshot);

        zookeeper->createOrUpdate(replica_path + "/metadata", current_metadata.toString(), zkutil::CreateMode::Persistent);
    }

    storage.checkTableStructure(replica_path, metadata_snapshot);
    storage.checkParts(skip_sanity_checks);

    if (zookeeper->exists(replica_path + "/metadata_version"))
    {
        storage.metadata_version = parse<int>(zookeeper->get(replica_path + "/metadata_version"));
    }
    else
    {
        /// This replica was created with old clickhouse version, so we have
        /// to take version of global node. If somebody will alter our
        /// table, then we will fill /metadata_version node in zookeeper.
        /// Otherwise on the next restart we can again use version from
        /// shared metadata node because it was not changed.
        Coordination::Stat metadata_stat;
        zookeeper->get(zookeeper_path + "/metadata", &metadata_stat);
        storage.metadata_version = metadata_stat.version;
    }

    /// Temporary directories contain uninitialized results of Merges or Fetches (after forced restart),
    /// don't allow to reinitialize them, delete each of them immediately.
    storage.clearOldTemporaryDirectories(0, {"tmp_", "delete_tmp_", "tmp-fetch_"});
    storage.clearOldWriteAheadLogs();
    if (storage.getSettings()->merge_tree_enable_clear_old_broken_detached)
        storage.clearOldBrokenPartsFromDetachedDirecory();

    storage.createNewZooKeeperNodes();
    storage.syncPinnedPartUUIDs();

    storage.createTableSharedID();
};

void ReplicatedMergeTreeAttachThread::finalizeInitialization() TSA_NO_THREAD_SAFETY_ANALYSIS
{
    storage.startupImpl();
    storage.initialization_done = true;
    LOG_INFO(log, "Table is initialized");
}

void ReplicatedMergeTreeAttachThread::setSkipSanityChecks(bool skip_sanity_checks_)
{
    skip_sanity_checks = skip_sanity_checks_;
}

}
