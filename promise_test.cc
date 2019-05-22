#include "promise.h"

// Note: this file shared for expository purposes only. 
// It is not buildable as a standalone.  That may improve.
#include "util/print.h"
#include "util/cluster.h"
#include "util/dynamodb.h"

namespace codulus {

#undef ERROR_CASE
#define ERROR_CASE 0
void StaticAssertionsTest() {
  Print("TEST: StaticAssertionsTest\n");
#if ERROR_CASE == 1
  // Conflicting promise type and then function.
  Promise<int> p0(0);
  p0.then([](std::string) {
    return Promise<int>();
  });
#endif
}

// Enable selected tests to test CHK constraints.
#define ERROR_CASE 0
void RuntimeAssertionsTest() {
  Print("TEST: RuntimeAssertionsTest\n");
#if ERROR_CASE == 1
  // Calling then() twice
  Promise<void> p;
  p.then([]{});
  p.then([]{});
#elif ERROR_CASE == 2
  // Calling resolved/deferred() twice
  Promise<void> p;
  p.deferred();
  p.resolve();
#elif ERROR_CASE == 3
  // Copying a non-tail promise
  Promise<int> p(1);
  Promise<void> q = p.then([](int){});
  Promise<int> r = p;
#elif ERROR_CASE == 4
  // then() after copy
  Promise<int> p(1);
  Promise<int> q = p;
  p.then([](int){});
#elif ERROR_CASE == 5
  // resolve() after copy
  Promise<int> p(1);
  Promise<int> q = p;
  q.resolve(2);
#elif ERROR_CASE == 6
  // resolve() after move
  Promise<int> p(1);
  Promise<int> q = std::move(p);
  p.resolve(2);
#elif ERROR_CASE == 7
  // then() after move
  Promise<int> p(1);
  Promise<int> q = std::move(p);
  p.then([](int){});
#endif
// TODO: fill out every case: move/copy/assign/conversion void-vs-non-void
}

void MemoryLeakTest() {
  Print("TEST: MemoryLeakTest\n");

  // The first statement should not cause a leak because we know there is no
  // invocation needed.  We are able to free its associated invocation handler
  // because we know it won't be called.
  Promise<void> p; // for memory leak check under asan

#if 0 // enable to see memory leak (by design) using asan
  // The second statement causes a by-design leak since we create a downstream
  // promise with then() that has no guarantee nor knowledge of whether it 
  // will get invoked.  This is really considered ill-formed code, and there
  // are similiar memory leak manifestations in Idina where we expected to be
  // called back so we can free memory.
  Promise<void> q = p.then([]{}); // Enable under asan to see the leak
#endif
}

void ConversionsTest() {
  Print("TEST: ConversionTest\n");
  int s = 0;
  {
    // Pass 1: non-void promises
    Promise<int> p1 = 1;
    p1.then([&s](int v) { s++; CHK(v == 1); });

    Promise<int> p2(2ULL);
    p2.then([&s](int v) { s++; CHK(v == 2); });
  
    int y = 3;
    int& x = y;
    Promise<int> p3(x);
    p3.then([&s](int v) { s++; CHK(v == 3); });

    // Conversion operator
    struct C { 
        C() : C(0) {}
        C(int x) : x_(x) {}
        operator int() { return x_; } 
        int x_;
    };

    C c(4);
    Promise<int> p4(c);
    p4.then([&s](int v) { s++; CHK(v == 4); });

    int z = 5;
    Promise<int> p5(std::move(z));
    p5.then([&s](int v) { s++; CHK(v == 5); });

    double d = 6;
    Promise<int> p6(d);
    p6.then([&s](int v) { s++; CHK(v == 6); });

    // Move constructor
    Promise<int> p7(7);
    Promise<int> p8(std::move(p7));
    p8.then([&s](int v) { s++; CHK(v == 7, v); });

    // Copy constructor, with extra then to make things more interesting
    Promise<int> p9(0);
    Promise<int> p10(p9.then([](int) { return 8; }));
    p10.then([&s](int v) { s++; CHK(v == 8, v); });

    // Assignment operator, with extra then to make things more interesting
    Promise<int> p11(0);
    Promise<int> p12 = p11.then([](int) { return 9; });
    p12.then([&s](int v) { s++; CHK(v == 9, v); });

    // Move assignment operator
    Promise<int> p13(10);
    Promise<int> p14 = std::move(p13);
    p14.then([&s](int v) { s++; CHK(v == 10, v); });

    // Class with conversions in and out 
    Promise<C> p15(11);
    p15.then([&s](int v) { s++; CHK(v == 11); });

    // Class, copy value
    Promise<C> p16(C(12));
    p16.then([&s](C c) { s++; CHK((int)c == 12); });

    // Class, move value
    C c17(13);
    Promise<C> p17(std::move(c17));
    p17.then([&s](C& c) {s++; CHK((int)c == 13, (int)c); });
  }
  CHK(s == 13, s);
}

void UniqueAndSharedPtrTest() {
  Print("TEST: UniqueAndSharedPtrTest\n");
  // std::unique_ptr does not have a copy constructor, so 
  // p.then([&x](std::unique_ptr<int> i) {...} does not compile.
  // So if we insist on having std::unique_ptr passed down, we have to
  // reference an instance that belongs to the caller. (rvalue args, i.e. &&)
  // are not supported).
  // Because of the inability to take ownership of the resulting object,
  // we prefer to use std::shared_ptr which can be copied.
  // Note: as a side-demonstration, we show that the order of resolve within
  // the lifetime scope of the Promise is not significant. 
  int x = 0;
  {
    Promise<std::unique_ptr<int>> p1;
    p1.resolve(std::make_unique<int>(10));
    p1.then([&x](const std::unique_ptr<int>& i) {x += *i;});

    // The following is not supported.
    //Promise<std::unique_ptr<int>> p2;
    //p2.then([&x](std::unique_ptr<int>&& i) {x += *i;});
    //p2.resolve(std::make_unique<int>(10));

    Promise<std::shared_ptr<int>> p3;
    p3.then([&x](std::shared_ptr<int> i) {x += *i;});
    p3.resolve(std::make_shared<int>(20));
  }

  CHK(x == 30, x);
}

void ChainTest() {
  Print("TEST: ChainTest\n");
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
  CHK(v == 271.5, v);
}

void CoalescenceTest() {
  Print("TEST: CoalescenceTest\n");

  std::function<void(int)> cb;
  double result = 0;

  {
    Promise<double> p1;
    p1.resolve(1);
    Promise<double> coalesced = p1.then([&cb](int v1) {
      Promise<int> p2;
      cb = p2.deferred();
      Promise<double> p3 = p2.then([v1](int v2) {
        return (double)(v1 + v2);
      }); 
      return p3;
    });
    coalesced.then([&result](double sum) {
      result = sum;
    });
  }
  cb(2);
  CHK(result == 3.0, result);
}

void NestedTest() {
  Print("TEST: NestedTest\n");

  int result = 0;
  std::function<void(int)> cb1;
  std::function<void(int)> cb2;

  {
    Promise<int> p1;
    cb1 = p1.deferred();
    p1.then([&cb2](int i) {
      Promise<int> p2;
      cb2 = p2.deferred();
      return p2.then([i](int j) {
        return i + j;
      });
    }).then([&result](int k) { result = k; });
  }
  cb1(1);
  cb2(2);
  CHK(result == 3, result);
}

void MakePromiseViaConstructorTest() {
  Print("TEST: MakePromiseViaConstructorTest\n");

  // MakePromise on type with callback in constructor.
  // This test assumes that table "xyz" does not exist.
  std::string response = MakePromise<DynamoDbDescribeTable>("xyz")
  .then([](auto describe) {
    return describe->response().ToString();
  }).get();
  CHK(response.find("xyz not found") != response.size());
}

void MakePromiseViaResetTest() {
  Print("TEST: MakePromiseViaResetTest\n");

  // MakePromise on type using default constructor and callback in Reset method, 
  // This test assumes there's at least one cluster running.
  ListClusters::Options options{};
  auto clusters = MakePromise<ListClusters>(options).get();
  CHK(clusters->clusters().size() > 0);
}

// TODO: more tests:
//   Promise<void|T> constructors/operators
//   different function types (std::function, c-style, operator())
//   all error checks
//   ForAll

void Main() {
  StaticAssertionsTest(); 
  RuntimeAssertionsTest();
  MemoryLeakTest();
  ConversionsTest();
  UniqueAndSharedPtrTest();
  ChainTest();
  CoalescenceTest();
  NestedTest();
  MakePromiseViaConstructorTest();
  MakePromiseViaResetTest();
  
  Print("TESTS OK\n");
}

}  // namespace codulus

int main(int argc, char** argv) {
  codulus::Main();
  return 0;
}
