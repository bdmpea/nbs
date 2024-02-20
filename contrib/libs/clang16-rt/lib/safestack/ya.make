# Generated by devtools/yamaker.

INCLUDE(${ARCADIA_ROOT}/build/platform/clang/arch.cmake)

LIBRARY(clang_rt.safestack${CLANG_RT_SUFFIX})

LICENSE(
    Apache-2.0 AND
    Apache-2.0 WITH LLVM-exception AND
    MIT AND
    NCSA
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

OWNER(g:cpp-contrib)

ADDINCL(
    contrib/libs/clang16-rt/lib
)

NO_COMPILER_WARNINGS()

NO_UTIL()

NO_SANITIZE()

CFLAGS(
    -fno-builtin
    -fno-exceptions
    -fno-lto
    -fno-rtti
    -fno-stack-protector
    -fomit-frame-pointer
    -funwind-tables
    -fvisibility=hidden
)

SRCDIR(contrib/libs/clang16-rt/lib)

SRCS(
    interception/interception_linux.cpp
    interception/interception_mac.cpp
    interception/interception_type_test.cpp
    interception/interception_win.cpp
    safestack/safestack.cpp
)

END()
