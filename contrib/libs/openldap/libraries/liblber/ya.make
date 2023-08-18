# Generated by devtools/yamaker.

LIBRARY()

LICENSE(
    Bsd-Original-Uc-1986 AND
    OLDAP-2.8
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

ADDINCL(
    contrib/libs/openldap/include
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

CFLAGS(
    -DLBER_LIBRARY
    -DLDAPI_SOCK=\"/run/openldap/ldapi\"
)

SRCS(
    assert.c
    bprint.c
    debug.c
    decode.c
    encode.c
    io.c
    memory.c
    options.c
    sockbuf.c
    stdio.c
    version.c
)

END()
