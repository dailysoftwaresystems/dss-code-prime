dss_add_test(NAME tokenizer/test_source_reader SOURCES test_source_reader.cpp)
dss_add_test(NAME tokenizer/test_token_stream  SOURCES test_token_stream.cpp)
dss_add_test(NAME tokenizer/test_tokenizer     SOURCES test_tokenizer.cpp)
# Mutates the SHIPPED language documents in memory (nlohmann consumer) to pin
# the loader's refusals of `endOfInputImplies` / `atEndOfInput`.
dss_add_test(NAME tokenizer/test_tokenizer_end_of_input SOURCES test_tokenizer_end_of_input.cpp)
target_link_libraries(dss_tokenizer_test_tokenizer_end_of_input
                      PRIVATE nlohmann_json::nlohmann_json)
