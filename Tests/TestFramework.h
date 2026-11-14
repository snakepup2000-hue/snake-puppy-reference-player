/*
    Snake Puppy Reference Player
    Minimal self-registering test framework used by the headless test target.
    Add a test file under Tests/ and write:

        #include "TestFramework.h"
        SP_TEST (Dsp, BassBoostMeasured)
        {
            SP_REQUIRE (1 + 1 == 2);
        }

    Tests are discovered automatically through static registration; no CMake
    changes are needed.
*/
#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined (__has_include)
 #if __has_include (<juce_core/juce_core.h>)
  #define SP_TESTFRAMEWORK_HAS_JUCE 1
  #include <juce_core/juce_core.h>
 #endif
#endif

namespace sptest
{

struct TestCase
{
    const char* suite;
    const char* name;
    std::function<void()> fn;
};

class Registry
{
public:
    static Registry& instance() { static Registry r; return r; }
    void add (TestCase t) { tests.push_back (t); }
    const std::vector<TestCase>& all() const { return tests; }

private:
    std::vector<TestCase> tests;
};

class Failure : public std::exception
{
public:
    explicit Failure (std::string m) : message (std::move (m)) {}
    const char* what() const noexcept override { return message.c_str(); }
    std::string message;
};

struct Registrar
{
    Registrar (const char* suite, const char* name, std::function<void()> fn)
    {
        Registry::instance().add ({ suite, name, std::move (fn) });
    }
};

inline void reportFailure (const char* file, int line, const std::string& expr, const std::string& detail)
{
    std::ostringstream os;
    os << file << ":" << line << "  CHECK(" << expr << ")";
    if (! detail.empty())
        os << "  [" << detail << "]";
    throw Failure (os.str());
}

inline std::string toStr (double v) { std::ostringstream o; o.precision (10); o << v; return o.str(); }
inline std::string toStr (float v) { return toStr ((double) v); }
inline std::string toStr (int v) { return std::to_string (v); }
inline std::string toStr (long long v) { return std::to_string (v); }
inline std::string toStr (bool v) { return v ? "true" : "false"; }
inline std::string toStr (const std::string& v) { return v; }
inline std::string toStr (const char* v) { return v ? v : "(null)"; }
#if defined (SP_TESTFRAMEWORK_HAS_JUCE)
inline std::string toStr (const juce::String& v) { return v.toStdString(); }
#endif

} // namespace sptest

#define SP_TEST(suite, name) \
    static void spTest_##suite##_##name(); \
    static ::sptest::Registrar spRegistrar_##suite##_##name (#suite, #name, &spTest_##suite##_##name); \
    static void spTest_##suite##_##name()

#define SP_REQUIRE(expr) \
    do { if (! (expr)) ::sptest::reportFailure (__FILE__, __LINE__, #expr, ""); } while (0)

#define SP_REQUIRE_MSG(expr, msg) \
    do { if (! (expr)) ::sptest::reportFailure (__FILE__, __LINE__, #expr, ::sptest::toStr (msg)); } while (0)

#define SP_REQUIRE_NEAR(a, b, tol) \
    do { const double _a = (a), _b = (b), _t = (tol); \
         if (! (std::abs (_a - _b) <= _t)) \
             ::sptest::reportFailure (__FILE__, __LINE__, #a " ~= " #b, \
                 ::sptest::toStr (_a) + " vs " + ::sptest::toStr (_b) + " tol " + ::sptest::toStr (_t)); } while (0)

#define SP_REQUIRE_LT(a, b) \
    do { const double _a = (a), _b = (b); \
         if (! (_a < _b)) ::sptest::reportFailure (__FILE__, __LINE__, #a " < " #b, \
             ::sptest::toStr (_a) + " vs " + ::sptest::toStr (_b)); } while (0)

#define SP_REQUIRE_GT(a, b) SP_REQUIRE_LT (b, a)

#define SP_REQUIRE_LE(a, b) \
    do { const double _a = (a), _b = (b); \
         if (! (_a <= _b)) ::sptest::reportFailure (__FILE__, __LINE__, #a " <= " #b, \
             ::sptest::toStr (_a) + " vs " + ::sptest::toStr (_b)); } while (0)

#define SP_REQUIRE_GE(a, b) SP_REQUIRE_LE (b, a)
