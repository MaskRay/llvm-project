add_custom_target(libc_sys_select_unittests)

add_libc_unittest(
  select_ui_test
  NO_RUN_POSTBUILD
  SUITE
    libc_sys_select_unittests
  SRCS
    select_ui_test.cpp
  DEPENDS
    libc.include.unistd
    libc.src.errno.errno
    libc.src.sys.select.select
    libc.src.unistd.read
    libc.test.UnitTest.ErrnoCheckingTest
    libc.test.UnitTest.ErrnoSetterMatcher
)

add_libc_unittest(
  select_failure_test
  SUITE
    libc_sys_select_unittests
  SRCS
    select_failure_test.cpp
  DEPENDS
    libc.include.unistd
    libc.src.errno.errno
    libc.src.sys.select.select
    libc.src.unistd.read
    libc.test.UnitTest.ErrnoCheckingTest
    libc.test.UnitTest.ErrnoSetterMatcher
)

add_subdirectory(testdata)
