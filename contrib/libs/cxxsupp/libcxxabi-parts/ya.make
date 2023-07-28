LIBRARY()

WITHOUT_LICENSE_TEXTS()

LICENSE(
    Apache-2.0
    WITH
    LLVM-exception
)

VERSION(14.0.6)

ORIGINAL_SOURCE(https://github.com/llvm/llvm-project/archive/llvmorg-14.0.6.tar.gz)

ADDINCL(
    contrib/libs/cxxsupp/libcxxabi/include
    contrib/libs/cxxsupp/libcxx/include
    contrib/libs/cxxsupp/libcxx
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

NO_UTIL()

CFLAGS(-D_LIBCXXABI_BUILDING_LIBRARY)

IF (EXPORT_CMAKE)
    # TODO(YMAKE-91) keep flags required for libc++ vendoring in a separate core.conf variable
    CXXFLAGS(GLOBAL -nostdinc++) 
ENDIF()

SRCDIR(contrib/libs/cxxsupp/libcxxabi)

SRCS(
    src/abort_message.cpp
    src/cxa_demangle.cpp
)

SRC_C_PIC(
    src/cxa_thread_atexit.cpp
    -fno-lto
)

END()
