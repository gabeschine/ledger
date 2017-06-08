// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/coroutine/coroutine_impl.h"

#include "gtest/gtest.h"

namespace coroutine {
namespace {

size_t Fact(size_t n) {
  if (n == 0) {
    return 1;
  }
  return Fact(n - 1) * n;
}

void UseStack() {
  EXPECT_EQ(120u, Fact(5));
}

TEST(Coroutine, SingleRoutine) {
  CoroutineServiceImpl coroutine_service;

  CoroutineHandler* handler = nullptr;
  constexpr int kLoopCount = 10;
  int result = kLoopCount;

  coroutine_service.StartCoroutine(
      [&handler, &result](CoroutineHandler* current_handler) {
        handler = current_handler;
        UseStack();
        do {
          EXPECT_FALSE(current_handler->Yield());
          UseStack();
          --result;
        } while (result);
      });

  EXPECT_TRUE(handler);
  EXPECT_EQ(kLoopCount, result);

  for (int i = kLoopCount - 1; i >= 0; --i) {
    handler->Continue(false);
    EXPECT_EQ(i, result);
  }
}

TEST(Coroutine, ManyRoutines) {
  constexpr size_t nb_routines = 1000;

  CoroutineServiceImpl coroutine_service;

  std::set<CoroutineHandler*> handlers;

  for (size_t i = 0; i < nb_routines; ++i) {
    coroutine_service.StartCoroutine([&handlers](CoroutineHandler* handler) {
      handlers.insert(handler);
      UseStack();

      for (size_t i = 0; i < 3; ++i) {
        EXPECT_FALSE(handler->Yield());
        UseStack();
      }

      handlers.erase(handlers.find(handler));
    });
  }

  EXPECT_EQ(nb_routines, handlers.size());

  for (size_t i = 0; i < 2; ++i) {
    for (CoroutineHandler* handler : handlers) {
      handler->Continue(false);
    }
  }

  EXPECT_EQ(nb_routines, handlers.size());

  for (size_t i = 0; i < nb_routines; ++i) {
    (*handlers.begin())->Continue(false);
  }

  EXPECT_TRUE(handlers.empty());
}

TEST(Coroutine, AsyncCall) {
  CoroutineServiceImpl coroutine_service;

  std::function<void(size_t)> callback;
  auto callable = [&callback](std::function<void(size_t)> called_callback) {
    callback = std::move(called_callback);
  };

  size_t received_value = 0;
  coroutine_service.StartCoroutine(
      [callable, &received_value](CoroutineHandler* handler) {
        UseStack();
        size_t value;
        EXPECT_FALSE(SyncCall(handler, callable, &value));
        UseStack();
        received_value = value;
      });

  EXPECT_TRUE(callback);
  EXPECT_EQ(0u, received_value);

  callback(1);

  EXPECT_EQ(1u, received_value);
}

TEST(Coroutine, SynchronousAsyncCall) {
  CoroutineServiceImpl coroutine_service;

  size_t received_value = 0;
  coroutine_service.StartCoroutine(
      [&received_value](CoroutineHandler* handler) {
        UseStack();
        EXPECT_FALSE(SyncCall(
            handler, [](std::function<void(size_t)> callback) { callback(1); },
            &received_value));
        UseStack();
      });
  EXPECT_EQ(1u, received_value);
}

TEST(Coroutine, DroppedAsyncCall) {
  CoroutineServiceImpl coroutine_service;

  size_t received_value = 0;
  bool called = false;
  coroutine_service.StartCoroutine(
      [&received_value, &called](CoroutineHandler* handler) {
        EXPECT_TRUE(SyncCall(
            handler, [&called](std::function<void(size_t)> callback) {
              called = true;
            },
            &received_value));
      });
  EXPECT_EQ(0u, received_value);
  EXPECT_TRUE(called);
}

TEST(Coroutine, Interrupt) {
  bool interrupted = false;

  {
    CoroutineServiceImpl coroutine_service;

    coroutine_service.StartCoroutine([&interrupted](CoroutineHandler* handler) {
      UseStack();
      interrupted = handler->Yield();
      UseStack();
    });

    EXPECT_FALSE(interrupted);
  }

  EXPECT_TRUE(interrupted);
}

TEST(Coroutine, ReuseStack) {
  CoroutineServiceImpl coroutine_service;
  CoroutineHandler* handler = nullptr;
  uintptr_t stack_pointer = 0;
  size_t nb_coroutines_calls = 0;

  for (size_t i = 0; i < 2; ++i) {
    coroutine_service.StartCoroutine(
        [&handler, &stack_pointer,
         &nb_coroutines_calls](CoroutineHandler* called_handler) {
          UseStack();
          int a;
          uintptr_t addr = reinterpret_cast<uintptr_t>(&a);
          if (stack_pointer == 0) {
            stack_pointer = addr;
          }
          EXPECT_EQ(addr, stack_pointer);
          handler = called_handler;
          EXPECT_FALSE(called_handler->Yield());
          UseStack();

          ++nb_coroutines_calls;
        });
    handler->Continue(false);
  }

  EXPECT_EQ(2u, nb_coroutines_calls);
}

}  // namespace
}  // namespace coroutine
