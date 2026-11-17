dss_add_test(NAME tokenizer/test_source_reader SOURCES test_source_reader.cpp)
dss_add_test(NAME tokenizer/test_token_stream  SOURCES test_token_stream.cpp)
dss_add_test(NAME tokenizer/test_tokenizer     SOURCES test_tokenizer.cpp)
# P69 (fold f26d, ruling 643 (3)): the dynamic delimiter tag read by the tokenizer — a bounded
# run, matched by a loop; and a frame entered by replacement captures it as a pushed one does.
dss_add_test(NAME tokenizer/test_tokenizer_delimiter_tag SOURCES test_tokenizer_delimiter_tag.cpp)
# Mutates the SHIPPED language documents in memory (nlohmann consumer) to pin
# the loader's refusals of `endOfInputImplies` / `atEndOfInput`.
dss_add_test(NAME tokenizer/test_tokenizer_end_of_input SOURCES test_tokenizer_end_of_input.cpp)
target_link_libraries(dss_tokenizer_test_tokenizer_end_of_input
                      PRIVATE nlohmann_json::nlohmann_json)
