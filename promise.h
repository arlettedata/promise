#ifndef PROMISE_H_
#define PROMISE_H_

// Promise: inspired by Javascript Promise/A+ 
// Intended for builds that disable exception support (errors are up to user).

#include <assert.h>
#include <stdlib.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <iterator>
#include <vector>

#include "util/require.h"

namespace promise_internal {

struct Flags {
  static const char kHead = 0x1;
  static const char kThenAttached = 0x2;
  static const char kResolved = 0x4;
  static const char kDeferred = 0x8;
  static const char kDisableThen = 0x10;
  static const char kDisableResolveAndDeferred = 0x20;
  static const char kDisableAll = kDisableThen | kDisableResolveAndDeferred;
};

// U will be int if T is void, T otherwise
template <typename T, typename F, typename U>
inline void finalize(F* invoke, char flags, U&& resolved_value) {
  if (flags & Flags::kResolved) {
    assert((flags & Flags::kHead) && "unexpected resolve on non-head promise");
    if constexpr (!std::is_void_v<T>) {
      (*invoke)(std::forward<T>(resolved_value));
    } else {
      (*invoke)();
    }
  } else if (flags & Flags::kDeferred) {
    assert((flags & Flags::kHead) && "unexpected deferred on non-head promise");
    // We depend on the deferred callback to eventually be called,
    // which will in turn call invoke_.
  } else if (flags & Flags::kHead) {
    assert(!(flags & Flags::kThenAttached) && "promise not resolved/deferred");
    delete invoke; // this is an unused promise, so we can clean up directly
  } else {
    // Otherwise, this is a downstream promise and which must be invoked in
    // order to be cleaned up.
  }
}

template<typename T>
class PromiseBase {
 public:
  PromiseBase() {
    invoke_ = new std::function<void(T)>;
    *invoke_ = [invoke{invoke_}](T) { delete invoke; };
    flags_ = Flags::kHead;
  }

  ~PromiseBase() { finalize<T>(invoke_, flags_, std::move(resolved_value_)); }

  PromiseBase(PromiseBase<T>&& rhs) {
    this->move(std::forward<PromiseBase<T>>(rhs));
  }

  void copy(const PromiseBase<T>& rhs) {
    assert(!(rhs.flags_ & Flags::kDisableThen) && "rhs promise must be at tail");
    this->flags_ = Flags::kDisableResolveAndDeferred;
    this->invoke_ = rhs.invoke_;
    const_cast<PromiseBase<T>&>(rhs).flags_ |= Flags::kDisableThen;
  }

  void move(PromiseBase<T>&& rhs) {
    invoke_ = rhs.invoke_;
    flags_ = rhs.flags_;
    resolved_value_ = std::move(rhs.resolved_value_);
    rhs.invoke_ = nullptr;
    rhs.flags_ = Flags::kDisableAll;
  }

  template <typename X>
  void resolve(X&& x) {
    assert((flags_ & Flags::kHead) && "resolve only callable on head of chain");
    assert(!(flags_ & Flags::kDisableResolveAndDeferred) &&
        "already resolved/deferred, was moved, or is copy of another promise");
    flags_ |= Flags::kResolved | Flags::kDisableResolveAndDeferred;
    resolved_value_ = std::move(x);
  }

  auto deferred() {
    assert((flags_ & Flags::kHead) && "deferred only callable on head of chain");
    assert(!(flags_ & Flags::kDisableResolveAndDeferred) &&
        "already resolved/deferred, was moved, or is copy of another promise");
    flags_ |= Flags::kDeferred | Flags::kDisableResolveAndDeferred;
    return [&invoke{*invoke_}](T t) { invoke(std::move(t)); };
  }

 protected:
  template <typename F>
  using is_invocable = std::is_invocable<F, T&>;

  std::function<void(T)>* invoke_;
  T resolved_value_{};
  char flags_ = 0;
};

template<>
class PromiseBase<void> {
 public:
  PromiseBase() {
    invoke_ = new std::function<void()>;
    *invoke_ = [invoke{invoke_}] { delete invoke; };
    flags_ = Flags::kHead;
  }

  ~PromiseBase() { finalize<void>(invoke_, flags_, 0); }

  PromiseBase(PromiseBase<void>&& rhs) { 
    this->move(std::forward<PromiseBase<void>>(rhs)); 
  }

  void copy(const PromiseBase<void>& rhs) {
    assert(!(rhs.flags_ & Flags::kDisableThen) && "rhs must be tail of chain");
    this->invoke_ = rhs.invoke_;
    this->flags_ = Flags::kDisableResolveAndDeferred;
    const_cast<PromiseBase<void>&>(rhs).flags_ |= Flags::kDisableThen;
  }

  void move(PromiseBase<void>&& rhs) {
    invoke_ = rhs.invoke_;
    flags_ = rhs.flags_;
    rhs.invoke_ = nullptr;
    rhs.flags_ = Flags::kDisableAll;
  }

  void resolve() {
    assert((flags_ & Flags::kHead) && "resolve only callable on head of chain");
    assert(!(flags_ & Flags::kDisableResolveAndDeferred) &&
        "already resolved/deferred, was moved, or is copy of another promise");
    flags_ |= Flags::kResolved | Flags::kDisableResolveAndDeferred;
  }

  auto deferred() {
    assert((flags_ & Flags::kHead) && "deferred only callable on head of chain");
    assert(!(flags_ & Flags::kDisableResolveAndDeferred) &&
        "already resolved/deferred, was moved, or is copy of another promise");
    flags_ |= Flags::kDeferred | Flags::kDisableResolveAndDeferred;
    return [&invoke{*invoke_}] { invoke(); };
  }

 protected:
  template <typename F>
  using is_invocable = std::is_invocable<F>;
  std::function<void()>* invoke_;
  char flags_;
};

// Traits
static inline std::false_type has_base(...);
template <typename U>
static inline std::true_type has_base(const volatile PromiseBase<U>*);

template<class T, class = void>
struct has_Reset : std::false_type {};
template<class T>
struct has_Reset<T, decltype(&T::Reset, void())> : std::true_type {};

}  // namespace promise_internal

// Forward declaration of MakePromise
template <typename T> class Promise;
inline Promise<void> MakePromise();

///////////////////////////////////////////////////////////////////////////////
// Promise: building block used to construct a continuation chain of functions.
template <typename T>
class Promise : private promise_internal::PromiseBase<T> {
  // Glue
  using type = T;
  using base_type = promise_internal::PromiseBase<T>;
  using Flags = promise_internal::Flags;
  template <typename F>
  using is_invocable = typename base_type::template is_invocable<F>;
  template <typename U>
  static const bool is_promise = 
      decltype(promise_internal::has_base(std::declval<U*>()))::value;
  template <class U>
  friend class Promise;

 public:
  // Default constructor is defined.
  Promise() : base_type() {}

  // Copy construction is defined for this promise specialization, but with 
  // following enforced constraints:
  // 1. The rhs can't have a then() attached (i.e. it's a tail of a continuation
  //    chain.
  // 2. The rhs is blocked from having a then() attached; only lhs is then-able.
  // 3. The lhs can't can't call resolve() or deferred(), i.e. lhs can't invoke.
  // In other words, while both the lhs and rhs reference the same invoke_ 
  // function object, only rhs can call it and only lhs assign to it with then().
  // The reason we want this is because there are times where we want to say
  // auto q = p.then(...1); q.then(...2), which should behave like
  // p.then(...1).then(...2), but with q live and p dead.
  Promise(const Promise<T>& rhs) : base_type(std::forward<base_type>(rhs)) {}

  // Copy construction is deleted for all other promise specializations.
  template <typename X>
  Promise(const Promise<X>&, REQUIRE(is_promise<X>)) = delete;

  // Move construction is defined for this promise specialization: the lhs
  // takes on all the properties of the rhs, and the rhs is prohibited from
  // any further operations.
  Promise(Promise<T>&& rhs) : base_type(std::forward<base_type>(rhs)) {}

  // Move construction is deleted for all other promise specializations.
  template <typename X>
  Promise(Promise<X>&&, REQUIRE(is_promise<X>)) = delete;

  // Other constructors with non-promise values are the same as using resolve().
  template <typename X>
  Promise(X x, REQUIRE(!is_promise<X>)) { this->resolve(x); }
  
  // Copy assignment same as copy construction
  const Promise<T>& operator=(const Promise<T>& rhs) {
    this->copy(std::forward<base_type>(rhs));
    return *this;
  }

  // Move assignment same as move construction
  const Promise<T>& operator=(Promise<T>&& rhs) {
    this->move(std::forward<base_type>(rhs));
    return *this;
  }

  // then() method: used to chain promises with target functions to form a 
  // "continuation chain", where U is the return type of the target function.
  // If the target function returns Promise<U>, instead of having then() wrap
  // that result, i.e. Promise<Promise<U>>, wire in a second invoke so then()
  // can return Promise<U>.
  //
  // Ten constexpr cases are implemented:
  // case 0: non-void T, F(T&) not invocable -> error
  // case 1: non-void T, F(T&) is a promise to non-void type V
  // case 2: non-void T, F(T&) is a promise to void type V
  // case 3: non-void T, F(T&) is a non-promise/non-void type U
  // case 4: non-void T, F(T&) is a void type U
  // case 5: void T, F() not invocable -> error
  // case 6: void T, F() is a promise to non-void type V
  // case 7: void T, F() is a promise to void type V
  // case 8: void T, F() is a non-promise/non-void type U
  // case 9: void T, F() is a void type U
  //
  template <typename F>
  auto then(F&& target, REQUIRE(!is_invocable<F>::value)) {
    // If the following assertion fires, a continuation passed into a then()
    // handler takes an argument that cannot be obtained by converting the 
    // invoked promise type.  The clang compiler provides a helpful note that 
    // indicates the both the promise type and the incompatible argument type.
    // For example: Promise<int> f; f.then([](std::string) {}); is an error
    // because there is no implicit conversion from int to std::string.
    // Since it's improtant that the note is easily seen (type mismatches 
    // can sometimes be obscure), we inhibit clang from moving the static_assert
    // message to the next line.
    // clang-format off
    static_assert(is_invocable<F>::value, "The conflicting T type and then() function are both given in the following note:");
    // clang-format on
  }
  template <typename F>
  auto then(F&& target, REQUIRE(is_invocable<F>::value)){
    assert(!(this->flags_ & Flags::kDisableThen) &&
        "then() already attached or promise was copied or moved");
    this->flags_ |= Flags::kDisableThen;
    if constexpr (!std::is_void_v<T>) {
      if constexpr (is_promise<std::invoke_result_t<typename std::decay<F>::type, T&>>) {
        using U = std::invoke_result_t<typename std::decay<F>::type, T&>;
        using V = typename U::type;
        Promise<V> promise;
        promise.flags_ = 0;
        *this->invoke_ = [this_invoke{this->invoke_},
                          &next_invoke{*promise.invoke_},
                          target{std::move(target)}](T t) mutable {
          U u = target(t);
          if constexpr (!std::is_void_v<V>) {
            u.then([&next_invoke](V v) mutable { next_invoke(v); });
          } else {
            u.then([&next_invoke]() { next_invoke(); });
          }
          delete this_invoke;
        };
        return promise;
      } else { // T is non-void, U is non-promise
        using U = std::invoke_result_t<F, T&>;
        Promise<U> promise;
        promise.flags_ = 0;
        *this->invoke_ = [this_invoke{this->invoke_},
                          &next_invoke{*promise.invoke_},
                          target{std::move(target)}](T t) mutable {
          if constexpr (!std::is_void_v<U>) {
            next_invoke(target(t));
          } else {
            target(t);
            next_invoke();
          }
          delete this_invoke;
        };
        return promise;
      }
    } else { // T is void
      if constexpr (is_promise<std::invoke_result_t<F>>) {
        using U = std::invoke_result_t<F>;
        using V = typename U::type;
        Promise<V> promise;
        promise.flags_ = 0;
        *this->invoke_ = [this_invoke{this->invoke_},
                          &next_invoke{*promise.invoke_},
                          target{std::move(target)}]() mutable {
          U u = target();
          if constexpr (!std::is_void_v<V>) {
            u.then([&next_invoke](V v) mutable { next_invoke(v); });
          } else {
            u.then([&next_invoke]() { next_invoke(); });
          }
          delete this_invoke;
        };
        return promise;
      } else { // T is void, U is non-promise
        using U = std::invoke_result_t<F>;
        Promise<U> promise;
        promise.flags_ = 0;
        *this->invoke_ = [this_invoke{this->invoke_},
                          &next_invoke{*promise.invoke_},
                          target{std::move(target)}]() mutable {
          if constexpr (!std::is_void_v<U>) {
            next_invoke(target());
          } else {
            target();
            next_invoke();
          }
          delete this_invoke;
        };
        return promise;
      }
    }
  }

  // deferred() method
  // This function returns a callback that is later called to invoke the start
  // of a continuation chain. It is intended for cases where callbacks are 
  // made asynchronously.
  // Note: if this callback is not made, memory is leaked.
  using promise_internal::PromiseBase<T>::deferred;

  // resolve() and resolve(T t) methods
  // This function is used to synchronously invoke the start of a continuation
  // chain. It is often used trivially such as providing promise wrappers
  // for values that are already in scope, i.e. by using MakePromise(T). 
  // It is important to note that the continuation chain is not actually called
  // until the Promise object goes out of scope, because we want that chain to
  // be finalized before invoking.
  using promise_internal::PromiseBase<T>::resolve;

  auto get() {
    std::mutex mutex;
    std::condition_variable cv;
    if constexpr (!std::is_void_v<T>) {
      T result;
      then([&cv, &result](T t) { cv.notify_one(); result = std::move(t); });
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock);
      return result;
    } else {
      then([&cv]() { cv.notify_one(); });
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock);
      return;
    }
  }

  // wait() is like get() but returns a promise for more chaining
  // ForAll(...).wait().then([](std::vector<...> values)) { ... });
  // It can also be used in lieu of Promise<void>.get() which is ill-defined.
  auto wait() {
    if constexpr (!std::is_void_v<T>) {
      return MakePromise(get());
    } else {
      get();
      return MakePromise();
    }
  }
};

///////////////////////////////////////////////////////////////////////////////
// MakePromise: a collection of overloads addressing several use cases:
// 1a. To take a value and create a promise that synchronously resolves
//     to it. This is typically used in then() bodies that returns promises
//     in some of its conditional branches. (Note: then() will flatten 
//     nested promises: i.e. if the body of then() returns Promise<Promise<T>>,
//     then() itself return a Promise<T>).
// 1b. To return a Promise<void>, a degenerate case of 1a.
// 2. To turn a type that has an asynchronous callback, where callback is the
//    the last argument in a.) a Reset method, or b.) a constructor.

// Use case 1a.
template <typename T>
inline Promise<T> MakePromise(T t) {
  Promise<T> promise;
  promise.resolve(std::move(t));
  return promise;
}

// Use case 1b.
inline Promise<void> MakePromise() {
  Promise<void> promise;
  promise.resolve();
  return promise;
}

// Use case 2a/2b.
template <typename T, typename... Args>
Promise<std::shared_ptr<T>> MakePromise(Args... args) {
  T* pt;
  Promise<void> cb;
  if constexpr (promise_internal::has_Reset<T>::value) {
    pt = new T();
    pt->Reset(args..., cb.deferred());
  } else {
    pt = new T(args..., cb.deferred());
  }
  return cb.then([pt] { return std::shared_ptr<T>(pt); });
}

///////////////////////////////////////////////////////////////////////////////
// ForAll: applies iterated iterated elements to given function that maps each
// to a promise. When all of these are resolved, the returned
// Promise<std::shared<std:vector<T>>> is resolved.
template <typename T, typename Iterator, typename Promise_Fn>
inline Promise<std::vector<T>> ForAll(Iterator begin, Iterator end,
                                      Promise_Fn&& promise_fn) {
  size_t count = std::distance(begin, end);
  if (count == 0) return MakePromise(std::vector<T>{});
  Promise<std::vector<T>> promise;
  auto cb = promise.deferred();
  std::vector<T>* values = new std::vector<T>();
  std::mutex* mutex = new std::mutex();
  while (begin != end) {
    promise_fn(*begin++).then([cb, mutex, values, count](T t) mutable {
      bool done;
      {
        std::lock_guard<std::mutex> lock(*mutex);
        values->push_back(std::move(t));
        done = values->size() == count;
      }
      if (done) {
        std::vector<T> ret;
        ret.swap(*values);
        delete values;
        delete mutex;
        cb(std::move(ret));
      }
    });
  }
  return promise;
}

// Version of ForAll that just returns Promise<void> when each of the iterated
// Promise<void> instances are resolved.
template<typename Iterator, typename Promise_Fn>
Promise<void> ForAll(Iterator begin, Iterator end, Promise_Fn&& promise_fn) {
  size_t count = std::distance(begin, end);
  if (count == 0) return MakePromise();
  Promise<void> promise;
  auto cb = promise.deferred();
  auto remaining = new std::atomic<int>(count);
  while (begin != end) {
    promise_fn(*begin++).then([cb, remaining]() mutable {
      if (--*remaining == 0) {
        delete remaining; 
        cb();
      }
    });
  }
  return promise;
}

#endif  // PROMISE_H_
