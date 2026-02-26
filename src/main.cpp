/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#include "srvcfg_manager.hpp"

#include <boost/algorithm/string/replace.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/types/tuple.hpp>
#include <cereal/types/unordered_map.hpp>
#include <sdbusplus/bus/match.hpp>

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <memory>
#include <functional>
#include <atomic>

std::unique_ptr<boost::asio::steady_timer> timer = nullptr;
std::unique_ptr<boost::asio::steady_timer> initTimer = nullptr;
std::map<std::string, std::shared_ptr<phosphor::service::ServiceConfig>>
    srvMgrObjects;
static bool unitQueryStarted = false;

static constexpr const char* srvCfgMgrFileOld = "/etc/srvcfg-mgr.json";
static constexpr const char* srvCfgMgrFile = "srvcfg-mgr.json";
static constexpr const char* tmpFileBad = "/tmp/srvcfg-mgr.json.bad";

// Base service name list. All instance of these services and
// units(service/socket) will be managed by this daemon.
static std::unordered_map<std::string /* unitName */,
                          bool /* isSocketActivated */>
    managedServices = {{"phosphor-ipmi-net", false}, {"bmcweb", false},
                       {"phosphor-ipmi-kcs", false}, {"start-ipkvm", false},
                       {"obmc-console", false},      {"dropbear", true},
                       {"obmc-console-ssh", true},   {"ssifbridge", false},
                       {"xyz.openbmc_project.Pmt", false},
                       {"xyz.openbmc_project.VirtualMedia", false},
                       {"ipmb", false}};

enum class UnitType
{
    service,
    socket,
    target,
    device,
    invalid
};

using MonitorListMap =
    std::unordered_map<std::string, std::tuple<std::string, std::string,
                                               std::string, std::string>>;
MonitorListMap unitsToMonitor;

enum class monitorElement
{
    unitName,
    instanceName,
    serviceObjPath,
    socketObjPath
};

struct UnitStatus
{
    bool serviceReady{false};
    bool socketReady{false};
    std::string servicePath;
    std::string socketPath;
    std::atomic<bool> serviceChecked{false};
    std::atomic<bool> socketChecked{false};
};

std::tuple<std::string, UnitType, std::string> getUnitNameTypeAndInstance(
    const std::string& fullUnitName)
{
    UnitType type = UnitType::invalid;
    std::string instanceName;
    std::string unitName;
    // get service type
    auto typePos = fullUnitName.rfind(".");
    if (typePos != std::string::npos)
    {
        const auto& typeStr = fullUnitName.substr(typePos + 1);
        // Ignore types other than service and socket
        if (typeStr == "service")
        {
            type = UnitType::service;
        }
        else if (typeStr == "socket")
        {
            type = UnitType::socket;
        }
        // get instance name if available
        auto instancePos = fullUnitName.rfind("@");
        if (instancePos != std::string::npos)
        {
            instanceName =
                fullUnitName.substr(instancePos + 1, typePos - instancePos - 1);
            unitName = fullUnitName.substr(0, instancePos);
        }
        else
        {
            unitName = fullUnitName.substr(0, typePos);
        }
    }
    return std::make_tuple(unitName, type, instanceName);
}

void checkUnitStatus(
    const std::string& unitPath,
    std::shared_ptr<sdbusplus::asio::connection>& conn,

    std::function<void(bool)> callback)
{
    if (unitPath.empty())
    {
        callback(true);
        return;
    }

    try
    {
        auto methodCallback =
            [callback, unitPath](
                boost::system::error_code ec,
                const std::map<std::string, std::variant<std::string>>& properties) {

            try
            {
                if (ec)
                {
                    lg2::error(
                        "Failed to get properties for unit {UNIT}: {ERROR}",
                        "UNIT", unitPath,
                        "ERROR", ec.message());
                    callback(false);
                    return;
                }

                std::string activeState = "inactive";
                std::string subState = "dead";

                auto activeIt = properties.find("ActiveState");
                if (activeIt != properties.end())
                {
                    activeState = std::get<std::string>(activeIt->second);
                }

                auto subIt = properties.find("SubState");
                if (subIt != properties.end())
                {
                    subState = std::get<std::string>(subIt->second);
                }

                // Consider service ready based on state
                bool isReady = (activeState == "active" ||
                              activeState == "inactive" ||
                              activeState == "failed");

                // For socket units, also consider "listening" state as ready
                if (subState == "listening")
                {
                    isReady = true;
                }
                callback(isReady);
            }
            catch (const std::exception& e)
            {
                lg2::error("Exception in status callback for {UNIT}: {ERROR}",
                          "UNIT", unitPath,
                          "ERROR", e.what());
                callback(false);
            }
        };

        conn->async_method_call(
            methodCallback,
            "org.freedesktop.systemd1",
            unitPath,
            "org.freedesktop.DBus.Properties",
            "GetAll",
            "org.freedesktop.systemd1.Unit");
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to send status request for {UNIT}: {ERROR}",
                   "UNIT", unitPath,
                   "ERROR", e.what());
        callback(false);
    }
}

static inline void handleListUnitsResponse(
    sdbusplus::asio::object_server& server,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::system::error_code /*ec*/,
    const std::vector<ListUnitsType>& listUnits)
{
    // Loop through all units, and mark all units, which has to be
    // managed, irrespective of instance name.
    for (const auto& unit : listUnits)
    {
        // Ignore non-existent units
        if (std::get<static_cast<int>(ListUnitElements::loadState)>(unit) ==
            loadStateNotFound)
        {
            continue;
        }

        const auto& fullUnitName =
            std::get<static_cast<int>(ListUnitElements::name)>(unit);
        auto [unitName, type,
              instanceName] = getUnitNameTypeAndInstance(fullUnitName);
        if (managedServices.count(unitName))
        {
            // For socket-activated units, ignore all its instances
            if (managedServices.at(unitName) == true && !instanceName.empty())
            {
                continue;
            }

            std::string instantiatedUnitName =
                unitName + addInstanceName(instanceName, "_40");
            boost::replace_all(instantiatedUnitName, "-", "_2d");
            boost::replace_all(instantiatedUnitName, ".", "_2e");
            const sdbusplus::message::object_path& objectPath =
                std::get<static_cast<int>(ListUnitElements::objectPath)>(unit);
            // Group the service & socket units together.. Same services
            // are managed together.
            auto it = unitsToMonitor.find(instantiatedUnitName);
            if (it != unitsToMonitor.end())
            {
                auto& value = it->second;
                if (type == UnitType::service)
                {
                    std::get<static_cast<int>(monitorElement::serviceObjPath)>(
                        value) = objectPath.str;
                }
                else if (type == UnitType::socket)
                {
                    std::get<static_cast<int>(monitorElement::socketObjPath)>(
                        value) = objectPath.str;
                }
                continue;
            }
            // If not grouped with any existing entry, create a new one
            if (type == UnitType::service)
            {
                unitsToMonitor.emplace(instantiatedUnitName,
                                       std::make_tuple(unitName, instanceName,
                                                       objectPath.str, ""));
            }
            else if (type == UnitType::socket)
            {
                unitsToMonitor.emplace(instantiatedUnitName,
                                       std::make_tuple(unitName, instanceName,
                                                       "", objectPath.str));
            }
        }
    }

    bool updateRequired = false;

    // Determine if we need to create our persistent config dir
    if (!std::filesystem::exists(srvDataBaseDir))
    {
        std::filesystem::create_directories(srvDataBaseDir);
    }

    std::string srvCfgMgrFilePath = std::string(srvDataBaseDir) + srvCfgMgrFile;

    // First check if our config manager file is in the old spot.
    // If it is, then move it to the new spot
    if ((std::filesystem::exists(srvCfgMgrFileOld)) &&
        (!std::filesystem::exists(srvCfgMgrFilePath)))
    {
        lg2::info("Moving {OLDFILEPATH} to new location, {FILEPATH}",
                  "OLDFILEPATH", srvCfgMgrFileOld, "FILEPATH",
                  srvCfgMgrFilePath);
        // Note that the rename() function can run into issues when /etc
        // is an overlay on /var so use copy/remove instead
        std::filesystem::copy(srvCfgMgrFileOld, srvCfgMgrFilePath);
        std::filesystem::remove(srvCfgMgrFileOld);
    }

    bool jsonExist = std::filesystem::exists(srvCfgMgrFilePath);
    if (jsonExist)
    {
        try
        {
            std::ifstream file(srvCfgMgrFilePath);
            cereal::JSONInputArchive archive(file);
            MonitorListMap savedMonitorList;
            archive(savedMonitorList);

            // compare the unit list read from systemd1 and the save list.
            MonitorListMap diffMap;
            std::set_difference(begin(unitsToMonitor), end(unitsToMonitor),
                                begin(savedMonitorList), end(savedMonitorList),
                                std::inserter(diffMap, begin(diffMap)));
            for (auto& unitIt : diffMap)
            {
                auto it = savedMonitorList.find(unitIt.first);
                if (it == savedMonitorList.end())
                {
                    savedMonitorList.insert(unitIt);
                    updateRequired = true;
                }
            }
            unitsToMonitor = savedMonitorList;
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "Failed to load {FILEPATH} file, need to rewrite: {ERROR}.",
                "FILEPATH", srvCfgMgrFilePath, "ERROR", e);

            // The "bad" files need to be moved to /tmp/ so that we can try to
            // find out the cause of the file corruption. If we encounter this
            // failure multiple times, we will only overwrite it to ensure that
            // we don't accidentally fill up /tmp/.
            std::error_code ec;
            std::filesystem::copy_file(
                srvCfgMgrFilePath, tmpFileBad,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                lg2::error("Failed to copy {SRCFILE} file to {DSTFILE}.",
                           "SRCFILE", srvCfgMgrFilePath, "DSTFILE", tmpFileBad);
            }

            updateRequired = true;
        }
    }
    if (!jsonExist || updateRequired)
    {
        std::ofstream file(srvCfgMgrFilePath);
        cereal::JSONOutputArchive archive(file);
        archive(CEREAL_NVP(unitsToMonitor));
    }

#ifdef USB_CODE_UPDATE
    unitsToMonitor.emplace(
        "phosphor-usb-code-update",
        std::make_tuple(
            phosphor::service::usbCodeUpdateUnitName, "",
            "/org/freedesktop/systemd1/unit/usb_2dcode_2dupdate_2eservice",
            ""));
#endif

    // Use map with shared ownership
    auto unitStatusMap = std::make_shared<std::map<std::string, std::shared_ptr<UnitStatus>>>();
    auto pendingChecks = std::make_shared<std::atomic<int>>(0);

    // Initialize status map and count pending checks
    try
    {
        for (const auto& it : unitsToMonitor)
        {
            const auto& servicePath =
                std::get<static_cast<int>(monitorElement::serviceObjPath)>(
                    it.second);
            const auto& socketPath =
                std::get<static_cast<int>(monitorElement::socketObjPath)>(
                    it.second);

            auto status = std::make_shared<UnitStatus>();
            status->servicePath = servicePath;
            status->socketPath = socketPath;

            // Count how many async checks we need
            if (!servicePath.empty())
            {
                (*pendingChecks)++;
            }
            if (!socketPath.empty())
            {
                (*pendingChecks)++;
            }

            (*unitStatusMap)[it.first] = status;
        }
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to initialize status map: {ERROR}",
                   "ERROR", e.what());
        return;
    }

    // Function to check if unit is ready
    auto isUnitReady = [](const std::shared_ptr<UnitStatus>& status) {
        bool serviceOk = status->servicePath.empty() ||
                        (status->serviceChecked.load() && status->serviceReady);
        bool socketOk = status->socketPath.empty() ||
                       (status->socketChecked.load() && status->socketReady);
        return serviceOk && socketOk;
    };

    // Function to check if all units are ready and create objects
    auto tryCreateObjects = [&server, &conn, unitStatusMap, isUnitReady, listUnits]() {
        try
        {
            // Check if all units are ready
            bool allReady = true;
            for (const auto& [unit, status] : *unitStatusMap)
            {
                if (!isUnitReady(status))
                {
                    allReady = false;
                    lg2::info("Unit {UNIT} not ready yet - service checked: {SVCCHK}, ready: {SVCRDY}, socket checked: {SOCKCHK}, ready: {SOCKRDY}",
                             "UNIT", unit,
                             "SVCCHK", status->serviceChecked.load(),
                             "SVCRDY", status->serviceReady,
                             "SOCKCHK", status->socketChecked.load(),
                             "SOCKRDY", status->socketReady);
                    break;
                }
            }

            if (!allReady)
            {
                lg2::info("Not all units ready yet, will retry...");
                timer->expires_after(std::chrono::seconds(5));
                timer->async_wait([&server, &conn, listUnits](
                                    const boost::system::error_code& ec) {
                    if (!ec)
                    {
                        lg2::info("Retry timer expired, re-checking units...");
                        handleListUnitsResponse(server, conn, ec, listUnits);
                    }
                    else if (ec != boost::asio::error::operation_aborted)
                    {
                        lg2::error("Timer error: {ERROR}", "ERROR", ec.message());
                    }
                });
                return;
            }

            lg2::info("All units ready, creating D-Bus objects...");

            // All units ready, create objects
            for (auto& it : unitsToMonitor)
            {
                try
                {
                    sdbusplus::message::object_path basePath(
                        phosphor::service::srcCfgMgrBasePath);
                    std::string objPath(basePath / it.first);

                    auto srvCfgObj =
                        std::make_unique<phosphor::service::ServiceConfig>(
                            server, conn, objPath,
                            std::get<static_cast<int>(
                                monitorElement::unitName)>(it.second),
                            std::get<static_cast<int>(
                                monitorElement::instanceName)>(it.second),
                            std::get<static_cast<int>(
                                monitorElement::serviceObjPath)>(it.second),
                            std::get<static_cast<int>(
                                monitorElement::socketObjPath)>(it.second));
                    srvMgrObjects.emplace(
                        std::make_pair(std::move(objPath),
                                     std::move(srvCfgObj)));
                }
                catch (const std::exception& e)
                {
                    lg2::error(
                        "Failed to create object for unit {UNIT}: {ERROR}",
                        "UNIT", it.first,
                        "ERROR", e.what());
                }
            }

            lg2::info("Created {COUNT} service objects", "COUNT", srvMgrObjects.size());
        }
        catch (const std::exception& e)
        {
            lg2::error("Exception in tryCreateObjects: {ERROR}",
                       "ERROR", e.what());
        }
    };

    // Callback function for status checks
    auto statusCallback = [unitStatusMap, pendingChecks, tryCreateObjects](
        const std::string& unit, bool isService, bool ready) {
        try
        {
            auto it = unitStatusMap->find(unit);
            if (it != unitStatusMap->end())
            {
                auto& status = it->second;
                if (isService)
                {
                    status->serviceReady = ready;
                    status->serviceChecked.store(true);
                }
                else
                {
                    status->socketReady = ready;
                    status->socketChecked.store(true);
                }
            }

            int remaining = pendingChecks->fetch_sub(1) - 1;

            if (remaining == 0)
            {
                lg2::info("All status checks complete, attempting to create objects");
                tryCreateObjects();
            }
        }
        catch (const std::exception& e)
        {
            lg2::error("Exception in status callback: {ERROR}", "ERROR", e.what());
        }
    };

    // Check status for all units
    for (const auto& [unit, status] : *unitStatusMap)
    {
        if (!status->servicePath.empty())
        {
            checkUnitStatus(status->servicePath, conn,
                [unit, statusCallback](bool ready) {
                    statusCallback(unit, true, ready);
                });
        }
        else
        {
            // No service path, mark as checked and ready
            status->serviceChecked.store(true);
            status->serviceReady = true;
        }

        if (!status->socketPath.empty())
        {
            checkUnitStatus(status->socketPath, conn,
                [unit, statusCallback](bool ready) {
                    statusCallback(unit, false, ready);
                });
        }
        else
        {
            // No socket path, mark as checked and ready
            status->socketChecked.store(true);
            status->socketReady = true;
        }
    }

    // If no async checks are needed, create objects immediately
    if (pendingChecks->load() == 0)
    {
        lg2::info("No async checks needed, creating objects immediately");
        tryCreateObjects();
    }
}

void init(sdbusplus::asio::object_server& server,
          std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    // Go through all systemd units, and dynamically detect and manage
    // the service daemons
    conn->async_method_call(
        [&server, &conn](boost::system::error_code ec,
                         const std::vector<ListUnitsType>& listUnits) {
            if (ec)
            {
                lg2::error("async_method_call error: ListUnits failed: {EC}",
                           "EC", ec.value());
                return;
            }
            handleListUnitsResponse(server, conn, ec, listUnits);
        },
        sysdService, sysdObjPath, sysdMgrIntf, "ListUnits");
}

void checkAndInit(sdbusplus::asio::object_server& server,
                  std::shared_ptr<sdbusplus::asio::connection>& conn)
{
    if (!unitQueryStarted)
    {
        unitQueryStarted = true;
        init(server, conn);
    }
}

bool isServiceActive(const std::string& serviceName) {
    std::string cmd = "systemctl is-active --quiet " + serviceName;
    int result = std::system(cmd.c_str());
    // systemctl returns 0 if the service is active
    return result == 0;
}

bool isServiceEnabled(const std::string& serviceName) {
    return std::system(("systemctl is-enabled --quiet " + serviceName).c_str()) == 0;
}

bool checkBmcWebServicesActive() {

    std::vector<std::string> services = {
        "bmcweb.service",
        "bmcweb.socket"
    };

    const int maxRetries = 6;
    const int sleepSeconds = 30;

    if (!isServiceEnabled("bmcweb.socket")) {
            return true;
    }

    for (int i = 0; i < maxRetries; ++i) {
        bool allActive = true;
        for (const auto& service : services) {
            if (!isServiceActive(service)) {
                allActive = false;
                break;
            }
        }

        if (allActive) {
            return true;
        }

        if (i < maxRetries - 1) {
            std::cout << "Check " << (i + 1) << ": Not all services active, retrying in "
                      << sleepSeconds << " seconds..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(sleepSeconds));
        }
    }

    std::cout << "bmcweb services are not active after " << maxRetries << " retries. so, exiting service-config-manager" << std::endl;
    return false;
}


int main()
{

    if(!checkBmcWebServicesActive())
    {
        return 1;
    }

    updateGlobalDataFromFile();
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    timer = std::make_unique<boost::asio::steady_timer>(io);
    initTimer = std::make_unique<boost::asio::steady_timer>(io);
    conn->request_name(phosphor::service::serviceConfigSrvName);
    auto server = sdbusplus::asio::object_server(conn, true);
    server.add_manager(phosphor::service::srcCfgMgrBasePath);

    // Start initialization directly
    checkAndInit(server, conn);

    io.run();

    return 0;
}
