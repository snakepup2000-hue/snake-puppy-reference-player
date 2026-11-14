// Snake Puppy Reference Player - test runner.
// Build target: SnakePuppyTests. Run from the repository root or pass the path
// to the build's fixtures directory with SP_TEST_FIXTURES.
#include "TestFramework.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <cstring>
#include <fstream>

namespace
{
struct Options
{
    juce::String filter;
    bool list = false;
    bool verbose = false;
    juce::String junitPath;
};

juce::String htmlEscape (const juce::String& s)
{
    return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;").replace ("\"", "&quot;");
}
} // namespace

int main (int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i)
    {
        juce::String arg (argv[i]);
        if (arg == "--filter" && i + 1 < argc) opts.filter = argv[++i];
        else if (arg == "--list") opts.list = true;
        else if (arg == "--verbose") opts.verbose = true;
        else if (arg == "--junit" && i + 1 < argc) opts.junitPath = argv[++i];
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: SnakePuppyTests [--filter <substr>] [--list] [--verbose] [--junit <file.xml>]\n";
            return 0;
        }
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    auto& registry = sptest::Registry::instance();
    const auto& tests = registry.all();

    struct Result { juce::String suite, name; bool passed; juce::String failure; double ms; };
    std::vector<Result> results;
    int failures = 0;
    int executed = 0;

    if (opts.list)
    {
        for (auto& t : tests)
            if (opts.filter.isEmpty() || juce::String (t.suite).containsIgnoreCase (opts.filter)
                || juce::String (t.name).containsIgnoreCase (opts.filter))
                std::cout << t.suite << "." << t.name << "\n";
        return 0;
    }

    std::cout << "Snake Puppy test suite: " << tests.size() << " test(s) registered\n";
    std::cout << "------------------------------------------------------------\n";

    for (auto& t : tests)
    {
        juce::String full = juce::String (t.suite) + "." + t.name;
        if (opts.filter.isNotEmpty() && ! full.containsIgnoreCase (opts.filter))
            continue;

        ++executed;
        auto start = juce::Time::getMillisecondCounterHiRes();
        bool passed = true;
        juce::String failure;
        try
        {
            t.fn();
        }
        catch (const sptest::Failure& f)
        {
            passed = false;
            failure = f.message;
        }
        catch (const std::exception& e)
        {
            passed = false;
            failure = juce::String ("unexpected exception: ") + e.what();
        }
        catch (...)
        {
            passed = false;
            failure = "unknown exception";
        }
        auto elapsed = juce::Time::getMillisecondCounterHiRes() - start;

        if (! passed) ++failures;
        results.push_back ({ t.suite, t.name, passed, failure, elapsed });

        std::cout << (passed ? "  PASS  " : "  FAIL  ") << full;
        if (elapsed > 50.0)
            std::cout << "   (" << juce::String (elapsed, 1) << " ms)";
        std::cout << "\n";
        if (! passed)
            std::cout << "        " << failure << "\n";
        else if (opts.verbose && elapsed > 10.0)
            std::cout << "        " << juce::String (elapsed, 1) << " ms\n";
    }

    std::cout << "------------------------------------------------------------\n";
    std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED")
              << "  (" << (executed - failures) << "/" << executed << " passed)\n";

    if (opts.junitPath.isNotEmpty())
    {
        juce::File out (opts.junitPath);
        out.getParentDirectory().createDirectory();
        juce::String xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        xml << "<testsuites>\n<testsuite name=\"SnakePuppy\" tests=\"" << executed
            << "\" failures=\"" << failures << "\">\n";
        for (auto& r : results)
        {
            xml << "  <testcase classname=\"" << htmlEscape (r.suite) << "\" name=\"" << htmlEscape (r.name)
                << "\" time=\"" << juce::String (r.ms / 1000.0, 4) << "\"";
            if (r.passed)
                xml << "/>\n";
            else
                xml << "><failure message=\"" << htmlEscape (r.failure) << "\"/></testcase>\n";
        }
        xml << "</testsuite>\n</testsuites>\n";
        out.replaceWithText (xml);
    }

    return failures == 0 ? 0 : 1;
}
