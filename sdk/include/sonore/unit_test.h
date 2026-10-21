// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a small unit-test framework for plugin authors' own tests.
//
// ── Why the SDK ships one ───────────────────────────────────────────────────
//
// The SDK's own tests do not use this: they are standalone programs run by
// ctest and by the sanitizer gate, and a framework inside the thing being
// tested would be a dependency those tests share with the code under test.
//
// A plugin author is in a different position. They have a DSP class, a
// preset parser, a tuning table, and want to check them without choosing,
// fetching and wiring up a test library first -- and code moving from the
// reference arrives with tests written against exactly this shape: a
// UnitTest subclass per area, beginTest / expect / expectEquals inside
// runTest(), and a static instance that registers itself.
//
//     struct FilterTests : sonore::UnitTest {
//       FilterTests() : UnitTest("Filter", "dsp") {}
//       void runTest() override {
//         beginTest("DC passes a lowpass");
//         expectWithinAbsoluteError(runDc(), 1.0f, 1e-4f);
//       }
//     };
//     static FilterTests filterTests;
//
//     int main() {
//       sonore::UnitTestRunner runner;
//       runner.runAllTests();
//       return runner.getTotalFailures() == 0 ? 0 : 1;
//     }
//
// Each test is given a reproducible random generator; the runner logs its
// seed, so a failure that depends on random data can be replayed exactly by
// passing that seed back. An exception escaping a test is a failure, caught
// and reported, not the end of the run.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "number_text.h"
#include "random.h"

namespace sonore {

class UnitTestRunner;

namespace detail {

// expectEquals(actual, expected): `expected` takes actual's type, so
// expectEquals(list.size(), 3) compiles rather than failing to deduce.
template <class T>
struct UnitTestSameType {
  using type = T;
};

// Value to text for failure messages: numbers in full precision and '.'
// whatever the locale, strings quoted, anything streamable streamed, and
// anything else admitted to be unprintable.
template <class T, class = void>
struct UnitTestStreamable : std::false_type {};
template <class T>
struct UnitTestStreamable<T, decltype((void) (std::declval<std::ostream&>() << std::declval<const T&>()))>
    : std::true_type {};

template <class T>
std::string unitTestDescribe(const T& value) {
  if constexpr (std::is_same<T, bool>::value) {
    return value ? "true" : "false";
  } else if constexpr (std::is_floating_point<T>::value) {
    // The fewest digits that read back as the same value: 0.1 is "0.1", not
    // "0.10000000000000001", and two values that differ never print alike.
    char buffer[48];
    const int fewest = sizeof(T) <= sizeof(float) ? 6 : 15;
    for (int digits = fewest; digits <= fewest + 3; ++digits) {
      char spec[8];
      std::snprintf(spec, sizeof(spec), "%%.%dg", digits);
      numtext::format(buffer, sizeof(buffer), (double) value, spec);
      double back = 0.0;
      const size_t n = std::strlen(buffer);
      if (numtext::parse(buffer, n, &back) == n && (T) back == value) break;
    }
    return buffer;
  } else if constexpr (std::is_convertible<const T&, std::string>::value) {
    return "\"" + std::string(value) + "\"";
  } else if constexpr (UnitTestStreamable<T>::value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value;
    return out.str();
  } else {
    return "(a value that cannot be printed)";
  }
}

} // namespace detail

/**
 * A group of tests. Subclass it, override runTest(), and create one static
 * instance: it registers itself, and a UnitTestRunner finds it.
 */
class UnitTest {
public:
  /** Registers this test under `name` and an optional `category`. Any
   *  thread, though tests are usually static objects. */
  explicit UnitTest(const std::string& name, const std::string& category = std::string())
      : name_(name), category_(category) {
    Registry& r = registry();
    std::lock_guard<std::mutex> hold(r.lock);
    r.tests.push_back(this);
  }
  /** Unregisters it. Not while a runner is running it. */
  virtual ~UnitTest() {
    Registry& r = registry();
    std::lock_guard<std::mutex> hold(r.lock);
    r.tests.erase(std::remove(r.tests.begin(), r.tests.end(), this), r.tests.end());
  }
  ///@{
  /** Not copyable: the registry holds this object's address. */
  UnitTest(const UnitTest&) = delete;
  UnitTest& operator=(const UnitTest&) = delete;
  ///@}

  /** The name given to the constructor. */
  const std::string& getName() const { return name_; }
  /** The category given to the constructor; "" for none. */
  const std::string& getCategory() const { return category_; }

  /** Runs this test, reporting to `runner`: initialise, runTest, shutdown. */
  void performTest(UnitTestRunner* runner);

  /** Every registered test, in the order they were created. */
  static std::vector<UnitTest*> getAllTests() {
    Registry& r = registry();
    std::lock_guard<std::mutex> hold(r.lock);
    return r.tests;
  }
  /** The registered tests whose category is exactly `category` (case
   *  counts), in the order they were created. */
  static std::vector<UnitTest*> getTestsInCategory(const std::string& category) {
    std::vector<UnitTest*> out;
    for (UnitTest* t : getAllTests())
      if (t->category_ == category) out.push_back(t);
    return out;
  }
  /** Every category any registered test has, each once, sorted. */
  static std::vector<std::string> getAllCategories() {
    std::vector<std::string> out;
    for (UnitTest* t : getAllTests())
      if (!t->category_.empty() && std::find(out.begin(), out.end(), t->category_) == out.end())
        out.push_back(t->category_);
    std::sort(out.begin(), out.end());
    return out;
  }

  /** Before runTest: set up what the tests share. */
  virtual void initialise() {}
  /** After runTest, even if it failed: tear it down. */
  virtual void shutdown() {}
  /** The tests: a series of beginTest() sections with expectations in each. */
  virtual void runTest() = 0;

protected:
  /** Starts a named section; the expectations after it are counted under it. */
  void beginTest(const std::string& testName);

  /** Passes if `testResult` is true; otherwise fails with `failureMessage`. */
  void expect(bool testResult, const std::string& failureMessage = std::string());

  ///@{
  /** Passes when the comparison the name says holds; a failure's message
   *  gives both values, printed where they can be. The second value takes
   *  the first one's type, so `expectEquals(count, 3)` compiles with any
   *  integer `count`. */
  template <class ValueType>
  void expectEquals(ValueType actual, typename detail::UnitTestSameType<ValueType>::type expected,
                    const std::string& failureMessage = std::string()) {
    expectResultAndPrint(actual, expected, actual == expected, "", failureMessage);
  }
  template <class ValueType>
  void expectNotEquals(ValueType value, typename detail::UnitTestSameType<ValueType>::type valueToCompareTo,
                       const std::string& failureMessage = std::string()) {
    expectResultAndPrint(value, valueToCompareTo, value != valueToCompareTo, "unequal to",
                         failureMessage);
  }
  template <class ValueType>
  void expectGreaterThan(ValueType value, typename detail::UnitTestSameType<ValueType>::type valueToCompareTo,
                         const std::string& failureMessage = std::string()) {
    expectResultAndPrint(value, valueToCompareTo, value > valueToCompareTo, "greater than",
                         failureMessage);
  }
  template <class ValueType>
  void expectLessThan(ValueType value, typename detail::UnitTestSameType<ValueType>::type valueToCompareTo,
                      const std::string& failureMessage = std::string()) {
    expectResultAndPrint(value, valueToCompareTo, value < valueToCompareTo, "less than",
                         failureMessage);
  }
  template <class ValueType>
  void expectGreaterOrEqual(ValueType value,
                            typename detail::UnitTestSameType<ValueType>::type valueToCompareTo,
                            const std::string& failureMessage = std::string()) {
    expectResultAndPrint(value, valueToCompareTo, value >= valueToCompareTo,
                         "greater or equal to", failureMessage);
  }
  template <class ValueType>
  void expectLessOrEqual(ValueType value, typename detail::UnitTestSameType<ValueType>::type valueToCompareTo,
                         const std::string& failureMessage = std::string()) {
    expectResultAndPrint(value, valueToCompareTo, value <= valueToCompareTo, "less or equal to",
                         failureMessage);
  }
  ///@}

  /** Passes if |actual - expected| <= maxAbsoluteError. A NaN never passes. */
  template <class ValueType>
  void expectWithinAbsoluteError(ValueType actual,
                                 typename detail::UnitTestSameType<ValueType>::type expected,
                                 typename detail::UnitTestSameType<ValueType>::type maxAbsoluteError,
                                 const std::string& failureMessage = std::string()) {
    const auto difference = actual < expected ? expected - actual : actual - expected;
    const bool ok = difference <= maxAbsoluteError; // false for NaN
    expectResultAndPrint(actual, expected, ok,
                         "within " + detail::unitTestDescribe(maxAbsoluteError) + " of",
                         failureMessage);
  }

  /** Passes if calling `fn` throws nothing. */
  template <class Function>
  void expectDoesNotThrow(Function&& fn, const std::string& failureMessage = std::string()) {
    bool threw = false;
    try {
      fn();
    } catch (...) {
      threw = true;
    }
    expect(!threw, failureMessage.empty() ? std::string("an exception was thrown") : failureMessage);
  }
  /** Passes if calling `fn` throws anything. */
  template <class Function>
  void expectThrows(Function&& fn, const std::string& failureMessage = std::string()) {
    bool threw = false;
    try {
      fn();
    } catch (...) {
      threw = true;
    }
    expect(threw, failureMessage.empty() ? std::string("no exception was thrown") : failureMessage);
  }
  /** Passes if calling `fn` throws an ExceptionType (or something derived). */
  template <class ExceptionType, class Function>
  void expectThrowsType(Function&& fn, const std::string& failureMessage = std::string()) {
    bool rightType = false, threw = false;
    try {
      fn();
    } catch (const ExceptionType&) {
      rightType = threw = true;
    } catch (...) {
      threw = true;
    }
    expect(rightType, !failureMessage.empty() ? failureMessage
                      : threw              ? std::string("an exception of another type was thrown")
                                           : std::string("no exception was thrown"));
  }

  /** Writes to the runner's log. */
  void logMessage(const std::string& message);

  /** A generator seeded from the run's seed and this test's name: the same
   *  numbers every time the run is repeated with that seed. */
  Random getRandom() const;

private:
  /** Every UnitTest alive, in the order they were constructed, under a
   *  mutex: what getAllTests() copies. A function-local static that the first
   *  test's constructor creates, so it outlives every static test. */
  struct Registry {
    std::mutex lock;              ///< guards `tests`
    std::vector<UnitTest*> tests; ///< in construction order; not owned
  };
  static Registry& registry() {
    static Registry r;
    return r;
  }

  template <class A, class B>
  void expectResultAndPrint(const A& value, const B& compareTo, bool ok, const std::string& relation,
                            const std::string& failureMessage) {
    if (ok) {
      expect(true);
      return;
    }
    std::string message = "Expected value" + (relation.empty() ? std::string() : " " + relation) +
                          ": " + detail::unitTestDescribe(compareTo) +
                          ", Actual value: " + detail::unitTestDescribe(value);
    if (!failureMessage.empty()) message = failureMessage + " -- " + message;
    expect(false, message);
  }

  std::string name_;
  std::string category_;
  UnitTestRunner* runner_ = nullptr;
};

/**
 * Runs UnitTests and keeps their results. Override logMessage to send the
 * log somewhere other than stdout, resultsUpdated to watch progress, and
 * shouldAbortTests to stop a long run early.
 */
class UnitTestRunner {
public:
  /** One beginTest() section's outcome. */
  struct TestResult {
    std::string unitTestName;          ///< the UnitTest's getName()
    std::string subcategoryName;       ///< the name given to beginTest()
    int passes = 0;                    ///< expectations that held
    int failures = 0;                  ///< expectations that did not
    std::vector<std::string> messages; ///< every failure's message
    double seconds = 0.0;              ///< how long the section took
  };

  /** No results yet. */
  UnitTestRunner() = default;
  /** Virtual: a runner is subclassed to redirect its log. */
  virtual ~UnitTestRunner() = default;

  /**
   * Runs `tests` in order. A seed of 0 picks one from the clock and logs
   * it; pass that seed back to repeat a run's random numbers exactly.
   */
  void runTests(const std::vector<UnitTest*>& tests, int64_t randomSeed = 0) {
    results_.clear();
    seed_ = randomSeed != 0 ? randomSeed
                            : (int64_t) std::chrono::steady_clock::now().time_since_epoch().count() | 1;
    char line[96];
    std::snprintf(line, sizeof(line), "Random seed: 0x%llx", (unsigned long long) seed_);
    logMessage(line);
    resultsUpdated();
    for (UnitTest* t : tests) {
      if (shouldAbortTests()) break;
      if (t) t->performTest(this);
    }
    endCurrentSection();
    const int failures = getTotalFailures();
    if (failures == 0) {
      logMessage("All tests completed successfully");
    } else {
      std::snprintf(line, sizeof(line), "%d test failure(s)", failures);
      logMessage(line);
    }
  }
  /** runTests() over every registered test. */
  void runAllTests(int64_t randomSeed = 0) { runTests(UnitTest::getAllTests(), randomSeed); }
  /** runTests() over the tests in `category`. */
  void runTestsInCategory(const std::string& category, int64_t randomSeed = 0) {
    runTests(UnitTest::getTestsInCategory(category), randomSeed);
  }

  /** Log each passing expectation too, not only failures. */
  void setPassesAreLogged(bool shouldLogPasses) { logPasses_ = shouldLogPasses; }

  /** How many sections the last run recorded. */
  int getNumResults() const { return (int) results_.size(); }
  /** Section `index` (0-based) of the last run, or null when out of range.
   *  Valid until the next run. */
  const TestResult* getResult(int index) const {
    return index >= 0 && (size_t) index < results_.size() ? &results_[(size_t) index] : nullptr;
  }
  /** Failed expectations across the last run. */
  int getTotalFailures() const {
    int n = 0;
    for (const TestResult& r : results_) n += r.failures;
    return n;
  }
  /** Passed expectations across the last run. */
  int getTotalPasses() const {
    int n = 0;
    for (const TestResult& r : results_) n += r.passes;
    return n;
  }
  /** The seed the last run used. */
  int64_t getRandomSeed() const { return seed_; }

protected:
  /** Called whenever a result changes. */
  virtual void resultsUpdated() {}
  /** Where the log goes: stdout unless overridden. */
  virtual void logMessage(const std::string& message) {
    std::fputs(message.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  }
  /** Checked between tests; true stops the run. */
  virtual bool shouldAbortTests() { return false; }

private:
  friend class UnitTest;

  void beginSection(UnitTest* test, const std::string& subcategory) {
    endCurrentSection();
    TestResult r;
    r.unitTestName = test ? test->getName() : std::string();
    r.subcategoryName = subcategory;
    results_.push_back(std::move(r));
    sectionOpen_ = true;
    sectionStart_ = std::chrono::steady_clock::now();
    logMessage("-----------------------------------------------------------------");
    logMessage("Starting test: " + results_.back().unitTestName + " / " + subcategory + "...");
    resultsUpdated();
  }

  void endCurrentSection() {
    if (!sectionOpen_ || results_.empty()) return;
    sectionOpen_ = false;
    TestResult& r = results_.back();
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - sectionStart_).count();
    char line[96];
    std::snprintf(line, sizeof(line), "%d passed, %d failed", r.passes, r.failures);
    logMessage(r.failures == 0 ? "All tests completed successfully (" + std::string(line) + ")"
                               : std::string(line));
    resultsUpdated();
  }

  void addPass() {
    TestResult& r = current();
    ++r.passes;
    if (logPasses_) {
      char line[64];
      std::snprintf(line, sizeof(line), "Test %d passed", r.passes + r.failures);
      logMessage(line);
    }
    resultsUpdated();
  }

  void addFail(const std::string& message) {
    TestResult& r = current();
    ++r.failures;
    char head[64];
    std::snprintf(head, sizeof(head), "!!! Test %d failed", r.passes + r.failures);
    const std::string line = message.empty() ? std::string(head) : std::string(head) + ": " + message;
    r.messages.push_back(line);
    logMessage(line);
    resultsUpdated();
  }

  // An expectation before any beginTest() still counts, under a section that
  // says so rather than being lost.
  TestResult& current() {
    if (!sectionOpen_) beginSection(currentTest_, "(before any beginTest)");
    return results_.back();
  }

  std::vector<TestResult> results_;
  UnitTest* currentTest_ = nullptr;
  bool sectionOpen_ = false;
  bool logPasses_ = false;
  int64_t seed_ = 0;
  std::chrono::steady_clock::time_point sectionStart_{};
};

inline void UnitTest::performTest(UnitTestRunner* runner) {
  if (!runner) return;
  runner_ = runner;
  runner->currentTest_ = this;
  runner->endCurrentSection();
  // Each stage is guarded on its own: an exception in runTest still gets
  // shutdown() called, and is a failure with its message, not a crash.
  auto guarded = [this](const char* stage, void (UnitTest::*fn)()) {
    try {
      (this->*fn)();
    } catch (const std::exception& e) {
      expect(false, std::string("an exception escaped ") + stage + ": " + e.what());
    } catch (...) {
      expect(false, std::string("an exception escaped ") + stage);
    }
  };
  guarded("initialise()", &UnitTest::initialise);
  guarded("runTest()", &UnitTest::runTest);
  guarded("shutdown()", &UnitTest::shutdown);
  runner->endCurrentSection();
  runner->currentTest_ = nullptr;
  runner_ = nullptr;
}

inline void UnitTest::beginTest(const std::string& testName) {
  if (runner_) runner_->beginSection(this, testName);
}

inline void UnitTest::expect(bool testResult, const std::string& failureMessage) {
  if (!runner_) return;
  if (testResult) runner_->addPass();
  else runner_->addFail(failureMessage);
}

inline void UnitTest::logMessage(const std::string& message) {
  if (runner_) runner_->logMessage(message);
}

inline Random UnitTest::getRandom() const {
  // FNV-1a of the name, mixed with the run's seed: a test's numbers do not
  // depend on which other tests ran before it.
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : name_) h = (h ^ c) * 1099511628211ull;
  const uint64_t seed = runner_ ? (uint64_t) runner_->getRandomSeed() : 0u;
  return Random(h ^ (seed * 0x9E3779B97F4A7C15ull));
}

} // namespace sonore
