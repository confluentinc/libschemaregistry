/**
 * WildcardMatcherTest
 * Tests for the wildcard matching functionality
 */

#include <gtest/gtest.h>
#include "schemaregistry/serdes/WildcardMatcher.h"

using namespace schemaregistry::serdes;

class WildcardMatcherTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Test setup if needed
    }
    
    void TearDown() override {
        // Test cleanup if needed
    }
};

TEST_F(WildcardMatcherTest, BasicMatching) {
    // Empty cases
    EXPECT_FALSE(wildcardMatch("", "Foo"));
    EXPECT_FALSE(wildcardMatch("Foo", ""));
    EXPECT_TRUE(wildcardMatch("", ""));
    
    // Exact match
    EXPECT_TRUE(wildcardMatch("Foo", "Foo"));
    
    // Wildcard cases
    EXPECT_TRUE(wildcardMatch("", "*"));
    EXPECT_FALSE(wildcardMatch("", "?"));
    EXPECT_TRUE(wildcardMatch("Foo", "Fo*"));
    EXPECT_TRUE(wildcardMatch("Foo", "Fo?"));
    EXPECT_TRUE(wildcardMatch("Foo Bar and Catflag", "Fo*"));
    EXPECT_TRUE(wildcardMatch("New Bookmarks", "N?w ?o?k??r?s"));
    
    // Non-matching cases
    EXPECT_FALSE(wildcardMatch("Foo", "Bar"));
    
    // Complex patterns
    EXPECT_TRUE(wildcardMatch("Foo Bar Foo", "F*o Bar*"));
    EXPECT_TRUE(wildcardMatch("Adobe Acrobat Installer", "Ad*er"));
    EXPECT_TRUE(wildcardMatch("Foo", "*Foo"));
    EXPECT_TRUE(wildcardMatch("BarFoo", "*Foo"));
    EXPECT_TRUE(wildcardMatch("Foo", "Foo*"));
    EXPECT_TRUE(wildcardMatch("FooBar", "Foo*"));
}

TEST_F(WildcardMatcherTest, CaseSensitivity) {
    // Case sensitive matching
    EXPECT_FALSE(wildcardMatch("FOO", "*Foo"));
    EXPECT_FALSE(wildcardMatch("BARFOO", "*Foo"));
    EXPECT_FALSE(wildcardMatch("FOO", "Foo*"));
    EXPECT_FALSE(wildcardMatch("FOOBAR", "Foo*"));
}

TEST_F(WildcardMatcherTest, DotSeparatorPatterns) {
    // Test with dot separator patterns
    EXPECT_TRUE(wildcardMatch("eve", "eve*"));
    EXPECT_TRUE(wildcardMatch("alice.bob.eve", "a*.bob.eve"));
    EXPECT_FALSE(wildcardMatch("alice.bob.eve", "a*"));
    EXPECT_TRUE(wildcardMatch("alice.bob.eve", "a**"));
    EXPECT_FALSE(wildcardMatch("alice.bob.eve", "alice.bob*"));
    EXPECT_TRUE(wildcardMatch("alice.bob.eve", "alice.bob**"));
}

TEST_F(WildcardMatcherTest, WildcardToRegexp) {
    // Test the wildcard to regexp conversion
    EXPECT_EQ(wildcardToRegexp("*"), "[^.]*");
    EXPECT_EQ(wildcardToRegexp("**"), ".*");
    EXPECT_EQ(wildcardToRegexp("?"), "[^.]");
    EXPECT_EQ(wildcardToRegexp("foo"), "foo");
    EXPECT_EQ(wildcardToRegexp("f*o"), "f[^.]*o");
    EXPECT_EQ(wildcardToRegexp("f**o"), "f.*o");
    EXPECT_EQ(wildcardToRegexp("f?o"), "f[^.]o");
    
    // Test with custom separator
    EXPECT_EQ(wildcardToRegexp("*", "/"), "[^/]*");
    EXPECT_EQ(wildcardToRegexp("**", "/"), ".*");
    EXPECT_EQ(wildcardToRegexp("?", "/"), "[^/]");
}

TEST_F(WildcardMatcherTest, RegexEscaping) {
    // Test that regex special characters are properly escaped
    EXPECT_TRUE(wildcardMatch("foo.bar", "foo.bar"));
    EXPECT_TRUE(wildcardMatch("foo+bar", "foo\\+bar"));
    EXPECT_TRUE(wildcardMatch("foo(bar)", "foo\\(bar\\)"));
    EXPECT_TRUE(wildcardMatch("foo{bar}", "foo\\{bar\\}"));
    EXPECT_TRUE(wildcardMatch("foo|bar", "foo\\|bar"));
    EXPECT_TRUE(wildcardMatch("foo^bar", "foo\\^bar"));
    EXPECT_TRUE(wildcardMatch("foo$bar", "foo\\$bar"));
} 