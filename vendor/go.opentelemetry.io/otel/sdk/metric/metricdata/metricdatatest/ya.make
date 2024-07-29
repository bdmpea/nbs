GO_LIBRARY()

SUBSCRIBER(g:go-contrib)

LICENSE(Apache-2.0)

SRCS(
    assertion.go
    comparisons.go
)

GO_TEST_SRCS(assertion_test.go)

END()

RECURSE(
    gotest
)
