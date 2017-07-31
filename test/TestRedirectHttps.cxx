#include "RedirectHttps.hxx"
#include "TestPool.hxx"

#include <gtest/gtest.h>

#include <string.h>

TEST(TestRedirectHttps, Basic)
{
    TestPool pool;

    ASSERT_STREQ(MakeHttpsRedirect(pool, "localhost", 0, "/foo"),
                 "https://localhost/foo");

    ASSERT_STREQ(MakeHttpsRedirect(pool, "localhost:80", 0, "/foo"),
                 "https://localhost/foo");

    ASSERT_STREQ(MakeHttpsRedirect(pool, "localhost:80", 443, "/foo"),
                 "https://localhost/foo");

    ASSERT_STREQ(MakeHttpsRedirect(pool, "localhost:80", 444, "/foo"),
                 "https://localhost:444/foo");
}

TEST(TestRedirectHttps, IPv6)
{
    TestPool pool;

    ASSERT_STREQ(MakeHttpsRedirect(pool, "::", 0, "/foo"),
                 "https://::/foo");

    ASSERT_STREQ(MakeHttpsRedirect(pool, "[::]:80", 0, "/foo"),
                 "https://::/foo");

    ASSERT_STREQ(MakeHttpsRedirect(pool, "::", 444, "/foo"),
                 "https://[::]:444/foo");
}
