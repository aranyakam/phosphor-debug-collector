#include "dump_utils.hpp"

#include <fmt/core.h>

#include <phosphor-logging/log.hpp>
#include <sdbusplus/async.hpp>

namespace phosphor
{
namespace dump
{

using namespace phosphor::logging;

std::string getService(sdbusplus::bus_t& bus, const std::string& path,
                       const std::string& interface)
{
    constexpr auto objectMapperName = "xyz.openbmc_project.ObjectMapper";
    constexpr auto objectMapperPath = "/xyz/openbmc_project/object_mapper";

    auto method = bus.new_method_call(objectMapperName, objectMapperPath,
                                      objectMapperName, "GetObject");

    method.append(path);
    method.append(std::vector<std::string>({interface}));

    std::vector<std::pair<std::string, std::vector<std::string>>> response;

    try
    {
        auto reply = bus.call(method);
        reply.read(response);
        if (response.empty())
        {
            log<level::ERR>(fmt::format("Error in mapper response for getting "
                                        "service name, PATH({}), INTERFACE({})",
                                        path, interface)
                                .c_str());
            return std::string{};
        }
    }
    catch (const sdbusplus::exception_t& e)
    {
        log<level::ERR>(fmt::format("Error in mapper method call, "
                                    "errormsg({}), PATH({}), INTERFACE({})",
                                    e.what(), path, interface)
                            .c_str());
        return std::string{};
    }
    return response[0].first;
}

BootProgress getBootProgress()
{
    constexpr auto bootProgressInterface =
        "xyz.openbmc_project.State.Boot.Progress";
    // TODO Need to change host instance if multiple instead "0"
    constexpr auto hostStateObjPath = "/xyz/openbmc_project/state/host0";
    auto value = getStateValue(bootProgressInterface, hostStateObjPath,
                               "BootProgress");
    return sdbusplus::xyz::openbmc_project::State::Boot::server::Progress::
        convertProgressStagesFromString(value);
}

HostState getHostState()
{
    constexpr auto hostStateInterface = "xyz.openbmc_project.State.Host";
    // TODO Need to change host instance if multiple instead "0"
    constexpr auto hostStateObjPath = "/xyz/openbmc_project/state/host0";
    auto value = getStateValue(hostStateInterface, hostStateObjPath,
                               "CurrentHostState");
    return sdbusplus::xyz::openbmc_project::State::server::Host::
        convertHostStateFromString(value);
}

std::string getStateValue(const std::string& intf, const std::string& objPath,
                          const std::string& state)
{
    std::string stateVal;
    try
    {
        auto bus = sdbusplus::bus::new_default();
        auto service = getService(bus, objPath, intf);

        auto method = bus.new_method_call(service.c_str(), objPath.c_str(),
                                          "org.freedesktop.DBus.Properties",
                                          "Get");

        method.append(intf, state);

        auto reply = bus.call(method);

        using DBusValue_t =
            std::variant<std::string, bool, std::vector<uint8_t>,
                         std::vector<std::string>>;
        DBusValue_t propertyVal;

        reply.read(propertyVal);

        stateVal = std::get<std::string>(propertyVal);
    }
    catch (const sdbusplus::exception_t& e)
    {
        log<level::ERR>(fmt::format("D-Bus call exception, OBJPATH({}), "
                                    "INTERFACE({}), PROPERTY({}) EXCEPTION({})",
                                    objPath, intf, state, e.what())
                            .c_str());
        throw std::runtime_error("Failed to get state property");
    }
    catch (const std::bad_variant_access& e)
    {
        log<level::ERR>(
            fmt::format("Exception raised while read host state({}) property "
                        "value,  OBJPATH({}), INTERFACE({}), EXCEPTION({})",
                        state, objPath, intf, e.what())
                .c_str());
        throw std::runtime_error("Failed to get host state property");
    }

    return stateVal;
}

bool isHostRunning()
{
    // TODO #ibm-openbmc/dev/2858 Revisit the method for finding whether host
    // is running.
    BootProgress bootProgressStatus = phosphor::dump::getBootProgress();
    if ((bootProgressStatus == BootProgress::SystemInitComplete) ||
        (bootProgressStatus == BootProgress::SystemSetup) ||
        (bootProgressStatus == BootProgress::OSStart) ||
        (bootProgressStatus == BootProgress::OSRunning) ||
        (bootProgressStatus == BootProgress::PCIInit))
    {
        return true;
    }
    return false;
}

bool isHostQuiesced()
{
    return (phosphor::dump::getHostState() == HostState::Quiesced);
}

void createPEL(sdbusplus::bus::bus& dBus, const std::string& dumpFilePath,
               const std::string& dumpFileType, const int dumpId,
               const std::string& pelSev, const std::string& errIntf)
{
    try
    {
        constexpr auto loggerObjectPath = "/xyz/openbmc_project/logging";
        constexpr auto loggerCreateInterface =
            "xyz.openbmc_project.Logging.Create";
        constexpr auto loggerService = "xyz.openbmc_project.Logging";

        constexpr auto dumpFileString = "File Name";
        constexpr auto dumpFileTypeString = "Dump Type";
        constexpr auto dumpIdString = "Dump ID";

        const std::unordered_map<std::string_view, std::string_view>
            userDataMap = {{dumpIdString, std::to_string(dumpId)},
                           {dumpFileString, dumpFilePath},
                           {dumpFileTypeString, dumpFileType}};

        // Set up a connection to D-Bus object
        auto busMethod = dBus.new_method_call(loggerService, loggerObjectPath,
                                              loggerCreateInterface, "Create");
        busMethod.append(errIntf, pelSev, userDataMap);

        // Implies this is a call from Manager. Hence we need to make an async
        // call to avoid deadlock with Phosphor-logging.
        auto retVal = busMethod.call_async(
            [&](sdbusplus::message::message&& reply) {
            if (reply.is_method_error())
            {
                log<level::ERR>("Error in calling async method to create PEL");
            }
        });
        if (!retVal)
        {
            log<level::ERR>("Return object contains null pointer");
        }
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::ERR>("Error in calling creating PEL. Exception caught",
                        entry("ERROR=%s", e.what()));
    }
}

} // namespace dump
} // namespace phosphor
