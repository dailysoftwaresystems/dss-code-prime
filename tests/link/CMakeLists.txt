# Linker unit tests (plan 14 LK4 substrate).

dss_add_test(NAME link/test_object_format_schema SOURCES test_object_format_schema.cpp)
dss_add_test(NAME link/test_linker               SOURCES test_linker.cpp)
dss_add_test(NAME link/test_elf_writer           SOURCES test_elf_writer.cpp)
dss_add_test(NAME link/test_pe_writer            SOURCES test_pe_writer.cpp)
dss_add_test(NAME link/test_macho_writer         SOURCES test_macho_writer.cpp)
dss_add_test(NAME link/test_string_table         SOURCES test_string_table.cpp)
dss_add_test(NAME link/test_elf_exec_writer      SOURCES test_elf_exec_writer.cpp)
