# SPDX-License-Identifier: MIT
# Host tests of the JSON library (work package J1), all under the address and
# undefined-behaviour sanitizers; texts are exact-size copies without a
# terminator so any read past the given length is reported.
xiaotai_host_test(json_parse SOURCES json_parse_test.c LINK xiaotai_json)
xiaotai_host_test(json_count SOURCES json_count_test.c LINK xiaotai_json)
xiaotai_host_test(json_query SOURCES json_query_test.c LINK xiaotai_json)
xiaotai_host_test(json_view SOURCES json_view_test.c LINK xiaotai_json)
xiaotai_host_test(json_write SOURCES json_write_test.c LINK xiaotai_json)
