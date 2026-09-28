#include "utils.hpp"

#include <boost/system/error_code.hpp>

#include <string>

#include <gtest/gtest.h>

TEST(AddInstanceName, EmptyInstance_ReturnsEmpty)
{
    EXPECT_EQ(addInstanceName("", "@"), "");
}

TEST(AddInstanceName, NonEmptyInstance_ReturnsSuffixPlusInstance)
{
    EXPECT_EQ(addInstanceName("eth0", "@"), "@eth0");
}

TEST(AddInstanceName, EmptySuffix_ReturnsInstance)
{
    EXPECT_EQ(addInstanceName("eth0", ""), "eth0");
}

TEST(AddInstanceName, BothEmpty_ReturnsEmpty)
{
    EXPECT_EQ(addInstanceName("", ""), "");
}

TEST(AddInstanceName, MultiCharSuffix)
{
    EXPECT_EQ(addInstanceName("sshd", "service@"), "service@sshd");
}

TEST(CheckAndThrowInternalFailure, NoError_DoesNotThrow)
{
    boost::system::error_code ec;
    EXPECT_NO_THROW(checkAndThrowInternalFailure(ec, "test msg"));
}

TEST(CheckAndThrowInternalFailure, NoError_EmptyMsg_DoesNotThrow)
{
    boost::system::error_code ec;
    EXPECT_NO_THROW(checkAndThrowInternalFailure(ec, ""));
}
