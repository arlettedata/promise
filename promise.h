#ifndef UTIL_PROMISE_H_
#define UTIL_PROMISE_H_

////////////////////////////////////////////////////////////////////////////////
// Promise: inspired by Javascript Promises/A+
//
// Customized for Idina:
//   * uses Arena memory where possible
//   * adapts to existing Class::Reset() idiom
//   * implements maximum parallelism in PromiseRange, with grouping to support
//     certain AWS batching scenarios
//   * no exceptions (i.e. Promise::reject not implemented)
// 
// Overview of public methods and functions:
//   Promise: builder of continuation chains that asynchronously returns values
//     Promise::resolve
//     Promise::deferred
//     Promise::then
//     Promise::get
//     Promise::wait
//   MakePromise:  wrap values and Idina functions with Promise instances
//   PromiseRange: given a vector of values and a mapping from those values to
//                 promises, wait until all those promises are resolved  
//
// See further comments for more details.
//
#include <stdlib.h>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <vector>

#include "util/arena.h"
#include "util/require.h"
#include "util/semaphore.h"

namespace codulus {
template <typename T> class Promise;
namespace promise_internal {

template<typename T, typename = void>
struct resolved_value {
  using type = std::optional<bool>;
};
template <typename T>
struct resolved_value<T, std::enable_if_t<!std::is_void_v<T>>> {
  using type = std::optional<T>;
};

template<typename T, typename = void>
struct continuation {
  using type = std::function<void()>;
};
template <typename T>
struct continuation<T, std::enable_if_t<!std::is_void_v<T> && 
    !std::is_copy_constructible_v<T>>> {
  static const bool always_move = true;
  static const bool always_copy = false;
  using type = std::function<void(T)>;
};
template <typename T>
struct continuation<T, std::enable_if_t<!std::is_void_v<T> &&
    std::is_copy_constructible_v<T> && std::is_scalar_v<T>>> {
  static const bool always_move = false;
  static const bool always_copy = true;
  using type = std::function<void(T)>;
};
template <typename T>
struct continuation<T, std::enable_if_t<!std::is_void_v<T> &&
    std::is_copy_constructible_v<T> && !std::is_scalar_v<T> &&
    !std::is_const_v<T> && std::is_reference_v<T>>> {
  static const bool always_move = false;
  static const bool always_copy = false;
  using type = std::function<void(T&)>;
};
template <typename T>
struct continuation<T, std::enable_if_t<!std::is_void_v<T> &&
    std::is_copy_constructible_v<T> && !std::is_scalar_v<T> &&
    std::is_const_v<T> && std::is_reference_v<T>>> {
  static const bool always_move = false;
  static const bool always_copy = false;
  using type = std::function<void(const T&)>;
};
template <typename T>
struct continuation<T, std::enable_if_t<!std::is_void_v<T> &&
    std::is_copy_constructible_v<T> && !std::is_scalar_v<T> &&
    !std::is_reference_v<T>>> {
  static const bool always_move = false;
  static const bool always_copy = false;
  using type = std::function<void(T&&)>;
};

template<typename F, typename T, typename = void>
struct is_invocable : std::is_invocable<F> {};
template<typename F, typename T>
struct is_invocable<F, T, std::enable_if_t<!std::is_void_v<T>>> :
    std::is_invocable<F, T&&> {};
template <typename F, typename T>
const bool is_invocable_v = is_invocable<F, T>::value;

static inline std::false_type is_promise(...);
template <typename U>
static inline std::true_type is_promise(const volatile Promise<U>*);
template <typename U>
const bool is_promise_v = decltype(is_promise(
    std::declval<typename std::decay<U>::type*>()))::value;

template <typename T>
struct promise_state {
  template <typename U = T, typename = std::enable_if_t<std::is_void_v<U>>>
  void resolve() {
    std::unique_lock<std::mutex> lock(mutex);
    if (fn) {
      typename continuation<T>::type fn;
      std::swap(this->fn, fn);
      lock.unlock();
      fn();
    } else {
      assert(!is_resolved());
      resolved_value = true;
    }
  };

  template <typename V>
  void resolve(V&& v) {
    std::unique_lock<std::mutex> lock(mutex);
    if (fn) {
      typename continuation<T>::type fn;
      std::swap(this->fn, fn);
      lock.unlock();
      if constexpr (continuation<T>::always_move) {
        fn(std::move(v));
      } else if constexpr (continuation<T>::always_copy ||
                           std::is_lvalue_reference_v<V>) {
        fn(T(v));
      } else {
        fn(std::forward<V>(v));
      }
    } else {
      assert(!is_resolved());
      if constexpr (std::is_lvalue_reference_v<V>) {
        resolved_value.template emplace<T>(T(v));
      } else {
        resolved_value.template emplace<T>(std::forward<V>(v));
      }
    }
  }

  bool is_resolved() { return resolved_value.has_value(); }

  std::mutex mutex;
  typename continuation<T>::type fn;
  typename resolved_value<T>::type resolved_value;
};

}  // namespace promise_internal

inline Promise<void> MakePromise();
template <typename T>
inline Promise<std::remove_cvref_t<T>> MakePromise(T&& t);

///////////////////////////////////////////////////////////////////////////////
//
// Promise: building block used to construct a continuation chain of functions
//          with support for asynchronous callbacks.
//
template <typename T>
class Promise {
  template <typename U>
  static const bool is_promise_v = promise_internal::is_promise_v<U>;
  template <typename F>
  static const bool is_invocable_v = promise_internal::is_invocable_v<F, T>;
  static_assert(!std::is_reference_v<T>, "Promised values cannot be references.");
  static_assert(!is_promise_v<T>, "Promised values cannot be promises.");

 public:
  using type = T;

  // Default constructor is defined.
  Promise() : state_(new promise_internal::promise_state<T>) {}

  // Move constructors for non-promise values are defined to be resolved.
  template <typename V, typename = std::enable_if_t<!std::is_void_v<V>>>
  Promise(V&& v) : Promise() { state_->resolve(std::forward<V>(v)); }

  // Move/copy constructors are deleted for all other promise specializations.
  template <typename U>
  Promise(Promise<U>&&, REQUIRE(is_promise_v<U>)) = delete;

  //////////////////////////////////////////////////////////////////////////////
  // resolve() and resolve(V)
  // Insinuate a callback to a chained then() with the provided value, if any.
  // Specifically: if there is a continuation, propagate the value now; 
  // otherwise, store the value in the promise state for later chaining.
  //          
  template <typename U = T, typename = std::enable_if_t<std::is_void_v<U>>>
  void resolve() { state_->resolve(); }
  template <typename V>
  void resolve(V&& v) { state_->resolve(std::forward<V>(v)); }

  //////////////////////////////////////////////////////////////////////////////
  // deferred()
  //
  // Obtain a callback that can be used to resolve later.
  //
  auto deferred() {
    if constexpr (std::is_void_v<T>) {
      return [state{state_}] { return state->resolve(); };
    } else {
      // Ideally, we would say T&&/std::forward but Idina callbacks would have
      // to change to use std::forward<T>. Breaking/requiring is not worth it.
      return [state{state_}](T t) { return state->resolve(std::move(t)); };
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  // then(F: T -> X)
  //
  // Specify a continuation function to be called with this promise is resolved.
  // X can be void, non-void, Promise<void>, or Promise<V> for non-void V
  // 
  template <typename F>
  auto then(F&& fn, REQUIRE(!is_invocable_v<F>)) {
    // If the following assertion fires, a continuation passed into a then()
    // handler takes an argument that cannot be obtained by converting the
    // subject promise type T.
    // For example: Promise<int> f; f.then([](std::string) {}); is an error
    // The clang compiler provides a helpful note that we encourage devs to see
    // by disabling clang-format from wrapping the static_assert message.
    // clang-format off
    static_assert(is_invocable_v<F>, "The conflicting T type and then() function are both given in the following note:");
    // clang-format on
  }
  template <typename F>
  auto then(F&& fn, REQUIRE(is_invocable_v<F>)){
    // There are a total of 16 cases (four of which are folded into two blocks):
    // T: resolved void | resolved non-void | pending void | pending non-void
    // F: T -> void | non-void | Promise<void> | Promise<non-void> 
    std::unique_lock<std::mutex> lock(state_->mutex);
    if constexpr (std::is_void_v<T>) {
      if constexpr (is_promise_v<std::invoke_result_t<F>>) {
        if (state_->is_resolved()) {
          // 1/2) T:      resolved void
          //      F:      void -> Promise<V> for void or non-void V
          //      action: eval fn(void) now
          //      return: pending Promise<V>
          lock.unlock();
          return fn();
        } else {
          using U = std::invoke_result_t<F>;
          using V = typename U::type;
          Promise<V> promise;
          // Note: state_->fn lambda captures are mutable because fn can be a
          //       mutable lambda capture.
          state_->fn = [next=promise.state_, fn=std::move(fn)]() mutable {
            if constexpr (std::is_void_v<V>) {
              // 3) T:      pending void
              //    F:      void -> Promise<void>
              //    action: eval fn() when T is resolved and with its result,
              //            chain T's resolution
              //    return: pending Promise<void> (this is an unwrapping action)
              fn().then([next] { next->resolve(); });
            } else {
              // 4) T:      pending void
              //    F:      void -> Promise<V> for non-void V
              //    action: eval fn() when T is resolved and with its result,
              //            chain T's resolution
              //    return: pending Promise<V> (this is an unwrapping action)
              fn().then([next](V&& v) { next->resolve(std::forward<V>(v)); });
            }
          };
          return promise;
        }
      } else {
        using U = std::invoke_result_t<F>;
        if (state_->is_resolved()) {
          lock.unlock();
          if constexpr (std::is_void_v<U>) {
              // 5) T:      resolved void
              //    F:      void -> void
              //    action: eval fn() now
              //    return: resolved Promise<void>
            fn();
            return MakePromise();
          } else {
              // 6) T:      resolved void
              //    F:      void -> V for non-void V
              //    action: eval v = fn() now
              //    return: resolved Promise<V>(v)
            return MakePromise(fn());
          }
        } else {
          Promise<U> promise;
          state_->fn = [next=promise.state_, fn=std::move(fn)]() mutable {
            if constexpr (std::is_void_v<U>) {
              // 7) T:      pending void
              //    F:      void -> void
              //    action: eval fn() when T is resolved
              //    return: pending Promise<void> that will automaticaly resolve
              //            to void
              fn();
              next->resolve();
            } else {
              // 8) T:      pending void
              //    F:      void -> V for non-void V
              //    action: eval v = fn() when T is resolved
              //    return: pending Promise<V> that will automatically resolve
              //            to v
              next->resolve(fn());
            }
          };
          return promise;
        }
      }
    } else {
      if constexpr (is_promise_v<std::invoke_result_t<F, T&&>>) {
        using U = std::invoke_result_t<F, T&&>;
        if (state_->is_resolved()) {
          // 9/10) T:      resolved T
          //       F:      T -> Promise<V> for void or non-void V
          //       action: eval fn(T)
          //       return: pending Promise<V>
          lock.unlock();
          return fn(std::move(*state_->resolved_value));
        } else {
          using V = typename U::type;
          Promise<V> promise;
          state_->fn = [next=promise.state_, fn=std::move(fn)](T&& t) mutable {
            if constexpr (std::is_void_v<V>) {
              // 11) T:      pending T
              //     F:      T -> Promise<void>
              //     action: eval fn() when T is resolved and with its result,
              //             chain T's resolution
              //     return: pending Promise<void> (this is an unwrapping action)
              fn(std::forward<T>(t)).then([next] { next->resolve(); });
            } else {
              // 12) T:      pending T
              //     F:      T -> Promise<V> for non-void V
              //     action: eval fn() when T is resolved and with its result,
              //             chain T's resolution
              //     return: pending Promise<V> (this is an unwrapping action)
              fn(std::forward<T>(t)).then([next](V&& v) mutable {
                next->resolve(std::forward<V>(v));
              });
            }
          };
          return promise;
        }
      } else {
        using U = std::invoke_result_t<F, T&&>;
        if (state_->is_resolved()) {
          lock.unlock();
          if constexpr (std::is_void_v<U>) {
            // 13) T:      resolved T
            //     F:      T -> void
            //     action: eval fn(T) now
            //     return: resolved Promise<void>
            fn(*std::move(state_->resolved_value));
            return MakePromise();
          } else {
            // 14) T:      resolved T
            //     F:      T -> V for non-void V
            //     action: eval fn(T) now
            //     return: resolved Promise<V>
            return MakePromise(fn(*std::move(state_->resolved_value)));
          }
        } else {
          Promise<U> promise;
          state_->fn = [next=promise.state_, fn=std::move(fn)](T&& t) mutable {
            if constexpr (std::is_void_v<U>) {
              // 15) T:      pending T
              //     F:      T -> void
              //     action: eval fn(T) when T is resolved
              //     return: pending Promise<void> that will automatically
              //             resolve to void
              fn(std::forward<T>(t));
              next->resolve();
            } else {
              // 16) T:      pending T
              //     F:      T -> V for some non-void V
              //     action: eval v = fn(T) when T is resolved
              //     return: pending Promise<V> that will automatically
              //             resolve to v
              next->resolve(fn(std::forward<T>(t)));
            }
          };
          return promise;
        }
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  // get()
  //
  // Obtain the resolved value of this promise, blocking as needed.
  //
  auto get() {
    if (state_->is_resolved()) {
      if constexpr (!std::is_void_v<T>) {
        return *std::move(state_->resolved_value);
      }
    } else {
      Semaphore sem;
      if constexpr (std::is_void_v<T>) {
        then([&sem]() mutable { sem.post(); });
        sem.wait();
      } else {
        T result;
        then([&sem, &result](T&& t) mutable {
          result = std::move(t);
          sem.post();
        });
        sem.wait();
        return std::move(result);
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  // wait()
  //
  // Use get() to get the resolved promise (blocking as needed), and create
  // a resolved promise to result. Often used at a tail of a Promise<void>
  // continuation chain as get() on a Promise<void> is somewhat non-idiomatic.
  //
  auto wait() {
    if constexpr (std::is_void_v<T>) {
      get();
      return MakePromise();
    } else {
      return MakePromise(get());
    }
  }

 private:
  template <class U>
  friend class Promise;
  std::shared_ptr<promise_internal::promise_state<T>> state_;
};

///////////////////////////////////////////////////////////////////////////////
// MakePromise
//
// This is a collection of overloads addressing several interop scenarios.
// 1. To wrap a void or non-void values with a synchronously resolved promise.
//    This is typically used in then() bodies that returns promises
//    in some of its conditional branches. (Note: then() will unwrap
//    nested promises: i.e. if the body of then() returns Promise<Promise<T>>,
//    then() itself return a Promise<T>.)
// 2. To wrap an asynchronous callback with Promise<void>, where callback is
//    found as the last argument in a a.) Reset method, b.) constructor, or
//    c.) function.
//    In cases 2a and 2b, allocate using Arena* whenever it is passed as the
//    first argument.
//
inline Promise<void> MakePromise() {
  // Case 1a: void -> Promise<void>
  Promise<void> promise;
  promise.resolve();
  return promise;
}
template <typename T>
inline Promise<std::remove_cvref_t<T>> MakePromise(T&& t) {
  // Case 1b: T -> Promise<T>
  using U = std::remove_reference_t<T>;
  return Promise<U>(std::forward<U>(t));
}

namespace promise_internal {
template<typename C, typename... Args>
struct has_Reset {
  template<typename C_>
  static constexpr auto check(C_*) ->
      decltype(std::declval<C_>().Reset(std::declval<Args>()..., 
      std::declval<std::function<void()>>), std::true_type());
  template<typename>
  static constexpr std::false_type check(...);
  static constexpr bool value = decltype(check<C>(nullptr))::value;
};
}  // namespace promise_internal

template <typename T, typename... Args,
    typename = std::enable_if_t<std::is_class_v<T>>>
auto MakePromise(Args&&... args) {
  Promise<void> promise;
  T* pt;
  if constexpr (std::is_same_v<Arena*,
      typename std::tuple_element<0, std::tuple<Args&&...>>::type>) {
    Arena* arena = std::get<0>(
        std::forward_as_tuple(std::forward<Args>(args)...));
    if constexpr (promise_internal::has_Reset<T, Args...>::value) {
      // Case 2a (arena): Class::Reset(arena, args..., cb) -> Promise<Class*>
      pt = arena->template New<T>();
      pt->Reset(std::forward<Args>(args)..., std::move(promise.deferred()));
    } else {
      // Case 2b (arena): Class::Class(arena, args... cb) -> Promise<Class*>
      pt = arena->template New<T>(std::forward<Args>(args)...,
          std::move(promise.deferred()));
    }
    return promise.then([pt] { return pt; });
  } else {
    if constexpr (promise_internal::has_Reset<T, Args...>::value) {
      // Case 2a: Class::Class(args..., cb) -> Promise<std::unique_ptr<Class>>
      pt = new T();
      pt->Reset(std::forward<Args>(args)..., std::move(promise.deferred()));
    } else {
      // Case 2a: Class::Reset(args..., cb) -> Promise<std::unique_ptr<Class>>
      pt = new T(std::forward<Args>(args)..., std::move(promise.deferred()));
    }
    return promise.then([pt] { return std::unique_ptr<T>(pt); });
  }
}

template <typename F, typename... Args,
    typename = std::enable_if_t<std::is_function_v<F(Args&&...)>>>
Promise<void> MakePromise(F&& fn, Args&&... args) {
  // Case 2c: fn(args..., cb) -> Promise<void>
  // Note: assumes callback is std::function<void()>. The case where we get
  //       back with a value, i.e. std::function<void(T)>, is NYI.
  Promise<void> promise;
  fn(std::forward<Args>(args)..., std::move(promise.deferred()));
  return promise;
}

///////////////////////////////////////////////////////////////////////////////
// PromiseRange
//
// Applies iterated iterated elements to given function that maps each to a
// promise. When all of these are resolved, the returned Promise<std:vector<T>>
// is resolved.  If T is void, we return Promise<void>.
//
// Optional behavior is specified with PromiseRangeOptions:
struct PromiseRangeOptions {
  size_t max_parallelism = 0; // set to 0 for unlimited
  size_t stride = 1;          // make inner promises on ranges of size stride
  Arena* arena = nullptr;     // optional Arena allocation of internal state
};

namespace promise_internal {

template <typename T>
struct range_state {
  range_state(const PromiseRangeOptions& options, size_t count_in,
      std::function<void(std::vector<T>)>&& done) : options(options),
      in_flight(options.max_parallelism), count_in(count_in),
      done(std::move(done)) {}
  PromiseRangeOptions options;
  Semaphore in_flight;
  std::atomic<size_t> count_in;
  std::function<void(std::vector<T>)> done;
  std::mutex mutex;
  std::vector<T> values;
};

template <>
struct range_state<void> {
  range_state(const PromiseRangeOptions& options, size_t count_in,
      std::function<void()>&& done) : options(options),
      in_flight(options.max_parallelism), count_in(count_in),
      done(std::move(done)) {}
  PromiseRangeOptions options;
  Semaphore in_flight;
  std::atomic<size_t> count_in;
  std::function<void()> done;
};

template<typename C, typename F, typename = void>
struct has_iter_args : std::false_type {};
template <typename C, typename F>
struct has_iter_args<C, F, std::enable_if_t<std::is_invocable_v<F,
    typename C::const_iterator, typename C::const_iterator>>> :
    std::true_type {};
template<typename C, typename F, typename I = typename C::const_iterator>
auto call_map(F& map, I begin, I end, REQUIRE(!has_iter_args<C, F>::value)) {
  return map(*begin);
}
template<typename C, typename F, typename I = typename C::const_iterator>
auto call_map(F& map, I begin, I end, REQUIRE(has_iter_args<C, F>::value)) {
  return map(begin, end);
}

}  // namespace promise_internal

template <typename C, typename F>
inline auto PromiseRange(const C& container, F&& map,
                         const PromiseRangeOptions& options = {}) {
  auto begin = std::begin(container), end = std::end(container);
  using P = decltype(promise_internal::call_map<C, F>(map, begin, end));
  static_assert(promise_internal::is_promise_v<P>, "map must return a promise");
  using T = typename P::type;

  Promise<std::conditional_t<std::is_void_v<T>, void, std::vector<T>>> promise;

  size_t count_out = std::distance(begin, end);
  if (count_out == 0) {
    if constexpr (std::is_void_v<T>) {
      promise.resolve();
    } else {
      promise.resolve(std::vector<T>{});
    }
    return promise;
  }

  size_t stride = std::max<size_t>(options.stride, 1);
  size_t count_in = count_out / stride + count_out % stride;

  promise_internal::range_state<T>* state = options.arena
      ? options.arena->New<promise_internal::range_state<T>>(
            options, count_in, std::move(promise.deferred()))
      : new promise_internal::range_state<T>(
            options, count_in, std::move(promise.deferred()));

  while (count_out) {
    size_t n = std::min(stride, count_out);
    auto next = begin + n;
    count_out -= n;
    if (options.max_parallelism) state->in_flight.wait();
    if constexpr (std::is_void_v<T>) {
      promise_internal::call_map<C, F>(map, begin, next).then([state] {
        if (state->options.max_parallelism) state->in_flight.post();
        if (--state->count_in == 0) {
          state->done();
          if (state->options.arena) {
            state->~range_state<void>();
          } else {
            delete state;
          }
        }
      });
    } else {
      promise_internal::call_map<C, F>(map, begin, next).then([state](T&& t) {
        if (state->options.max_parallelism) state->in_flight.post();
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->values.push_back(std::forward<T>(t));
        }
        if (--state->count_in == 0) {
          state->done(std::move(state->values));
          if (state->options.arena) {
            state->~range_state<T>();
          } else {
            delete state;
          }
        }
      });
    }
    begin = next;
  }
  assert(begin == end);
  return promise;
}

}  // namespace codulus

#endif  // UTIL_PROMISE_H_
