#include <Storages/NATS/NATSSource.h>

#include <Formats/FormatFactory.h>
#include <Interpreters/Context.h>
#include <Processors/Executors/StreamingFormatExecutor.h>
#include <Storages/NATS/NATSConsumer.h>
#include <IO/EmptyReadBuffer.h>

namespace DB
{

static std::pair<Block, Block> getHeaders(const StorageSnapshotPtr & storage_snapshot)
{
    auto non_virtual_header = storage_snapshot->metadata->getSampleBlockNonMaterialized();
    auto virtual_header = storage_snapshot->getSampleBlockForColumns({"_subject"});

    return {non_virtual_header, virtual_header};
}

static Block getSampleBlock(const Block & non_virtual_header, const Block & virtual_header)
{
    auto header = non_virtual_header;
    for (const auto & column : virtual_header)
        header.insert(column);

    return header;
}

NATSSource::NATSSource(
    StorageNATS & storage_,
    const StorageSnapshotPtr & storage_snapshot_,
    ContextPtr context_,
    const Names & columns,
    size_t max_block_size_)
    : NATSSource(storage_, storage_snapshot_, getHeaders(storage_snapshot_), context_, columns, max_block_size_)
{
}

NATSSource::NATSSource(
    StorageNATS & storage_,
    const StorageSnapshotPtr & storage_snapshot_,
    std::pair<Block, Block> headers,
    ContextPtr context_,
    const Names & columns,
    size_t max_block_size_)
    : ISource(getSampleBlock(headers.first, headers.second))
    , storage(storage_)
    , storage_snapshot(storage_snapshot_)
    , context(context_)
    , column_names(columns)
    , max_block_size(max_block_size_)
    , non_virtual_header(std::move(headers.first))
    , virtual_header(std::move(headers.second))
{
    storage.incrementReader();
}


NATSSource::~NATSSource()
{
    storage.decrementReader();

    if (!consumer)
        return;

    storage.pushConsumer(consumer);
}

bool NATSSource::checkTimeLimit() const
{
    if (max_execution_time != 0)
    {
        auto elapsed_ns = total_stopwatch.elapsed();

        if (elapsed_ns > static_cast<UInt64>(max_execution_time.totalMicroseconds()) * 1000)
            return false;
    }

    return true;
}

Chunk NATSSource::generate()
{
    if (!consumer)
    {
        auto timeout = std::chrono::milliseconds(context->getSettingsRef().rabbitmq_max_wait_ms.totalMilliseconds());
        consumer = storage.popConsumer(timeout);
        consumer->subscribe();
    }

    if (!consumer || is_finished)
        return {};

    is_finished = true;

    MutableColumns virtual_columns = virtual_header.cloneEmptyColumns();
    EmptyReadBuffer empty_buf;
    auto input_format
        = FormatFactory::instance().getInputFormat(storage.getFormatName(), empty_buf, non_virtual_header, context, max_block_size);

    StreamingFormatExecutor executor(non_virtual_header, input_format);

    size_t total_rows = 0;

    while (true)
    {
        if (consumer->queueEmpty())
            break;

        size_t new_rows = 0;
        if (auto buf = consumer->consume())
            new_rows = executor.execute(*buf);

        if (new_rows)
        {
            auto subject = consumer->getSubject();
            virtual_columns[0]->insertMany(subject, new_rows);

            total_rows = total_rows + new_rows;
        }

        if (total_rows >= max_block_size || consumer->queueEmpty() || consumer->isConsumerStopped() || !checkTimeLimit())
            break;
    }

    if (total_rows == 0)
        return {};

    auto result_columns = executor.getResultColumns();
    for (auto & column : virtual_columns)
        result_columns.push_back(std::move(column));

    return Chunk(std::move(result_columns), total_rows);
}

}
