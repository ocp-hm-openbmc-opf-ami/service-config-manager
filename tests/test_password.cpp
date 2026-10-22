#include "dropbear_srvcfgmgr.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#ifdef TEST_ENABLED

class DropinFileTest : public ::testing::Test
{
  protected:
    const std::filesystem::path dropinDir{"/tmp/test-dropbear-dropin/"};
    const std::filesystem::path dropinFile{
        "/tmp/test-dropbear-dropin/maxsessions.conf"};

    void SetUp() override
    {
        std::error_code ec;
        std::filesystem::remove_all(dropinDir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(dropinDir, ec);
    }
};

// DBus reload inside createOrUpdateDropinFile fails gracefully via its
// internal catch(SdBusError) when the system bus is absent.
TEST_F(DropinFileTest, FileDoesNotExist_CreatesFileWithCorrectContent)
{
    createOrUpdateDropinFile(5);

    ASSERT_TRUE(std::filesystem::exists(dropinFile));
    std::ifstream f(dropinFile);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_NE(content.find("[Socket]"), std::string::npos);
    EXPECT_NE(content.find("MaxConnections=5"), std::string::npos);
}

TEST_F(DropinFileTest, CalledTwice_SecondCallNoOp)
{
    createOrUpdateDropinFile(3);
    ASSERT_TRUE(std::filesystem::exists(dropinFile));

    // Capture content after first call
    std::string first;
    {
        std::ifstream f(dropinFile);
        first.assign((std::istreambuf_iterator<char>(f)), {});
    }

    // Second call should not overwrite because file already exists
    createOrUpdateDropinFile(99);

    std::string second;
    {
        std::ifstream f(dropinFile);
        second.assign((std::istreambuf_iterator<char>(f)), {});
    }
    EXPECT_EQ(first, second);
}

#endif
