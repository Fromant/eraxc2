# Prints a per-source coverage report with gcov.
#
# Run it through the `coverage` target rather than by hand:
#   cmake -S . -B build-cov -DERAX_COVERAGE=ON
#   cmake --build build-cov
#   cmake --build build-cov --target coverage
#
# Inputs: ERAX_SOURCE_DIR, ERAX_BINARY_DIR, ERAX_GCOV, ERAX_TEST_EXE.
# Only this repository's sources are reported: gcov also walks the standard
# library, and a project's coverage number should not drown in it.

foreach(required ERAX_SOURCE_DIR ERAX_BINARY_DIR ERAX_GCOV)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "coverage.cmake: -D${required}=... is required")
    endif()
endforeach()

file(TO_CMAKE_PATH "${ERAX_SOURCE_DIR}" source_dir)
file(TO_CMAKE_PATH "${ERAX_BINARY_DIR}" binary_dir)

if(DEFINED ERAX_TEST_EXE AND EXISTS "${ERAX_TEST_EXE}")
    message(STATUS "coverage: running ${ERAX_TEST_EXE}")
    execute_process(
        COMMAND "${ERAX_TEST_EXE}"
        RESULT_VARIABLE test_result
        OUTPUT_VARIABLE test_output
        ERROR_VARIABLE test_output)
    if(NOT test_result EQUAL 0)
        message(FATAL_ERROR "coverage: the test suite failed (${test_result})\n${test_output}")
    endif()
    string(REGEX MATCH "All tests passed[^\n]*" passed "${test_output}")
    if(passed)
        message(STATUS "coverage: ${passed}")
    endif()
endif()

# Every instrumented object has a .gcno beside it, and gcov on that file reports
# the sources the object pulled in. Anything from a dependency is skipped.
file(GLOB_RECURSE notes "${binary_dir}/*.gcno")
set(measured "")

foreach(note IN LISTS notes)
    get_filename_component(note_dir "${note}" DIRECTORY)
    if(note_dir MATCHES "_deps")
        continue()
    endif()
    execute_process(
        COMMAND "${ERAX_GCOV}" -b -c "${note}"
        WORKING_DIRECTORY "${note_dir}"
        OUTPUT_VARIABLE out
        ERROR_VARIABLE out)
    string(REPLACE "\r\n" "\n" out "${out}")

    # gcov prints `File '<path>'` then a summary per file; keep the project's own
    # .cpp files, which are each measured by exactly one object.
    set(current "")
    set(lines "")
    string(REPLACE "\n" ";" entries "${out}")
    foreach(entry IN LISTS entries)
        if(entry MATCHES "^File '(.*)'$")
            set(current "${CMAKE_MATCH_1}")
        elseif(current MATCHES "^${source_dir}/src/.*\\.cpp$")
            string(REGEX MATCH "Lines executed:([0-9.]+)%" match "${entry}")
            if(match)
                set(lines "${CMAKE_MATCH_1}")
            endif()
            string(REGEX MATCH "Branches executed:([0-9.]+)%" match "${entry}")
            if(match)
                set(branches "${CMAKE_MATCH_1}")
            endif()
            string(REGEX MATCH "Taken at least once:([0-9.]+)%" match "${entry}")
            if(match)
                set(taken "${CMAKE_MATCH_1}")
            endif()
            if(lines AND branches AND taken)
                list(APPEND measured "${current}|${lines}|${branches}|${taken}")
                unset(lines)
                unset(branches)
                unset(taken)
            endif()
        endif()
    endforeach()
endforeach()

if(NOT measured)
    message(FATAL_ERROR "coverage: gcov reported nothing — is the build instrumented (ERAX_COVERAGE=ON)?")
endif()
list(REMOVE_DUPLICATES measured)
list(SORT measured)

# A number right-padded so the columns line up: gcov prints one decimal place.
function(pad out text width)
    string(LENGTH "${text}" length)
    set(spaces "")
    while(length LESS width)
        string(APPEND spaces " ")
        math(EXPR length "${length} + 1")
    endwhile()
    set(${out} "${text}${spaces}" PARENT_SCOPE)
endfunction()

set(report "source                                     lines   branches visited / taken both ways")
foreach(seen IN LISTS measured)
    string(REPLACE "|" ";" fields "${seen}")
    list(GET fields 0 source)
    list(GET fields 1 lines)
    list(GET fields 2 branches)
    list(GET fields 3 taken)
    file(RELATIVE_PATH relative "${source_dir}" "${source}")
    pad(source_cell "${relative}" 44)
    pad(lines_cell "${lines}%" 8)
    pad(visited_cell "${branches}%" 8)
    string(APPEND report "\n${source_cell}${lines_cell}  ${visited_cell}  ${taken}%")
endforeach()
string(APPEND report
       "\n\ngcov counts a branch as visited when one of its outcomes runs; \"taken\" is the")
string(APPEND report "\nshare of branches where both outcomes ran, which is full coverage.\n")

message("${report}")
file(WRITE "${binary_dir}/coverage.txt" "${report}\n")
message(STATUS "coverage: wrote ${binary_dir}/coverage.txt")
