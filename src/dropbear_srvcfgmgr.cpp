#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <cstdlib>
#include "dropbear_srvcfgmgr.hpp"
#include "utils.hpp"

const std::string DROPBEAR_DROPIN_DIR = "/etc/systemd/system/dropbear.socket.d/";
const std::string DROPBEAR_DROPIN_FILE = DROPBEAR_DROPIN_DIR + "maxsessions.conf";
const std::string DROPBEAR_ENV = "/etc/default/dropbear";

bool updateDropbearTimeout(uint64_t timeout) {
    std::ifstream inputFile(DROPBEAR_ENV);
    std::stringstream buffer;

    if (inputFile) {
        buffer << inputFile.rdbuf();
        inputFile.close();
    } else {
        std::cerr << "Error: Cannot open the file " << DROPBEAR_ENV << std::endl;
        return false;
    }

    std::string fileContent = buffer.str();
    std::string searchKey = "DROPBEAR_IDLE_TIMEOUT";
    std::string newTimeout = "DROPBEAR_IDLE_TIMEOUT=\" -I " + std::to_string(timeout) + "\"\n";

    size_t pos = fileContent.find(searchKey);
    if (pos != std::string::npos) {
        size_t endPos = fileContent.find("\n", pos);
        fileContent.replace(pos, endPos - pos + 1, newTimeout);
    } else {
        fileContent.append(newTimeout);
    }

    std::ofstream outputFile(DROPBEAR_ENV);
    if (outputFile) {
        outputFile << fileContent;
        outputFile.close();
    } else {
        std::cerr << "Error: Cannot write to the file " << DROPBEAR_ENV << std::endl;
        return false;
    }

    return true;
}

void createDropinDirectory() {
    struct stat info;
    if (stat(DROPBEAR_DROPIN_DIR.c_str(), &info) != 0) {
        if (mkdir(DROPBEAR_DROPIN_DIR.c_str(), 0755) != 0) {
            std::cerr << "Failed to create drop-in directory: " << DROPBEAR_DROPIN_DIR << std::endl;
            exit(EXIT_FAILURE);
        } else {
            std::cout << "Drop-in directory created: " << DROPBEAR_DROPIN_DIR << std::endl;
        }
    }
}

void createOrUpdateDropinFile(int maxSessions) {
    createDropinDirectory();
    if(access(DROPBEAR_DROPIN_FILE.c_str(),F_OK) != 0)
    {
       std::ofstream fileOut(DROPBEAR_DROPIN_FILE);
        if (!fileOut.is_open()) {
            std::cerr << "Failed to open drop-in file for writing: " << DROPBEAR_DROPIN_FILE << std::endl;
            exit(EXIT_FAILURE);
        }

        fileOut << "[Socket]\n";
        fileOut << "MaxConnections=" << maxSessions << "\n";
        fileOut.close();

        try{
            auto bus = sdbusplus::bus::new_default();
            auto reloadUnit = bus.new_method_call(sysdService, sysdObjPath, sysdMgrIntf, sysdReloadMethod);
            bus.call(reloadUnit);
        }
        catch (const sdbusplus::exception::SdBusError &e)
        {
            std::cerr << "Failed to reload" << e.what() << std::endl;
        }
        std::cout << "Drop-in file updated with MaxConnections=" << maxSessions << " in " << DROPBEAR_DROPIN_FILE << std::endl;
    }
}
