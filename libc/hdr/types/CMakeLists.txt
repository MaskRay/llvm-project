add_proxy_header_library(
  char8_t 
  HDRS
    char8_t.h
  DEPENDS
    libc.hdr.uchar_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.char8_t
    libc.include.uchar
)

add_proxy_header_library(
  char32_t 
  HDRS
    char32_t.h
  DEPENDS
    libc.hdr.uchar_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.char32_t
    libc.include.uchar
)

add_proxy_header_library(
  mbstate_t
  HDRS
    mbstate_t.h
  DEPENDS
    libc.include.llvm-libc-types.mbstate_t
  FULL_BUILD_DEPENDS
    libc.include.uchar
)

add_proxy_header_library(
  div_t
  HDRS
    div_t.h
  DEPENDS
    libc.hdr.stdlib_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.div_t
    libc.include.stdlib
)

add_proxy_header_library(
  struct_dl_phdr_info
  HDRS
    struct_dl_phdr_info.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_dl_phdr_info
)

add_proxy_header_library(
  ldiv_t
  HDRS
    ldiv_t.h
  DEPENDS
    libc.hdr.stdlib_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.ldiv_t
    libc.include.stdlib
)

add_proxy_header_library(
  lldiv_t
  HDRS
    lldiv_t.h
  DEPENDS
    libc.hdr.stdlib_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.lldiv_t
    libc.include.stdlib
)

add_proxy_header_library(
  sigset_t
  HDRS
    sigset_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.sigset_t
)

add_proxy_header_library(
  struct_epoll_event
  HDRS
    struct_epoll_event.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_epoll_event
)

add_proxy_header_library(
  struct_flock
  HDRS
    struct_flock.h
  DEPENDS
    libc.hdr.fcntl_overlay
  FULL_BUILD_DEPENDS
    libc.hdr.stdint_proxy
    libc.include.llvm-libc-types.struct_flock
)

add_proxy_header_library(
  struct_flock64
  HDRS
    struct_flock64.h
  DEPENDS
    libc.hdr.fcntl_overlay
  FULL_BUILD_DEPENDS
    libc.hdr.stdint_proxy
    libc.include.llvm-libc-types.struct_flock64
)

add_proxy_header_library(
  struct_f_owner_ex
  HDRS
    struct_f_owner_ex.h
  DEPENDS
    libc.hdr.fcntl_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_f_owner_ex
)

add_proxy_header_library(
  struct_timespec
  HDRS
    struct_timespec.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_timespec
)

add_proxy_header_library(
  struct_tm
  HDRS
    struct_tm.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_tm
)

add_proxy_header_library(
  size_t
  HDRS
    size_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.size_t
)

add_proxy_header_library(
  ssize_t
  HDRS
    ssize_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.ssize_t
)

add_proxy_header_library(
  mode_t
  HDRS
    mode_t.h
  DEPENDS
    libc.hdr.fcntl_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.mode_t
    libc.include.fcntl
)

add_proxy_header_library(
  fenv_t
  HDRS
    fenv_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.fenv_t
    libc.include.fenv
)

add_proxy_header_library(
  fexcept_t
  HDRS
    fexcept_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.fexcept_t
    libc.include.fenv
)

add_proxy_header_library(
  time_t
  HDRS
    time_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.time_t
    libc.include.time
)

add_proxy_header_library(
  clockid_t
  HDRS
    clockid_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.clockid_t
)

add_proxy_header_library(
  clock_t
  HDRS
    clock_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.clock_t
    libc.include.time
)

add_proxy_header_library(
  suseconds_t
  HDRS
    suseconds_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.suseconds_t
    libc.include.sys_time
)

add_proxy_header_library(
  struct_timeval
  HDRS
    struct_timeval.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_timeval
    libc.include.sys_time
)

add_proxy_header_library(
  struct_itimerval
  HDRS
    struct_itimerval.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_itimerval
    libc.include.sys_time
)

add_proxy_header_library(
  pid_t
  HDRS
    pid_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.pid_t
)

add_proxy_header_library(
  pthread_barrier_t
  HDRS
    pthread_barrier_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.pthread_barrier_t
)

add_proxy_header_library(
  pthread_barrierattr_t
  HDRS
    pthread_barrierattr_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.pthread_barrierattr_t
)

add_proxy_header_library(
  atexithandler_t
  HDRS
    atexithandler_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.atexithandler_t
    libc.include.stdlib
)

add_proxy_header_library(
  struct_sigaction
  HDRS
    struct_sigaction.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_sigaction
    libc.include.signal
)

add_proxy_header_library(
  FILE
  HDRS
    FILE.h
  DEPENDS
    libc.hdr.stdio_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.FILE
    libc.include.stdio
)

add_proxy_header_library(
  off_t
  HDRS
    off_t.h
  DEPENDS
    libc.hdr.stdio_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.off_t
    libc.include.stdio
)

add_proxy_header_library(
  cookie_io_functions_t
  HDRS
    cookie_io_functions_t.h
  DEPENDS
    libc.hdr.stdio_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.cookie_io_functions_t
    libc.include.stdio
)

add_proxy_header_library(
  locale_t
  HDRS
    locale_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.locale_t
    libc.include.locale
)

add_proxy_header_library(
  stack_t
  HDRS
    stack_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.stack_t
    libc.include.signal
)

add_proxy_header_library(
  jmp_buf
  HDRS
    jmp_buf.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.jmp_buf
    libc.include.setjmp
)

add_proxy_header_library(
  sigjmp_buf
  HDRS
    sigjmp_buf.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.sigjmp_buf
    libc.include.setjmp
)

add_proxy_header_library(
  struct_msghdr
  HDRS
    struct_msghdr.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_msghdr
    libc.include.sys_socket
)

add_proxy_header_library(
  struct_sockaddr
  HDRS
    struct_sockaddr.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_sockaddr
    libc.include.sys_socket
)

add_proxy_header_library(
  socklen_t
  HDRS
    socklen_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.socklen_t
    libc.include.sys_socket
)

add_proxy_header_library(
  wchar_t
  HDRS
    wchar_t.h
  DEPENDS
    libc.hdr.wchar_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.wchar_t
    libc.include.wchar
)

add_proxy_header_library(
  wint_t
  HDRS
    wint_t.h
  DEPENDS
    libc.hdr.wchar_overlay
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.wint_t
    libc.include.wchar
)

add_proxy_header_library(
  uid_t
  HDRS
    uid_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.uid_t
)

add_proxy_header_library(
  struct_iovec
  HDRS
    struct_iovec.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_iovec
    libc.include.sys_uio
)

add_proxy_header_library(
  nfds_t
  HDRS
    nfds_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.nfds_t
)

add_proxy_header_library(
  struct_pollfd
  HDRS
    struct_pollfd.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.struct_pollfd
)

add_proxy_header_library(
  cpu_set_t
  HDRS
    cpu_set_t.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.cpu_set_t
)

add_proxy_header_library(
  ENTRY
  HDRS
    ENTRY.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.ENTRY
    libc.include.search
)

add_proxy_header_library(
  ACTION
  HDRS
    ACTION.h
  FULL_BUILD_DEPENDS
    libc.include.llvm-libc-types.ACTION
    libc.include.search
)
