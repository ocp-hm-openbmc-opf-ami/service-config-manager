#include "dropbear_srvcfgmgr.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

class DropbearTimeoutTest : public ::testing::Test
{
  protected:
    const std::filesystem::path envFile{
#ifdef TEST_ENABLED
        "/tmp/test-dropbear-default"
#else
        "/etc/default/dropbear"
#endif
    };

    void SetUp() override
    {
        std::error_code ec;
        std::filesystem::remove(envFile, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(envFile, ec);
    }
};

TEST_F(DropbearTimeoutTest, FileNotFound_ReturnsFalse)
{
    EXPECT_FALSE(updateDropbearTimeout(300));
}

#ifdef TEST_ENABLED

TEST_F(DropbearTimeoutTest, FileWithExistingKey_UpdatesTimeout_ReturnsTrue)
{
    {
        std::ofstream f(envFile);
        f << "SOME_VAR=\"value\"\n"
          << "DROPBEAR_IDLE_TIMEOUT=\" -I 600\"\n"
          << "OTHER_VAR=\"x\"\n";
    }
    EXPECT_TRUE(updateDropbearTimeout(300));

    std::ifstream f(envFile);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_NE(content.find("DROPBEAR_IDLE_TIMEOUT=\""), std::string::npos);
    EXPECT_NE(content.find(" -I 300"), std::string::npos);
    EXPECT_EQ(content.find(" -I 600"), std::string::npos);
}

TEST_F(DropbearTimeoutTest, FileWithoutKey_AppendsTimeout_ReturnsTrue)
{
    {
        std::ofstream f(envFile);
        f << "SOME_VAR=\"value\"\n";
    }
    EXPECT_TRUE(updateDropbearTimeout(120));

    std::ifstream f(envFile);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_NE(content.find("DROPBEAR_IDLE_TIMEOUT="), std::string::npos);
    EXPECT_NE(content.find(" -I 120"), std::string::npos);
}

TEST_F(DropbearTimeoutTest, EmptyFile_AppendsTimeout_ReturnsTrue)
{
    {
        std::ofstream f(envFile);
    }
    EXPECT_TRUE(updateDropbearTimeout(0));

    std::ifstream f(envFile);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_NE(content.find("DROPBEAR_IDLE_TIMEOUT="), std::string::npos);
    EXPECT_NE(content.find(" -I 0"), std::string::npos);
}

#endif
