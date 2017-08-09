# Testing

Ledger has three types of tests.

## Unit tests

**unit tests** are low-level tests written against the smallest testable parts of
the code. Tests for `some_class.{h,cc}` are placed side-by-side the code being
tested, in a `some_class_unittest.cc` file.

Unit tests are regular [Google Test] tests, although most of them use our own
[TestWithMessageLoop] base class to conveniently run delayed tasks with a
timeout, ensuring that a failing test does not hang forever.

All unit tests in the Ledger tree are built into a single `ledger_unittests`
binary, that by default can be executed on Fuchsia by running
`/system/test/ledger_unittests`.

## Integration tests

**integration tests** are written against client-facing FIDL services exposed by
Ledger, although these services still run in the same process as the test code.

Integration tests inherit from [IntegrationTest] and are placed under
`src/test/integration`.

All integration tests in the Ledger tree are built into a single
`ledger_integration_tests` binary, that by default can be executed on Fuchsia by
running `/system/test/ledger_integration_tests`.

## End-to-end tests

**End-to-end tests** are also written against client-facing FIDL services
exposed by Ledger, but in this case the test code runs in a separate process,
and connects to Ledger the same way any other client application would do. This
is the highest-level way of testing that exercises all of the Ledger stack.

End-to-end tests inherit from [LedgerAppTest]. End-to-end tests exercising only the local part of the Ledger are in `src/test/e2e_local`. [Synchronization end-to-end tests] exercising multi-device synchronization are in `src/test/e2e_sync`.

All local application tests in the Ledger tree are built into a single
`ledger_e2e_local` binary, that by default can be executed on Fuchsia by running
`/system/test/ledger_e2e_local`. Synchronization tests are built into a single `ledger_e2e_sync`
binary, that by default can be executed on Fuchsia by running `/system/test/ledger_e2e_sync`

[Google Test]: https://github.com/google/googletest
[TestWithMessageLoop]: https://fuchsia.googlesource.com/ledger/+/master/src/test/test_with_message_loop.h
[IntegrationTest]: https://fuchsia.googlesource.com/ledger/+/master/src/test/integration/integration_test.h
[LedgerAppTest]: https://fuchsia.googlesource.com/ledger/+/master/src/test/e2e_local/e2e_local.cc
[Synchronization end-to-end tests]: https://fuchsia.googlesource.com/ledger/+/master/src/test/e2e_sync/README.md
