#include "config.h"

#include "dump_manager_system.hpp"

#include "dump-extensions/openpower-dumps/openpower_dumps_config.h"

#include "dump_utils.hpp"
#include "op_dump_consts.hpp"
#include "op_dump_util.hpp"
#include "system_dump_entry.hpp"
#include "system_dump_serialize.hpp"
#include "xyz/openbmc_project/Common/error.hpp"

#include <fmt/core.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>

namespace openpower
{
namespace dump
{
namespace system
{

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;

void Manager::notify(uint32_t dumpId, uint64_t size)
{
    // Get the timestamp
    uint64_t timeStamp =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    // System dump can get created due to a fault in server
    // or by request from user. A system dump by fault is
    // first reported here, but for a user requested dump an
    // entry will be created first with invalid source id.
    // Since there can be only one system dump creation at a time,
    // if there is an entry with invalid sourceId update that.
    openpower::dump::system::Entry* upEntry = nullptr;
    for (auto& entry : entries)
    {
        openpower::dump::system::Entry* sysEntry =
            dynamic_cast<openpower::dump::system::Entry*>(entry.second.get());

        // If there is already a completed entry with input source id then
        // ignore this notification
        if ((sysEntry->sourceDumpId() == dumpId) &&
            (sysEntry->status() == phosphor::dump::OperationStatus::Completed))
        {
            log<level::INFO>(
                fmt::format("System dump entry with source dump id({}) is "
                            "already present with entry id({})",
                            dumpId, sysEntry->getDumpId())
                    .c_str());
            return;
        }

        // Save the first entry with INVALID_SOURCE_ID
        // but continue in the loop to make sure the
        // new entry is not duplicate
        if ((sysEntry->sourceDumpId() == INVALID_SOURCE_ID) &&
            (upEntry == nullptr))
        {
            upEntry = sysEntry;
        }
    }

    if (upEntry != nullptr)
    {
        log<level::INFO>(
            fmt::format("System Dump Notify: Updating dump entry with Id({}) "
                        "with source  Id({}) and Size({})",
                        upEntry->getDumpId(), dumpId, size)
                .c_str());
        upEntry->update(timeStamp, size, dumpId);
        return;
    }

    // Get the id
    auto id = lastEntryId + 1;
    auto idString = std::to_string(id);
    auto objPath = std::filesystem::path(baseEntryPath) / idString;

    // TODO: Get the originator Id, Type from the persisted file.
    // For now replacing it with null
    try
    {
        log<level::INFO>(
            fmt::format("System Dump Notify: creating new dump "
                        "entry with dumpId({}) with source Id({}) and Size({})",
                        id, dumpId, size)
                .c_str());

        auto entry = std::make_unique<system::Entry>(
            bus, objPath.c_str(), id, timeStamp, size, dumpId,
            phosphor::dump::OperationStatus::Completed, std::string(),
            originatorTypes::Internal, baseEntryPath, *this);
        serialize(*entry.get());
        entries.insert(std::make_pair(id, std::move(entry)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(
            fmt::format(
                "Error in creating system dump entry, errormsg({}), "
                "OBJECTPATH({}), ID({}), TIMESTAMP({}),SIZE({}), SOURCEID({})",
                e.what(), objPath.c_str(), id, timeStamp, size, dumpId)
                .c_str());
        report<InternalFailure>();
        return;
    }
    lastEntryId++;
    return;
}

sdbusplus::message::object_path
    Manager::createDump(phosphor::dump::DumpCreateParams params)
{
    constexpr auto SYSTEMD_SERVICE = "org.freedesktop.systemd1";
    constexpr auto SYSTEMD_OBJ_PATH = "/org/freedesktop/systemd1";
    constexpr auto SYSTEMD_INTERFACE = "org.freedesktop.systemd1.Manager";
    constexpr auto DIAG_MOD_TARGET = "obmc-host-crash@0.target";

    if (params.size() > CREATE_DUMP_MAX_PARAMS)
    {
        log<level::WARNING>(
            "System dump accepts not more than 2 additional parameters");
    }

    if (openpower::dump::util::isSystemDumpInProgress())
    {
        log<level::ERR>(
            fmt::format("Another dump in progress or available to offload")
                .c_str());
        elog<sdbusplus::xyz::openbmc_project::Common::Error::Unavailable>();
    }

    using NotAllowed =
        sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed;
    using Reason = xyz::openbmc_project::Common::NotAllowed::REASON;

    auto hostState = phosphor::dump::getHostState();
    // Allow creating system dump only when the host is up.
    if (!(((phosphor::dump::isHostRunning()) ||
           (hostState == phosphor::dump::HostState::Quiesced) ||
           (hostState == phosphor::dump::HostState::TransitioningToOff))))
    {
        elog<NotAllowed>(
            Reason("System dump can be initiated only when the host is up"));
        return std::string();
    }

    // Get the originator id and type from params
    std::string originatorId;
    originatorTypes originatorType;

    phosphor::dump::extractOriginatorProperties(params, originatorId,
                                                originatorType);

    auto b = sdbusplus::bus::new_default();
    auto method = bus.new_method_call(SYSTEMD_SERVICE, SYSTEMD_OBJ_PATH,
                                      SYSTEMD_INTERFACE, "StartUnit");
    method.append(DIAG_MOD_TARGET); // unit to activate
    method.append("replace");
    bus.call_noreply(method);

    auto id = lastEntryId + 1;
    auto idString = std::to_string(id);
    auto objPath = std::filesystem::path(baseEntryPath) / idString;
    uint64_t timeStamp =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    try
    {
        auto entry = std::make_unique<system::Entry>(
            bus, objPath.c_str(), id, timeStamp, 0, INVALID_SOURCE_ID,
            phosphor::dump::OperationStatus::InProgress, originatorId,
            originatorType, baseEntryPath, *this);
        serialize(*entry.get());
        entries.insert(std::make_pair(id, std::move(entry)));
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>(
            fmt::format("Error in creating system dump entry, errormsg({}), "
                        "OBJECTPATH({}), ID({})",
                        e.what(), objPath.c_str(), id)
                .c_str());
        elog<InternalFailure>();
        return std::string();
    }
    lastEntryId++;
    return objPath.string();
}

void Manager::restore()
{
    std::filesystem::path dir(SYSTEM_DUMP_SERIAL_PATH);
    if (!std::filesystem::exists(dir) || std::filesystem::is_empty(dir))
    {
        log<level::INFO>(
            fmt::format(
                "System dump does not exist in the path ({}) to restore",
                dir.c_str())
                .c_str());
        return;
    }

    std::vector<uint32_t> dumpIds;
    for (auto& file : std::filesystem::directory_iterator(dir))
    {
        try
        {
            auto idNum = std::stol(file.path().filename().c_str());
            auto idString = std::to_string(idNum);

            auto objPath = std::filesystem::path(baseEntryPath) / idString;
            auto entry = std::make_unique<system::Entry>(
                bus, objPath, 0, 0, 0, 0,
                phosphor::dump::OperationStatus::InProgress, std::string(),
                originatorTypes::Internal, baseEntryPath, *this, false);
            if (deserialize(file.path(), *entry))
            {
                entries.insert(std::make_pair(idNum, std::move(entry)));
                dumpIds.push_back(idNum);
            }
            else
            {
                log<level::ERR>(fmt::format("Error in restoring the dump ids, "
                                            "OBJECTPATH({}), ID({})",
                                            objPath.c_str(), idNum)
                                    .c_str());
            }
        }
        // Continue to retrieve the next dump entry
        catch (const std::invalid_argument& e)
        {
            log<level::ERR>(fmt::format("Exception caught during restore with "
                                        "errormsg({}) and ID({})",
                                        e.what(),
                                        file.path().filename().c_str())
                                .c_str());
        }
        catch (const std::exception& e)
        {
            log<level::ERR>(fmt::format("Exception caught during restore with "
                                        "errormsg({}) and ID({})",
                                        e.what(),
                                        file.path().filename().c_str())
                                .c_str());
        }
    }

    if (!dumpIds.empty())
    {
        lastEntryId = *(std::max_element(dumpIds.begin(), dumpIds.end()));
    }
}

} // namespace system
} // namespace dump
} // namespace openpower
