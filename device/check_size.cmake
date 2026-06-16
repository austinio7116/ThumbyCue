file(SIZE "${BIN}" SZ)
if(SZ GREATER ${LIMIT})
    message(FATAL_ERROR
        "CUE SLOT OVERFLOW: ${SZ} bytes > ${LIMIT} partition - the "
        "device boots BLANK. Shrink the binary or grow the partition.")
endif()
math(EXPR FREE "${LIMIT} - ${SZ}")
message(STATUS "cue slot image ${SZ} bytes (${FREE} free of ${LIMIT})")
