#include "util/promise.h"

#include <pthread.h>
#include <array>
#include <unordered_set>

#include "util/event_loop.h"
#include "util/print.h"
#include "util/semaphore.h"
#include "util/testharness.h"

//#define STRESS // define to enable stress test

namespace codulus {
namespace test {

class PromiseTests {};

TEST(PromiseTests, ConversionsTest) {
  int s = 0;
  {
    // Pass 1: int promises
    Promise<int> p1 = 1;
    p1.then([&s](int v) { s++; ASSERT_EQ(v, 1); });

    Promise<int> p2(2ULL);
    p2.then([&s](int v) { s++; ASSERT_EQ(v, 2); });
  
    int y = 3;
    int& x = y;
    Promise<int> p3(x);
    p3.then([&s](int v) { s++; ASSERT_EQ(v, 3); });

    // Conversion operator
    struct C { 
        C() : C(0) {}
        C(int x) : x_(x) {}
        operator int() const { return x_; } 
        int x_;
    };

    C c(4);
    Promise<int> p4(c);
    p4.then([&s](int v) { s++; ASSERT_EQ(v, 4); });

    int z = 5;
    Promise<int> p5(std::move(z));
    p5.then([&s](int v) { s++; ASSERT_EQ(v, 5); });

    double d = 6;
    Promise<int> p6(d);
    p6.then([&s](int v) { s++; ASSERT_EQ(v, 6); });

    // Move constructor
    Promise<int> p7(7);
    Promise<int> p8(std::move(p7));
    p8.then([&s](int v) { s++; ASSERT_EQ(v, 7); });

    // Copy constructor, with extra then to make things more interesting
    Promise<int> p9(0);
    Promise<int> p10(p9.then([](int) { return 8; }));
    p10.then([&s](int v) { s++; ASSERT_EQ(v, 8); });

    // Assignment operator, with extra then to make things more interesting
    Promise<int> p11(0);
    Promise<int> p12 = p11.then([](int) { return 9; });
    p12.then([&s](int v) { s++; ASSERT_EQ(v, 9); });

    // Move assignment operator
    Promise<int> p13(10);
    Promise<int> p14 = std::move(p13);
    p14.then([&s](int v) { s++; ASSERT_EQ(v, 10); });

    // Class with conversions in and out 
    Promise<C> p15(11);
    p15.then([&s](int v) { s++; ASSERT_EQ(v, 11); });

    // Class, copy value
    Promise<C> p16(C(12));
    p16.then([&s](C c) { s++; ASSERT_EQ((int)c, 12); });

    // Class, move value
    C c17(13);
    Promise<C> p17(std::move(c17));
    p17.then([&s](const C& c) {s++; ASSERT_EQ((int)c, 13); });
  }
  ASSERT_EQ(s, 13);
}

TEST(PromiseTests, UniqueAndSharedPtrTest) {
  int x = 0;
  {
    Promise<std::unique_ptr<int>> p1;
    p1.resolve(std::make_unique<int>(1));
    p1.then([&x](const std::unique_ptr<int>& i) {x += *i;});

    Promise<std::unique_ptr<int>> p2;
    p2.then([&x](std::unique_ptr<int>&& i) {x += *i;});
    p2.resolve(std::make_unique<int>(2));

    Promise<std::shared_ptr<int>> p3;
    p3.then([&x](std::shared_ptr<int> i) {x += *i;});
    p3.resolve(std::make_shared<int>(3));
  }

  ASSERT_EQ(x, 6);
}

TEST(PromiseTests, ChainTest) {
  std::function<void(int)> cb_;
  auto fake_async_request = [&cb_](std::function<void(int)>&& cb) {
    cb_ = std::move(cb);
  };
  auto complete_async_request = [&cb_] { cb_(2); };

  struct X { X() {} X(int value) { a = value; } int a; };
  double v = 0.0;

  Promise<int> promise;
  fake_async_request(promise.deferred());
  promise
      .then([](int n) { return n + 1; })
      .then([](int n) { return (double)n / 2; })
      .then([&v](double d) { v = d; return true; })
      .then([&v](bool b) { v += b ? 20 : 30; return X(200); })
      .then([&v](X x) { v += x.a; })
      .then([&v]() { v += 50.0; });

  complete_async_request();
  ASSERT_EQ(v, 271.5);
}

TEST(PromiseTests, NestedPromiseTest) {
  // Case 1: resolved promise then'd by nested promise, with inner deferred
  {
    std::function<void(int)> cb;
    double result = 0;
    {
      Promise<double> p1;
      p1.resolve(1);
      Promise<double> coalesced = p1.then([&cb](double v1) {
        Promise<int> p2;
        cb = p2.deferred();
        Promise<double> p3 = p2.then([v1](int v2) {
          return (double)(v1 + v2);
        }); 
        return p3;
      });
      coalesced.then([&result](double sum) { result = sum; });
    }
    cb(2);
    ASSERT_EQ(result, 3.0);
  }

  // Case 2: deferred promise then'd by nested promise, with inner deferred
  { 
    int result = 0;
    std::function<void(int)> cb1;
    std::function<void(int)> cb2;
    {
      Promise<int> p1;
      cb1 = p1.deferred();
      p1.then([&cb2](int i) {
        Promise<int> p2;
        cb2 = p2.deferred();
        return p2.then([i](int j) { return i + j; });
      }).then([&result](int k) { result = k; });
    }
    cb1(1);
    cb2(2);
    ASSERT_EQ(result, 3);
  }
}

TEST(PromiseTests, DeferredTest) {
  std::atomic<int> s(0);
  for (int async = 0; async <= 1; async++) {
    auto call = [async](auto cb) { 
      if (async) {
        InvokeFromEventLoop([cb{std::move(cb)}] { cb(1); });
      } else {
        cb(1);
      }
    };
    auto call_void = [async](auto cb) { 
      if (async) {
        InvokeFromEventLoop([cb{std::move(cb)}] { cb(); });
      } else {
        cb();
      }
    };
    for (int value = 0; value <= 1; value++) {
      for (int another_then = 0; another_then <= 1; another_then++) {
        if (value) {
          Promise<int> promise;
          auto cb = promise.deferred();
          call(cb);
          promise = promise.then([&s](int i) { s += i; return 1; });
          if (another_then) promise.then([&s](int i) { s += i; });
        } else {
          Promise<void> promise;
          auto cb = promise.deferred();
          call_void(cb);
          promise = promise.then([&s]() { s += 1; });
          if (another_then) promise.then([&s]() { s += 1; });
        }
        if (value) {
          Promise<int> promise;
          auto cb = promise.deferred();
          promise = promise.then([&s](int i) { s += i; return 1; });
          if (another_then) promise.then([&s](int i) { s += i; });
          call(cb);
        } else {
          Promise<void> promise;
          auto cb = promise.deferred();
          promise = promise.then([&s]() { s += 1; });
          if (another_then) promise.then([&s]() { s += 1; });
          call_void(cb);
        }
        if (value) {
          std::function<void(int)> cb_outer;
          {
            Promise<int> promise;
            cb_outer = promise.deferred();
            promise = promise.then([&s](int i) { s += i; return 1; });
            if (another_then) promise.then([&s](int i) { s += i; });
          }
          call(cb_outer);
        } else {
          std::function<void()> cb_outer;
          {
            Promise<void> promise;
            cb_outer = promise.deferred();
            promise = promise.then([&s]() { s += 1; });
            if (another_then) promise.then([&s]() { s += 1; });
          }
          call_void(cb_outer);
        }
      }
    }
  }

  // Keep polling sum for expected value.
  while (int(s) != 36) { // (2 * 2 * 1 + 2 * 2 * 2) * 3 = 36
    Promise<void> promise;
    InvokeFromEventLoop(promise.deferred());
    promise.wait();
  }
}

TEST(PromiseTests, ThenTest) {
  // These tests line up with the 16 cases in then(). 
  // (Verified we actually covered the 16 implementation cases separately.)
  int cases = 0;

  { // case 1: T: resolved void, F: void -> Promise<void>
    Promise<void> p;
    p.resolve();
    p.then([] { Promise<void> p; p.resolve(); return p; })
        .then([&cases] { cases |= 1<<0; });
  }
  { // case 2: T: resolved void, F: void -> Promise<int>   
    Promise<void> p;
    p.resolve();
    p.then([] { return Promise<int>(1<<1); })
        .then([&cases](int i) {cases |= i; });
  }
  { // case 3: pending void, F: void -> Promise<void>   
    Promise<void> p;
    p.then([] { Promise<void> p; p.resolve(); return p; })
        .then([&cases] { cases |= (1<<2); });
    p.resolve();
  }
  { // case 4: T: pending void, F: void -> Promise<int>   
    Promise<void> p;
    p.then([] { return Promise<int>(1<<3); })
      .then([&cases](int i) { cases |= i; });
    p.resolve();
  }
  { // case 5: T: resolved void, F: void -> void
    Promise<void> p;
    p.resolve();
    p.then([] {}).then([&cases] { cases |= (1<<4); });
  }
  { // case 6: T: resolved void, F: void -> int
    Promise<void> p;
    p.resolve();
    p.then([] { return 1<<5; }).then([&cases](int i) {cases |= i; });
  }
  { // case 7: T: pending void, F: void -> void
    Promise<void> p;
    p.then([] {}).then([&cases] { cases |= (1<<6); });
    p.resolve();
  }
  { // case 8: T: pending void, F: void -> int
    Promise<void> p;
    p.then([] { return 1<<7; }).then([&cases](int i) { cases |= i; });
    p.resolve();
  }
  { // case 9: T: resolved int, F: T -> Promise<void>
    int j = 0;
    Promise<int> p(1<<8);
    p.then([&j](int i) { j = i; Promise<void> p; p.resolve(); return p; })
        .then([&cases, &j] { cases |= j; });
  }
  { // case 10: T: resolved int, F: T -> Promise<int>
    Promise<int> p(1<<9);
    p.then([](int i) { return Promise<int>(i); })
        .then([&cases](int i) { cases |= i; });
  }
  { // case 11: pending int, F: int -> Promise<void>   
    int j = 0;
    Promise<int> p;
    p.then([&j](int i) { j = i; Promise<void> p; p.resolve(); return p; })
        .then([&cases, &j] { cases |= j; });
    p.resolve(1<<10);
  }
  { // case 12: T: pending int, F: int -> Promise<int>   
    Promise<int> p;
    p.then([](int i) { return Promise<int>(i); })
        .then([&cases](int i) { cases |= i; });
    p.resolve(1<<11);
  }
  { // case 13: T: resolved int, F: int -> void
    int j = 0;
    Promise<int> p(1<<12);
    p.then([&j](int i) { j = i; }).then([&cases, &j] { cases |= j; });
  }
  { // case 14: T: resolved int, F: int -> int
    Promise<int> p(1<<13);
    p.then([](int i) { return i; }).then([&cases](int i) { cases |= i; });
  }
  { // case 15: T: pending int, F: int -> void
    int j = 0;
    Promise<int> p;
    p.then([&j](int i) { j = i; }).then([&cases, &j] { cases |= j; });
    p.resolve(1<<14);
  }
  { // case 16: T: pending int, F: int -> int
    Promise<int> p;
    p.then([](int i) { return i; }).then([&cases](int i) { cases |= i; });
    p.resolve(1<<15);
  }
  CHK(cases = 0xFFFF);
}

int value_2c_2 = 0;
void fn_2c_2(int a, std::function<void()>&& cb) {
  InvokeFromEventLoop([a, cb{std::move(cb)}] {
    value_2c_2 = a;
    cb();
  }); 
}

TEST(PromiseTests, MakePromiseTest) {
  Promise<void> promise_1a = MakePromise();
  promise_1a.get();

  Promise<int> promise_1b_1 = MakePromise(-101);
  ASSERT_EQ(promise_1b_1.get(), -101);

  Promise<int> promise_1b_2 = MakePromise(102);
  auto next_promise_2 = promise_1b_2.then([](int i) { return -i; });
  ASSERT_EQ(next_promise_2.get(), -102);

  Promise<int> promise_1b_3 = MakePromise(103);
  auto next_promise_3 = promise_1b_3.then([](int i) { return -i; });
  ASSERT_EQ(next_promise_3.get(), -103);

  Promise<int> promise_1b_4 = MakePromise(104).then([](int i) { return -i; });
  ASSERT_EQ(promise_1b_4.get(), -104);

  Promise<int> promise_1b_5 = MakePromise(105);
  auto next_promise_5 = promise_1b_5.then([](int i) { return -i; });
  ASSERT_EQ(next_promise_5.get(), -105);

  struct Class_2a {
    Class_2a(int a, std::function<void()>&& cb) {
      InvokeFromEventLoop([this, a, cb{std::move(cb)}] {
        value = a;
        cb();
      }); 
    }
    int value;
  };
  auto promise_2a = MakePromise<Class_2a>(201);
  auto next_promise_7 = promise_2a.then([](auto instance) {
      return -instance->value; });
  ASSERT_EQ(next_promise_7.get(), -201);

  struct Class_2b {
    void Reset(int a, std::function<void()>&& cb) {
      InvokeFromEventLoop([this, a, cb{std::move(cb)}] {
        value = a;
        cb();
      }); 
    }
    int value;
  };
  auto promise_2b = MakePromise<Class_2b>(202);
  auto next_promise_8 = promise_2b.then([](auto instance) {
      return -instance->value; });
  ASSERT_EQ(next_promise_8.get(), -202);

  struct Class_2b_2 { // two Reset functions
    void Reset(int a, std::function<void()>&& cb) {
      InvokeFromEventLoop([this, a, cb{std::move(cb)}] {
        value = a;
        cb();
      }); 
    }
    void Reset(int a, int b, std::function<void()>&& cb) {
      assert(false);
    }
    int value;
  };
  auto promise_2b_2 = MakePromise<Class_2b_2>(203);
  auto next_promise_9 = promise_2b_2.then([](auto instance) {
      return -instance->value; });
  ASSERT_EQ(next_promise_9.get(), -203);

  int value_2c_1 = 0;
  auto fn_2c_1 = [&value_2c_1](int a, std::function<void()>&& cb) {
    InvokeFromEventLoop([&value_2c_1, a, cb{std::move(cb)}] {
      value_2c_1 = a;
      cb();
    }); 
  };
  auto promise_2c_1 = MakePromise(fn_2c_1, -204).wait();
  ASSERT_EQ(value_2c_1, -204);

  auto promise_2c_2 = MakePromise(fn_2c_2, -205).wait();
  ASSERT_EQ(value_2c_2, -205);

  // Check automatic use of arena by MakePromise
  Arena arena;
  struct X {
    X(Arena* arena, std::function<void()>&&cb) { cb(); }
    int data[100];
  };
  CHK(sizeof(X) == 100 * sizeof(int));
  struct Y {
    void Reset(Arena* arena, std::function<void()>&&cb) { cb(); }
    int data[100];
  };
  CHK(sizeof(Y) == 100 * sizeof(int));
  X* prev = arena.New<X>(&arena, []{});
  X* next = arena.New<X>(&arena, []{});
  CHK((char*)next - (char*)prev == sizeof(X));
  prev = next;
  auto promisex = MakePromise<X>(&arena);
  next = arena.New<X>(&arena, []{});
  CHK((char*)next - (char*)prev == sizeof(X));
  prev = next;
  auto promisey = MakePromise<Y>(&arena);
  next = (X*)arena.New<Y>();
  CHK((char*)next - (char*)prev == sizeof(Y));
}

TEST(PromiseTests, PromiseRangeTest) {
  std::vector<int> values = {1, 2, 3, 4, 5, 6, 7};

  // values: Basic
  {
    auto promise_range = PromiseRange(values, [](int i) { 
      Promise<int> promise;
      InvokeFromEventLoop([cb{promise.deferred()}, i] {
        cb(i * 2);
      });
      return promise;
    });
    std::vector<int> result = promise_range.get();
    std::sort(result.begin(), result.end());
    CHK(result.size() == 7);
    CHK(result.back() == 14, result.back());
  }

  // values: Same but with iterator args
  {
    auto promise_range = PromiseRange(values, [](auto begin, auto end) { 
      Promise<int> promise;
      InvokeFromEventLoop([cb{promise.deferred()}, begin, end] {
        CHK(begin + 1 == end); // since default stride is 1
        cb(*begin * 2);
      });
      return promise;
    });
    std::vector<int> result = promise_range.get();
    std::sort(result.begin(), result.end());
    CHK(result.size() == 7);
    CHK(result.back() == 14, result.back());
  }

  // values: Arena and stride (note: these options are orthogonal).
  Arena arena;
  {
    long* prev = arena.New<long>();
    auto promise_range = PromiseRange(values, [](auto begin, auto end) { 
      Promise<int> promise;
      InvokeFromEventLoop([cb{promise.deferred()}, begin, end] {
        int sum = 0;
        for (auto it = begin; it != end; it++) sum += *it;
        cb(sum);
      });
      return promise;
    }, {.arena = &arena, .stride = 3});
    long* next = arena.New<long>();
    CHK((char*)next - (char*)prev ==
        sizeof(long) + sizeof(promise_internal::range_state<int>));
    std::vector<int> result = promise_range.get();
    std::sort(result.begin(), result.end());
    CHK((result == std::vector<int>{6, 7, 15})); // i.e. 1+2+3, 4+5+6, 7 (sorted)
  }

  // values: max_parallelism
  {
    std::array<int, 100> arr;
    arr.fill(7);
    std::atomic<int> in_flight(0);
    auto promise_range = PromiseRange(arr, [&in_flight](int i) {
      Promise<int> promise;
      in_flight++;
      InvokeFromEventLoop([cb{promise.deferred()}, &in_flight, i]() mutable {
        CHK(in_flight <= 5);
        in_flight--;
        cb(i);
      });
      return promise;
    }, {.max_parallelism = 5});
    std::vector<int> result = promise_range.get();
    CHK(result.size() == arr.size(), result.size());
    CHK(result.front() == 7 && result.back() == 7);
  }

  // void: Basic
  {
    std::atomic<int> sum(0);
    auto promise_range = PromiseRange(values, [&sum](int i) { 
      Promise<void> promise;
      InvokeFromEventLoop([&sum, cb{promise.deferred()}, i]() mutable {
        while(i--) ++sum;
        cb();
      });
      return promise;
    });
    promise_range.get();
    CHK((int)sum == 28, (int)sum);
  }

  // void: Same but with iterator args
  {
    std::atomic<int> sum(0);
    auto promise_range = PromiseRange(values, [&sum](auto begin, auto end) { 
      Promise<void> promise;
      InvokeFromEventLoop([&sum, cb{promise.deferred()}, begin, end]() mutable {
        for (; begin != end; begin++) sum += *begin;
        cb();
      });
      return promise;
    });
    promise_range.wait();
    CHK((int)sum == 28, (int)sum);
  }

  // void: Arena and stride (note: these options are orthogonal).
  {
    std::atomic<int> sum(0), count(0);
    long* prev = arena.New<long>();
    auto promise_range = PromiseRange(values, [&sum, &count]
        (auto begin, auto end) { 
      Promise<void> promise;
      InvokeFromEventLoop([&sum, &count, cb{promise.deferred()}, begin, end]()
          mutable {
        for (; begin != end; begin++) sum += *begin;
        count++;
        cb();
      });
      return promise;
    }, {.arena = &arena, .stride = 3});
    long* next = arena.New<long>();
    CHK((char*)next - (char*)prev ==
        sizeof(long) + sizeof(promise_internal::range_state<void>));
    promise_range.wait();
    CHK((int)sum == 28, (int)sum); 
    CHK((int)count == 3, (int)count); 
  }

  // void: max_parallelism
  {
    std::array<int, 100> arr;
    arr.fill(7);
    std::atomic<int> sum(0);
    std::atomic<int> in_flight(0);
    auto promise_range = PromiseRange(arr, [&sum, &in_flight](int i) {
      Promise<void> promise;
      in_flight++;
      InvokeFromEventLoop([cb{promise.deferred()}, &in_flight, &sum, i]() mutable {
        CHK(in_flight <= 5);
        in_flight--;
        sum += i;
        cb();
      });
      return promise;
    }, {.max_parallelism = 5});
    promise_range.get();
    CHK((int)sum == 700, (int)sum);
  }
}

TEST(PromiseTests, MoveTest) {
  struct M {
    M() = delete;
    M(int* init_count) : move_count(init_count) {}
    M(const M& rhs) {
      move_count = &default_move_count;
      // leave val = 99
    }
    M(M&& rhs) {
      move_count = rhs.move_count;
      val = rhs.val;
      (*move_count)++;
    }
    int* move_count;
    int default_move_count = 0;
    int val = 99;
  };
  {
    int move_count = 0;
    M m(&move_count);
    m.val = 42;
    Promise<M> p(m); // copy and then internal move into std::optional BEFORE then
    p.then([&move_count](const M& m) {
      CHK(move_count == 0, move_count);
      CHK(m.val == 99);
    });
    CHK(move_count == 0, move_count);
  }
  {
    int move_count = 0;
    M m(&move_count);
    Promise<M> p(std::move(m)); // move 1 into std::optional
    CHK(move_count == 1, move_count);
  }
  {
    int move_count = 0;
    M m(&move_count);
    m.val = 42;
    CHK(move_count == 0, move_count);
    // move 1 into std::optional in implied resolve in constructor BEFORE then
    Promise<M> p(std::move(m));
    CHK(move_count == 1, move_count);
    p.then([&move_count](M&& m) {
      CHK(move_count == 1, move_count);
      M m2 = std::move(m);
      CHK(move_count == 2, move_count);
      CHK(m2.val == 42);
    }); // move 2
    CHK(move_count == 2, move_count);
  }
  {
    int move_count = 0;
    M m(&move_count);
    m.val = 42;
    CHK(move_count == 0, move_count);
    Promise<M> p;
    // move 1 into std::optional in explicit resolve BEFORE then
    p.resolve(std::move(m));
    CHK(move_count == 1, move_count);
    p.then([&move_count](M&& m) {
      CHK(move_count == 1, move_count);
      M m2 = std::move(m); // move 2
      CHK(move_count == 2, move_count);
      CHK(m2.val == 42);
    }); 
    CHK(move_count == 2, move_count);
  }
  {
    int move_count = 0;
    M m(&move_count);
    m.val = 42;
    CHK(move_count == 0, move_count);
    Promise<M> p;
    auto q = p.then([&move_count](M&& m) {
      M m2 = std::move(m);  // move 2
      CHK(move_count == 1, move_count);
      CHK(m2.val == 42);
      return m2; 
    });
    q.then([&move_count](auto&& m) {
      M m3 = std::move(m);  // move 3
      CHK(move_count == 2, move_count);
      CHK(m3.val == 42);
    });
    // Since we're resolving AFTER then, there is no move into std::optional.
    p.resolve(std::move(m));
    CHK(move_count == 2, move_count);
  }
}

}  // namespace test
}  // namespace codulus

int main(int argc, char** argv) {
#ifdef STRESS
  // Stress test (passes asan)
  for(int i = 1; i <= 1'00'000; i++) {
    codulus::test::RunAllTests(true);
    if (i % 10'000 == 0) printf("Count: %d\n", i);
  }
#endif
  return codulus::test::RunAllTests();
}
