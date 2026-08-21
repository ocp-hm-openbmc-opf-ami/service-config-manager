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

#include <boost/asio/detached.hpp>
#include <boost/asio/spawn.hpp>

#ifdef USB_CODE_UPDATE
#include <cereal/archives/json.hpp>
#include <cereal/types/tuple.hpp>
#include <cereal/types/unordered_map.hpp>

#include <cstdio>
#endif

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#ifdef PERSIST_SETTINGS
#include <nlohmann/json.hpp>
#endif
#include <regex>

extern std::unique_ptr<boost::asio::steady_timer> timer;
extern std::map<std::string, std::shared_ptr<phosphor::service::ServiceConfig>>
    srvMgrObjects;
#ifdef PERSIST_SETTINGS
extern bool useJsonDefaults;
#endif
static bool updateInProgress = false;

const std::string filename = "/etc/srvcfg-manager/srvcfg.json";
using srvcfgMap = std::map<std::string, std::pair<uint16_t, uint16_t>>;
using json = nlohmann::json;
// using namespace std;
json global_data;

void updateGlobalDataFromFile()
{
    std::ifstream file(filename);
    if (!file.is_open())
    {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return;
    }

    try
    {
        file >> global_data;
    }
    catch (json::parse_error& e)
    {
        std::cerr << "Parse error while reading JSON file: " << e.what()
                  << std::endl;
    }

    file.close();
}

void updateFileFromGlobalData()
{
    std::ofstream file(filename);
    if (!file.is_open())
    {
        std::cerr << "Failed to open file for writing: " << filename
                  << std::endl;
        return;
    }

    try
    {
        file << std::setw(4) << global_data << std::endl;
        if (debug)
        {
            std::cout << "JSON data successfully written to file: " << filename
                      << std::endl;
        }
    }
    catch (json::exception& e)
    {
        std::cerr << "Error while writing JSON data to file: " << e.what()
                  << std::endl;
    }

    file.close();
}

bool checkServicetoAddTimeOutandMaxSessProp(const std::string& service_name)
{
#ifdef PERSIST_SETTINGS
    if (useJsonDefaults)
    {
        for (const auto& service : global_data["services"])
        {
            if (service["name"] == service_name)
            {
                return true;
            }
        }
        return false;
    }
    std::string persistFile = std::string(srvDataBaseDir) + service_name;
    if (std::filesystem::exists(persistFile))
    {
        std::ifstream pfile(persistFile);
        if (pfile.good())
        {
            nlohmann::json stateMap =
                nlohmann::json::parse(pfile, nullptr, false, true);
            if (!stateMap.is_discarded() &&
                (stateMap.contains("SessionTimeOut") ||
                 stateMap.contains("MaxSession") ||
                 stateMap.contains("WebMaxSession")))
            {
                return true;
            }
        }
    }
    return false;
#else
    for (const auto& service : global_data["services"])
    {
        if (service["name"] == service_name)
        {
            return true;
        }
    }
    return false;
#endif
}

namespace phosphor
{
namespace service
{

static constexpr const char* overrideConfFileName = "override.conf";
static constexpr const size_t restartTimeout = 15; // seconds

static constexpr const char* systemdOverrideUnitBasePath =
    "/etc/systemd/system/";

#ifdef PERSIST_SETTINGS
static constexpr const char* persistDataFileVersionStr = "Version";
static constexpr const size_t persistDataFileVersion = 1;
#endif

#ifdef USB_CODE_UPDATE
static constexpr const char* usbCodeUpdateStateFilePath =
    "/var/lib/srvcfg_manager";
static constexpr const char* usbCodeUpdateStateFile =
    "/var/lib/srvcfg_manager/usb-code-update-state";
static constexpr const char* emptyUsbCodeUpdateRulesFile =
    "/etc/udev/rules.d/70-bmc-usb.rules";

using UsbCodeUpdateStateMap = std::unordered_map<std::string, bool>;

void ServiceConfig::setUSBCodeUpdateState(const bool& state)
{
    // Enable usb code update
    if (state)
    {
        if (std::filesystem::exists(emptyUsbCodeUpdateRulesFile))
        {
            lg2::info("Enable usb code update");
            std::filesystem::remove(emptyUsbCodeUpdateRulesFile);
        }
        return;
    }

    // Disable usb code update
    if (std::filesystem::exists(emptyUsbCodeUpdateRulesFile))
    {
        std::filesystem::remove(emptyUsbCodeUpdateRulesFile);
    }
    std::error_code ec;
    std::filesystem::create_symlink("/dev/null", emptyUsbCodeUpdateRulesFile,
                                    ec);
    if (ec)
    {
        lg2::error("Disable usb code update failed");
        return;
    }
    lg2::info("Disable usb code update");
}

void ServiceConfig::saveUSBCodeUpdateStateToFile(const bool& maskedState,
                                                 const bool& enabledState)
{
    if (!std::filesystem::exists(usbCodeUpdateStateFilePath))
    {
        std::filesystem::create_directories(usbCodeUpdateStateFilePath);
    }

    UsbCodeUpdateStateMap usbCodeUpdateState;
    usbCodeUpdateState[srvCfgPropMasked] = maskedState;
    usbCodeUpdateState[srvCfgPropEnabled] = enabledState;

    std::ofstream file(usbCodeUpdateStateFile, std::ios::out);
    cereal::JSONOutputArchive archive(file);
    archive(CEREAL_NVP(usbCodeUpdateState));
}

void ServiceConfig::getUSBCodeUpdateStateFromFile()
{
    if (!std::filesystem::exists(usbCodeUpdateStateFile))
    {
        lg2::info("usb-code-update-state file does not exist");

        unitMaskedState = false;
        unitEnabledState = true;
        unitRunningState = true;
        setUSBCodeUpdateState(unitEnabledState);
        return;
    }

    std::ifstream file(usbCodeUpdateStateFile);
    cereal::JSONInputArchive archive(file);
    UsbCodeUpdateStateMap usbCodeUpdateState;
    archive(usbCodeUpdateState);

    auto iterMask = usbCodeUpdateState.find(srvCfgPropMasked);
    if (iterMask != usbCodeUpdateState.end())
    {
        unitMaskedState = iterMask->second;
        if (unitMaskedState)
        {
            unitEnabledState = !unitMaskedState;
            unitRunningState = !unitMaskedState;
            setUSBCodeUpdateState(unitEnabledState);
            return;
        }

        auto iterEnable = usbCodeUpdateState.find(srvCfgPropEnabled);
        if (iterEnable != usbCodeUpdateState.end())
        {
            unitEnabledState = iterEnable->second;
            unitRunningState = iterEnable->second;
            setUSBCodeUpdateState(unitEnabledState);
        }
    }
}
#endif

void ServiceConfig::updateSocketProperties(
    const boost::container::flat_map<std::string, VariantType>& propertyMap)
{
    auto listenIt = propertyMap.find("Listen");
    if (listenIt != propertyMap.end())
    {
        auto listenVal =
            std::get<std::vector<std::tuple<std::string, std::string>>>(
                listenIt->second);
        if (listenVal.size())
        {
            protocol = std::get<0>(listenVal[0]);
            std::string port = std::get<1>(listenVal[0]);
            auto tmp = std::stoul(port.substr(port.find_last_of(":") + 1),
                                  nullptr, 10);
            if (tmp > std::numeric_limits<uint16_t>::max())
            {
                throw std::out_of_range("Out of range");
            }
            portNum = tmp;
            if (sockAttrIface && sockAttrIface->is_initialized())
            {
                internalSet = true;
                sockAttrIface->set_property(sockAttrPropPort, portNum);
                internalSet = false;
            }
        }
    }
}

void ServiceConfig::updateServiceProperties(
    const boost::container::flat_map<std::string, VariantType>& propertyMap)
{
    auto stateIt = propertyMap.find("UnitFileState");
    if (stateIt != propertyMap.end())
    {
        stateValue = std::get<std::string>(stateIt->second);
        unitEnabledState = unitMaskedState = false;
        if (stateValue == stateMasked)
        {
            unitMaskedState = true;
        }
        else if (stateValue == stateEnabled)
        {
            unitEnabledState = true;
        }
        if (srvCfgIface && srvCfgIface->is_initialized())
        {
            internalSet = true;
            srvCfgIface->set_property(srvCfgPropMasked, unitMaskedState);
            srvCfgIface->set_property(srvCfgPropEnabled, unitEnabledState);
            internalSet = false;
        }
    }
    auto subStateIt = propertyMap.find("SubState");
    if (subStateIt != propertyMap.end())
    {
        subStateValue = std::get<std::string>(subStateIt->second);
        if (subStateValue == subStateRunning ||
            subStateValue == subStateListening)
        {
            unitRunningState = true;
        }
        if (srvCfgIface && srvCfgIface->is_initialized())
        {
            internalSet = true;
            srvCfgIface->set_property(srvCfgPropRunning, unitRunningState);
            internalSet = false;
        }
    }

#ifdef USB_CODE_UPDATE
    if (baseUnitName == usbCodeUpdateUnitName)
    {
        getUSBCodeUpdateStateFromFile();
    }
#endif
}

void ServiceConfig::queryAndUpdateProperties(bool isRestore = false)
{
    std::string objectPath =
        isSocketActivatedService ? socketObjectPath : serviceObjectPath;
    if (objectPath.empty())
    {
        return;
    }

    conn->async_method_call(
        [this,
         isRestore](boost::system::error_code ec,
                    const boost::container::flat_map<std::string, VariantType>&
                        propertyMap) {
            if (ec)
            {
                lg2::error(
                    "async_method_call error: Failed to service unit properties: {EC}",
                    "EC", ec.value());
                return;
            }
            try
            {
                updateServiceProperties(propertyMap);
                if (!socketObjectPath.empty())
                {
                    conn->async_method_call(
                        [this](boost::system::error_code ec,
                               const boost::container::flat_map<
                                   std::string, VariantType>& propertyMap) {
                            if (ec)
                            {
                                lg2::error(
                                    "async_method_call error: Failed to get all property: {EC}",
                                    "EC", ec.value());
                                return;
                            }
                            try
                            {
                                updateSocketProperties(propertyMap);
                                if (!srvCfgIface)
                                {
                                    registerProperties();
                                }
                            }
                            catch (const std::exception& e)
                            {
                                lg2::error(
                                    "Exception in getting socket properties: {ERROR}",
                                    "ERROR", e);
                                return;
                            }
                        },
                        sysdService, socketObjectPath, dBusPropIntf,
                        dBusGetAllMethod, sysdSocketIntf);
                }
                else if (!srvCfgIface)
                {
                    registerProperties();
                }
                if (!isRestore)
                {
                    // This is just an update once we're already running so
                    // write the values out to our persistent settings
                    writeStateFile();
                }
                // On startup, loadStateFile() is called from
                // registerProperties() to ensure hasExtendedProps
                // is set before any persistent file read/write.
            }
            catch (const std::exception& e)
            {
                lg2::error("Exception in getting socket properties: {ERROR}",
                           "ERROR", e);
                return;
            }
        },
        sysdService, objectPath, dBusPropIntf, dBusGetAllMethod, sysdUnitIntf);
    return;
}

void ServiceConfig::createSocketOverrideConf()
{
    if (!socketObjectPath.empty())
    {
        std::string socketUnitName(instantiatedUnitName + ".socket");
        /// Check override socket directory exist, if not create it.
        std::filesystem::path ovrUnitFileDir(systemdOverrideUnitBasePath);
        ovrUnitFileDir += socketUnitName;
        ovrUnitFileDir += ".d";
        if (!std::filesystem::exists(ovrUnitFileDir))
        {
            if (!std::filesystem::create_directories(ovrUnitFileDir))
            {
                lg2::error("Unable to create the {DIR} directory.", "DIR",
                           ovrUnitFileDir);
                phosphor::logging::elog<sdbusplus::xyz::openbmc_project::
                                            Common::Error::InternalFailure>();
            }
        }
        overrideConfDir = std::string(ovrUnitFileDir);
    }
}

void ServiceConfig::writeStateFile()
{
#ifdef PERSIST_SETTINGS
    lg2::debug("Writing Persistent State File Information to {STATE_FILE}",
               "STATE_FILE", stateFile);
    nlohmann::json stateMap;
    stateMap[persistDataFileVersionStr] = persistDataFileVersion;
    stateMap[srvCfgPropMasked] = unitMaskedState;
    stateMap[srvCfgPropEnabled] = unitEnabledState;
    stateMap[srvCfgPropRunning] = unitRunningState;

    // write timeout and session limit properties to the state file
    //  so that JSON defaults are not needed on subsequent boots
    if (hasExtendedProps)
    {
        if (hasTimeoutProp)
        {
            stateMap[srvCfgPropTimeOut] = timeOut;
        }
        if (instantiatedUnitName == "bmcweb")
        {
            stateMap["WebMaxSession"] = webMaxSess;
            stateMap["RedfishMaxSession"] = redfishMaxSess;
        }
        else
        {
            stateMap[srvCfgPropMaxSess] = maxSess;
        }
    }

    std::ofstream file(stateFile);
    file << stateMap;
    file.close();
#endif
}

void ServiceConfig::loadStateFile()
{
#ifdef PERSIST_SETTINGS
    lg2::debug("Loading Persistent State File Information from {STATE_FILE}",
               "STATE_FILE", stateFile);
    if (std::filesystem::exists(stateFile))
    {
        std::ifstream file(stateFile);
        if (!file.good())
        {
            lg2::error("Error reading {FILEPATH}; delete it and continue",
                       "FILEPATH", stateFile);
            std::filesystem::remove(stateFile);
            // rewrite file with what was ready from systemd
            writeStateFile();
            return;
        }

        nlohmann::json stateMap =
            nlohmann::json::parse(file, nullptr, false, true);
        if (stateMap.is_discarded())
        {
            lg2::error("Error loading {FILEPATH}; delete it and continue",
                       "FILEPATH", stateFile);
            std::filesystem::remove(stateFile);
            // rewrite file with what was ready from systemd
            writeStateFile();
            return;
        }
        else if (stateMap[persistDataFileVersionStr] != persistDataFileVersion)
        {
            lg2::error(
                "Error version:{VERSION} read from {FILEPATH} does not match expected {VERSION_EXP}; delete it and continue",
                "VERSION", stateMap[persistDataFileVersionStr], "FILEPATH",
                stateFile, "VERSION_EXP", persistDataFileVersion);
            std::filesystem::remove(stateFile);
            // rewrite file with what was ready from systemd
            writeStateFile();
            return;
        }

        // If there are any differences, the persistent config file wins so
        // update the dbus properties and trigger a reload to apply the changes
        if (stateMap[srvCfgPropMasked] != unitMaskedState)
        {
            lg2::info(
                "Masked property for {FILEPATH} not equal. Setting to {SETTING}",
                "FILEPATH", stateFile, "SETTING", stateMap[srvCfgPropMasked]);
            unitMaskedState = stateMap[srvCfgPropMasked];
            updatedFlag |=
                (1 << static_cast<uint8_t>(UpdatedProp::maskedState));
            updatedFlag |=
                (1 << static_cast<uint8_t>(UpdatedProp::enabledState));
            startServiceRestartTimer();
        }
        if (stateMap.contains(srvCfgPropEnabled) &&
            stateMap[srvCfgPropEnabled] != unitEnabledState)
        {
            lg2::info(
                "Enabled property for {FILEPATH} not equal. Setting to {SETTING}",
                "FILEPATH", stateFile, "SETTING", stateMap[srvCfgPropEnabled]);
            unitEnabledState = stateMap[srvCfgPropEnabled];
            updatedFlag |=
                (1 << static_cast<uint8_t>(UpdatedProp::enabledState));
            unitRunningState = stateMap[srvCfgPropEnabled];
            updatedFlag |=
                (1 << static_cast<uint8_t>(UpdatedProp::runningState));
            startServiceRestartTimer();
        }
    }
    else
    {
        // Just write out what we got from systemd if no existing config file
        writeStateFile();
    }
#endif
}

void ServiceConfig::reloadServiceConfig()
{
    queryAndUpdateProperties(true);
}

ServiceConfig::ServiceConfig(
    sdbusplus::asio::object_server& srv_,
    std::shared_ptr<sdbusplus::asio::connection>& conn_,
    const std::string& objPath_, const std::string& baseUnitName_,
    const std::string& instanceName_, const std::string& serviceObjPath_,
    const std::string& socketObjPath_) :
    conn(conn_), server(srv_), objPath(objPath_), baseUnitName(baseUnitName_),
    instanceName(instanceName_), serviceObjectPath(serviceObjPath_),
    socketObjectPath(socketObjPath_)
{
    isSocketActivatedService = serviceObjectPath.empty();
    instantiatedUnitName = baseUnitName + addInstanceName(instanceName, "@");
    updatedFlag = 0;
    stateFile = srvDataBaseDir + instantiatedUnitName;
    queryAndUpdateProperties(true);
    return;
}

std::string ServiceConfig::getSocketUnitName()
{
    return instantiatedUnitName + ".socket";
}

std::string ServiceConfig::getServiceUnitName()
{
    return instantiatedUnitName + ".service";
}

bool ServiceConfig::isMaskedOut()
{
    // return true  if state is masked & no request to update the maskedState
    return (
        stateValue == "masked" &&
        !(updatedFlag & (1 << static_cast<uint8_t>(UpdatedProp::maskedState))));
}

void ServiceConfig::stopAndApplyUnitConfig(boost::asio::yield_context yield)
{
    if (!updatedFlag || isMaskedOut())
    {
        // No updates / masked - Just return.
        return;
    }
    lg2::info("Applying new settings: {OBJPATH}", "OBJPATH", objPath);
    // Enabled-only changes should not stop the service
    bool needsStop =
        (updatedFlag & ((1 << static_cast<uint8_t>(UpdatedProp::maskedState)) |
                        (1 << static_cast<uint8_t>(UpdatedProp::runningState)) |
                        (1 << static_cast<uint8_t>(UpdatedProp::port)))) != 0;
    if (needsStop && (subStateValue == subStateRunning ||
                      subStateValue == subStateListening))
    {
        if (!socketObjectPath.empty())
        {
            systemdUnitAction(conn, yield, getSocketUnitName(), sysdStopUnit);
        }
        if (!isSocketActivatedService)
        {
            systemdUnitAction(conn, yield, getServiceUnitName(), sysdStopUnit);
        }
        else
        {
            // For socket-activated service, each connection will spawn a
            // service instance from template. Need to find all spawned service
            // `<unitName>@<attribute>.service` and stop them through the
            // systemdUnitAction method
            boost::system::error_code ec;
            auto listUnits =
                conn->yield_method_call<std::vector<ListUnitsType>>(
                    yield, ec, sysdService, sysdObjPath, sysdMgrIntf,
                    "ListUnits");

            checkAndThrowInternalFailure(
                ec, "yield_method_call error: ListUnits failed");

            for (const auto& unit : listUnits)
            {
                const auto& service =
                    std::get<static_cast<int>(ListUnitElements::name)>(unit);
                const auto& status =
                    std::get<static_cast<int>(ListUnitElements::subState)>(
                        unit);
                if (service.find(baseUnitName + "@") != std::string::npos &&
                    service.find(".service") != std::string::npos &&
                    status == subStateRunning)
                {
                    systemdUnitAction(conn, yield, service, sysdStopUnit);
                }
            }
        }
    }

    if (updatedFlag & (1 << static_cast<uint8_t>(UpdatedProp::port)))
    {
        createSocketOverrideConf();
        // Create override config file and write data.
        std::string ovrCfgFile{overrideConfDir + "/" + overrideConfFileName};
        std::string tmpFile{ovrCfgFile + "_tmp"};
        std::ofstream cfgFile(tmpFile, std::ios::out);
        if (!cfgFile.good())
        {
            lg2::error("Failed to open the {TMPFILE} file.", "TMPFILE",
                       tmpFile);
            phosphor::logging::elog<sdbusplus::xyz::openbmc_project::Common::
                                        Error::InternalFailure>();
        }

        // Write the socket header
        cfgFile << "[Socket]\n";
        // Listen
        cfgFile << "Listen" << protocol << "="
                << "\n";
        cfgFile << "Listen" << protocol << "=" << portNum << "\n";
        cfgFile.close();

        if (std::rename(tmpFile.c_str(), ovrCfgFile.c_str()) != 0)
        {
            lg2::error("Failed to rename {TMPFILE} file as {OVERCFGFILE} file.",
                       "TMPFILE", tmpFile, "OVERCFGFILE", ovrCfgFile);
            std::remove(tmpFile.c_str());
            phosphor::logging::elog<sdbusplus::xyz::openbmc_project::Common::
                                        Error::InternalFailure>();
        }
    }

    if (updatedFlag & ((1 << static_cast<uint8_t>(UpdatedProp::maskedState)) |
                       (1 << static_cast<uint8_t>(UpdatedProp::enabledState))))
    {
        std::vector<std::string> unitFiles;
        if (socketObjectPath.empty())
        {
            unitFiles = {getServiceUnitName()};
        }
        else if (serviceObjectPath.empty())
        {
            unitFiles = {getSocketUnitName()};
        }
        else
        {
            unitFiles = {getSocketUnitName(), getServiceUnitName()};
        }
        systemdUnitFilesStateChange(conn, yield, unitFiles, stateValue,
                                    unitMaskedState, unitEnabledState);
    }
    return;
}
void ServiceConfig::restartUnitConfig(boost::asio::yield_context yield)
{
    if (!updatedFlag || isMaskedOut())
    {
        // No updates. Just return.
        return;
    }

    // Skip restart for Enabled-only changes
    bool onlyEnabledChanged =
        (updatedFlag == (1 << static_cast<uint8_t>(UpdatedProp::enabledState)));
    if (unitRunningState && !onlyEnabledChanged)
    {
        if (!socketObjectPath.empty())
        {
            systemdUnitAction(conn, yield, getSocketUnitName(),
                              sysdRestartUnit);
        }
        if (!serviceObjectPath.empty())
        {
            systemdUnitAction(conn, yield, getServiceUnitName(),
                              sysdRestartUnit);
        }
    }

    // Reset the flag
    updatedFlag = 0;

    lg2::info("Applied new settings: {OBJPATH} {UNIT_RUNNING_STATE}", "OBJPATH",
              objPath, "UNIT_RUNNING_STATE", unitRunningState);

    queryAndUpdateProperties();
    return;
}

void ServiceConfig::startServiceRestartTimer()
{
    // Ensure our persistent files are updated with changes
    writeStateFile();
    timer->expires_after(std::chrono::seconds(restartTimeout));
    timer->async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            // Timer reset.
            return;
        }
        else if (ec)
        {
            lg2::error("async wait error: {EC}", "EC", ec.value());
            return;
        }
        updateInProgress = true;
        boost::asio::spawn(
            conn->get_io_context(),
            [this](boost::asio::yield_context yield) {
                // Stop and apply configuration for all objects
                for (auto& srvMgrObj : srvMgrObjects)
                {
                    auto& srvObj = srvMgrObj.second;
                    if (srvObj->updatedFlag)
                    {
                        srvObj->stopAndApplyUnitConfig(yield);
                    }
                }
                // Do system reload
                systemdDaemonReload(conn, yield);
                // restart unit config.
                for (auto& srvMgrObj : srvMgrObjects)
                {
                    auto& srvObj = srvMgrObj.second;
                    if (srvObj->updatedFlag)
                    {
                        srvObj->restartUnitConfig(yield);
                    }
                }
                updateInProgress = false;
            },
            boost::asio::detached);
    });
}

void ServiceConfig::registerProperties()
{
    srvCfgIface = server.add_interface(objPath, serviceConfigIntfName);
    bool TimoutPropStatus = false;
    bool MaxSessPropStatus = false;
    bool EnabledPropStatus = false;
    bool EnabledStatus = false;

    if (checkServicetoAddTimeOutandMaxSessProp(instantiatedUnitName))
    {
        hasExtendedProps = true;

#ifdef PERSIST_SETTINGS
        if (useJsonDefaults)
        {
            for (auto& service : global_data["services"])
            {
                if (service["name"] == instantiatedUnitName)
                {
                    TimoutPropStatus = service.contains("timeout");
                    hasTimeoutProp = TimoutPropStatus;
                    EnabledPropStatus = service.contains("Enabled");

                    if (instantiatedUnitName == "bmcweb")
                    {
                        MaxSessPropStatus =
                            service.contains("web_max_session_limit") &&
                            service.contains("redfish_max_session_limit");
                    }
                    else
                    {
                        MaxSessPropStatus =
                            service.contains("max_session_limit");
                    }

                    if (MaxSessPropStatus)
                    {
                        if (instantiatedUnitName == "bmcweb")
                        {
                            webMaxSess = service["web_max_session_limit"];
                            redfishMaxSess =
                                service["redfish_max_session_limit"];
                        }
                        else
                        {
                            maxSess = service["max_session_limit"];
                        }
                    }
                    if (TimoutPropStatus)
                    {
                        uint64_t timeout_tmp = service["timeout"];
                        if (timeout_tmp < 30 || timeout_tmp > 86400)
                        {
                            timeOut = 600;
                        }
                        else
                        {
                            timeOut = timeout_tmp;
                        }
                    }
                    if (EnabledPropStatus)
                    {
                        EnabledStatus = service["Enabled"];
                    }

                    break;
                }
            }
        }
        else
        {
            std::ifstream pfile(stateFile);
            if (pfile.good())
            {
                nlohmann::json stateMap =
                    nlohmann::json::parse(pfile, nullptr, false, true);
                if (!stateMap.is_discarded())
                {
                    TimoutPropStatus = stateMap.contains(srvCfgPropTimeOut);
                    hasTimeoutProp = TimoutPropStatus;
                    EnabledPropStatus = stateMap.contains(srvCfgPropEnabled);

                    if (instantiatedUnitName == "bmcweb")
                    {
                        MaxSessPropStatus =
                            stateMap.contains("WebMaxSession") &&
                            stateMap.contains("RedfishMaxSession");
                    }
                    else
                    {
                        MaxSessPropStatus =
                            stateMap.contains(srvCfgPropMaxSess);
                    }

                    if (MaxSessPropStatus)
                    {
                        if (instantiatedUnitName == "bmcweb")
                        {
                            webMaxSess = stateMap["WebMaxSession"];
                            redfishMaxSess = stateMap["RedfishMaxSession"];
                        }
                        else
                        {
                            maxSess = stateMap[srvCfgPropMaxSess];
                        }
                    }
                    if (TimoutPropStatus)
                    {
                        uint64_t timeout_tmp = stateMap[srvCfgPropTimeOut];
                        if (timeout_tmp < 30 || timeout_tmp > 86400)
                        {
                            timeOut = 600;
                        }
                        else
                        {
                            timeOut = timeout_tmp;
                        }
                    }
                    if (EnabledPropStatus)
                    {
                        EnabledStatus = stateMap[srvCfgPropEnabled];
                    }
                }
            }
        }
#else
        for (auto& service : global_data["services"])
        {
            if (service["name"] == instantiatedUnitName)
            {
                TimoutPropStatus = service.contains("timeout");
                EnabledPropStatus = service.contains("Enabled");

                if (instantiatedUnitName == "bmcweb")
                {
                    MaxSessPropStatus =
                        service.contains("web_max_session_limit") &&
                        service.contains("redfish_max_session_limit");
                }
                else
                {
                    MaxSessPropStatus = service.contains("max_session_limit");
                }

                if (MaxSessPropStatus)
                {
                    if (instantiatedUnitName == "bmcweb")
                    {
                        webMaxSess = service["web_max_session_limit"];
                        redfishMaxSess = service["redfish_max_session_limit"];
                    }
                    else
                    {
                        maxSess = service["max_session_limit"];
                    }
                }
                if (TimoutPropStatus)
                {
                    uint64_t timeout_tmp = service["timeout"];
                    if (timeout_tmp < 30 || timeout_tmp > 86400)
                    {
                        timeOut = 600;
                    }
                    else
                    {
                        timeOut = timeout_tmp;
                    }
                }
                if (EnabledPropStatus)
                {
                    EnabledStatus = service["Enabled"];
                }

                break;
            }
        }
#endif
    }

    if (!socketObjectPath.empty())
    {
        sockAttrIface = server.add_interface(objPath, sockAttrIntfName);
        sockAttrIface->register_property(
            sockAttrPropPort, portNum,
            [this](const uint16_t& req, uint16_t& res) {
                if (!internalSet)
                {
                    if (req == res)
                    {
                        return 1;
                    }
                    if (updateInProgress)
                    {
                        return 0;
                    }
                    portNum = req;
                    updatedFlag |=
                        (1 << static_cast<uint8_t>(UpdatedProp::port));
                    startServiceRestartTimer();
                }
                res = req;
                return 1;
            });
    }
    if (TimoutPropStatus)
    {
        srvCfgIface->register_property(
            srvCfgPropTimeOut, timeOut,
            [this](const uint64_t& req, uint64_t& res) {
                if (!internalSet)
                {
                    if (req < 30 || req > 86400)
                    {
                        std::cout << "inavlid data :" << req << std::endl;
                        return 0;
                    }

                    if (req == res)
                    {
                        return 1;
                    }
                    if (updateInProgress)
                    {
                        return 0;
                    }
#ifdef PERSIST_SETTINGS
                    timeOut = req;
#else
                    for (auto& service : global_data["services"])
                    {
                        if (service["name"] == instantiatedUnitName)
                        {
                            service["timeout"] = timeOut = req;
                            break;
                        }
                    }
#endif
                    if (instantiatedUnitName == "dropbear")
                    {
                        if (!updateDropbearTimeout(timeOut))
                        {
                            std::cout << "Fail to update the Timeout value"
                                      << std::endl;
                            return 0;
                        }
                    }
#ifndef PERSIST_SETTINGS
                    updateFileFromGlobalData();
#endif
                    startServiceRestartTimer();
                }
                res = req;
                return 1;
            });
    }

    if (MaxSessPropStatus)
    {
        if (instantiatedUnitName == "bmcweb")
        {
            srvCfgIface->register_property(
                "WebMaxSession", webMaxSess,
                sdbusplus::asio::PropertyPermission::readOnly);

            srvCfgIface->register_property(
                "RedfishMaxSession", redfishMaxSess,
                sdbusplus::asio::PropertyPermission::readOnly);
        }
        else
        {
            if (instantiatedUnitName == "dropbear")
            {
                createOrUpdateDropinFile(maxSess);
            }

            srvCfgIface->register_property(
                srvCfgPropMaxSess, maxSess,
                sdbusplus::asio::PropertyPermission::readOnly);
        }
    }

    srvCfgIface->register_property(
        srvCfgPropMasked, unitMaskedState, [this](const bool& req, bool& res) {
            if (!internalSet)
            {
#ifdef USB_CODE_UPDATE
                if (baseUnitName == usbCodeUpdateUnitName)
                {
                    unitMaskedState = req;
                    unitEnabledState = !unitMaskedState;
                    unitRunningState = !unitMaskedState;
                    internalSet = true;
                    srvCfgIface->set_property(srvCfgPropEnabled,
                                              unitEnabledState);
                    srvCfgIface->set_property(srvCfgPropRunning,
                                              unitRunningState);
                    srvCfgIface->set_property(srvCfgPropMasked,
                                              unitMaskedState);
                    internalSet = false;
                    setUSBCodeUpdateState(unitEnabledState);
                    saveUSBCodeUpdateStateToFile(unitMaskedState,
                                                 unitEnabledState);
                    return 1;
                }
#endif
                if (req == res)
                {
                    return 1;
                }
                if (updateInProgress)
                {
                    return 0;
                }
                unitMaskedState = req;
                unitEnabledState = !unitMaskedState;
                unitRunningState = !unitMaskedState;
                updatedFlag |=
                    (1 << static_cast<uint8_t>(UpdatedProp::maskedState)) |
                    (1 << static_cast<uint8_t>(UpdatedProp::enabledState)) |
                    (1 << static_cast<uint8_t>(UpdatedProp::runningState));
                internalSet = true;
                srvCfgIface->set_property(srvCfgPropEnabled, unitEnabledState);
                srvCfgIface->set_property(srvCfgPropRunning, unitRunningState);
                internalSet = false;
#ifndef PERSIST_SETTINGS
                if (checkServicetoAddTimeOutandMaxSessProp(
                        instantiatedUnitName))
                {
                    for (auto& service : global_data["services"])
                    {
                        if (service["name"] == instantiatedUnitName)
                        {
                            service["Enabled"] = unitEnabledState;
                            break;
                        }
                    }
                    updateFileFromGlobalData();
                }
#endif
                startServiceRestartTimer();
            }
            res = req;
            return 1;
        });

    srvCfgIface->register_property(
        srvCfgPropEnabled, unitEnabledState,
        [this](const bool& req, bool& res) {
            if (!internalSet)
            {
#ifdef USB_CODE_UPDATE
                if (baseUnitName == usbCodeUpdateUnitName)
                {
                    if (unitMaskedState)
                    { // block updating if masked
                        lg2::error("Invalid value specified");
                        return -EINVAL;
                    }
                    unitEnabledState = req;
                    unitRunningState = req;
                    internalSet = true;
                    srvCfgIface->set_property(srvCfgPropEnabled,
                                              unitEnabledState);
                    srvCfgIface->set_property(srvCfgPropRunning,
                                              unitRunningState);
                    internalSet = false;
                    setUSBCodeUpdateState(unitEnabledState);
                    saveUSBCodeUpdateStateToFile(unitMaskedState,
                                                 unitEnabledState);
                    res = req;
                    return 1;
                }
#endif
                if (req == res)
                {
                    return 1;
                }
                if (updateInProgress)
                {
                    return 0;
                }
                if (unitMaskedState)
                { // block updating if masked
                    lg2::error("Invalid value specified");
                    return -EINVAL;
                }
#ifdef PERSIST_SETTINGS
                unitEnabledState = req;
#else
                if (checkServicetoAddTimeOutandMaxSessProp(
                        instantiatedUnitName))
                {
                    for (auto& service : global_data["services"])
                    {
                        if (service["name"] == instantiatedUnitName)
                        {
                            service["Enabled"] = unitEnabledState = req;
                            break;
                        }
                    }
                    updateFileFromGlobalData();
                }
                else
                {
                    unitEnabledState = req;
                }
#endif

                updatedFlag |=
                    (1 << static_cast<uint8_t>(UpdatedProp::enabledState));
                startServiceRestartTimer();
            }
            res = req;
            return 1;
        });

    srvCfgIface->register_property(
        srvCfgPropRunning, unitRunningState,
        [this](const bool& req, bool& res) {
            if (!internalSet)
            {
#ifdef USB_CODE_UPDATE
                if (baseUnitName == usbCodeUpdateUnitName)
                {
                    if (unitMaskedState)
                    { // block updating if masked
                        lg2::error("Invalid value specified");
                        return -EINVAL;
                    }
                    unitEnabledState = req;
                    unitRunningState = req;
                    internalSet = true;
                    srvCfgIface->set_property(srvCfgPropEnabled,
                                              unitEnabledState);
                    srvCfgIface->set_property(srvCfgPropRunning,
                                              unitRunningState);
                    internalSet = false;
                    setUSBCodeUpdateState(unitEnabledState);
                    saveUSBCodeUpdateStateToFile(unitMaskedState,
                                                 unitEnabledState);
                    res = req;
                    return 1;
                }
#endif
                if (req == res)
                {
                    return 1;
                }
                if (updateInProgress)
                {
                    return 0;
                }
                if (unitMaskedState)
                { // block updating if masked
                    lg2::error("Invalid value specified");
                    return -EINVAL;
                }
                unitRunningState = req;
                updatedFlag |=
                    (1 << static_cast<uint8_t>(UpdatedProp::runningState));
                startServiceRestartTimer();
            }
            res = req;
            return 1;
        });

    srvCfgIface->initialize();
    if (checkServicetoAddTimeOutandMaxSessProp(instantiatedUnitName))
    {
        internalSet = true;
        if (TimoutPropStatus)
        {
            if (instantiatedUnitName == "dropbear")
            {
                if (!updateDropbearTimeout(timeOut))
                {
                    std::cout << "Fail to update the Timeout for "
                              << instantiatedUnitName << std::endl;
                }
            }
            srvCfgIface->set_property(srvCfgPropTimeOut, timeOut);
        }

        // Only apply Enabled override from defaults on fresh image.
        // On subsequent boots, loadStateFile() handles persistent state.
#ifdef PERSIST_SETTINGS
        if (useJsonDefaults && EnabledPropStatus && (!unitMaskedState))
#else
        if (EnabledPropStatus && (!unitMaskedState))
#endif
        {
            if (unitEnabledState != EnabledStatus)
            {
                unitEnabledState = unitRunningState = EnabledStatus;
                srvCfgIface->set_property(srvCfgPropEnabled, unitEnabledState);
                srvCfgIface->set_property(srvCfgPropRunning, unitRunningState);
                updatedFlag |=
                    (1 << static_cast<uint8_t>(UpdatedProp::enabledState));
                updatedFlag |=
                    (1 << static_cast<uint8_t>(UpdatedProp::runningState));
                startServiceRestartTimer();
            }
            if (unitEnabledState != unitRunningState)
            {
                unitRunningState = EnabledStatus;
                srvCfgIface->set_property(srvCfgPropRunning, unitRunningState);
                updatedFlag |=
                    (1 << static_cast<uint8_t>(UpdatedProp::runningState));
                startServiceRestartTimer();
            }
        }
        internalSet = false;
    }

#ifdef PERSIST_SETTINGS
    if (useJsonDefaults)
    {
        writeStateFile();
    }
    else
    {
        loadStateFile();
        // Fix for bmcweb socket dead but running state is true issue after
        // reboot.
        if (unitRunningState && !unitEnabledState &&
            !socketObjectPath.empty() && !serviceObjectPath.empty() &&
            !(updatedFlag &
              (1 << static_cast<uint8_t>(UpdatedProp::runningState))))
        {
            lg2::info(
                "Disabled service {OBJPATH} has Running=true but socket may be dead; forcing restart",
                "OBJPATH", objPath);
            updatedFlag |=
                (1 << static_cast<uint8_t>(UpdatedProp::runningState));
            startServiceRestartTimer();
        }
    }
#endif

    if (!socketObjectPath.empty())
    {
        sockAttrIface->initialize();
    }
    return;
}

} // namespace service
} // namespace phosphor
